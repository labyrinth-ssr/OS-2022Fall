#pragma once

// #include "common/spinlock.h"
#include <kernel/proc.h>
#include <kernel/schinfo.h>
#define PID_NUM 100

struct pid_pool {
  int freelist[PID_NUM];
  int avail;
};

struct container {
  struct container *parent;
  struct proc *rootproc;

  struct schinfo schinfo;
  struct schqueue schqueue;

  // TODO: namespace (local pid?)
  SpinLock pid_lock;
  struct pid_pool pids;
};

struct container *create_container(void (*root_entry)(), u64 arg);
void set_container_to_this(struct proc *);
