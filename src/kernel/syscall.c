#include "aarch64/mmu.h"
#include "common/defines.h"
#include <common/sem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/pt.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

void *syscall_table[NR_SYSCALL];

void syscall_entry(UserContext *context) {
  // TODO
  // Invoke syscall_table[id] with args and set the return value.
  // id is stored in x8. args are stored in x0-x5. return value is stored in x0.

  // for (u64* p = (u64*)&early_init; p < (u64*)&rest_init; p++)
  // ((void(*)())*p)();
  // printk("in syscall\n");
  u64 id = context->x[8], ret = 0;
  if (id < NR_SYSCALL) {
    auto function_ptr = (u64 *)syscall_table[id];
    ret = ((u64(*)(u64, u64, u64, u64, u64, u64))function_ptr)(
        context->x[0], context->x[1], context->x[2], context->x[3],
        context->x[4], context->x[5]);
    context->x[0] = ret;
  }
}

// check if the virtual address [start,start+size) is READABLE by the current
// user process
bool user_readable(const void *start, usize size) {
  // TODO
  return ((u64)start & PTE_USER_DATA) &&
         ((u64)(start + size - 1) & PTE_USER_DATA);
}

// check if the virtual address [start,start+size) is READABLE & WRITEABLE by
// the current user process
bool user_writeable(const void *start, usize size) {
  // TODO
  return ((u64)start & PTE_USER_DATA & PTE_RW) &&
         ((u64)(start + size - 1) & PTE_USER_DATA & PTE_RW);
}

// get the length of a string including tailing '\0' in the memory space of
// current user process return 0 if the length exceeds maxlen or the string is
// not readable by the current user process
usize user_strlen(const char *str, usize maxlen) {
  for (usize i = 0; i < maxlen; i++) {
    printk("user_readable?%d\n", user_readable(&str[i], 1));

    if (user_readable(&str[i], 1)) {
      if (str[i] == 0)
        return i + 1;
    } else
      return 0;
  }
  return 0;
}
