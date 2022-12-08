#pragma once

#include "common/list.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/rc.h>

#define REVERSED_PAGES 1024 // Reversed pages
#define TOTAL_PHY_PAGES                                                        \
  (P2K(PHYSTOP) - PAGE_BASE((u64)&end) - PAGE_SIZE) / PAGE_SIZE

struct page {
  // QueueNode node;
  RefCount ref;
};

#define PAGE_META(p) p + PAGE_SIZE - sizeof(struct page)

WARN_RESULT void *kalloc_page();
void kfree_page(void *);

WARN_RESULT void *kalloc(isize);
void kfree(void *);

u64 left_page_cnt();
WARN_RESULT void *get_zero_page();
bool check_zero_page();
u32 write_page_to_disk(void *ka);
void read_page_from_disk(void *ka, u32 bno);
