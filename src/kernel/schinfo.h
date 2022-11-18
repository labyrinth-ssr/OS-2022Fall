#pragma once

#include "common/defines.h"
#include "common/rbtree.h"
// #include "kernel/container.h"
#include <common/list.h>

#define ELAPSE 20
#define MIN_PERMIT 1

struct proc; // dont include proc.h here

// embedded data for cpus
struct sched {
  // TODO: customize your sched info
  struct proc *thisproc;
  struct proc *idle; // always exists
};

// embeded data for procs
struct schinfo {
  // TODO: customize your sched info
  struct rb_node_ rq;
  // int prio;
  bool group;
  // int running_num;
  u64 start_time;
  u64 vruntime;

  unsigned long long permit_time; // = Max(ELAPSE/running_num,MIN_PERMIT)

  // int time_counter;
  // nice value?
};

// embedded data for containers
struct schqueue {
  // TODO: customize your sched queue
  struct rb_root_ rq;
  // ListNode rq;
};
