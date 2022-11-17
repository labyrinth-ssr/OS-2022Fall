#include "common/sem.h"
#include <common/list.h>
#include <common/string.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

#define PID_NUM 1000

struct proc root_proc;

void kernel_entry();
void proc_entry();
static SpinLock plock;

struct pid_pool {
  int freelist[PID_NUM];
  int avail;
};

static struct pid_pool pids;

define_early_init(plock) {
  init_spinlock(&plock);
  pids.avail = 1;
  for (int i = 0; i < PID_NUM; i++) {
    pids.freelist[i] = i;
  }
}

void set_parent_to_this(struct proc *proc) {
  _acquire_spinlock(&plock);
  auto this = thisproc();
  proc->parent = thisproc();
  _insert_into_list(&this->children, &proc->ptnode);
  _release_spinlock(&plock);
}

NO_RETURN void exit(int code) {
  // TODO
  // 1. set the exitcode
  // 2. clean up the resources
  // 3. transfer children to the root_proc, and notify the root_proc if there is
  // zombie
  // 4. notify the parent
  // 5. sched(ZOMBIE)
  // NOTE: be careful of concurrency

  setup_checker(0);
  _acquire_spinlock(&plock);
  auto this = thisproc();
  this->exitcode = code;
  _for_in_list(rcp, &this->children) {
    if (rcp == &this->children) {
      continue;
    }
    auto rc = container_of(rcp, struct proc, ptnode);
    if (is_zombie(rc)) {
      post_sem(&root_proc.childexit);
    }
  }
  if (!_empty_list(&this->children)) {
    _for_in_list(rcp, &this->children) {
      if (rcp == &this->children) {
        continue;
      }
      auto rc = container_of(rcp, struct proc, ptnode);
      rc->parent = &root_proc;
    }
    auto merged_list = this->children.next;
    _detach_from_list(&this->children);
    _merge_list(merged_list, &root_proc.children);
  }
  free_pgdir(&this->pgdir);
  post_sem(&this->parent->childexit);
  lock_for_sched(0);
  pids.freelist[--pids.avail] = this->pid;
  _release_spinlock(&plock);
  sched(0, ZOMBIE);
  PANIC(); // prevent the warning of 'no_return function returns'
}

int wait(int *exitcode) {
  _acquire_spinlock(&plock);
  auto this = thisproc();
  if (_empty_list(&this->children)) {
    _release_spinlock(&plock);
    return -1;
  }
  _release_spinlock(&plock);
  auto wait_sem_ret = wait_sem(&this->childexit);
  if (!wait_sem_ret) {
    return -1;
  }
  _acquire_spinlock(&plock);
  _for_in_list(c, &this->children) {
    if (c == &this->children) {
      continue;
    }
    auto child = container_of(c, struct proc, ptnode);
    if (is_zombie(child)) {
      auto pid = child->pid;
      *exitcode = child->exitcode;
      _detach_from_list(&child->ptnode);
      kfree(child);
      _release_spinlock(&plock);
      return pid;
    }
  }
  PANIC();
}

struct proc *dfs(struct proc *proc, int pid) {
  if (proc->pid == pid && proc->state != UNUSED) {
    return proc;
  }

  if (_empty_list(&proc->children)) {
    return NULL;
  }

  _for_in_list(cp, &proc->children) {
    if (cp == &proc->children) {
      continue;
    }
    auto c_proc = container_of(cp, struct proc, ptnode);
    auto child_res = dfs(c_proc, pid);
    if (child_res != NULL) {
      return child_res;
    }
  }
  return NULL;
}

int kill(int pid) {
  // TODO
  // Set the killed flag of the proc to true and return 0.
  // Return -1 if the pid is invalid (proc not found).
  _acquire_spinlock(&plock);
  auto kill_proc = dfs(&root_proc, pid);
  if (kill_proc != NULL) {
    kill_proc->killed = true;
    _release_spinlock(&plock);
    return 0;
  }
  _release_spinlock(&plock);
  return -1;
}

bool is_killed(struct proc *proc) {
  _acquire_spinlock(&plock);
  auto ret = proc->killed;
  _release_spinlock(&plock);
  return ret;
}

int start_proc(struct proc *p, void (*entry)(u64), u64 arg) {
  _acquire_spinlock(&plock);
  if (p->parent == NULL) {
    p->parent = &root_proc;
    _insert_into_list(&root_proc.children, &p->ptnode);
  }
  p->kcontext->lr = (u64)&proc_entry;
  p->kcontext->x0 = (u64)entry;
  p->kcontext->x1 = (u64)arg;
  int id = p->pid;
  activate_proc(p);
  _release_spinlock(&plock);
  return id;
}

void init_proc(struct proc *p) {
  _acquire_spinlock(&plock);
  memset(p, 0, sizeof(*p));
  p->pid = pids.freelist[pids.avail++];
  init_sem(&p->childexit, 0);
  init_list_node(&p->children);
  init_list_node(&p->ptnode);
  init_pgdir(&p->pgdir);
  p->kstack = kalloc_page();
  init_schinfo(&p->schinfo);
  p->kcontext = (KernelContext *)((u64)p->kstack + PAGE_SIZE - 16 -
                                  sizeof(KernelContext) - sizeof(UserContext));
  p->ucontext =
      (UserContext *)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
  _release_spinlock(&plock);
}

struct proc *create_proc() {
  struct proc *p = kalloc(sizeof(struct proc));
  init_proc(p);
  return p;
}

define_init(root_proc) {
  init_proc(&root_proc);
  root_proc.parent = &root_proc;
  start_proc(&root_proc, kernel_entry, 123456);
}

// void yield() {
//     // TODO: lab7 container
//     // Give up cpu resources and switch to the scheduler
//     // 1. get sched lock
//     // 2. set process state and call scheduler
//     // 3. don't forget to release your lock

// }

void sleep(void *chan, SpinLock *lock) {
  // TODO: lab7 container
  // release lock and sleep on channel, reacquire lock when awakened
}

void wakeup(void *chan) {
  // TODO lab7 container
  // wake up all sleeping processses on channel
}