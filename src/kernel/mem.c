#include "aarch64/mmu.h"
#include "common/defines.h"
#include "fs/cache.h"
#include "fs/defines.h"
#include "kernel/proc.h"
#include "kernel/pt.h"
#include "kernel/sched.h"
#include <common/list.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <driver/memlayout.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>

extern char end[];

#define COLOUR_OFF 0
#define CPU_NUM 4
#define AC_LIMIT 10
#define P2INDEX

static SpinLock mem_lock;
static SpinLock mem_lock2;
static SpinLock refcnt_lock;
// static bool zero_init = true;
static void *zero_page;
// struct page page_arr[TOTAL_PAGE];
RefCount zero_page_cnt;

RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt) {
  init_rc(&alloc_page_cnt);
  init_spinlock(&mem_lock);
  init_spinlock(&mem_lock2);
}

int page2index(void *p) {
  auto start = PAGE_BASE((u64)&end) + PAGE_SIZE;
  return ((u64)p - start) / PAGE_SIZE;
}

int total_page() {
  return (P2K(PHYSTOP) - (PAGE_BASE((u64)&end) + PAGE_SIZE)) / PAGE_SIZE;
}

// All usable pages are added to the queue.
// NOTE: You can use the page itself to store allocator data of it.
// In this example, the fix-lengthed meta-data of the allocator are stored in
// .bss (static QueueNode* pages),
//  and the per-page allocator data are stored in the first sizeof(QueueNode)
//  bytes of pages themselves.
//
// See API Reference for more information on given data structures.
static QueueNode *pages;
define_early_init(pages) {
  for (u64 p = PAGE_BASE((u64)&end) + PAGE_SIZE; p < P2K(PHYSTOP);
       p += PAGE_SIZE) {
    // init_rc(&page_arr[page2index((void *)p)].ref);
    add_to_queue(&pages, (QueueNode *)p);
  }
  printk("page list finish\n");
}

define_init(zero_page) {
  init_rc(&zero_page_cnt);
  zero_page = kalloc_page();
  memset(zero_page, 0, PAGE_SIZE);
  _increment_rc(&zero_page_cnt);
  _increment_rc(&alloc_page_cnt);
}

// Allocate: fetch a page from the queue of usable pages.
void *kalloc_page() {
  _increment_rc(&alloc_page_cnt);
  auto node = fetch_from_queue(&pages);
  // _increment_rc(&page_arr[page2index(node)].ref);
  return node;
}

// Free: add the page to the queue of usable pages.
void kfree_page(void *p) {
  if (p == zero_page) {
    _decrement_rc(&zero_page_cnt);
    if (zero_page_cnt.count == 0) {
      _decrement_rc(&alloc_page_cnt);
      add_to_queue(&pages, p);
      zero_page = NULL;
    }
  } else {
    _decrement_rc(&alloc_page_cnt);
    add_to_queue(&pages, p);
  }
}

typedef struct Array_cache {
  unsigned int avail;
  void *entry[AC_LIMIT];
} Array_cache_t;

// put the slab descripter in the head of slabs_partial,then set freelist and
// avail.
typedef struct CacheNode {
  unsigned int pgorder; /* order of pages per slab (2^n) */
  unsigned int num;     /* objects per slab */
  isize object_size;
  unsigned int colour_off; /* colour offset */
  Array_cache_t array_cache[CPU_NUM];
  ListNode slabs_partial_head;
  ListNode slabs_full_head;
  ListNode slabs_free_head;
  ListNode cnode;
} CacheNode;

typedef struct SlabNode {
  int colour;
  unsigned int active;
  CacheNode *owner_cache;
  int *freelist;
  ListNode snode;
} SlabNode;

// first means a cache node's first slab, save cache data (slab descripter data)
void init_slab_node(SlabNode *slab_node, int obj_num) {
  slab_node->active = 0;
  slab_node->colour = 0;
  slab_node->freelist = (int *)((u64)slab_node + sizeof(SlabNode));
  for (int i = 0; i < obj_num; i++) {
    slab_node->freelist[i] = i;
  }
  init_list_node(&slab_node->snode);
}

// must ensure the slab is not full
void *alloc_obj(SlabNode *slab_node, isize size, int obj_num) {
  auto ret = (void *)((u64)slab_node + sizeof(SlabNode) +
                      (sizeof(int) * obj_num / 8 + 1) * 8 + slab_node->colour +
                      (slab_node->freelist[slab_node->active++]) * size);
  return ret;
}

int get_obj_index(SlabNode *slab_node, isize size, int obj_num,
                  void *obj_addr) {
  return ((u64)obj_addr - (u64)slab_node - (sizeof(int) * obj_num / 8 + 1) * 8 -
          slab_node->colour - sizeof(SlabNode)) /
         size;
}

// head can be null
void init_cache_node(isize size, CacheNode *cache_node, int num) {
  cache_node->pgorder = 0;
  cache_node->object_size = size;
  cache_node->colour_off = COLOUR_OFF;
  cache_node->num = num;
  init_list_node(&cache_node->slabs_partial_head);
  init_list_node(&cache_node->slabs_full_head);
  init_list_node(&cache_node->slabs_free_head);
  for (int i = 0; i < CPU_NUM; i++) {
    cache_node->array_cache[i].avail = 0;
    for (int j = 0; j < AC_LIMIT; j++) {
      cache_node->array_cache[i].entry[j] = 0;
    }
  }
  init_list_node(&cache_node->cnode);
}

static CacheNode *kmem_cache_array[2048];

// objects in the middle be freed
void *kalloc(isize size) {
  setup_checker(one);
  acquire_spinlock(one, &mem_lock);
  void *objp;
  CacheNode *search_res = kmem_cache_array[size];
  if (NULL == search_res) {
    auto slab_node = (SlabNode *)(kalloc_page());
    auto cache_node = (CacheNode *)(PAGE_BASE((u64)slab_node) + PAGE_SIZE -
                                    sizeof(CacheNode));
    kmem_cache_array[size] = cache_node;
    auto num = (PAGE_SIZE - 3 * COLOUR_OFF - sizeof(SlabNode) - 8 -
                sizeof(CacheNode)) /
               (size + sizeof(int));
    init_cache_node(size, cache_node, num);
    init_slab_node(slab_node, num);
    slab_node->owner_cache = cache_node;
    _insert_into_list(&cache_node->slabs_partial_head, &slab_node->snode);
    auto ret = alloc_obj(slab_node, size, num);
    if (slab_node->active >= cache_node->num) {
      _detach_from_list(&slab_node->snode);
      _merge_list(&slab_node->snode, &cache_node->slabs_full_head);
    }
    release_spinlock(one, &mem_lock);
    return ret;
  }

  auto kmem_cache = search_res;
  Array_cache_t *ac = &(kmem_cache->array_cache[cpuid()]);
  if (ac->avail > 0) {
    objp = ac->entry[--ac->avail];
    release_spinlock(one, &mem_lock);
    return objp;
  }
  // avail is zero,refill the entry from global
  if (!_empty_list(&kmem_cache->slabs_partial_head)) {
    auto slab_node = container_of(kmem_cache->slabs_partial_head.next,
                                  struct SlabNode, snode);
    auto ret = alloc_obj(slab_node, size, kmem_cache->num);
    if (slab_node->active >= kmem_cache->num) {
      _detach_from_list(&slab_node->snode);
      _merge_list(&slab_node->snode, &kmem_cache->slabs_full_head);
    }
    release_spinlock(one, &mem_lock);
    return ret;
  }

  if (!_empty_list(&kmem_cache->slabs_free_head)) {
    auto slab_node =
        container_of(kmem_cache->slabs_free_head.next, struct SlabNode, snode);
    auto ret = alloc_obj(slab_node, size, kmem_cache->num);

    _detach_from_list(&slab_node->snode);

    if (slab_node->active >= kmem_cache->num) {
      _merge_list(&slab_node->snode, &kmem_cache->slabs_full_head);
    } else {
      _merge_list(&slab_node->snode, &kmem_cache->slabs_partial_head);
    }

    release_spinlock(one, &mem_lock);
    return ret;
  }

  auto slab_node = (SlabNode *)kalloc_page();
  init_slab_node(slab_node, kmem_cache->num);
  slab_node->owner_cache = kmem_cache;

  _insert_into_list(&kmem_cache->slabs_partial_head, &slab_node->snode);
  auto ret = alloc_obj(slab_node, size, kmem_cache->num);
  if (slab_node->active >= kmem_cache->num) {
    _detach_from_list(&slab_node->snode);
    _merge_list(&slab_node->snode, &kmem_cache->slabs_full_head);
  }
  release_spinlock(one, &mem_lock);
  return ret;
}

// find in the global slab: full or partial. from different slabs.kmem_cache
void cache_flusharray(Array_cache_t *ac, CacheNode *cache_node) {
  SlabNode *flush_slab = (SlabNode *)PAGE_BASE((u64)(ac->entry[--ac->avail]));
  flush_slab->freelist[--flush_slab->active] =
      get_obj_index(flush_slab, cache_node->object_size, cache_node->num,
                    ac->entry[ac->avail]);
  _detach_from_list(&flush_slab->snode);
  if (flush_slab->active == cache_node->num - 1) {
    _insert_into_list(&cache_node->slabs_partial_head, &flush_slab->snode);
  } else if (flush_slab->active == 0) {
    _insert_into_list(&cache_node->slabs_free_head, &flush_slab->snode);
  }
}

void kfree(void *p) {
  setup_checker(one);
  acquire_spinlock(one, &mem_lock);
  SlabNode *owner_slab = (SlabNode *)PAGE_BASE((u64)p);
  auto kmem_cache = owner_slab->owner_cache;
  Array_cache_t *ac = &(kmem_cache->array_cache[cpuid()]);
  if (ac->avail == AC_LIMIT) {
    cache_flusharray(ac, kmem_cache);
  }
  ac->entry[ac->avail++] = p;
  if (ac->avail == 0 && _empty_list(&kmem_cache->slabs_full_head) &&
      _empty_list(&kmem_cache->slabs_partial_head)) {
    kfree_page((void *)PAGE_BASE((u64)p));
  }

  release_spinlock(one, &mem_lock);
}

u64 left_page_cnt() {
  _acquire_spinlock(&refcnt_lock);
  auto ret = total_page() - alloc_page_cnt.count;
  _release_spinlock(&refcnt_lock);
  return ret;
}

u32 write_page_to_disk(void *ka) {
  auto first_bno = find_and_set_8_blocks();
  for (u32 i = 0; i < 8; i++) {
    auto block = bcache.acquire(first_bno + i);
    attach_pgdir(&thisproc()->pgdir);
    PTEntriesPtr pte_p =
        get_pte(&thisproc()->pgdir, (u64)(ka + i * BLOCK_SIZE), FALSE);
    if (pte_p != NULL && *pte_p != 0) {
      memcpy(block->data, (void *)(P2K(PTE_ADDRESS(*pte_p))), BLOCK_SIZE);
      bcache.sync(NULL, block);
    }
    bcache.release(block);
  }
  return first_bno;
}

void read_page_from_disk(void *ka, u32 bno) {
  if (!((u64)ka & KSPACE_MASK)) {
    printk("not kernel addr\n");
    PANIC();
  }
  for (u32 i = 0; i < 8; i++) {
    auto block = bcache.acquire(bno + i);
    memcpy(ka + i * BLOCK_SIZE, block->data, BLOCK_SIZE);
    bcache.release(block);
  }
}

void *get_zero_page() {
  if (zero_page == NULL) {
    zero_page = kalloc_page();
    init_rc(&zero_page_cnt);
    _increment_rc(&alloc_page_cnt);
  }
  _increment_rc(&zero_page_cnt);
  return zero_page;
}

bool check_zero_page() {

  for (auto i = 0; i < PAGE_SIZE; i++) {
    if (((u8 *)zero_page)[i] != 0) {
      return false;
      break;
    }
  }
  return true;
}