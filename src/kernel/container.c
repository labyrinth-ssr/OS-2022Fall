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
  // TODO
  struct container *new_container = kalloc(sizeof(struct container));
  init_container(new_container);
  new_container->id = arg;

  new_container->parent = thisproc()->container;
  struct proc *rootproc = create_proc();
  rootproc->container = new_container;
  new_container->rootproc = rootproc;
  set_parent_to_this(rootproc);
  start_proc(rootproc, root_entry, arg);
  activate_group(new_container);

  return new_container;
}

define_early_init(root_container) {
  init_container(&root_container);
  root_container.id = 4;
  root_container.rootproc = &root_proc;
}
