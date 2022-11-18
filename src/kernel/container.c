#include "common/spinlock.h"
#include "kernel/proc.h"
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
  container->pids->avail = 1;
  for (int i = 0; i < PID_NUM; i++) {
    container->pids->freelist[i] = i;
  }
}

struct container *create_container(void (*root_entry)(), u64 arg) {
  // TODO
  struct container *new_container = kalloc(sizeof(struct container));
  memset(new_container, 0, sizeof(struct container));

  new_container->parent = thisproc()->container;
  struct proc *rootproc = create_proc();
  new_container->rootproc = rootproc;
  init_schinfo(&new_container->schinfo, true);
  init_schqueue(&new_container->schqueue);
  new_container->pids->avail = 1;
  for (int i = 0; i < PID_NUM; i++) {
    new_container->pids->freelist[i] = i;
  }
  init_spinlock(&new_container->pid_lock);

  // rootproc lock?
  set_parent_to_this(rootproc);
  rootproc->localpid = 0;
  rootproc->container = new_container;
  start_proc(rootproc, root_entry, arg);
  activate_group(new_container);

  return new_container;
}

define_early_init(root_container) {
  init_container(&root_container);
  root_container.rootproc = &root_proc;
}
