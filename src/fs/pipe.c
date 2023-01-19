#include "common/defines.h"
#include "common/spinlock.h"
#include "fs/file.h"
#include <common/string.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/sched.h>

int pipeAlloc(File **f0, File **f1) {
  // Modified
  struct pipe *pi = NULL;
  if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  // kalloc_page fail
  if ((pi = (struct pipe *)kalloc(sizeof(struct pipe))) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  init_spinlock(&pi->lock);
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;

  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

bad:
  if (pi)
    kfree((char *)pi);
  if (*f0)
    fileclose(*f0);
  if (*f1)
    fileclose(*f1);
  return -1;
}

void pipeClose(Pipe *pi, int writable) {
  // Modified
  _acquire_spinlock(&pi->lock);
  if (writable) {
    pi->writeopen = 0;
    // 唤醒未关闭的一端
    wakeup(&pi->rlock);
  } else {
    pi->readopen = 0;
    wakeup(&pi->wlock);
  }
  if (pi->readopen == 0 && pi->writeopen == 0) {
    _release_spinlock(&pi->lock);
    kfree(pi);
  } else
    _release_spinlock(&pi->lock);
}

int pipeWrite(Pipe *pi, u64 addr, int n) {
  // Modified
  int i = 0;
  struct proc *pr = thisproc();
  _acquire_spinlock(&pi->lock);
  while (i < n) {
    if (pi->readopen == 0 || is_killed(pr)) {
      _release_spinlock(&pi->lock);
      return -1;
    }
    if (pi->nwrite == pi->nread + PIPESIZE) { // DOC: pipewrite-full
      wakeup(&pi->rlock);
      sleep(&pi->wlock);
    } else {
      char *dest = &pi->data[pi->nwrite++ % PIPESIZE];
      memmove(dest, (void *)addr, 1);
      i++;
    }
  }
  wakeup(&pi->rlock);
  _release_spinlock(&pi->lock);

  return i;
}

int pipeRead(Pipe *pi, u64 addr, int n) {
  // Modified
  int i;
  struct proc *pr = thisproc();
  char ch;
  _acquire_spinlock(&pi->lock);
  while (pi->nread == pi->nwrite && pi->writeopen) { // DOC: pipe-empty
    if (is_killed(pr)) {
      _release_spinlock(&pi->lock);
      return -1;
    }
    sleep(&pi->rlock); // DOC: piperead-sleep
  }
  for (i = 0; i < n; i++) { // DOC: piperead-copy
    if (pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    memmove((void *)(addr + i), &ch, 1);
  }
  wakeup(&pi->wlock); // DOC: piperead-wakeup
  _release_spinlock(&pi->lock);
  return i;
}