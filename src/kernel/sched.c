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

static SpinLock rqlock;
static ListNode rq;
extern bool panic_flag;

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
    // TODO: return the current process
    return cpus[cpuid()].sched.thisproc;

}

void init_schinfo(struct schinfo* p)
{
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p->rq);
    p->prio=0;

}

void _acquire_sched_lock()
{
    // TODO: acquire the sched_lock if need
    _acquire_spinlock(&rqlock);

}

void _release_sched_lock()
{
    // TODO: release the sched_lock if need
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

bool activate_proc(struct proc* p)
{
    // TODO
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic
    _acquire_sched_lock();
    if (p->state == RUNNING || p->state == RUNNABLE)
    {
        _release_sched_lock();
        return false;
    }
    if (p->state == SLEEPING || p->state == UNUSED)
    {
                if (p->state==SLEEPING)
        {
            printk("sleep awake\n");
        }
        else
        {
            printk("unused activate\n");
        }
        p->state = RUNNABLE;
        printk("\n");
        _insert_into_list(&rq, &p->schinfo.rq);
        printk("insert into rq %d\n",p->pid);

        printk("pid:%d lr: %llx\n",p->pid,p->kcontext->lr);
        _for_in_list(rqe,&rq){
            auto process=container_of(rqe,struct proc,schinfo.rq);
            printk("pid: %d ",process->pid);
        }
        printk("\n");
    }
    else
    {
        PANIC();
    }
    _release_sched_lock();
    return true;

}

static void update_this_state(enum procstate new_state)
{
    // TODO: if using simple_sched, you should implement this routinue
    // update the state of current process to new_state, and remove it from the sched queue if new_state=SLEEPING/ZOMBIE
    thisproc()->state = new_state;
    if (new_state == SLEEPING || new_state == ZOMBIE)
    {
        _detach_from_list(&thisproc()->schinfo.rq);
        printk("%d sleeping or zombie, lr : %llx\n",thisproc()->pid,thisproc()->kcontext->lr);
        // _for_in_list(rqe,&rq){
        //     auto process=container_of(rqe,struct proc,schinfo.rq);
        //     printk("pid: %d ",process->pid);
        // }
        // printk("\n");
    }
        // _insert_into_list(&rq, &p->schinfo.rq);
}

static struct proc* pick_next()
{
    // TODO: if using simple_sched, you should implement this routinue
    // choose the next process to run, and returfn idle if no runnable process
    if (panic_flag)
    {
        return cpus[cpuid()].sched.idle;
    }
    int cnt=0;
    _for_in_list(p, &rq){
        if (p == &rq)
        {
            cnt+=1;
            continue;
        }
        cnt +=1;
        auto proc = container_of(p, struct proc, schinfo.rq);
        if (proc->state == RUNNABLE && (ListNode*)proc != p)
        {
            // printk("next is %p",proc);
            return proc;
        } 
    }
    return cpus[cpuid()].sched.idle;
}

static void update_this_proc(struct proc* p)
{
    // TODO: if using simple_sched, you should implement this routinue
    // update thisproc to the choosen process, and reset the clock interrupt if need
    reset_clock(1000);
    cpus[cpuid()].sched.thisproc = p;
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
static void simple_sched(enum procstate new_state)
{
    auto cpu_id=cpuid();
    (void)cpu_id;
    auto this = thisproc();
    printk("this process %d\n",this->pid);
    ASSERT(this->state == RUNNING);
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    printk("next process %d\n",next->pid);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this)
    {
        swtch(next->kcontext, &this->kcontext);
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

