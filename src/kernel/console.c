#include "common/defines.h"
#include "common/sem.h"
#include "common/spinlock.h"
#include "driver/interrupt.h"
#include "fs/inode.h"
#include "kernel/printk.h"
#include <aarch64/intrinsic.h>
#include <driver/uart.h>
#include <kernel/console.h>
#include <kernel/init.h>
#include <kernel/sched.h>
#define INPUT_BUF 128
struct {
  char buf[INPUT_BUF];
  usize r; // Read index
  usize w; // Write index
  usize e; // Edit index
} input;
#define C(x) ((x) - '@') // Control-x
#define BACKSPACE 0x100
// the use of lock?
static SpinLock lock;
static SleepLock slplock;
void _console_intr() { (console_intr(uart_get_char)); }

define_early_init(console_init) {
  init_spinlock(&lock);
  init_sleeplock(&slplock);
  set_interrupt_handler(IRQ_AUX, _console_intr);
}

void consputc(int c) {
  if (c == BACKSPACE) {
    uart_put_char('\b');
    uart_put_char(' ');
    uart_put_char('\b');
  } else {
    uart_put_char(c);
  }
}

//将 buf 中的内容在console显示。
isize console_write(Inode *ip, char *buf, isize n) {
  inodes.unlock(ip);
  _acquire_spinlock(&lock);
  for (int i = 0; i < n; i++)
    consputc(buf[i]);
  _release_spinlock(&lock);
  inodes.lock(ip);
  return n;
}

//读出console缓冲区的内容n个字符到 dst 。遇见 EOF 提前结束。返回读出的字符数。
isize console_read(Inode *ip, char *dst, isize n) {
  inodes.unlock(ip);
  isize target = n;
  _acquire_spinlock(&lock);
  while (n) {
    while (input.r == input.w) {
      if (thisproc()->killed) {
        _release_spinlock(&lock);
        inodes.lock(ip);
        return -1;
      }
      ASSERT((_acquire_sleeplock(&slplock)));
    }
    int c = input.buf[input.r++ % INPUT_BUF];
    if (c == C('D')) {
      if (n < target) {
        // 如果是EOF，不读取，保证下一次是0 byte？
        input.r--;
      }
      break;
    }
    *dst++ = c;
    n--;
    // maybe
    if (c == '\n') {
      break;
    }
  }
  _release_spinlock(&lock);
  inodes.lock(ip);
  return target - n;
}

// TODO
void console_intr(char (*getc)()) {
  int c = 0;

  while (1) {
    char c = uart_get_char();
    if (c == (char)-1)
      break;
    _acquire_spinlock(&lock);
    switch (c) {
    case C('U'):
      while (input.e != input.w && input.buf[(input.e) % INPUT_BUF != '\n']) {
        input.e--;
        uart_put_char(c);
      }
    }
  }
}
