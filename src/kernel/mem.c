#include <common/rc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <common/list.h>
#include <driver/memlayout.h>

#define COLOUR_OFF 8
#define CPU_NUM 4

RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt)
{
    init_rc(&alloc_page_cnt);
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
    return fetch_from_queue(&pages);
}

// Free: add the page to the queue of usable pages.
void kfree_page(void* p)
{
    _decrement_rc(&alloc_page_cnt);
    add_to_queue(&pages, (QueueNode*)p);
}

typedef struct SlabNode
{
    int* freelist;
    int colour;
    int active;
    ListNode* self;
}SlabNode;

typedef struct Array_cache {
    unsigned int avail;
    unsigned int limit;
    void **entry; 
}Array_cache_t;

//put the slab descripter in the head of slabs_partial,then set freelist and avail.
typedef struct CacheNode {
    unsigned int pgorder;    /* order of pages per slab (2^n) */
    unsigned int num;         /* objects per slab */
    int object_size;
    unsigned int colour_off;  /* colour offset */
    ListNode* slabs_partial;
    ListNode* slabs_full;
    ListNode* slabs_free;
    Array_cache_t array_cache[CPU_NUM];
    ListNode* self;
}CacheNode;

//set colour = 0 first
void init_slab_node(SlabNode* slab_node,int obj_num,bool first,isize size){
    slab_node->active=0;
    //add meta data while keep alignment.
    int offset=first? sizeof(CacheNode)/size+1 :0;
    slab_node->colour=0;
    for (int i = 0; i < obj_num; i++)
    {
        slab_node->freelist[i]=i+offset;
    }
    init_list_node(slab_node->self);
}

//must ensure the slab is not full
void* alloc_obj (SlabNode* slab_node,isize size,int obj_num){
    slab_node->active++;
    return (void*)((u64)slab_node+(slab_node->freelist[slab_node->active])*size+slab_node->colour+sizeof(int)*obj_num);
}

int get_obj_index(SlabNode* slab_node,isize size,int obj_num,void* obj_addr){
    return ((u64)obj_addr-sizeof(int)*obj_num-slab_node->colour-(u64)slab_node)/size;
}

void add_to_cache_queue(CacheNode** head,isize size,CacheNode* cache_node){
    cache_node->pgorder=0;
    cache_node->colour_off=COLOUR_OFF;
    cache_node->num=(PAGE_SIZE-3*COLOUR_OFF)/size;
    cache_node->slabs_partial=NULL;
    cache_node->slabs_full=NULL;
    cache_node->slabs_free=NULL;
    for (int i = 0; i < CPU_NUM; i++)
    {
        cache_node->array_cache->limit=3;
        cache_node->array_cache->avail=0;
        cache_node->array_cache->entry=NULL;
    }
    auto free_head=(SlabNode*)kalloc_page();
    init_slab_node(free_head,cache_node->num,true,size);
    kmem_cache->slabs_partial=free_head;
    cache_node->self= free_head;
}

static CacheNode* kmem_cache;

//objects in the middle be freed
void* kalloc(isize size)
{
    void* objp;
    Array_cache_t* ac=&(kmem_cache->array_cache[cpuid()]);
    if (ac->avail!=0)
    {
        objp=ac->entry[--ac->avail];
        return objp;
    }

    //avail is zero,refill the entry from global
    if (NULL!=kmem_cache->slabs_partial)
    {
        ac->entry[ac->avail++]=alloc_obj(kmem_cache->slabs_partial,size,kmem_cache->num);
        return ac->entry[--ac->avail];
    }

    //no partial slabs
    if (NULL!=kmem_cache->slabs_free)
    {
        ac->entry[ac->avail++]=alloc_obj(kmem_cache->slabs_free,size,kmem_cache->num);
        return ac->entry[--ac->avail];
    }

    //queuenode is concurrency secure, no need clock. level3, allocate slab
    auto free_head=(SlabNode*)kalloc_page();
    init_slab_node(free_head,kmem_cache->num,true,size);
    kmem_cache->slabs_partial=free_head;
    return alloc_obj(free_head,size,kmem_cache->num);

}

//find in the global slab: full or partial. from different slabs.kmem_cache 
void cache_flusharray(Array_cache_t* ac,CacheNode* cache_node){
    SlabNode* flush_slab= PAGE_BASE((u64)(ac->entry[ac->avail]));
    flush_slab->freelist[--flush_slab->active]=get_obj_index(flush_slab,cache_node->object_size,cache_node->num,ac->entry[ac->avail]);
}

void kfree(void* p)
{
    Array_cache_t* ac=&(kmem_cache->array_cache[cpuid()]);

    if (ac->avail==ac->limit)
    {
        cache_flusharray(ac,kmem_cache);
    }
    
    ac->entry[ac->avail++] = p;
}
