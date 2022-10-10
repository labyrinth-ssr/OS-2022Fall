#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/sched.h>

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
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL
    _acquire_spinlock(&plock);
    auto this=thisproc();
    proc->parent=thisproc();
    _insert_into_list(&this->children,&proc->ptnode);
    printk("%d is %d 's parent\n",thisproc()->pid,proc->pid);
    _release_spinlock(&plock);
}

NO_RETURN void exit(int code)
{
    // TODO
    // 1. set the exitcode
    // 2. clean up the resources (dont have yet)
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie ()
    // 4. notify the parent_proc the exit
    // 5. sched(ZOMBIE) 设为zombie ，让调度器调出
    // NOTE: be careful of concurrency
    setup_checker(0);
    _acquire_spinlock(&plock);
    auto this = thisproc();
    this->exitcode = code;
    if (!_empty_list(&this->children))
    {
        auto merged_list=this->children.next;
        _detach_from_list(&this->children);
        _merge_list(merged_list,&root_proc.children);
    }
    post_sem(&this->parent->childexit);
    lock_for_sched(0);
    _release_spinlock(&plock);
    printk("%d exit\n",this->pid);
    sched(0,ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}
int wait(int* exitcode)
{
    // TODO
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency
    auto this = thisproc();
    printk("%d wait\n",this->pid);
    bool all_zombie=false;
    if (this->pid==2)
    {
        all_zombie=true;
        _for_in_list(c,&this->children){
            if (c==&this->children)
            {
                continue;
            }
            auto child=container_of(c,struct proc,ptnode);
            if (child->state!=ZOMBIE)
            {
                all_zombie=false;
                printk("root_proc child %d zombie \n",child->pid);
            }
        }
    }
    (void)all_zombie;
    if (_empty_list(&this->children))
    {
        return -1;
    }
    if (!all_zombie)
    {
        wait_sem(&this->childexit);
    }
    
    _for_in_list(c,&this->children){
        auto child=container_of(c,struct proc,ptnode);
        if (child->state==ZOMBIE)
        {
            auto pid=child->pid;
            printk("child exitcode:%d\n",child->exitcode);
            *exitcode=child->exitcode;
            _detach_from_list(&child->ptnode);
            printk("%d child %d free\n",this->pid,child->pid);
            kfree(child);
            return pid;
        }
    }
    PANIC();
}

int start_proc(struct proc* p, void(*entry)(u64), u64 arg)
{
    // TODO
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency
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
    // TODO
    // setup the struct proc with kstack and pid allocated
    // NOTE: be careful of concurrency
    _acquire_spinlock(&plock);
    memset(p,0,sizeof(*p));
    p->pid=pids.freelist[++pids.avail];
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
