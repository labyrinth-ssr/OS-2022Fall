#include <common/rc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <common/list.h>
#include <driver/memlayout.h>
#include <kernel/printk.h>
#include <common/spinlock.h>

#define COLOUR_OFF 0
#define CPU_NUM 4
#define AC_LIMIT 3

static SpinLock mem_lock;
static SpinLock mem_lock2;

// define_early_init(mem)
// {buddy
    
// }

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
    // printk (" cpu %d enter kalloc %p ",cpuid(),&pages);
    auto ret=fetch_from_queue(&pages);
    // printk (" cpu %d kalloc page %p \n ",cpuid(),ret);
    
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
    ListNode* self;
    ListNode* take_place;
    int colour;
    unsigned int active;
    // bool is_head;
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
    ListNode* self;
    ListNode* take_up;
    unsigned int pgorder;    /* order of pages per slab (2^n) */
    unsigned int num;         /* objects per slab */
    isize object_size;
    unsigned int colour_off;  /* colour offset */
    Array_cache_t array_cache[CPU_NUM];
    SlabNode* slabs_partial;
    SlabNode* take_up1;
    SlabNode* slabs_full;
    SlabNode* take_up2;
    SlabNode* slabs_free;
    SlabNode* take_up3;
}CacheNode;

//set colour = 0
//first means a cache node's first slab, save cache data (slab descripter data)
void init_slab_node(SlabNode* slab_node,int obj_num,bool first,isize size,CacheNode* owner_cache){

    //printk("active:%d",slab_node->active);
    int offset=first&&(obj_num!=1)? sizeof(CacheNode)/size+1 :0;
    slab_node->active=offset;
    //printk("offset:%d",offset);
    slab_node->colour=0;
    // slab_node->is_head=first;
    slab_node->owner_cache= first? (void*)((u64)slab_node+sizeof(SlabNode)+(sizeof(int)*obj_num/8+1)*8) : owner_cache;
    slab_node->freelist= (int*)((u64)slab_node+sizeof(SlabNode));
    for (int i = 0; i < obj_num; i++)
    {
        slab_node->freelist[i]=i;
    }
    slab_node->self=(ListNode*)slab_node;
    //printk("self:%lld",(u64)slab_node);
    init_list_node(slab_node->self);
}

// SlabNode* search_slab_head(SlabNode* p){
//     printk("cpu %d searching head: %d\n",cpuid(),p->is_head);
//     while (!p->is_head)
//     {
//         printk("cpu %d searching head p: %p p2: %p\n",cpuid(),p->self,p->self->next);
//         p= (SlabNode*)(p->self->next);
//     }
//     return p;
    
// }

//must ensure the slab is not full
void* alloc_obj (SlabNode* slab_node,isize size,int obj_num){
    // slab_node->active++;
   //printk("cpu %d alloc_obj size %lld: %p\n",cpuid(),size,(void*)((u64)slab_node+(slab_node->freelist[slab_node->active])*size+slab_node->colour+ (sizeof(int)*obj_num/8+1)*8 ));
//    printk ("cpu %d alloc obj,active:%d,num_limit: %d\n",cpuid(),slab_node->active,((CacheNode*)slab_node->owner_cache)->num);

//    printk ("cpu %d alloc obj,obj offset:%lld,colour:%d,list length:%ld\n",cpuid(),(slab_node->freelist[slab_node->active])*size,slab_node->colour,(sizeof(int)*obj_num/8+1)*8);
//    for (unsigned int i = 0; i <= slab_node->active; i++)
//    {
//         printk("cpu %d : freelist member %d\n",cpuid(),slab_node->freelist[i]);
//    }
    // printk("cpu %d , colour: %d",cpuid(),slab_node->colour);
    if (obj_num==1)
    {
        slab_node->active++;
        return (void*)((u64)slab_node+PAGE_SIZE/2);
    }
    
    
    return (void*)((u64)slab_node+sizeof(SlabNode)+(slab_node->freelist[slab_node->active++])*size+slab_node->colour+ (sizeof(int)*obj_num/8+1)*8 );
}

int get_obj_index(SlabNode* slab_node,isize size,int obj_num,void* obj_addr){

    // (void*)((slab_node->freelist[slab_node->active++])*size )
    return obj_num==1? 0: ((u64)obj_addr-(sizeof(int)*obj_num/8+1)*8-slab_node->colour-(u64)slab_node-sizeof(SlabNode))/size;
}

//head can be null
void add_to_cache_queue(CacheNode* head,isize size,CacheNode* cache_node){

    cache_node->pgorder=0;
    cache_node->object_size=size;
    cache_node->colour_off=COLOUR_OFF;
    cache_node->num=(PAGE_SIZE-3*COLOUR_OFF-sizeof(SlabNode)-8)/(size+sizeof(int));
    // //printk("num:%d\n",cache_node->num);
    cache_node->slabs_partial=(SlabNode*)((u64)cache_node-sizeof(SlabNode)-(sizeof(int)*(cache_node->num)/8+1)*8);
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

    cache_node->self= (ListNode*)cache_node;
    _insert_into_list(head->self,cache_node->self);


}

static CacheNode* kmem_cache_head;

CacheNode* search_cache_size(CacheNode* head,isize size){
//    printk ("in search cache\n");
    auto listp=head->self;
    (void)listp;
    _for_in_list(listp,head->self){
    //    printk("cpu %d search cache list... size %lld\n",cpuid(),((CacheNode*)listp)->object_size);
        if (((CacheNode*)listp)->object_size==size)
        {
            //printk("cpu %d find size %lld",cpuid(),size);
            return (CacheNode*) listp;
        }
    }
    return NULL;
}

void partial_to_full(CacheNode* kmem_cache){
    // printk("cpu:%d slab full, kmem_cache: %p,partial head %p,\n",cpuid(),kmem_cache,kmem_cache->slabs_partial);
    auto partial_head=kmem_cache->slabs_partial;

    // printk("cpu:%d slab partial prev: %p\n",cpuid(),kmem_cache->self->prev);
    kmem_cache->slabs_partial=(SlabNode*)_detach_from_list(kmem_cache->slabs_partial->self);
    // printk("cpu:%d slab full after detach %p\n",cpuid(),kmem_cache->slabs_partial);
    if (NULL==kmem_cache->slabs_full)
    {
        kmem_cache->slabs_full=partial_head;
        // printk("cpu:%d slab full before init %p\n",cpuid(),kmem_cache->slabs_full->self);
        init_list_node(kmem_cache->slabs_full->self);
        // printk("cpu:%d slab full after init \n",cpuid());
    } else
    {
        // printk("cpu:%d slab full before insert %p, %p\n",cpuid(),kmem_cache->slabs_full->self,partial_head->self);
        _insert_into_list(kmem_cache->slabs_full->self,partial_head->self);
        // printk("cpu:%d slab full after insert \n",cpuid());
    }
}

void free_to_partial(CacheNode* kmem_cache){
    // printk("cpu:%d free to partial",cpuid());
    auto free_head=kmem_cache->slabs_free;

    // printk("cpu:%d slab partial prev: %p\n",cpuid(),kmem_cache->self->prev);
    kmem_cache->slabs_free=(SlabNode*)_detach_from_list(kmem_cache->slabs_free->self);
    // printk("cpu:%d slab partial move to %p\n",cpuid(),kmem_cache->slabs_free);
    if (NULL==kmem_cache->slabs_partial)
    {
        kmem_cache->slabs_partial=free_head;
        init_list_node(kmem_cache->slabs_partial->self);
    } else
    {
        _insert_into_list(kmem_cache->slabs_partial->self,free_head->self);
    }
}

void free_to_full(CacheNode* kmem_cache){
    // printk("cpu:%d free to full",cpuid());
    auto free_head=kmem_cache->slabs_free;

    // printk("cpu:%d slab full prev: %p\n",cpuid(),kmem_cache->self->prev);
    kmem_cache->slabs_free=(SlabNode*)_detach_from_list(kmem_cache->slabs_free->self);
    // printk("cpu:%d slab full move to %p\n",cpuid(),kmem_cache->slabs_free);
    if (NULL==kmem_cache->slabs_full)
    {
        kmem_cache->slabs_full=free_head;
        init_list_node(kmem_cache->slabs_full->self);
    } else
    {
        _insert_into_list(kmem_cache->slabs_full->self,free_head->self);
    }
}

//objects in the middle be freed
void* kalloc(isize size)
{
    setup_checker(one);
    acquire_spinlock(one,&mem_lock);
    // printk("cpu %d kalloc size %lld\n",cpuid(),size);
    void* objp;
    CacheNode* search_res=NULL;

    //printk("cpu %d kmem cache head %p \n",cpuid(),kmem_cache_head);
    if (NULL!=kmem_cache_head)
    {
        search_res=search_cache_size(kmem_cache_head,size);
    } 

    if (NULL==kmem_cache_head||NULL== search_res)
    {
        // printk("cpu %d search cache fail \n",cpuid());
        auto free_head=(SlabNode*)kalloc_page();
        auto num=(PAGE_SIZE-3*COLOUR_OFF-sizeof(SlabNode))/(size+sizeof(int));
        init_slab_node(free_head,num,true,size,NULL);
        // printk("cpu %d search cache fail, init slab node \n",cpuid());
        
        CacheNode* offset_node=(CacheNode*)((u64)free_head+sizeof(SlabNode)+(sizeof(int)*num/8+1)*8);
// (4096-40)/(2032+4)
        if (NULL==kmem_cache_head)
        {
            kmem_cache_head=offset_node;
            kmem_cache_head->self=(ListNode*)offset_node;
            kmem_cache_head->pgorder=0;
            kmem_cache_head->object_size=size;
            kmem_cache_head->colour_off=COLOUR_OFF;
            kmem_cache_head->num=num;
            // //printk("num:%d\n",kmem_cache_head->num);
            kmem_cache_head->slabs_partial=free_head;
            kmem_cache_head->slabs_full=NULL;
            kmem_cache_head->slabs_free=NULL;
            for (int i = 0; i < CPU_NUM; i++)
            {
                kmem_cache_head->array_cache[i].avail=0;
                 for (int j = 0; j < AC_LIMIT; j++)
                {
                    kmem_cache_head->array_cache[i].entry[j]= 0;
                }
                // kmem_cache_head->array_cache[i].entry= (void**)&(kmem_cache_head->array_cache[i].entry);
            }
            init_list_node(kmem_cache_head->self);
        } else
        {
            // printk("add cacahe:")
            add_to_cache_queue(kmem_cache_head,size,(CacheNode*)offset_node);
        }
        // printk("cpu %d , free_head %p \n",cpuid(),free_head);
        auto ret=alloc_obj(free_head,size,num);
        // printk("cpu %d alloc from new cache: %p ,active: %d,num: %d\n",cpuid(),ret,offset_node->slabs_partial->active,offset_node->num);

                            if (PAGE_BASE((u64)ret)!=PAGE_BASE((u64)free_head))
            {
               // printk("cpu %d in partial not in one page slabnode: %p,ret %p\n",cpuid(),free_head,ret);
            } else
            {
                //printk("cpu %d in partial within one page\n",cpuid());
            }
        if (offset_node->slabs_partial->active>=offset_node->num)
        {
            partial_to_full(offset_node);
        }
        release_spinlock(one,&mem_lock);
        return ret;
    }
        auto kmem_cache=search_res;
        // printk("cpu %d get searched\n",cpuid());
        Array_cache_t* ac=&(kmem_cache->array_cache[cpuid()]);
        if (ac->avail!=0)
        {
            objp=ac->entry[--ac->avail];
            // printk ("l1: cpu %d\n",cpuid());
            release_spinlock(one,&mem_lock);
            return objp;
        }
        //avail is zero,refill the entry from global
        if (NULL!=kmem_cache->slabs_partial)
        {
            //printk("cpu %d, partial\n",cpuid());
            auto ret=alloc_obj(kmem_cache->slabs_partial,size,kmem_cache->num);
            
            if (PAGE_BASE((u64)ret)!=PAGE_BASE((u64)(kmem_cache->slabs_partial)))
            {
               // printk("cpu %d in partial not in one page slabnode: %p,ret %p\n",cpuid(),kmem_cache->slabs_partial,ret);
            } else
            {
                //printk("cpu %d in partial within one page\n",cpuid());
            }
            //printk("cpu %d alloc from partial: %p ,active: %d,num: %d\n",cpuid(),ret,kmem_cache->slabs_partial->active,kmem_cache->num);
            if (kmem_cache->slabs_partial->active==kmem_cache->num)
            {
                partial_to_full(kmem_cache);
            }

            release_spinlock(one,&mem_lock);
       // printk ("l2: cpu %d fetch from partial\n",cpuid());
            return ret;
        }

        //no partial slabs
        // if (NULL!=kmem_cache->slabs_free)
        // {
        //     printk("cpu %d, free,",cpuid());
        //     auto ret=alloc_obj(kmem_cache->slabs_free,size,kmem_cache->num);
        //     if (kmem_cache->slabs_free->active==kmem_cache->num)
        //     {
        //         free_to_full(kmem_cache);
        //     } else
        //     {
        //         free_to_partial(kmem_cache);
        //     }
        //     release_spinlock(one,&mem_lock);
        //     printk ("l2: cpu %d fetch from free\n",cpuid());
        //     return ret;
        // }
        //no free slabs
        // printk ("l3: cpu %d fetch from buddy\n",cpuid());
        auto free_head=kalloc_page();
        // printk (" cpu %d buddy get %p \n ",cpuid(),free_head);
        // printk (" cpu %d ee ",cpuid());
        init_slab_node((SlabNode*) free_head,kmem_cache->num,false,size,kmem_cache);
        // printk (" cpu %d aa ",cpuid());
        kmem_cache->slabs_partial=(SlabNode*) free_head;
        auto ret=alloc_obj(kmem_cache->slabs_partial,size,kmem_cache->num);
        // printk (" cpu %d bb ",cpuid());
        // printk("cpu %d alloc from l3: %p ,active: %d,num: %d\n",cpuid(),ret,kmem_cache->slabs_partial->active,kmem_cache->num);

                    if (PAGE_BASE((u64)ret)!=PAGE_BASE((u64)(kmem_cache->slabs_partial)))
            {
               // printk("cpu %d in partial not in one page slabnode: %p,ret %p\n",cpuid(),kmem_cache->slabs_partial,ret);
            } else
            {
                //printk("cpu %d in partial within one page\n",cpuid());
            }

        if (kmem_cache->slabs_partial->active==kmem_cache->num)
        {
                // printk (" cpu %d cc ",cpuid());
                partial_to_full(kmem_cache);
        }
                // printk (" cpu %d dd ",cpuid());

        release_spinlock(one,&mem_lock);
        return ret;
}

//find in the global slab: full or partial. from different slabs.kmem_cache 
void cache_flusharray(Array_cache_t* ac,CacheNode* cache_node){
    SlabNode* flush_slab= (SlabNode*) PAGE_BASE((u64)(ac->entry[--ac->avail]));
    int offset=0;
    if ( PAGE_BASE((u64)cache_node) == (u64)flush_slab && cache_node->num!=1)
    {
        offset=sizeof(CacheNode)/cache_node->object_size+1;
        // printk("cpu %d free page head, \n",cpuid());
    }
    (void)offset;
    
    flush_slab->freelist[--flush_slab->active]=get_obj_index(flush_slab,cache_node->object_size,cache_node->num,ac->entry[ac->avail]);
}

void kfree(void* p)
{
    setup_checker(two);

        acquire_spinlock(two,&mem_lock2);
            // printk("cpu %d begin free\n",cpuid());

    SlabNode* owner_slab=(SlabNode*) PAGE_BASE((u64)p);
    auto kmem_cache= (CacheNode*) owner_slab->owner_cache;
    
    Array_cache_t* ac=&(kmem_cache->array_cache[cpuid()]);
    // printk("cpu %d colour %d",cpuid(),kmem_cache->colour_off);

    if (ac->avail==AC_LIMIT)
    {
        // printk("cpu %d ac entry full",cpuid());
        cache_flusharray(ac,kmem_cache);
        ac->avail--;
    }
    // printk("cpu %d free %p\n",cpuid(),p);
    // printk("cpu %d avail %d",cpuid(),ac->avail);
    ac->entry[ac->avail] = p;
    ac->avail++;
    release_spinlock(two,&mem_lock2);


}
