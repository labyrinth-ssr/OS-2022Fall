#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <kernel/pt.h>
#include <kernel/paging.h>

void* syscall_table[NR_SYSCALL];

void syscall_entry(UserContext* context)
{
    // TODO
    // Invoke syscall_table[id] with args and set the return value.
    // id is stored in x8. args are stored in x0-x5. return value is stored in x0.
    u64 id = context->x[8];
    u64 x0 = context->x[0];
    u64 x1 = context->x[1];
    u64 x2 = context->x[2];
    u64 x3 = context->x[3];
    u64 x4 = context->x[4];
    u64 x5 = context->x[5];
    if (id < NR_SYSCALL) {
        void* func = syscall_table[id];
        if (func != NULL) {
            context->x[0] = ((u64(*)(u64, u64, u64, u64, u64, u64))func)(x0, x1, x2, x3, x4, x5);
        } else {
            printk("syscall %lld not find\n", id);
            PANIC();
        }
    }
}

// check if the virtual address [start,start+size) is READABLE by the current user process
bool user_readable(const void* start, usize size) {
    // TODO
    if (((u64)start >> 48) != 0) {
        return true;
    }
    auto this = thisproc();
    bool ok = FALSE;
    auto st_node = this->pgdir.section_head.next;
    while (st_node != &(this->pgdir.section_head)) {
        auto st = container_of(st_node, struct section, stnode);
        if ((u64)st->begin <= (u64)start && (u64)start + size <= st->end) {
            ok = TRUE;
            break;
        }
        st_node = st_node->next;
    }
    return ok;
}

// check if the virtual address [start,start+size) is READABLE & WRITEABLE by the current user process
bool user_writeable(const void* start, usize size) {
    // TODO
    auto this = thisproc();
    bool ok = FALSE;
    auto st_node = this->pgdir.section_head.next;
    while (st_node != &(this->pgdir.section_head)) {
        auto st = container_of(st_node, struct section, stnode);
        if (st->flags & ST_RO) {
            continue;
        }
        if ((u64)st->begin <= (u64)start && (u64)start + size <= st->end) {
            ok = TRUE;
            break;
        }
        st_node = st_node->next;
    }
    return ok;
}

// get the length of a string including tailing '\0' in the memory space of current user process
// return 0 if the length exceeds maxlen or the string is not readable by the current user process
usize user_strlen(const char* str, usize maxlen) {
    for (usize i = 0; i < maxlen; i++) {
        if (user_readable(&str[i], 1)) {
            if (str[i] == 0)
                return i + 1;
        } else
            return 0;
    }
    return 0;
}
