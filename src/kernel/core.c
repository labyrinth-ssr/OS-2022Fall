#include <driver/sd.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>

bool panic_flag;
extern void sd_init();
extern void sd_test();

NO_RETURN void idle_entry() {
  set_cpu_on();
  while (1) {
    yield();
    if (panic_flag)
      break;
    // if (cpuid() == 0) {
    arch_with_trap { arch_wfi(); }
    // } else {
    //   arch_stop_cpu();
    // }
  }
  set_cpu_off();
  arch_stop_cpu();
}

NO_RETURN void kernel_entry() {

  // sd_init();
  // sd_test();

  // vm_test();
  // user_proc_test();

  // proc_test();
  // user_proc_test();
  // container_test();
  // sd_test();
  do_rest_init();

  pgfault_first_test();
  pgfault_second_test();

  while (1)
    yield();
}

NO_INLINE NO_RETURN void _panic(const char *file, int line) {
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
