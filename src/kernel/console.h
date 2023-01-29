#pragma once
#include <common/defines.h>
#include <fs/inode.h>
void console_intr(char (*)());
void _console_intr();
isize console_write(Inode *ip, char *buf, isize n);
isize console_read(Inode *ip, char *dst, isize n);