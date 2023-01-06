#include "aarch64/mmu.h"
#include "common/defines.h"
#include "common/list.h"
#include "common/spinlock.h"
#include "kernel/paging.h"
#include "kernel/sched.h"
#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/pt.h>

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc) {
  // TODO
  // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
  // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or
  // return NULL if false. THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY
  // PTE.
  auto pt0 = pgdir->pt;
  if (pt0 == NULL) {
    if (!alloc)
      return NULL;
    pt0 = (PTEntriesPtr)kalloc_page();
    pgdir->pt = pt0;
    memset(pt0, 0, PAGE_SIZE);
  }
  PTEntriesPtr pt1 = NULL;
  if (pt0[VA_PART0(va)] == NULL || !(pt0[VA_PART0(va)] & PTE_VALID)) {
    if (!alloc)
      return NULL;
    pt1 = (PTEntriesPtr)kalloc_page();
    memset(pt1, 0, PAGE_SIZE);
    pt0[VA_PART0(va)] = K2P(pt1) | PTE_TABLE;
  } else {
    pt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt0[VA_PART0(va)]));
  }
  PTEntriesPtr pt2 = NULL;
  if (pt1[VA_PART1(va)] == NULL || !(pt1[VA_PART1(va)] & PTE_VALID)) {
    if (!alloc)
      return NULL;
    pt2 = (PTEntriesPtr)kalloc_page();
    memset(pt2, 0, PAGE_SIZE);
    pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
  } else {
    pt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt1[VA_PART1(va)]));
  }
  PTEntriesPtr pt3 = NULL;
  if (pt2[VA_PART2(va)] == NULL || !(pt2[VA_PART2(va)] & PTE_VALID)) {
    if (!alloc)
      return NULL;
    pt3 = (PTEntriesPtr)kalloc_page();
    memset(pt3, 0, PAGE_SIZE);
    pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;
  } else {
    pt3 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt2[VA_PART2(va)]));
  }
  return &pt3[VA_PART3(va)];
}

void init_pgdir(struct pgdir *pgdir) {
  memset(pgdir, 0, sizeof(struct pgdir));
  init_spinlock(&pgdir->lock);
  init_list_node(&pgdir->section_head);
  init_sections(&pgdir->section_head);
  // pgdir->pt = NULL;

  ASSERT(get_pte(pgdir, 0, true));
}

void free_pt_r(PTEntriesPtr pt, int num) {
  if (num == 3) {
    if (pt != NULL) {
      kfree_page((PTEntriesPtr)P2K(PTE_ADDRESS((PTEntry)pt)));
    }
    return;
  }
  if (pt != NULL) {
    for (int i = 0; i < N_PTE_PER_TABLE; i++) {
      auto ptin = num == 0 ? pt : (PTEntriesPtr)P2K(PTE_ADDRESS((PTEntry)pt));
      free_pt_r((PTEntriesPtr)(ptin[i]), num + 1);
    }
    kfree_page((PTEntriesPtr)P2K(PTE_ADDRESS((PTEntry)pt)));
  }
}

void free_pgdir(struct pgdir *pgdir) {
  // TODO
  // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
  // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
  free_sections(pgdir);
  if (pgdir->pt == NULL) {
    return;
  }
  free_pt_r(pgdir->pt, 0);
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len) {
  // TODO
}
void attach_pgdir(struct pgdir *pgdir) {
  extern PTEntries invalid_pt;
  _acquire_spinlock(&thisproc()->pgdir.lock);
  thisproc()->pgdir.online = false;
  _release_spinlock(&thisproc()->pgdir.lock);

  if (pgdir->pt) {
    _acquire_spinlock(&pgdir->lock);
    pgdir->online = TRUE;
    _release_spinlock(&thisproc()->pgdir.lock);

    arch_set_ttbr0(K2P(pgdir->pt));
  } else {
    arch_set_ttbr0(K2P(&invalid_pt));
  }
}

// 在给定的页表上，建立虚拟地址到物理地址的映射
void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags) {

  *get_pte(pd, va, true) = K2P(ka) | flags | PTE_VALID; // or PTE_KERNEL_DEVICE?
};
