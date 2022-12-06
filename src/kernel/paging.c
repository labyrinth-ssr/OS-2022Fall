#include "common/checker.h"
#include "common/spinlock.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>

define_rest_init(paging) {
  // TODO init
  init_sections(&thisproc()->pgdir.section_head);
}

void init_sections(ListNode *section_head) {
  struct section *heap_section = kalloc(sizeof(struct section));
  memset(heap_section, 0, sizeof(struct section));
  init_sleeplock(&heap_section->sleeplock);
  init_list_node(&heap_section->stnode);
  _merge_list(&heap_section->stnode, section_head);
}

u64 sbrk(i64 size) {
  ListNode *section_head = &thisproc()->pgdir.section_head;
  struct section *section = NULL;

  _for_in_list(sp, section_head) {
    struct section *section_p = container_of(sp, struct section, stnode);
    // how to use flag?
    if (section_p->flags & ST_HEAP) {
      section = section_p;
      break;
    }
  }

  if (section == NULL) {
    printk("no heap section\n");
    PANIC();
  }

  u64 ret_addr = section->end;
  u64 sz = size * PAGE_SIZE;
  if (size >= 0) {
    section->end += sz;
  } else {
    if (sz <= section->end - section->begin) {

      // 不需要拿section的sleeplock
      while (sz > 0) {

        PTEntriesPtr pte_p = get_pte(&thisproc()->pgdir, section->end, FALSE);
        if (pte_p != NULL && *pte_p != 0 && (*pte_p & PTE_VALID)) {
          release_8_blocks(*pte_p >> 12);
          kfree_page((void *)(*pte_p));
        }
        if (pte_p != NULL || *pte_p != 0) {
          memset(pte_p, 0, sizeof(PTEntry));
        }
        section->end -= PAGE_SIZE;
        sz -= PAGE_SIZE;
      }

    } else {
      printk("no enough space in heap section\n");
      PANIC();
    }
  }
  arch_tlbi_vmalle1is();

  return ret_addr;
}

void *alloc_page_for_user() {
  //若两个CPU获得了样的cnt开始分配页，而一个分配完成后，另一个再进入就已经达到软上限,所以要加锁
  while (left_page_cnt() <= REVERSED_PAGES) { // this is a soft limit
                                              // TODO
    while (1) {
      auto pd = &get_offline_proc()->pgdir;
      struct section *section = NULL;
      _for_in_list(p, &pd->section_head) {
        if (p != NULL) {
          section = container_of(p, struct section, stnode);
        }
      }
      if (section != NULL) {
        swapout(pd, section);
        break;
      }
      printk("proc has no section\n");
    }
  }
  return kalloc_page();
}

// caller must have the pd->lock
void swapout(struct pgdir *pd, struct section *st) {
  ASSERT(!(st->flags & ST_SWAP));
  st->flags |= ST_SWAP;
  // TODO
  // bno = (*pte >>12)
  u64 begin = st->begin, end = st->end;
  for (u64 va = st->begin; va <= st->end; va += PAGE_SIZE) {
    PTEntriesPtr pte_p = get_pte(pd, va, FALSE);
    // null ptr exists?
    if (pte_p != NULL && *pte_p != 0) {
      *pte_p &= ~PTE_VALID;
    }
  }

  setup_checker(0);
  ASSERT(acquire_sleeplock(0, &st->sleeplock));
  _release_spinlock(&pd->lock);
  if (!(st->flags & ST_FILE)) {
    for (u64 p = begin; p <= end; p += PAGE_SIZE) {
      PTEntriesPtr pte_p = get_pte(pd, p, FALSE);
      if (pte_p != NULL && *pte_p != 0) {
        // not sure, whether to >> 12
        u32 bno = *pte_p >> 12;
        write_page_to_disk((void *)p);
        *pte_p = bno & ~PTE_VALID;
      }
    }
  }

  for (u64 p = begin; p <= end; p += PAGE_SIZE) {
    PTEntriesPtr pte_p = get_pte(pd, p, FALSE);
    if (pte_p != NULL && *pte_p != 0) {
      kfree_page((void *)p);
    }
  }

  release_sleeplock(0, &st->sleeplock);
}
// Free 8 continuous disk blocks
void swapin(struct pgdir *pd, struct section *st) {
  ASSERT(st->flags & ST_SWAP);
  // TODO
  setup_checker(0);
  ASSERT(acquire_sleeplock(0, &st->sleeplock));

  for (u64 p = st->begin; p <= st->end; p += PAGE_SIZE) {
    PTEntriesPtr pte_p = get_pte(pd, p, FALSE);
    if (pte_p != NULL && *pte_p != 0) {
      auto page_p = alloc_page_for_user();
      read_page_from_disk((void *)page_p, *pte_p);
      vmmap(pd, p, page_p, PTE_KERNEL_DATA);
    }
  }

  release_sleeplock(0, &st->sleeplock);

  for (u64 p = st->begin; p <= st->end; p += PAGE_SIZE) {
    PTEntriesPtr pte_p = get_pte(pd, p, FALSE);
    if (pte_p != NULL && *pte_p != 0) {
      *pte_p |= PTE_VALID;
    }
  }

  st->flags &= ~ST_SWAP;
}

int pgfault(u64 iss) {
  printk("iss is %lld\n", iss);
  struct proc *p = thisproc();
  struct pgdir *pd = &p->pgdir;
  u64 addr = arch_get_far();
  ListNode *section_head = &pd->section_head;
  struct section *section = NULL;
  // TODO
  // addr find sectioin : begin ?
  _for_in_list(p, section_head) {
    struct section *section_p = container_of(p, struct section, stnode);
    if (section_p->begin <= addr && section_p->end > addr) {
      section = section_p;
      break;
    }
  }

  if (section == NULL) {
    printk("no corresponding section\n");
    PANIC();
  }

  PTEntriesPtr pte_p = get_pte(pd, addr, false);

  if (pte_p == NULL || *pte_p == 0) {
    // swapin if need?
    swapin(pd, section);
  } else if (*pte_p & PTE_RO) {
    auto page = alloc_page_for_user();
    vmmap(pd, addr, page, PTE_USER_DATA | PTE_RW);

    memcpy((void *)addr, (void *)P2K(PTE_ADDRESS(*pte_p)), PAGE_SIZE);

    kfree_page((void *)(*pte_p));

  } else if (*pte_p & PTE_VALID) {
    swapin(pd, section);
  }
  arch_tlbi_vmalle1is();

  return 0;
}
