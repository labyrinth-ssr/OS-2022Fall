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

void uvmunmap(struct pgdir *pgdir, u64 va, u64 npages, int do_free) {
  u64 a;
  u64 *pte;
  if (va % PAGE_SIZE) {
    panic("not aligned");
  }
  for (a = va; a < va + npages * PAGE_SIZE; a += PAGE_SIZE) {
    pte = get_pte(pgdir, a, false);
    if (!pte)
      panic("walk");
    if (!(*pte & PTE_VALID))
      panic("not mapped");
    if (PTE_FLAGS(*pte) == PTE_VALID)
      panic("not a leaf");
    if (do_free) {
      u64 pa = P2K(PTE_ADDRESS(*pte));
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

int uvm_dealloc(struct pgdir *pgdir, usize oldsz, usize newsz) {
  /* TODO: Lab9 Shell */
  if (newsz >= oldsz)
    return (int)oldsz;
  if (ROUNDUP(newsz, PAGE_SIZE) < ROUNDUP(oldsz, PAGE_SIZE)) {
    int npgs = (int)((ROUNDUP(oldsz, PAGE_SIZE) - ROUNDUP(newsz, PAGE_SIZE)) /
                     PAGE_SIZE);
    uvmunmap(pgdir, ROUNDUP(newsz, PAGE_SIZE), npgs, 1);
  }
  return 0;
}

int uvm_map(struct pgdir *pgdir, void *va, usize sz, u64 pa) {
  /* TO-DO: Lab2 memory*/
  u64 a, last;
  PTEntriesPtr pte;
  if (!sz)
    panic("map:sz 0");
  a = (u64)ROUNDDOWN(va, PAGE_SIZE);
  last = (u64)ROUNDDOWN((va + sz - 1), PAGE_SIZE);
  while (true) {
    if ((pte = get_pte(pgdir, a, 1)) == 0)
      return -1;
    if (*pte & PTE_VALID) {
      printk("va remap: %llx", a);
      PANIC();
    }
    *pte = (pa) | PTE_USER_DATA;
    printk("*pte = %llx ", (pa) | PTE_USER_DATA);
    // printf("!%llx pte:%llx!\n", *pte, pte);
    printk("va %llx map pa %llx\n", a, *pte);
    if (a == last)
      break;
    a += PAGE_SIZE;
    pa += PAGE_SIZE;
  }
  return 0;
}

void uvmclear(struct pgdir *pgdir, u64 va) {
  auto pte = *get_pte(pgdir, va, 0);
  if (pte == 0)
    panic("uvmclear");
  pte &= ~PTE_USER;
}

int uvm_alloc(struct pgdir *pgdir, usize oldsz, usize newsz) {
  char *mem;
  u64 a;
  if (newsz < oldsz)
    return oldsz;
  // if (base + newsz > stksz)
  //     PANIC("overflow");
  printk("uvm_alloc:%llx", oldsz);
  oldsz = ROUNDDOWN(oldsz, PAGE_SIZE);
  for (a = oldsz; a < newsz; a += PAGE_SIZE) {
    mem = kalloc_page();
    if (mem == 0) {
      uvm_dealloc(pgdir, a, oldsz);
      return 0;
    }
    memset(mem, 0, PAGE_SIZE);
    if (uvm_map(pgdir, (void *)a, PAGE_SIZE, K2P(mem)) != 0) {
      printk("already map\n");
      kfree(mem);
      uvm_dealloc(pgdir, a, oldsz);
    }
  }
  return (int)newsz;
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on su  ccess, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(struct pgdir *old, struct pgdir *new, u64 sz) {
  PTEntry *pte_p;
  u64 pa, i;
  // u32 flags;
  char *mem;

  for (i = 0; i < sz; i += PAGE_SIZE) {
    printk("upa user:%llx, \n", (u64)get_pte(old, 400000, false));
    printk("upa user:%llx, \n", *get_pte(old, 400000, false));
    if ((pte_p = get_pte(old, i, false)) == 0)
      panic("uvmcopy: pte should exist");
    printk("upa:%llx, \n", *pte_p);
    printk("upa:%llx, \n", (u64)pte_p);

    if ((*pte_p & PTE_VALID) == 0) {
      printk("invalid upa:%llx\n", *pte_p);
      panic("uvmcopy: page not present");
    }
    pa = P2K(PTE_ADDRESS(*pte_p));
    // flags = PTE_FLAGS(*pte_p);
    if ((mem = kalloc_page()) == 0)
      panic("no free page\n");
    // goto err;
    memmove(mem, (char *)pa, PAGE_SIZE);
    // create pte for virtual address, return 0 if success.
    auto pte_p = get_pte(new, i, true);
    if (pte_p == NULL) {
      panic("alloc pte fail");
    }
    *pte_p = K2P(mem) | PTE_USER_DATA;
    // if (mappages(new, i, PAGE_SIZE, (uint64)mem, flags) != 0) {
    //   kfree(mem);
    //   // goto err;
    // }
  }
  return 0;

  // err:
  //   uvmunmap(new, 0, i / PAGE_SIZE, 1);
  //   return -1;
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len) {
  // TODO
  char *buf;
  u64 pa0;
  u64 n, va0;
  buf = p;
  while (len > 0) {
    va0 = (u64)ROUNDDOWN(va, PAGE_SIZE);
    // FIXME
    pa0 = P2K(PTE_ADDRESS(*get_pte(pd, (u64)va, true)));
    if (pa0 == 0)
      return -1;
    n = PAGE_SIZE - ((u64)va - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + ((u64)va - va0)), buf, n);
    len -= n;
    buf += n;
    va = (void *)(va0 + PAGE_SIZE);
  }
  return 0;
}
void attach_pgdir(struct pgdir *pgdir) {
  extern PTEntries invalid_pt;
  _acquire_spinlock(&thisproc()->pgdir.lock);
  thisproc()->pgdir.online = false;
  _release_spinlock(&thisproc()->pgdir.lock);
  if (pgdir->pt) {
    // _acquire_spinlock(&pgdir->lock);
    pgdir->online = TRUE;
    // _release_spinlock(&thisproc()->pgdir.lock);
    arch_set_ttbr0(K2P(pgdir->pt));
  } else {
    arch_set_ttbr0(K2P(&invalid_pt));
  }
}

u64 uva2ka(struct pgdir *pgdir, u64 uva) {
  // FIXME:alloc or not
  u64 *pte = get_pte(pgdir, uva, 0);
  if ((*pte & (PTE_VALID)) == 0)
    return 0;
  if (((*pte) & PTE_USER) == 0)
    return 0;
  return P2K(PTE_ADDRESS(*pte));
}

// 在给定的页表上，建立虚拟地址到物理地址的映射
void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags) {

  *get_pte(pd, va, true) = K2P(ka) | flags | PTE_VALID; // or PTE_KERNEL_DEVICE?
};
