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

#define TIMER_ELAPSE 1
#define WEIGHT_SUM 1
#define SLICE 5

static int con_to_weight[5] = {1, 1, 1, 1, 1};
static int con_to_limit[5] = {1, 1, 1, 1, 1};

extern bool panic_flag;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);
extern struct proc root_proc;
extern struct container root_container;

static SpinLock rqlock;
static struct timer sched_timer[4];
static bool sched_timer_set[4];

static bool __sched_cmp(rb_node lnode, rb_node rnode) {
  i64 d = container_of(lnode, struct schinfo, rq)->vruntime -
          container_of(rnode, struct schinfo, rq)->vruntime;
  if (d < 0)
    return true;
  if (d == 0)
    return lnode < rnode;
  return false;
}

void _acquire_sched_lock() { _acquire_spinlock(&rqlock); }

void _release_sched_lock() { _release_spinlock(&rqlock); }

static void sched_timer_handler(struct timer *t) {
  if (t->triggered) {
    set_cpu_timer(&sched_timer[cpuid()]);
    if (thisproc()->idle ||
        (get_timestamp_ms() - thisproc()->schinfo.start_time) *
                con_to_weight[thisproc()->container->id] / WEIGHT_SUM >=
            (u64)SLICE / con_to_limit[thisproc()->container->id]) {
      yield();
    }
  }
}

define_early_init(rq) { init_spinlock(&rqlock); }

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
  p->vruntime = 0;
  p->group = group;
}

void init_schqueue(struct schqueue *s) {
  s->rq.rb_node = NULL;
  s->node_cnt = 0;
}

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
  if (p->state == RUNNING || p->state == RUNNABLE || p->state == ZOMBIE ||
      (p->state == DEEPSLEEPING && onalert)) {
    _release_sched_lock();
    return false;
  }
  if (p->state == SLEEPING || p->state == UNUSED ||
      (p->state == DEEPSLEEPING && !onalert)) {
    p->state = RUNNABLE;
    ASSERT(_rb_insert(&p->schinfo.rq, &p->container->schqueue.rq,
                      __sched_cmp) == 0);
    p->container->schqueue.node_cnt++;
  }
  _release_sched_lock();
  return true;
}

static void update_this_state(enum procstate new_state) {
  // update the state of current process to new_state, and remove it from the
  // sched queue if new_state=SLEEPING/ZOMBIE
  auto this = thisproc();
  if (!this->idle && this->state == RUNNING && new_state == RUNNABLE) {

    auto delta_time = (get_timestamp_ms() - thisproc()->schinfo.start_time) *
                      con_to_weight[thisproc()->container->id] / WEIGHT_SUM;
    thisproc()->schinfo.vruntime += delta_time;

    ASSERT(_rb_insert(&this->schinfo.rq, &this->container->schqueue.rq,
                      __sched_cmp) == 0);
    this->container->schqueue.node_cnt++;

    auto container = thisproc()->container;

    while (container->parent != NULL) {
      container->schinfo.vruntime += delta_time;
      // container_of(container->schqueue.rq.rb_node, struct schinfo, rq)
      //     ->vruntime;
      _rb_erase(&container->schinfo.rq, &container->parent->schqueue.rq);
      ASSERT(_rb_insert(&container->schinfo.rq, &container->parent->schqueue.rq,
                        __sched_cmp) == 0);
      container = container->parent;
    }
  }
  this->state = new_state;
}

rb_node _rb_first_ex_p(rb_node n) {
  rb_node left = NULL;
  if (!n)
    return NULL;
  if (n->rb_left != NULL) {
    left = _rb_first_ex_p(n->rb_left);
  }
  if (left != NULL) {
    return left;
  }
  if (!(container_of(n, struct schinfo, rq)->group &&
        container_of(n, struct schinfo, rq)->skip)) {
    return n;
  }
  if (n->rb_right != NULL) {
    return _rb_first_ex_p(n->rb_right);
  }
  return NULL;
}

void _rb_fix_skip(rb_node rt_node) {
  if (rt_node != NULL) {
    container_of(rt_node, struct schinfo, rq)->skip = false;
    struct schinfo *info = container_of(rt_node, struct schinfo, rq);
    if (info->group) {
      _rb_fix_skip(
          container_of(info, struct container, schinfo)->schqueue.rq.rb_node);
    }
    _rb_fix_skip(rt_node->rb_left);
    _rb_fix_skip(rt_node->rb_right);
  }
}

static struct proc *pick_next_r(rb_root rt) {
  if (panic_flag) {
    return cpus[cpuid()].sched.idle;
  }
  rb_node get_node = NULL;
  get_node = _rb_first_ex_p(rt->rb_node);
  struct container *this_container =
      container_of(rt, struct container, schqueue.rq);

  if (this_container != &root_container &&
      this_container->schqueue.node_cnt != 0 && get_node == NULL) {
    this_container->schinfo.skip = true;
    return pick_next_r(&this_container->parent->schqueue.rq);
  }

  if (get_node == NULL) {
    return cpus[cpuid()].sched.idle;
  }

  struct schinfo *schinfo = container_of(get_node, struct schinfo, rq);

  if (!schinfo->group) {
    struct proc *res_proc = container_of(schinfo, struct proc, schinfo);
    _rb_erase(&res_proc->schinfo.rq, &res_proc->container->schqueue.rq);
    res_proc->container->schqueue.node_cnt--;
    ASSERT(res_proc->container->schqueue.node_cnt >= 0);
    return res_proc;
  } else {
    if (container_of(schinfo, struct container, schinfo)->schqueue.node_cnt ==
        0) {
      schinfo->skip = true;
      return pick_next_r(rt);
    }
    return pick_next_r(
        &container_of(schinfo, struct container, schinfo)->schqueue.rq);
  }
}

static struct proc *pick_next() {
  auto ret = pick_next_r(&root_container.schqueue.rq);
  _rb_fix_skip(root_container.schqueue.rq.rb_node);
  return ret;
}

void activate_group(struct container *group) {
  // TODO: add the schinfo node of the group to the schqueue of its parent
  _acquire_sched_lock();
  ASSERT(_rb_insert(&group->schinfo.rq, &group->parent->schqueue.rq,
                    __sched_cmp) == 0);
  group->parent->schqueue.node_cnt++;
  _release_sched_lock();
}

static void update_this_proc(struct proc *p) {

  if (p->pid != 0 && p != &root_proc && !sched_timer_set[cpuid()]) {
    sched_timer[cpuid()].elapse = TIMER_ELAPSE;
    sched_timer[cpuid()].handler = sched_timer_handler;
    set_cpu_timer(&sched_timer[cpuid()]);
    sched_timer_set[cpuid()] = true;
  }

  cpus[cpuid()].sched.thisproc = p;
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
static void simple_sched(enum procstate new_state) {
  auto this = thisproc();
  ASSERT(this->state == RUNNING);
  if (this->killed && new_state != ZOMBIE) {
    _release_sched_lock();
    return;
  }
  update_this_state(new_state);
  auto next = pick_next();
  if (next->pid != 1) {
    // printk("next proc is:%d\n", next->pid);
  }
  if (!next->idle) {
  }
  update_this_proc(next);
  ASSERT(next->state == RUNNABLE);
  next->state = RUNNING;

  if (next != this) {
    attach_pgdir(&next->pgdir);
    // printk("attach\n");
    next->schinfo.start_time = get_timestamp_ms();
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
  if (thisproc()->pid == 2) {
    printk("2\n");
  }
  set_return_addr(entry);
  return arg;
}
