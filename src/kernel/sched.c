#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <driver/clock.h>

extern bool panic_flag;

extern void swtch(KernelContext* new_ctx, KernelContext** old_ctx);
extern struct proc root_proc;

static SpinLock rqlock;
static ListNode rq;
extern bool panic_flag;
static struct timer sched_timer[4];
static bool sched_timer_set[4];

static void sched_timer_handler(struct timer* t)
{
    (void)t;
        set_cpu_timer(&sched_timer[cpuid()]);
        yield();
}

define_early_init(rq){
    init_spinlock(&rqlock);
    init_list_node(&rq);
}

define_init(sched){
    for (int i = 0; i < NCPU; i++)
    {
        struct proc* p=kalloc(sizeof(struct proc));
        p->idle = true;
        p->state = RUNNING;
        cpus[i].sched.thisproc = cpus[i].sched.idle = p;
    }
}

struct proc* thisproc()
{
    return cpus[cpuid()].sched.thisproc;

}

void init_schinfo(struct schinfo* p)
{
    init_list_node(&p->rq);
    p->prio=5;

}

void _acquire_sched_lock()
{
    _acquire_spinlock(&rqlock);

}

void _release_sched_lock()
{
    _release_spinlock(&rqlock);
}

bool is_zombie(struct proc* p)
{
    bool r;
    _acquire_sched_lock();
    r = p->state == ZOMBIE;
     _release_sched_lock();
    return r;
}

bool is_used(struct proc* p)
{
    bool r;
    _acquire_sched_lock();
    r = p->state != UNUSED;
    _release_sched_lock();
    return r;
}

bool activate_proc(struct proc* p)
{
    // TODO
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    _acquire_sched_lock();
    if (p->state == RUNNING || p->state == RUNNABLE)
    {
        _release_sched_lock();
        return false;
    }
    if (p->state == SLEEPING || p->state == UNUSED)
    {
        p->state = RUNNABLE;
        p->schinfo.prio=100;
        _insert_into_list(&rq, &p->schinfo.rq);
    }
    else
    {
        return false;
    }
    _release_sched_lock();
    return true;

}

static void update_this_state(enum procstate new_state)
{
    auto this=thisproc();
    this->state = new_state;
    if (new_state == SLEEPING || new_state == ZOMBIE)
    {
        _detach_from_list(&this->schinfo.rq);
    }
    else if (new_state==RUNNABLE)
    {
        this->schinfo.prio=0;
    }
}

static struct proc* pick_next()
{
    if (panic_flag)
    {
        return cpus[cpuid()].sched.idle;
    }
    int cnt = 0;
    struct proc* res_proc=NULL;
    (void)res_proc;
    int max_prior = 0;
    (void)max_prior;
    _for_in_list(p, &rq){
        if (p == &rq)
        {
            continue;
        }
        auto proc = container_of(p, struct proc, schinfo.rq);
        cnt++;
        if (proc->state == RUNNABLE && proc->schinfo.prio >= max_prior)
        {
            max_prior = proc->schinfo.prio;
            res_proc=proc;
        }
    }
    if (res_proc!=NULL)
    {
        _for_in_list(p, &rq){
        if (p == &rq)
        {
            continue;
        }
        auto proc = container_of(p, struct proc, schinfo.rq);
        if (proc->state == RUNNABLE && proc != res_proc)
        {
            proc->schinfo.prio ++;
        }
    }
        return res_proc;
    }
    return cpus[cpuid()].sched.idle;
}

static void update_this_proc(struct proc* p)
{
    
    if (p->pid != 0 && p != &root_proc && !sched_timer_set[cpuid()])
    {
        sched_timer[cpuid()].elapse = 10;
        sched_timer[cpuid()].handler = sched_timer_handler;
        set_cpu_timer(&sched_timer[cpuid()]);
        sched_timer_set[cpuid()]=true;
    }

    cpus[cpuid()].sched.thisproc = p;
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
static void simple_sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    if (this->killed && new_state != ZOMBIE)
    {
        _release_sched_lock();
        return;
    }
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this)
    {
        attach_pgdir (&next->pgdir);
        swtch(next->kcontext, &this->kcontext);
        attach_pgdir(&this->pgdir);
    }
    _release_sched_lock();
}

//调度器锁：保持state、调度队列等一致，保持原子性
__attribute__((weak, alias("simple_sched"))) void _sched(enum procstate new_state);

u64 proc_entry(void(*entry)(u64), u64 arg)
{
    _release_sched_lock();
    set_return_addr(entry);
    return arg;
}

