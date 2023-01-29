#include <common/string.h>
#include <common/list.h>
#include <kernel/container.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/mem.h>
#include <kernel/sched.h>

struct container root_container;
extern struct proc root_proc;

void activate_group(struct container* group);

void set_container_to_this(struct proc* proc)
{
    proc->container = thisproc()->container;
}

void init_container(struct container* container)
{
    memset(container, 0, sizeof(struct container));
    container->parent = NULL;
    container->rootproc = NULL;
    init_schinfo(&container->schinfo, true);
    init_schqueue(&container->schqueue);
    // TODO: initialize namespace (local pid allocator)
    memset(container->local_pid.bitmap, 0, MAX_PID / 8);
    container->local_pid.last_pid = -1;
    container->local_pid.size = MAX_PID;
    init_spinlock(&(container->local_pid.pid_lock));
}

struct container* create_container(void (*root_entry)(), u64 arg)
{
    // TODO
    struct container* new_container = (struct container*)kalloc(sizeof(struct container));
    init_container(new_container);
    new_container->parent = thisproc()->container;
    new_container->rootproc = create_proc();
    new_container->rootproc->container = new_container;
    
    set_parent_to_this(new_container->rootproc);
    start_proc(new_container->rootproc, root_entry, arg);
    activate_group(new_container);
    
    return new_container;
}

define_early_init(root_container)
{
    init_container(&root_container);
    root_container.rootproc = &root_proc;
}
