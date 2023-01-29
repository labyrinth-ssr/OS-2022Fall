#include <common/rc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <common/list.h>
#include <driver/memlayout.h>
#include <kernel/printk.h>
#include <common/defines.h>
#include <common/string.h>
#include <fs/cache.h>

#define OBJ_SIZE 16
RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt) {
    init_rc(&alloc_page_cnt);
}

extern char end[];
static QueueNode* pages;
static QueueNode* slab[PAGE_SIZE / OBJ_SIZE];
static struct page page_cnt[(u64)PHYSTOP / (u64)PAGE_SIZE];
static u64 tot_page_cnt;
static void* zero_page;
define_early_init(init_page) {
    for (u64 p = P2K(0); p < PAGE_BASE((u64)&end) + PAGE_SIZE; p += PAGE_SIZE) {
        inc_page_cnt((void*)p);
    }
    for (u64 p = PAGE_BASE((u64)&end) + PAGE_SIZE; p < P2K(PHYSTOP); p += PAGE_SIZE) {
        add_to_queue(&pages, (QueueNode*)p);
        tot_page_cnt++;
    }
}

define_init(init_zero_page) {
    zero_page = kalloc_page();
    memset(zero_page, 0, PAGE_SIZE);
}

void* kalloc_page() {
    _increment_rc(&alloc_page_cnt);
    // TODO
    void* ret = fetch_from_queue(&pages);
    u64 idx = K2P(ret) / (u64)PAGE_SIZE;
    init_rc(&(page_cnt[idx].ref));

    return ret;
}

void kfree_page(void* p) {
    // TODO
    if (p == zero_page) { 
        return;
    }
    u64 idx = K2P(p) / (u64)PAGE_SIZE;
    bool need_free = _decrement_rc(&(page_cnt[idx].ref));
    if (need_free) {
        add_to_queue(&pages, (QueueNode*)p);
        _decrement_rc(&alloc_page_cnt);
    }
}

// TODO: kalloc kfree
void* kalloc(isize size) {
    size = (size + OBJ_SIZE - 1) / OBJ_SIZE;
    void* ret = NULL;
    while ((ret =  fetch_from_queue(&slab[size])) == NULL) {
        void *p = kalloc_page();
        *(u64*) p = size;
        p += 8;
        for (int i = 0; i < (PAGE_SIZE - 8) / (OBJ_SIZE * size); i++) {
            add_to_queue(&slab[size], p);
            p += size * OBJ_SIZE;
        }
    }
    return ret;
}

void kfree(void* p) {
    void *page_start = (void*)((u64)p & ~(PAGE_SIZE - 1));
    u64 size = *(u64*)page_start;
    add_to_queue(&slab[size], p);
}

u64 left_page_cnt() {
    return tot_page_cnt - alloc_page_cnt.count;
}

void* get_zero_page() {
    return zero_page;
}

bool check_zero_page() {
    for (u64 i = 0; i < PAGE_SIZE / sizeof(u64); ++i) {
        if (*((u64*)zero_page + i) != 0) {
            return false;
        }
    }
    return true;
}

u32 write_page_to_disk(void* ka) {
    u32 bno = find_and_set_8_blocks();
    for (u32 i = bno; i < bno + 8; ++i) {
        Block* block = bcache.acquire(i);
        memcpy(block->data, ka + (i - bno) * BLOCK_SIZE, BLOCK_SIZE);
        bcache.sync(NULL, block);
        bcache.release(block);
    }
    return bno;
}

void read_page_from_disk(void* ka, u32 bno) {
    for (u32 i = bno; i < bno + 8; ++i) {
        Block* block = bcache.acquire(i);
        memcpy(ka + (i - bno) * BLOCK_SIZE, block->data, BLOCK_SIZE);
        bcache.release(block);
    }
    release_8_blocks(bno);
}

void inc_page_cnt(void* ka) {
    _increment_rc(&(page_cnt[(u64)K2P(ka) / PAGE_SIZE].ref));
}