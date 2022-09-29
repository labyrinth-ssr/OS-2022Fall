#include <common/rc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <common/list.h>
#include <driver/memlayout.h>
#include <kernel///printk.h>
#include <common/spinlock.h>

#define COLOUR_OFF 0
#define CPU_NUM 4
#define AC_LIMIT 10

static SpinLock mem_lock;
static SpinLock mem_lock2;

RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt)
{
    init_rc(&alloc_page_cnt);
    init_spinlock(&mem_lock);
    init_spinlock(&mem_lock2);
}

// All usable pages are added to the queue.
// NOTE: You can use the page itself to store allocator data of it.
// In this example, the fix-lengthed meta-data of the allocator are stored in .bss (static QueueNode* pages),
//  and the per-page allocator data are stored in the first sizeof(QueueNode) bytes of pages themselves.
//
// See API Reference for more information on given data structures.
static QueueNode* pages;
extern char end[];
define_early_init(pages)
{
    for (u64 p = PAGE_BASE((u64)&end) + PAGE_SIZE; p < P2K(PHYSTOP); p += PAGE_SIZE)
	   add_to_queue(&pages, (QueueNode*)p); 
}

// Allocate: fetch a page from the queue of usable pages.
void* kalloc_page()
{
    _increment_rc(&alloc_page_cnt);
    auto ret=fetch_from_queue(&pages);
    return ret;
}

// Free: add the page to the queue of usable pages.
void kfree_page(void* p)
{
    _decrement_rc(&alloc_page_cnt);
    add_to_queue(&pages, (QueueNode*)p);
}

typedef struct SlabNode
{
    ListNode* prev;
    ListNode* next;
    int colour;
    unsigned int active;
    void* owner_cache;
    void* take_up1;
    int* freelist;
}SlabNode;

typedef struct Array_cache {
    unsigned int avail;
    void *entry[AC_LIMIT]; 
}Array_cache_t;

//put the slab descripter in the head of slabs_partial,then set freelist and avail.
typedef struct CacheNode {
    ListNode* prev;
    ListNode* next;
    unsigned int pgorder;    /* order of pages per slab (2^n) */
    unsigned int num;         /* objects per slab */
    isize object_size;
    unsigned int colour_off;  /* colour offset */
    Array_cache_t array_cache[CPU_NUM];
    SlabNode* slabs_partial;
    SlabNode* slabs_full;
    SlabNode* slabs_free;
}CacheNode;

//first means a cache node's first slab, save cache data (slab descripter data)
void init_slab_node(SlabNode* slab_node,int obj_num,bool first,CacheNode* owner_cache){
    slab_node->active=0;
    slab_node->colour=0;
    slab_node->owner_cache= first? (void*)((u64)slab_node+PAGE_SIZE-sizeof(CacheNode)):owner_cache;
    slab_node->freelist= (int*)((u64)slab_node+sizeof(SlabNode));
    for (int i = 0; i < obj_num; i++)
    {
        slab_node->freelist[i]=i;
    }
    init_list_node((ListNode*)slab_node);
}

//must ensure the slab is not full
void* alloc_obj (SlabNode* slab_node,isize size,int obj_num){
    auto ret=(void*)((u64)slab_node+sizeof(SlabNode)+ (sizeof(int)*obj_num/8+1)*8+slab_node->colour+(slab_node->freelist[slab_node->active++])*size );
    return ret;
}

int get_obj_index(SlabNode* slab_node,isize size,int obj_num,void* obj_addr){
    return ((u64)obj_addr-(u64)slab_node-(sizeof(int)*obj_num/8+1)*8-slab_node->colour-sizeof(SlabNode))/size;
}

//head can be null
void init_cache_node(isize size,CacheNode* cache_node,int num,SlabNode* slabs_partial){
    cache_node->pgorder=0;
    cache_node->object_size=size;
    cache_node->colour_off=COLOUR_OFF;
    cache_node->num=num;
    cache_node->slabs_partial=slabs_partial;
    cache_node->slabs_full=NULL;
    cache_node->slabs_free=NULL;
    for (int i = 0; i < CPU_NUM; i++)
    {
        cache_node->array_cache[i].avail=0;
        for (int j = 0; j < AC_LIMIT; j++)
        {
            cache_node->array_cache[i].entry[j]= 0;
        }
    }
    init_list_node((ListNode*)cache_node);
}

static CacheNode* kmem_cache_array[2048];

CacheNode* search_cache_size(CacheNode* head,isize size){
    auto listp=(ListNode*)head;
    (void)listp;
    _for_in_list(listp,(ListNode*)head){
        if (((CacheNode*)listp)->object_size==size)
        {
            return (CacheNode*) listp;
        }
    }
    return NULL;
}

void full_to_partial(CacheNode* kmem_cache,SlabNode* full_slab){
    kmem_cache->slabs_full=(SlabNode*)_detach_from_list((ListNode*)(full_slab));
    if (NULL==kmem_cache->slabs_partial)
    {
        init_list_node((ListNode*)(full_slab));
        kmem_cache->slabs_partial=full_slab;
    } else
    {
        _insert_into_list((ListNode*) kmem_cache->slabs_partial,(ListNode*)full_slab);
    }
}

void partial_to_full(CacheNode* kmem_cache,SlabNode* partial_slab){
    kmem_cache->slabs_partial=  (SlabNode*)_detach_from_list((ListNode*)partial_slab);
    if (NULL==kmem_cache->slabs_full)
    {
        init_list_node((ListNode*) partial_slab);
        kmem_cache->slabs_full=partial_slab;
    } else
    {
        _insert_into_list((ListNode*) kmem_cache->slabs_full,(ListNode*) partial_slab);
    }
}

void partial_to_free(CacheNode* kmem_cache,SlabNode* partial_slab){
    // printk("partial to free\n");
    kmem_cache->slabs_partial=  (SlabNode*)_detach_from_list((ListNode*)partial_slab);
    if (NULL==kmem_cache->slabs_free)
    {
        init_list_node((ListNode*) partial_slab);
        kmem_cache->slabs_free=partial_slab;
    } else
    {
        _insert_into_list((ListNode*) kmem_cache->slabs_free,(ListNode*) partial_slab);
    }
}

void free_to_partial(CacheNode* kmem_cache,SlabNode* free_slab){
    // printk("free to partial\n");
    kmem_cache->slabs_free=(SlabNode*)_detach_from_list((ListNode*)(free_slab));
    if (NULL==kmem_cache->slabs_partial)
    {
        init_list_node((ListNode*)(free_slab));
        kmem_cache->slabs_partial=free_slab;
    } else
    {
        _insert_into_list((ListNode*) kmem_cache->slabs_partial,(ListNode*)free_slab);
    }
}

//objects in the middle be freed
void* kalloc(isize size)
{
    setup_checker(one);
    acquire_spinlock(one,&mem_lock);
    void* objp;
    CacheNode* search_res=kmem_cache_array[size];
    if (NULL== search_res)
    {
        auto free_head=(SlabNode*)kalloc_page();
        auto num=(PAGE_SIZE-3*COLOUR_OFF-sizeof(SlabNode)-8-sizeof(CacheNode))/(size+sizeof(int));
        init_slab_node(free_head,num,true,NULL);
        CacheNode* offset_node =(CacheNode*)((u64)free_head+PAGE_SIZE-sizeof(CacheNode));
        kmem_cache_array[size]=offset_node;
        init_cache_node(size,offset_node,num,free_head);
        auto ret=alloc_obj(free_head,size,num);
        if (offset_node->slabs_partial->active>=offset_node->num)
        {
            partial_to_full(offset_node,offset_node->slabs_partial);
        }
        release_spinlock(one,&mem_lock);
        return ret;
    }

    auto kmem_cache=search_res;
    Array_cache_t* ac=&(kmem_cache->array_cache[cpuid()]);
    if (ac->avail>0)
    {
        objp=ac->entry[--ac->avail];
        release_spinlock(one,&mem_lock);
        return objp;
    }
    //avail is zero,refill the entry from global
    if (NULL!=kmem_cache->slabs_partial)
    {
        auto ret=alloc_obj(kmem_cache->slabs_partial,size,kmem_cache->num);
        if (kmem_cache->slabs_partial->active>=kmem_cache->num)
        {
            partial_to_full(kmem_cache,kmem_cache->slabs_partial);
        }
        release_spinlock(one,&mem_lock);
        return ret;
    }

    if (NULL!=kmem_cache->slabs_free)
    {
        auto ret=alloc_obj(kmem_cache->slabs_free,size,kmem_cache->num);
        free_to_partial(kmem_cache,kmem_cache->slabs_free);
        release_spinlock(one,&mem_lock);
        return ret;
    }
    
    auto free_head=kalloc_page();
    init_slab_node((SlabNode*) free_head,kmem_cache->num,false,kmem_cache);
    kmem_cache->slabs_partial=(SlabNode*) free_head;
    auto ret=alloc_obj(kmem_cache->slabs_partial,size,kmem_cache->num);
    if (kmem_cache->slabs_partial->active>=kmem_cache->num)
    {
            partial_to_full(kmem_cache,kmem_cache->slabs_partial);
    }
    release_spinlock(one,&mem_lock);
    return ret;
}

//find in the global slab: full or partial. from different slabs.kmem_cache 
void cache_flusharray(Array_cache_t* ac,CacheNode* cache_node){
    SlabNode* flush_slab= (SlabNode*) PAGE_BASE((u64)(ac->entry[--ac->avail]));
    flush_slab->freelist[--flush_slab->active]=get_obj_index(flush_slab,cache_node->object_size,cache_node->num,ac->entry[ac->avail]);
    if (flush_slab->active==cache_node->num-1)
    {
        full_to_partial(cache_node,flush_slab);
    } 
    else if (flush_slab->active==0)
    {
        partial_to_free(cache_node,flush_slab);
    }
}

void kfree(void* p)
{
    setup_checker(one);
    acquire_spinlock(one,&mem_lock);
    SlabNode* owner_slab=(SlabNode*) PAGE_BASE((u64)p);
    auto kmem_cache= (CacheNode*) owner_slab->owner_cache;
    Array_cache_t* ac=&(kmem_cache->array_cache[cpuid()]);
    if (ac->avail==AC_LIMIT)
    {
        cache_flusharray(ac,kmem_cache);
    }
    ac->entry[ac->avail++] = p;
    if (ac->avail==0 && NULL==kmem_cache->slabs_full && NULL==kmem_cache->slabs_partial )
    {
        kfree_page((void*)PAGE_BASE((u64)p));
    }
    
    release_spinlock(one,&mem_lock);
}
