#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/pt.h>

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc) {
  // TODO
  // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
  // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or
  // return NULL if false. THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY
  // PTE.
  PTEntriesPtr pt0 = pgdir->pt;
  if (pt0 == NULL) {
    if (!alloc)
      return NULL;
    pt0 = (PTEntriesPtr)kalloc_page();
    memset(pt0, 0, PAGE_SIZE);
  }
  auto pt1 = (PTEntriesPtr)pt0[VA_PART0(va)];
  if (pt1 == NULL) {
    if (!alloc)
      return NULL;
    pt1 = (PTEntriesPtr)kalloc_page();
    memset(pt1, 0, PAGE_SIZE);
    pt0[VA_PART0(va)] = K2P(pt1) | PTE_TABLE;
  }
  auto pt2 = (PTEntriesPtr)pt1[VA_PART1(va)];
  if (pt2 == NULL) {
    if (!alloc)
      return NULL;
    pt2 = (PTEntriesPtr)kalloc_page();
    memset(pt2, 0, PAGE_SIZE);
    pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
  }
  auto pt3 = (PTEntriesPtr)pt2[VA_PART2(va)];
  if (pt3 == NULL) {
    if (!alloc)
      return NULL;
    pt3 = (PTEntriesPtr)kalloc_page();
    memset(pt3, 0, PAGE_SIZE);
    pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;
  }
  auto pa = pt3[VA_PART3(va)];
  return &pa;
}

void init_pgdir(struct pgdir *pgdir) { pgdir->pt = NULL; }

void free_pgdir(struct pgdir *pgdir) {
  // TODO
  // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
  // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
  if (pgdir->pt!=NULL)
  {
    kfree_page(pgdir->pt);
  }
}

void attach_pgdir(struct pgdir *pgdir) {
  extern PTEntries invalid_pt;
  if (pgdir->pt)
    arch_set_ttbr0(K2P(pgdir->pt));
  else
    arch_set_ttbr0(K2P(&invalid_pt));
}
