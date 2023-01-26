#pragma once

__attribute__((format(printf, 1, 2))) void printk(const char *, ...);
#define panic(string)                                                          \
  ({                                                                           \
    printk("%s\n", string);                                                    \
    PANIC();                                                                   \
  })

// Should be provided by drivers. By default, putch = uart_put_char.
extern void putch(char);
