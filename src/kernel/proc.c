#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/paging.h>

struct proc root_proc;
extern struct container root_container;

void kernel_entry();
void proc_entry();

// a map from pid to pcb
typedef struct pid_map_pcb {
    int pid;
    struct proc* pcb;
    struct rb_node_ node;
} pid_map_pcb_t;

typedef struct pid_pcb_tree {
    struct rb_root_ root;
} pid_pcb_tree_t;

static bool _cmp_pid_pcb(rb_node lnode, rb_node rnode) {
    auto lp = container_of(lnode, pid_map_pcb_t, node);
    auto rp = container_of(rnode, pid_map_pcb_t, node);
    return lp->pid < rp->pid;
}

static pid_pcb_tree_t pid_pcb;
static pid_bitmap_t global_pid;
static SpinLock ptree_lock, pid_pcb_lock;

static int alloc_pid(pid_bitmap_t* pid_bitmap) {
    _acquire_spinlock(&(pid_bitmap->pid_lock)); 
    int ret = -1;
    for (int i = pid_bitmap->last_pid + 1; i < pid_bitmap->size; ++i) {
        if (bitmap_get(pid_bitmap->bitmap, i) == false) {
            ret = i;
            bitmap_set(pid_bitmap->bitmap, i);
            break;
        }
    }
    if (ret == -1) {
        for (int i = 0; i < pid_bitmap->last_pid; ++i) {
            if (bitmap_get(pid_bitmap->bitmap, i) == false) {
                ret = i;
                bitmap_set(pid_bitmap->bitmap, i);
                break;
            }
        }
    }
    ASSERT(ret != -1);
    pid_bitmap->last_pid = ret;
    _release_spinlock(&(pid_bitmap->pid_lock));
    return ret;
}

static void free_pid(pid_bitmap_t* pid_bitmap, int pid) {
    _acquire_spinlock(&(pid_bitmap->pid_lock));
    bitmap_clear(pid_bitmap->bitmap, pid);
    _release_spinlock(&(pid_bitmap->pid_lock));
}

define_early_init(global_pid) {
    init_spinlock(&(global_pid.pid_lock));
    memset(global_pid.bitmap, 0, MAX_PID / 8);
    bitmap_set(global_pid.bitmap, 0);
    global_pid.last_pid = 0;
    global_pid.size = MAX_PID;
}

define_early_init(ptree_lock) {
    init_spinlock(&ptree_lock);
    init_spinlock(&pid_pcb_lock);
}

void set_parent_to_this(struct proc* proc) {
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL
    _acquire_spinlock(&ptree_lock);
    if (proc->parent == NULL) {
        // insert into rb_tree
        pid_map_pcb_t *in_node = kalloc(sizeof(pid_map_pcb_t));
        in_node->pid = proc->pid;
        in_node->pcb = proc;
        _acquire_spinlock(&pid_pcb_lock);
        ASSERT(_rb_insert(&(in_node->node), &pid_pcb.root, _cmp_pid_pcb) == 0);
        _release_spinlock(&pid_pcb_lock);
    }
    proc->parent = thisproc();
    _insert_into_list(&(thisproc()->children), &(proc->ptnode));
    _release_spinlock(&ptree_lock);
}

NO_RETURN void exit(int code) {
    // TODO
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the rootproc of the container, and notify the it if there is zombie
    // 4. notify the parent
    // 5. sched(ZOMBIE)
    // NOTE: be careful of concurrency
    _acquire_spinlock(&ptree_lock);
    auto this = thisproc();
    ASSERT(this != this->container->rootproc && !this->idle); 
    this->exitcode = code;
    // transfer children to thisproc's container's rootproc
    auto p = (this->children).next;
    struct proc* rp = this->container->rootproc;
    while (p != &(this->children)) {
        auto q = p->next;
        _insert_into_list(&(rp->children), p);
        auto child = container_of(p, struct proc, ptnode);
        child->parent = rp;
        p = q;
    }

    // transfer zombie children to thisproc's container's rootproc
    p = (this->zombie_children).next;
    while (p != &(this->zombie_children)) {
        auto q = p->next;
        _insert_into_list(&(rp->zombie_children), p);
        auto child = container_of(p, struct proc, ptnode);
        child->parent = rp;
        p = q;
        post_sem(&(rp->childexit));
    }
    // free resource
    free_pgdir(&(this->pgdir));
    // free file
    for (int i = 0; i < NOFILE; ++i) {
        if (this->oftable.otable[i]) {
            fileclose(this->oftable.otable[i]);
            this->oftable.otable[i] = NULL;
        }
    }
    OpContext ctx;
    bcache.begin_op(&ctx);
    inodes.put(&ctx, this->cwd);
    bcache.end_op(&ctx);
    this->cwd = NULL;
    
    // notify parent proc
    _detach_from_list(&(this->ptnode));
    _insert_into_list(&(this->parent->zombie_children), &(this->ptnode));
    post_sem(&(this->parent->childexit));
    
    _acquire_sched_lock();
    _release_spinlock(&ptree_lock);
    _sched(ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}

int wait(int* exitcode, int* pid)
{
    // TODO
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its local pid and exitcode
    // NOTE: be careful of concurrency
    _acquire_spinlock(&ptree_lock);
    auto this = thisproc();

    // if no children
    if ((this->children).next == &(this->children)
        && (this->zombie_children).next == &(this->zombie_children)
    ) {
        _release_spinlock(&ptree_lock);
        return -1;
    }
    _release_spinlock(&ptree_lock);

    if (wait_sem(&this->childexit) == false) {
        return -1;
    }
    _acquire_spinlock(&ptree_lock);
    auto p = (this->zombie_children).next;
    _detach_from_list(p);
    auto child = container_of(p, struct proc, ptnode);
    *pid = child->pid;
    *exitcode = child->exitcode;
    
    // erase from rb_tree
    pid_map_pcb_t del_node = {child->pid, NULL, {0, 0, 0}};
    _acquire_spinlock(&pid_pcb_lock);
    auto find_node = _rb_lookup(&(del_node.node), &pid_pcb.root, _cmp_pid_pcb);
    _rb_erase(find_node, &pid_pcb.root);
    _release_spinlock(&pid_pcb_lock);
    kfree(container_of(find_node, pid_map_pcb_t, node));
    
    int ret = child->localpid;
    // free pid resource
    free_pid(&global_pid, child->pid);
    free_pid(&(this->container->local_pid), child->localpid);

    kfree_page(child->kstack);
    kfree(child);
    _release_spinlock(&ptree_lock);
    return ret;
}

int kill(int pid)
{
    // TODO
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).
    _acquire_spinlock(&ptree_lock);
    
    // find from rb_tree 
    pid_map_pcb_t kill_node = {pid, NULL, {0, 0, 0}};
    _acquire_spinlock(&pid_pcb_lock);
    auto find_node = _rb_lookup(&(kill_node.node), &pid_pcb.root, _cmp_pid_pcb);
    _release_spinlock(&pid_pcb_lock);

    if (find_node == NULL) {
        _release_spinlock(&ptree_lock);
        return -1;
    }
    auto p = container_of(find_node, pid_map_pcb_t, node);
    if (is_unused(p->pcb)) {
        _release_spinlock(&ptree_lock);
        return -1;
    }
    p->pcb->killed = true;
    alert_proc(p->pcb);
    _release_spinlock(&ptree_lock);
    return 0;
}

int start_proc(struct proc* p, void(*entry)(u64), u64 arg)
{
    // TODO
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its local pid
    // NOTE: be careful of concurrency
    if (p->parent == NULL) {
        _acquire_spinlock(&ptree_lock);
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
        _release_spinlock(&ptree_lock);

        // insert into rb_tree
        pid_map_pcb_t *in_node = kalloc(sizeof(pid_map_pcb_t));
        in_node->pid = p->pid;
        in_node->pcb = p;
        _acquire_spinlock(&pid_pcb_lock);
        ASSERT(_rb_insert(&(in_node->node), &pid_pcb.root, _cmp_pid_pcb) == 0);
        _release_spinlock(&pid_pcb_lock);
    }
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;
    p->localpid = alloc_pid(&(p->container->local_pid));
    int id = p->localpid;
    activate_proc(p);
    return id;
}

void init_proc(struct proc* p) {
    // TODO
    // setup the struct proc with kstack and pid allocated
    // NOTE: be careful of concurrency
    memset(p, 0, sizeof(*p));
    p->pid = alloc_pid(&global_pid);
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_list_node(&p->zombie_children);
    init_pgdir(&p->pgdir);
    init_schinfo(&p->schinfo, false);
    init_oftable(&(p->oftable));
    p->kstack = kalloc_page();
    p->container = &root_container;
    memset(p->kstack, 0, PAGE_SIZE);
    p->kcontext = (KernelContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(KernelContext) - sizeof(UserContext));
    p->ucontext = (UserContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
}

struct proc* create_proc()
{
    struct proc* p = kalloc(sizeof(struct proc));
    init_proc(p);
    return p;
}

static struct proc* recursive_get_offline_proc(struct proc* cur_proc) {
    ListNode* child_node = cur_proc->children.next;
    struct proc* child;
    while (child_node != &(cur_proc->children)) {
        child = container_of(child_node, struct proc, ptnode);
        
        _acquire_spinlock(&(child->pgdir.lock));
        if (child->pgdir.online == false) {
            _release_spinlock(&(child->pgdir.lock));
            return child;
        }
        _release_spinlock(&(child->pgdir.lock));

        auto ret = recursive_get_offline_proc(child);
        if (ret != NULL) {
            _acquire_spinlock(&(ret->pgdir.lock));
            if (ret->pgdir.online == false) {
                _release_spinlock(&(ret->pgdir.lock));
                return ret;
            }
            _release_spinlock(&(ret->pgdir.lock));
        }
        child_node = child_node->next;
    }
    return NULL;
}

struct proc* get_offline_proc() {
    _acquire_spinlock(&ptree_lock);
    struct proc* ret = recursive_get_offline_proc(&root_proc);
    if (ret == NULL) {
        _release_spinlock(&ptree_lock);
        return ret;
    }
    _acquire_spinlock(&(ret->pgdir.lock));
    _release_spinlock(&ptree_lock);
    return ret;
}

define_init(root_proc)
{
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();
int fork() {
    /* TODO: Your code here. */
    auto this = thisproc();
    auto child = create_proc();
    set_parent_to_this(child);
    set_container_to_this(child);
    init_pgdir(&(child->pgdir));
    
    // copy trapframe
    *(child->ucontext) = *(this->ucontext);
    child->cwd = inodes.share(this->cwd);
    // child process's fork() returns 0
    child->ucontext->x[0] = 0;

    // copy file
    for (int i = 0; i < NOFILE; ++i) {
        if (this->oftable.otable[i] != NULL) {
            child->oftable.otable[i] = filedup(this->oftable.otable[i]);
        }
    }

    // copy pgdir
    copy_sections(&(this->pgdir.section_head), &(child->pgdir.section_head));
    auto st = this->pgdir.section_head.next;
    while (st != &(this->pgdir.section_head)) {
        auto section = container_of(st, struct section, stnode);
        for (u64 i = PAGE_BASE(section->begin); i < section->end; i += PAGE_SIZE) {
            auto pte_ptr = get_pte(&(this->pgdir), i, false);
            if (pte_ptr == NULL || !(*pte_ptr & PTE_VALID)) { // what if swapout?
                break;
            }
            void* ka = kalloc_page();
            memcpy(ka, (void*)P2K(PTE_ADDRESS(*pte_ptr)), PAGE_SIZE);
            vmmap(&(child->pgdir), i, ka, PTE_FLAGS(*pte_ptr));
        }
        st = st->next;
    }

    start_proc(child, trap_return, 0);
    arch_tlbi_vmalle1is();
    return child->localpid;
}