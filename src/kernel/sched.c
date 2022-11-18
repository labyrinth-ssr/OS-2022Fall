#include "common/defines.h"
#include "common/rbtree.h"
#include "kernel/container.h"
#include "kernel/schinfo.h"
#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <driver/clock.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

extern bool panic_flag;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);
extern struct proc root_proc;
extern struct container root_container;

static SpinLock rqlock;
// static ListNode rq;
static struct timer sched_timer[4];
static bool sched_timer_set[4];

static bool __sched_cmp(rb_node lnode, rb_node rnode) {
  i64 d = container_of(lnode, struct proc, schinfo.rq)->schinfo.vruntime -
          container_of(rnode, struct proc, schinfo.rq)->schinfo.vruntime;
  if (d < 0)
    return true;
  if (d == 0)
    return lnode < rnode;
  return false;
}

static void sched_timer_handler(struct timer *t) {
  // (void)t;
  if (t->triggered) {
    set_cpu_timer(&sched_timer[cpuid()]);
    if (thisproc()->schinfo.vruntime >= thisproc()->schinfo.permit_time) {
      yield();
    }
  }
}

define_early_init(rq) {
  init_spinlock(&rqlock);
  // init_list_node(&rq);
}

define_init(sched) {
  for (int i = 0; i < NCPU; i++) {
    struct proc *p = kalloc(sizeof(struct proc));
    p->idle = true;
    p->state = RUNNING;
    cpus[i].sched.thisproc = cpus[i].sched.idle = p;
  }
}

struct proc *thisproc() {
  return cpus[cpuid()].sched.thisproc;
}

void init_schinfo(struct schinfo *p, bool group) {
  // TODO: initialize your customized schinfo for every newly-created process
  memset(p, 0, sizeof(struct schinfo));
  // init_list_node(&p->rq);
  p->group = group;
}

void init_schqueue(struct schqueue *s) {
  // init_list_node(&s->rq);
  s->rq.rb_node = NULL;
}

void _acquire_sched_lock() { _acquire_spinlock(&rqlock); }

void _release_sched_lock() { _release_spinlock(&rqlock); }

bool is_zombie(struct proc *p) {
  bool r;
  _acquire_sched_lock();
  r = p->state == ZOMBIE;
  _release_sched_lock();
  return r;
}

bool is_used(struct proc *p) {
  bool r;
  _acquire_sched_lock();
  r = p->state != UNUSED;
  _release_sched_lock();
  return r;
}

bool _activate_proc(struct proc *p, bool onalert) {
  // TODO
  // if the proc->state is RUNNING/RUNNABLE, do nothing
  // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE
  // and add it to the sched queue
  _acquire_sched_lock();
  if (!onalert && p->state == ZOMBIE) {
    PANIC();
  }
  if (p->state == RUNNING || p->state == RUNNABLE) {
    _release_sched_lock();
    return false;
  }
  if (p->state == SLEEPING || p->state == UNUSED) {
    p->state = RUNNABLE;
    // p->schinfo.prio = 100;
    // _insert_into_list(&p->container->schqueue.rq, &p->schinfo.rq);
    printk("root container %p\n", &root_container);
    printk("pid %d insert container %p\n", p->pid, p->container);
    ASSERT(_rb_insert(&p->schinfo.rq, &p->container->schqueue.rq,
                      __sched_cmp) == 0);

  } else if (p->state == ZOMBIE) {
    return false;
  }
  _release_sched_lock();
  return true;
}

static void update_this_state(enum procstate new_state) {
  // update the state of current process to new_state, and remove it from the
  // sched queue if new_state=SLEEPING/ZOMBIE
  auto this = thisproc();
  this->state = new_state;
  printk("pid %d new stater %d\n", thisproc()->pid, thisproc()->state);
  if (new_state == SLEEPING || new_state == ZOMBIE) {
    // _detach_from_list(&this->schinfo.rq);
    _rb_erase(&this->schinfo.rq, &this->container->schqueue.rq);
    printk("pid %d insert container %p\n", thisproc()->pid,
           thisproc()->container);
  }
  // else if (new_state == RUNNABLE) {
  //   this->schinfo.prio = 0;
  // }
}

static struct proc *pick_next() {
  if (panic_flag) {
    return cpus[cpuid()].sched.idle;
  }

  // (void)res_proc;
  // int max_prior = 0;
  // (void)max_prior;
  // _for_in_list(p, &thisproc()->container->schqueue.rq) {
  //   if (p == &thisproc()->container->schqueue.rq) {
  //     continue;
  //   }
  //   auto proc = container_of(p, struct proc, schinfo.rq);
  //   cnt++;
  //   if (proc->state == RUNNABLE && proc->schinfo.prio >= max_prior) {
  //     max_prior = proc->schinfo.prio;
  //     res_proc = proc;
  //   }
  // }

  // if (res_proc != NULL) {
  //   _for_in_list(p, &thisproc()->container->schqueue.rq) {
  //     if (p == &thisproc()->container->schqueue.rq) {
  //       continue;
  //     }
  //     auto proc = container_of(p, struct proc, schinfo.rq);
  //     if (proc->state == RUNNABLE && proc != res_proc) {
  //       proc->schinfo.prio++;
  //     }
  //   }
  // return res_proc;
  // }
  //应该不存在时间片耗尽仍在first的情况
  struct proc *res_proc = NULL;
  rb_node get_node;
  rb_root this_rq = &root_container.schqueue.rq;
  while (1) {
    get_node = _rb_first(this_rq);
    if (get_node == NULL) {
      break;
    }
    auto candidate_proc = container_of(get_node, struct proc, schinfo.rq);
    if (candidate_proc->schinfo.group) {
      this_rq = &res_proc->container->schqueue.rq;
    } else if (candidate_proc->state == RUNNABLE) {
      res_proc = candidate_proc;
      break;
    }
  }
  if (res_proc != NULL) {
    return res_proc;
  }

  return cpus[cpuid()].sched.idle;
}

void activate_group(struct container *group) {
  // TODO: add the schinfo node of the group to the schqueue of its parent
  ASSERT(
      _rb_insert(&group->schinfo.rq, &group->parent->schqueue.rq, __sched_cmp));
}

static void update_this_proc(struct proc *p) {

  if (p->pid != 0 && p != &root_proc && !sched_timer_set[cpuid()]) {
    sched_timer[cpuid()].elapse = 1;
    sched_timer[cpuid()].handler = sched_timer_handler;
    set_cpu_timer(&sched_timer[cpuid()]);
    sched_timer_set[cpuid()] = true;
  }

  cpus[cpuid()].sched.thisproc = p;
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
static void simple_sched(enum procstate new_state) {
  printk("sched %d,cpu%d\n", thisproc()->pid, cpuid());
  auto this = thisproc();
  ASSERT(this->state == RUNNING);
  if (this->killed && new_state != ZOMBIE) {
    _release_sched_lock();
    return;
  }
  update_this_state(new_state);
  auto next = pick_next();
  update_this_proc(next);
  printk("cpu%d next proc%p,pid:%d,state:%d,idle?:%d\n", cpuid(), next,
         next->pid, next->state, next->idle);
  ASSERT(next->state == RUNNABLE);
  next->state = RUNNING;
  if (next != this) {
    attach_pgdir(&next->pgdir);
    next->schinfo.start_time = get_timestamp_ms();
    this->schinfo.vruntime += get_timestamp_ms() - this->schinfo.start_time;
    swtch(next->kcontext, &this->kcontext);
    attach_pgdir(&this->pgdir);
  }
  _release_sched_lock();
}

//调度器锁：保持state、调度队列等一致，保持原子性
__attribute__((weak, alias("simple_sched"))) void
_sched(enum procstate new_state);

u64 proc_entry(void (*entry)(u64), u64 arg) {
  _release_sched_lock();
  set_return_addr(entry);
  return arg;
}
