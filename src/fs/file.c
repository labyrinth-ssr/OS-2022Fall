/* File descriptors */

#include "file.h"
#include "fs.h"
#include "fs/cache.h"
#include "fs/defines.h"
#include "fs/pipe.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/spinlock.h>
#include <fs/inode.h>
#include <kernel/mem.h>

static struct ftable ftable;

void init_ftable() {
  // Modified: initialize your ftable
  init_spinlock(&ftable.lock);
}

void init_oftable(struct oftable *oftable) {
  // Modified: initialize your oftable for a new process
  for (int fd = 0; fd < NOFILE; fd++) {
    oftable->ofile[fd] = NULL;
  }
}

/* Allocate a file structure. */
struct file *filealloc() {
  /* Modified: Lab10 Shell */
  struct file *f;
  _acquire_spinlock(&ftable.lock);
  for (f = ftable.file; f < ftable.file + NFILE; f++) {
    if (f->ref == 0) {
      f->ref = 1;
      _release_spinlock(&ftable.lock);
      return f;
    }
  }
  _release_spinlock(&ftable.lock);
  return 0;
}

/* Increment ref count for file f. */
struct file *filedup(struct file *f) {
  /* Modified: Lab10 Shell */
  _acquire_spinlock(&ftable.lock);
  if (f->ref < 1)
    panic("file is not opened");
  f->ref++;
  _release_spinlock(&ftable.lock);
  return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void fileclose(struct file *f) { /* Modified: Lab10 Shell */
  struct file ff;

  _acquire_spinlock(&ftable.lock);
  if (f->ref < 1)
    panic("file is not opened");
  if (--f->ref > 0) {
    _release_spinlock(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  _release_spinlock(&ftable.lock);

  if (ff.type == FD_PIPE) {
    pipeClose(ff.pipe, ff.writable);
  } else if (ff.type == FD_INODE /* || ff.type == FD_DEVICE */) {
    OpContext ctx;
    bcache.begin_op(&ctx);
    inodes.put(&ctx, ff.ip);
    bcache.end_op(&ctx);
  }
}

/* Get metadata about file f. */
int filestat(struct file *f, struct stat *st) {
  /* Modified: Lab10 Shell */
  // struct proc *p = thisproc();

  if (f->type == FD_INODE /* || f->type == FD_DEVICE */) {
    inodes.lock(f->ip);
    stati(f->ip, st);
    inodes.unlock(f->ip);
    // if (copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
    //   return -1;
    return 0;
  }
  return -1;
}

/* Read from file f. */
isize fileread(struct file *f, char *addr, isize n) {
  /* Modified: Lab10 Shell */
  int r = 0;

  if (f->readable == 0)
    return -1;

  if (f->type == FD_PIPE) {
    r = pipeRead(f->pipe, (u64)addr, n);
  } /* else if (f->type == FD_DEVICE) {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  }  */
  else if (f->type == FD_INODE) {
    inodes.lock(f->ip);
    if ((r = inodes.read(f->ip, (u8 *)addr, f->off, n)) > 0)
      f->off += r;
    inodes.unlock(f->ip);
  } else {
    panic("invalid file");
  }

  return r;
}

/* Write to file f. */
isize filewrite(struct file *f, char *addr, isize n) {
  /* Modified: Lab10 Shell */
  int r, ret = 0;

  if (f->writable == 0)
    return -1;

  if (f->type == FD_PIPE) {
    ret = pipeWrite(f->pipe, (u64)addr, n);
  } /* else if (f->type == FD_DEVICE) {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  }  */
  else if (f->type == FD_INODE) {
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((OP_MAX_NUM_BLOCKS - 1 - 1 - 2) / 2) * BLOCK_SIZE;
    int i = 0;
    while (i < n) {
      int n1 = n - i;
      if (n1 > max)
        n1 = max;
      OpContext ctx;
      bcache.begin_op(&ctx);
      inodes.lock(f->ip);
      if ((r = inodes.write(&ctx, f->ip, (u8 *)(addr + i), f->off, n1)) > 0)
        f->off += r;
      inodes.unlock(f->ip);
      bcache.end_op(&ctx);

      if (r != n1) {
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("invalid file.");
  }

  return ret;
}
