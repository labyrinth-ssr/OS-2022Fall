#include "common/rbtree.h"
#include "common/spinlock.h"
#include "kernel/proc.h"
#include "kernel/schinfo.h"
#include <common/list.h>
#include <common/string.h>
#include <kernel/container.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/sched.h>

struct container root_container;
extern struct proc root_proc;

void activate_group(struct container *group);

void set_container_to_this(struct proc *proc) {
  proc->container = thisproc()->container;
}

void init_container(struct container *container) {
  memset(container, 0, sizeof(struct container));
  container->parent = NULL;
  container->rootproc = NULL;
  init_schinfo(&container->schinfo, true);
  init_schqueue(&container->schqueue);
  // TODO: initialize namespace (local pid allocator)
  init_spinlock(&container->pid_lock);
  _acquire_spinlock(&container->pid_lock);
  container->pids.avail = 0;
  for (int i = 0; i < PID_NUM; i++) {
    container->pids.freelist[i] = i;
  }
  _release_spinlock(&container->pid_lock);
}

struct container *create_container(void (*root_entry)(), u64 arg) {
  // struct container *parent;
  // struct proc *rootproc;

  // struct schinfo schinfo;
  // struct schqueue schqueue;

  // // TODO: namespace (local pid?)
  // SpinLock pid_lock;
  // struct pid_pool pids;

  // auto res = sizeof(struct schinfo) + sizeof(struct schqueue)
  // +sizeof(SpinLock) + sizeof(struct pid_pool) ;
  // TODO
  struct container *new_container = kalloc(sizeof(struct container));
  init_container(new_container);
  printk("create container %p\n", new_container);
  // memset(new_container, 0, sizeof(struct container));

  new_container->parent = thisproc()->container;
  printk("container %lld,ptr %p,parent %p\n", arg, new_container,
         thisproc()->container);
  struct proc *rootproc = create_proc();
  new_container->rootproc = rootproc;
  printk("container %p rootproc %d\n", new_container, rootproc->pid);
  // init_schinfo(&new_container->schinfo, true);
  // init_schqueue(&new_container->schqueue);
  // new_container->pids.avail = 1;
  // for (int i = 0; i < PID_NUM; i++) {
  //   new_container->pids.freelist[i] = i;
  // }
  // init_spinlock(&new_container->pid_lock);
  // rootproc lock?
  set_parent_to_this(rootproc);
  // rootproc->localpid = 0;
  rootproc->container = new_container;
  // _rb_insert(&rootproc->schinfo.rq, &new_container->schqueue.rq, bool
  // (*cmp)(rb_node, rb_node))
  start_proc(rootproc, root_entry, arg);
  activate_group(new_container);

  return new_container;
}

define_early_init(root_container) {
  init_container(&root_container);
  root_container.rootproc = &root_proc;
}
