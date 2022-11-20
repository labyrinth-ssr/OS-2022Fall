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

#define TIMER_ELAPSE 50

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

void _acquire_sched_lock() { _acquire_spinlock(&rqlock); }

void _release_sched_lock() { _release_spinlock(&rqlock); }

static void sched_timer_handler(struct timer *t) {
  // (void)t;
  if (t->triggered) {
    set_cpu_timer(&sched_timer[cpuid()]);
    thisproc()->schinfo.vruntime +=
        get_timestamp_ms() - thisproc()->schinfo.start_time;
    if (!thisproc()->idle && thisproc() == thisproc()->container->rootproc) {
      thisproc()->container->schinfo.vruntime = thisproc()->schinfo.vruntime;
    }

    if (thisproc()->schinfo.vruntime >= thisproc()->schinfo.permit_time) {
      if (!thisproc()->idle) {
        _acquire_sched_lock();
        _rb_erase(&thisproc()->schinfo.rq, &thisproc()->container->schqueue.rq);
        ASSERT(_rb_insert(&thisproc()->schinfo.rq,
                          &thisproc()->container->schqueue.rq,
                          __sched_cmp) == 0);

        auto container = thisproc()->container;

        if (container != &root_container) {
          _rb_erase(&container->schinfo.rq, &container->parent->schqueue.rq);
          // printk("erase done\n");
          ASSERT(_rb_insert(&container->schinfo.rq,
                            &container->parent->schqueue.rq, __sched_cmp) == 0);

          // printk("reinsert container %p\n", container);
        }
        // else {
        // printk("container %p parent null")
        // }
        _release_sched_lock();
      }

      yield();
    }
    // _release_sched_lock();
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
  p->vruntime = 0;
  p->group = group;
}

void init_schqueue(struct schqueue *s) {
  // init_list_node(&s->rq);
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
  (void)onalert;
  // if (!onalert && p->state == ZOMBIE) {
  //   PANIC();
  // }
  if (p->state == RUNNING || p->state == RUNNABLE) {
    _release_sched_lock();
    return false;
  }
  if (p->state == SLEEPING || p->state == UNUSED || p->state == DEEPSLEEPING) {
    p->state = RUNNABLE;
    // p->schinfo.prio = 100;
    // _insert_into_list(&p->container->schqueue.rq, &p->schinfo.rq);
    // printk("root container %p\n", &root_container);
    printk("pid %d insert container %p\n", p->pid, p->container);
    // printk("proc insert node %p,root %p", &p->schinfo.rq,
    //        &p->container->schqueue.rq);
    ASSERT(_rb_insert(&p->schinfo.rq, &p->container->schqueue.rq,
                      __sched_cmp) == 0);
    p->container->schqueue.node_cnt++;

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
  // printk("pid %d new state %d\n", thisproc()->pid, thisproc()->state);
  // if (new_state == SLEEPING || new_state == ZOMBIE ||
  //     new_state == DEEPSLEEPING) {
  //   // _detach_from_list(&this->schinfo.rq);
  //   // _rb_erase(&this->schinfo.rq, &this->container->schqueue.rq);
  //   // this->container->schqueue.node_cnt--;
  //   // ASSERT(this->container->schqueue.node_cnt >= 0);
  //   // printk("%d sleeping or zombie:%d,tree remain:%d\n", this->pid,
  //   new_state,
  //   //        this->container->schqueue.node_cnt);
  //   // this->container->schinfo.permit_time = MAX(TIMER_ELAPSE, _b)
  //   // printk("pid %d insert container %p\n", thisproc()->pid,
  //   //        thisproc()->container);
  // } else
  if (!this->idle && this->state == RUNNING && new_state == RUNNABLE) {
    printk("sched runnable %d\n", this->pid);
    ASSERT(_rb_insert(&this->schinfo.rq, &this->container->schqueue.rq,
                      __sched_cmp) == 0);
    this->container->schqueue.node_cnt++;
  }
  this->state = new_state;
}

rb_node _rb_first_ex(rb_root root) {
  rb_node n;
  n = root->rb_node;
  if (!n)
    return NULL;
  while (n->rb_left && !container_of(n->rb_left, struct schinfo, rq)->skip)
    n = n->rb_left;
  return n;
}

static struct proc *pick_next() {
  // printk("in pick_next");
  if (panic_flag) {
    return cpus[cpuid()].sched.idle;
  }
  //应该不存在时间片耗尽仍在first的情况
  struct proc *res_proc = NULL;
  rb_node get_node;
  rb_root this_rq = &root_container.schqueue.rq;
  // printk("root container:%d\n", root_container.schinfo.group);
  while (1) {
    // printk("rb root is %p , from container %p\n", this_rq,
    //        container_of(this_rq, struct container, schqueue.rq));
    get_node = _rb_first_ex(this_rq);
    // }
    // printk("first node is %p,", get_node);
    if (get_node == NULL) {
      // printk("tree first null\n");
      container_of(this_rq, struct container, schqueue.rq)->schinfo.skip = true;
      break;
    }
    struct schinfo *schinfo = container_of(get_node, struct schinfo, rq);
    // printk("first proc is:%d,state %d\n", candidate_proc->pid,
    //        candidate_proc->state);
    if (schinfo->group) {
      // printk("is group %p\n", container_of(schinfo, struct container,
      // schinfo));
      this_rq = &container_of(schinfo, struct container, schinfo)->schqueue.rq;
      // &((struct container *)candidate_proc)->schqueue.rq;
    } else {
      struct proc *candidate_proc = container_of(schinfo, struct proc, schinfo);
      if (candidate_proc->state == RUNNABLE) {
        // printk("pick %d\n", candidate_proc->pid);
        res_proc = candidate_proc;
      } else {
        printk("proc %d state:%d\n", candidate_proc->pid,
               candidate_proc->state);
        // printk("running\n");
      }
      break;
    }
  }
  if (res_proc != NULL) {
    printk("erase proc %d \n", res_proc->pid);
    _rb_erase(&res_proc->schinfo.rq, &res_proc->container->schqueue.rq);
    res_proc->container->schqueue.node_cnt--;
    ASSERT(res_proc->container->schqueue.node_cnt >= 0);
    return res_proc;
  }
  // printk("idle...");
  return cpus[cpuid()].sched.idle;
}

void activate_group(struct container *group) {
  // TODO: add the schinfo node of the group to the schqueue of its parent
  _acquire_sched_lock();

  printk("group insert node %p,root %p,node vruntime:%lld\n",
         &group->schinfo.rq, &group->parent->schqueue.rq,
         group->schinfo.vruntime);
  ASSERT(_rb_insert(&group->schinfo.rq, &group->parent->schqueue.rq,
                    __sched_cmp) == 0);
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
  // printk("root container%p\n", &root_container);
  if (thisproc()->pid != 0) {
    printk("sched %d,cpu%d,container %p\n", thisproc()->pid, cpuid(),
           thisproc()->container);
  } else {
    // printk("idle... ");
  }
  auto this = thisproc();
  ASSERT(this->state == RUNNING);
  if (this->killed && new_state != ZOMBIE) {
    _release_sched_lock();
    return;
  }
  update_this_state(new_state);
  auto next = pick_next();
  update_this_proc(next);
  // printk("cpu%d next proc%p,pid:%d,state:%d,idle?:%d\n", cpuid(), next,
  //        next->pid, next->state, next->idle);
  ASSERT(next->state == RUNNABLE);
  next->state = RUNNING;

  if (next != this) {
    attach_pgdir(&next->pgdir);
    next->schinfo.start_time = get_timestamp_ms();
    // printk("%d vruntime %lld\n", this->pid, this->schinfo.vruntime);
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
