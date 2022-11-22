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
  // (void)t;
  // if (thisproc()->killed) {
  // printk("proc %d in time inter,killed?:%d\n", thisproc()->pid,
  //        thisproc()->killed);
  // }
  if (t->triggered) {
    set_cpu_timer(&sched_timer[cpuid()]);
    if (thisproc()->schinfo.vruntime >= thisproc()->schinfo.permit_time) {
      if (!thisproc()->idle) {
        _acquire_sched_lock();
        // printk("reinsert %d\n", thisproc()->pid);

        auto container = thisproc()->container;

        if (container != &root_container) {
          _rb_erase(&container->schinfo.rq, &container->parent->schqueue.rq);
          // printk("erase done\n");
          ASSERT(_rb_insert(&container->schinfo.rq,
                            &container->parent->schqueue.rq, __sched_cmp) == 0);

          // printk("reinsert container %p\n", container);
        }
        _release_sched_lock();
      }

      yield();
    } else {
      // printk("proc %d not yield\n", thisproc()->pid);
    }
    // _release_sched_lock();
  } else {
    // printk("proc %d not triggered\n", thisproc()->pid);
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
  // if (!onalert && p->state == ZOMBIE) {
  //   PANIC();
  // }
  // printk("activate %d ,onalert?:%d,state:%d\n", p->pid, onalert, p->state);
  if (p->state == RUNNING || p->state == RUNNABLE || p->state == ZOMBIE ||
      (p->state == DEEPSLEEPING && onalert)) {
    _release_sched_lock();
    return false;
  }
  if (p->state == SLEEPING || p->state == UNUSED ||
      (p->state == DEEPSLEEPING && !onalert)) {
    p->state = RUNNABLE;
    // p->schinfo.prio = 100;
    // _insert_into_list(&p->container->schqueue.rq, &p->schinfo.rq);
    // printk("root container %p\n", &root_container);
    // printk("pid %d insert container %p\n", p->pid, p->container);
    ASSERT(_rb_insert(&p->schinfo.rq, &p->container->schqueue.rq,
                      __sched_cmp) == 0);
    p->container->schqueue.node_cnt++;
    // if (p->container->id == 0) {
    // printk("activate: proc %d to container %d,node cnt:%d\n", p->pid,
    //        p->container->id, p->container->schqueue.node_cnt);
    // }
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
    thisproc()->schinfo.vruntime +=
        get_timestamp_ms() - thisproc()->schinfo.start_time;
    // if (!thisproc()->idle && thisproc() == thisproc()->container->rootproc) {
    //   thisproc()->container->schinfo.vruntime = thisproc()->schinfo.vruntime;
    // }

    ASSERT(_rb_insert(&this->schinfo.rq, &this->container->schqueue.rq,
                      __sched_cmp) == 0);
    this->container->schqueue.node_cnt++;
    // if (this->container->id == 0) {
    // printk("runnable: proc %d to container %d,node cnt:%d\n", this->pid,
    //        this->container->id, this->container->schqueue.node_cnt);
    // }
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

rb_node _rb_first_ex_p(rb_node n) {
  // rb_node n;
  // n = root->rb_node;
  rb_node left = NULL;
  if (!n)
    return NULL;
  if (n->rb_left != NULL /*&&
      !(container_of(n->rb_left, struct schinfo, rq)->group &&
        container_of(n->rb_left, struct schinfo, rq)->skip)*/) {
    // printk("go left,");
    left = _rb_first_ex_p(n->rb_left);
  }
  if (left != NULL) {
    // printk("1");
    return left;
  }
  if (container_of(n, struct schinfo, rq)->group) {
    // printk("container:%dskip?:%d\n",
    //        container_of(n, struct container, schinfo.rq)->id,
    //        container_of(n, struct schinfo, rq)->skip);
  }
  if (!(container_of(n, struct schinfo, rq)->group &&
        container_of(n, struct schinfo, rq)->skip)) {
    // printk("2");
    return n;
  }
  if (n->rb_right != NULL) {
    // printk("3");
    return _rb_first_ex_p(n->rb_right);
  }
  return NULL;

  // // if (expression) {
  // // statements
  // // }
  // // printk("in tree:left:%p,skip?:%d", n->rb_left,
  // //        container_of(n->rb_left, struct schinfo, rq)->skip);
  // while (n->rb_left && !container_of(n->rb_left, struct schinfo, rq)->skip) {

  //   n = n->rb_left;
  //   // printk("in tree while:left:%p,skip?:%d", n->rb_left,
  //   //        container_of(n->rb_left, struct schinfo, rq)->skip);
  // }
  // // printk("tree return:%p\n", n);
  // if (container_of(n, struct schinfo, rq)->skip) {
  //   // printk("middle skip\n");
  //   // printk("right:%p", n->rb_right);
  //   n = n->rb_right;
  //   return _rb_first_ex_p(n);
  // }
  // return n;
}

void _rb_walk(rb_node rt_node) {
  if (rt_node != NULL) {
    container_of(rt_node, struct schinfo, rq)->skip = false;
    _rb_walk(rt_node->rb_left);
    _rb_walk(rt_node->rb_right);
  }
}

static struct proc *pick_next_r(rb_root rt) {
  if (panic_flag) {
    return cpus[cpuid()].sched.idle;
  }
  // printk("cpu %d sched: container %d schqueue\n", cpuid(),
  //        container_of(rt, struct container, schqueue.rq)->id);

  rb_node get_node = NULL;
  get_node = _rb_first_ex_p(rt->rb_node);

  if (container_of(rt, struct container, schqueue.rq) != &root_container &&
      container_of(rt, struct container, schqueue.rq)->schqueue.node_cnt != 0 &&
      get_node == NULL) {
    // printk("container %d and its children rq empty\n",
    //        container_of(rt, struct container, schqueue.rq)->id);
    container_of(rt, struct container, schqueue.rq)->schinfo.skip = true;
    return pick_next_r(
        &container_of(rt, struct container, schqueue.rq)->parent->schqueue.rq);
  }

  if (get_node == NULL) {
    // printk("cpu %d return idle\n", cpuid());
    return cpus[cpuid()].sched.idle;
  }

  struct schinfo *schinfo = container_of(get_node, struct schinfo, rq);

  if (!schinfo->group) {
    struct proc *res_proc = container_of(schinfo, struct proc, schinfo);
    _rb_erase(&res_proc->schinfo.rq, &res_proc->container->schqueue.rq);
    res_proc->container->schqueue.node_cnt--;
    ASSERT(res_proc->container->schqueue.node_cnt >= 0);
    // if (container_of(rt, struct container, schqueue.rq)->id == 0) {
    // printk("cpu %d container %d pick process %d,node cnt:%d\n", cpuid(),
    //        container_of(rt, struct container, schqueue.rq)->id,
    //        res_proc->pid, res_proc->container->schqueue.node_cnt);
    // }

    return res_proc;
  } else {

    // printk(4994
    //     "container rq node:%p,left:%p,right:%p,first_res %p",
    //     &container_of(schinfo, struct container,
    //     schinfo)->schqueue.rq.rb_node, &container_of(schinfo, struct
    //     container, schinfo)
    //          ->schqueue.rq.rb_node->rb_left,
    //     &container_of(schinfo, struct container, schinfo)
    //          ->schqueue.rq.rb_node->rb_right,
    //     _rb_first_ex(
    //         &container_of(schinfo, struct container, schinfo)->schqueue.rq));
    // printk("container %d sched container %d\n",
    //        container_of(rt, struct container, schqueue.rq)->id,
    //        container_of(schinfo, struct container, schinfo)->id);
    if (container_of(schinfo, struct container, schinfo)->schqueue.node_cnt ==
        0) {
      // printk("former skip:%d", schinfo->skip);
      // printk("former ptr:%p", get_node);
      schinfo->skip = true;
      // printk("  2  ");
      // printk("container %d rq empty\n",
      //        container_of(schinfo, struct container, schinfo)->id);

      return pick_next_r(rt);
    }
    // printk("3");

    return pick_next_r(
        &container_of(schinfo, struct container, schinfo)->schqueue.rq);
  }
}

// public
// void preOrderTraverse1(TreeNode root) {
//   if (root != null) {
//     System.out.print(root.val + "  ");
//     preOrderTraverse1(root.left);
//     preOrderTraverse1(root.right);
//   }
// }
static struct proc *pick_next() {
  // printk("begin\n");
  auto ret = pick_next_r(&root_container.schqueue.rq);
  // printk("end\n");
  _rb_walk(root_container.schqueue.rq.rb_node);

  return ret;
  // printk("in pick_next");
  // if (panic_flag) {
  //   return cpus[cpuid()].sched.idle;
  // }
  // //应该不存在时间片耗尽仍在first的情况
  // struct proc *res_proc = NULL;
  // rb_node get_node;
  // rb_root this_rq = &root_container.schqueue.rq;
  // // printk("root container:%d\n", root_container.schinfo.group);
  // while (1) {
  //   // printk("rb root is %p , from container %p\n", this_rq,
  //   //        container_of(this_rq, struct container, schqueue.rq));
  //   get_node = _rb_first_ex(this_rq);
  //   // }
  //   // printk("first node is %p,", get_node);
  //   if (get_node == NULL) {
  //     // printk("tree first null\n");
  //     container_of(this_rq, struct container, schqueue.rq)->schinfo.skip =
  //     true; continue;
  //   }
  //   struct schinfo *schinfo = container_of(get_node, struct schinfo, rq);
  //   // printk("first proc is:%d,state %d\n", candidate_proc->pid,
  //   //        candidate_proc->state);
  //   if (schinfo->group) {
  //     // printk("is group %p\n", container_of(schinfo, struct container,
  //     // schinfo));
  //     this_rq = &container_of(schinfo, struct container,
  //     schinfo)->schqueue.rq;
  //     // &((struct container *)candidate_proc)->schqueue.rq;
  //   } else {
  //     struct proc *candidate_proc = container_of(schinfo, struct proc,
  //     schinfo);
  //     // if (candidate_proc->state == RUNNABLE) {
  //     // printk("pick %d\n", candidate_proc->pid);
  //     res_proc = candidate_proc;
  //     // }
  //     // else {
  //     //   printk("proc %d state:%d\n", candidate_proc->pid,
  //     //          candidate_proc->state);
  //     //   // printk("running\n");
  //     // }
  //     _rb_erase(&res_proc->schinfo.rq, &res_proc->container->schqueue.rq);
  //     res_proc->container->schqueue.node_cnt--;
  //     ASSERT(res_proc->container->schqueue.node_cnt >= 0);
  //     return res_proc;
  //   }
  // }
  // // if (res_proc != NULL) {
  // //   // printk("pick and erase proc %d from container %p\n", res_proc->pid,
  // //   //        container_of(this_rq, struct container, schqueue.rq));
  // //   _rb_erase(&res_proc->schinfo.rq, &res_proc->container->schqueue.rq);
  // //   res_proc->container->schqueue.node_cnt--;
  // //   ASSERT(res_proc->container->schqueue.node_cnt >= 0);
  // //   return res_proc;
  // // }
  // // printk("idle...");
  // return cpus[cpuid()].sched.idle;
}

void activate_group(struct container *group) {
  // TODO: add the schinfo node of the group to the schqueue of its parent
  _acquire_sched_lock();

  ASSERT(_rb_insert(&group->schinfo.rq, &group->parent->schqueue.rq,
                    __sched_cmp) == 0);
  group->parent->schqueue.node_cnt++;

  // if (group->parent->id == 0) {
  // printk("add container %d to parent container%d,node vruntime:%lld,node cnt
  // "
  //        "%d\n",
  //        group->id, group->parent->id, group->schinfo.vruntime,
  //        group->parent->schqueue.node_cnt);
  // }

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
  // if (new_state == RUNNABLE && thisproc()->killed) {
  // printk("proc %d in sched,killed?:%d\n", thisproc()->pid,
  // thisproc()->killed);
  // }
  // printk("root container%p\n", &root_container);
  if (thisproc()->pid != 0) {
    // printk("sched %d,cpu%d,container %p\n", thisproc()->pid, cpuid(),
    //        thisproc()->container);
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
  if (!next->idle) {
    next->container->schinfo.vruntime = next->schinfo.vruntime;
  }
  update_this_proc(next);
  // printk("cpu%d next proc%p,pid:%d,state:%d,idle?:%d\n", cpuid(), next,
  //        next->pid, next->state, next->idle);
  // printk("state:%d ", next->state);
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
