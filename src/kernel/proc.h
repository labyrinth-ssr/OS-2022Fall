#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <kernel/schinfo.h>
#include <kernel/pt.h>

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, ZOMBIE };

typedef struct UserContext
{
    // TODO: customize your trap frame
    u64 spsr,elr;
    u64 x[18];
} UserContext;

typedef struct KernelContext
{
    // TODO: customize your context
    u64 lr,x0,x1;
    u64 x[11];

} KernelContext;

//root has child zombie info?
struct proc
{
    bool killed;
    bool idle;
    int pid;
    int exitcode;
    enum procstate state;
    Semaphore childexit;
    ListNode children;
    ListNode ptnode;
    struct proc* parent;
    struct schinfo schinfo;
    struct pgdir pgdir;
    void* kstack;
    UserContext* ucontext;
    KernelContext* kcontext;
    bool children_zombie;
};

// void init_proc(struct proc*);
struct proc* create_proc();
int start_proc(struct proc*, void(*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
int wait(int* exitcode);
int kill(int pid);
