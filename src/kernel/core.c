#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/init.h>
#include <kernel/sched.h>
#include <test/test.h>
#include <driver/sd.h>
#include <fs/cache.h>
#include <kernel/paging.h>
#include <kernel/mem.h>

bool panic_flag;

NO_RETURN void idle_entry() {
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap {
            arch_wfi();
        }
    }
    set_cpu_off();
    arch_stop_cpu();
}

extern void icode();
extern void eicode();
extern void trap_return();
void kernel_entry() {
    printk("hello world %d\n", (int)sizeof(struct proc));

    // proc_test();
    // user_proc_test();
    // container_test();
    // sd_test();
    do_rest_init();

    // TODO: map init.S to user space and trap_return to run icode
    auto this = thisproc();
    this->cwd = inodes.share(inodes.root);
    struct section* st = (struct section*)kalloc(sizeof(struct section));
    st->flags = ST_TEXT;
    init_sleeplock(&(st->sleeplock));
    st->begin = 0x8000;
    st->end = 0x8000 + (u64)eicode - PAGE_BASE((u64)icode) + 1;
    init_list_node(&(st->stnode));
    _insert_into_list(&(this->pgdir.section_head), &(st->stnode));
    
    // map
    for (u64 i = PAGE_BASE((u64)(icode)); i <= (u64)eicode; i += PAGE_SIZE) {
        vmmap(&(this->pgdir), 0x8000 + (i - PAGE_BASE((u64)icode)), (void*)i, PTE_USER_DATA | PTE_RO);
    }

    // return
    this->ucontext->elr = (u64)0x8000 + (u64)icode - (PAGE_BASE((u64)icode));
    set_return_addr(trap_return);
}

NO_INLINE NO_RETURN void _panic(const char* file, int line) {
    printk("=====%s:%d PANIC%d!=====\n", file, line, cpuid());
    panic_flag = true;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}
