#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>

#define PID_NUM 1000

struct proc root_proc;

void kernel_entry();
void proc_entry();
static SpinLock plock;

struct pid_pool{
    int freelist[PID_NUM];
    int avail;
};

static struct pid_pool pids;

define_early_init(plock){
    init_spinlock(&plock);
    pids.avail=1;
    for (int i = 0; i < PID_NUM; i++)
    {
        pids.freelist[i]=i;
    }
}


void set_parent_to_this(struct proc* proc)
{
    _acquire_spinlock(&plock);
    auto this=thisproc();
    proc->parent=thisproc();
    _insert_into_list(&this->children,&proc->ptnode);
    _release_spinlock(&plock);
}

NO_RETURN void exit(int code)
{
    setup_checker(0);
    _acquire_spinlock(&plock);
    auto this = thisproc();
    this->exitcode = code;
    _for_in_list(rcp,&this->children){
        if (rcp==&this->children)
        {
            continue;
        }
        auto rc=container_of(rcp,struct proc,ptnode);
        if (is_zombie(rc))
        {
            post_sem(&root_proc.childexit);
        }
    }
    if (!_empty_list(&this->children))
    {
        _for_in_list(rcp,&this->children){
            if (rcp==&this->children)
            {
                continue;
            }
            auto rc=container_of(rcp,struct proc,ptnode);
            rc->parent=&root_proc;
        }
        auto merged_list=this->children.next;
        _detach_from_list(&this->children);
        _merge_list(merged_list,&root_proc.children);
    }
    post_sem(&this->parent->childexit);
    lock_for_sched(0);
    pids.freelist[--pids.avail]=this->pid;
    _release_spinlock(&plock);
    sched(0,ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}

int wait(int* exitcode)
{
    _acquire_spinlock(&plock);
    auto this = thisproc();
    if (_empty_list(&this->children))
    {
        _release_spinlock(&plock);
        return -1;
    }
    _release_spinlock(&plock);
    wait_sem(&this->childexit);
    _acquire_spinlock(&plock);
    _for_in_list(c,&this->children){
        auto child=container_of(c,struct proc,ptnode);
        if (is_zombie(child))
        {
            auto pid=child->pid;
            *exitcode=child->exitcode;
            _detach_from_list(&child->ptnode);
            kfree(child);
            _release_spinlock(&plock);
            return pid;
        }
    }
    PANIC();
}

int start_proc(struct proc* p, void(*entry)(u64), u64 arg)
{
    _acquire_spinlock(&plock);
    if (p->parent == NULL)
    {
        p->parent=&root_proc;
        _insert_into_list(&root_proc.children,&p->ptnode);
    }
    p->kcontext->lr= (u64)&proc_entry;
    p->kcontext->x0=(u64)entry;
    p->kcontext->x1=(u64)arg;
    int id=p->pid;
    activate_proc(p);
    _release_spinlock(&plock);
    return id;
}

void init_proc(struct proc* p)
{
    _acquire_spinlock(&plock);
    memset(p,0,sizeof(*p));
    p->pid=pids.freelist[pids.avail++];
    init_sem(&p->childexit,0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    p->kstack=kalloc_page();
    init_schinfo(&p->schinfo);
    p->kcontext=(KernelContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(KernelContext) - sizeof(UserContext));
    p->ucontext = (UserContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
    _release_spinlock(&plock);
}

struct proc* create_proc()
{
    struct proc* p = kalloc(sizeof(struct proc));
    init_proc(p);
    return p;
}


define_init(root_proc)
{
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}
