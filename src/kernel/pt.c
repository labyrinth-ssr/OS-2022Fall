#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <aarch64/intrinsic.h>
#include <kernel/paging.h>
#include <kernel/sched.h>
#include <kernel/printk.h>

PTEntriesPtr get_pte(struct pgdir* pgdir, u64 va, bool alloc)
{
    // TODO
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.
    if (pgdir->pt == NULL) {
        if (alloc) {
            pgdir->pt = kalloc_page();
            memset(pgdir->pt, 0, PAGE_SIZE);
        } else {
            return NULL;
        }
    }
    PTEntriesPtr pt0 = pgdir->pt;
    if (!(pt0[VA_PART0(va)] & PTE_VALID)) {
        if (alloc) {
            PTEntriesPtr pt1 = kalloc_page();
            memset(pt1, 0, PAGE_SIZE);
            pt0[VA_PART0(va)] = K2P(pt1) | PTE_TABLE;
        } else {
            return NULL;
        }
    }
    PTEntriesPtr pt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt0[VA_PART0(va)]));
    if (!(pt1[VA_PART1(va)] & PTE_VALID)) {
        if (alloc) {
            PTEntriesPtr pt2 = kalloc_page();
            memset(pt2, 0, PAGE_SIZE);
            pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
        } else {
            return NULL;
        }
    }
    PTEntriesPtr pt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt1[VA_PART1(va)]));
    if (!(pt2[VA_PART2(va)]  & PTE_VALID)) {
        if (alloc) {
            PTEntriesPtr pt3 = kalloc_page();
            memset(pt3, 0, PAGE_SIZE);
            pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;
        } else {
            return NULL;
        }
    }
    PTEntriesPtr pt3 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt2[VA_PART2(va)]));
    return (PTEntriesPtr)(pt3 + VA_PART3(va));
}

void init_pgdir(struct pgdir* pgdir)
{
    pgdir->pt = kalloc_page();
	memset(pgdir->pt, 0, PAGE_SIZE);
    pgdir->online = false;
    init_spinlock(&(pgdir->lock));
    init_list_node(&(pgdir->section_head));
}

void free_pgdir(struct pgdir* pgdir)
{
    // TODO
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
    if (pgdir->pt == NULL) {
        return;
    }
    free_sections(pgdir);
    auto pt0 = pgdir->pt;
    for (int i = 0; i < N_PTE_PER_TABLE; ++i) {
        if (pt0[i] & PTE_VALID) {
            PTEntriesPtr pt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt0[i]));
            for (int j = 0; j < N_PTE_PER_TABLE; ++j) {
                if (pt1[j] & PTE_VALID) {
                    PTEntriesPtr pt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt1[j]));
                    for (int k = 0; k < N_PTE_PER_TABLE; ++k) {
                        if (pt2[k] & PTE_VALID) {
                            kfree_page((void*)P2K(PTE_ADDRESS(pt2[k])));
                        }
                    }
                    kfree_page((void*)pt2);
                }
            }
            kfree_page((void*)pt1);
        }
    }
    kfree_page((void*)pgdir->pt);
    pgdir->pt = NULL;
}

void attach_pgdir(struct pgdir* pgdir)
{
    extern PTEntries invalid_pt;
    auto this = thisproc();

    _acquire_spinlock(&(this->pgdir.lock));
    if (this->pgdir.pt != NULL) {
        this->pgdir.online = false;
    }
    _release_spinlock(&(this->pgdir.lock));
    
    if (pgdir->pt) {
        _acquire_spinlock(&(pgdir->lock));
        pgdir->online = true;
        _release_spinlock(&(pgdir->lock));

        arch_tlbi_vmalle1is();
        arch_set_ttbr0(K2P(pgdir->pt));
    } else {
        arch_set_ttbr0(K2P(&invalid_pt));
    }
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir* pd, void* va, void *p, usize len){
    // TODO
    while (len > 0) {
        u64 n = len;
        u64 off = (u64)va - PAGE_BASE((u64)va);
        u64 max_n = PAGE_SIZE - off;
        if (n > max_n) {
            n = max_n;
        }
        u64 pa = *get_pte(pd, (u64)va, TRUE);
        if (pa == 0) {
            return -1;
        }
        memcpy((void*)(P2K(PTE_ADDRESS(pa)) + off), p, n);
        len -= n;
        p += n;
        va += n;
    }
    return 0;
}

void vmmap(struct pgdir* pd, u64 va, void* ka, u64 flags) {
    PTEntriesPtr pt3 = get_pte(pd, va, true);
    if (flags != PTE_USER_DATA && flags != (PTE_USER_DATA | PTE_RO)) {
        printk("dfs\n");
    }
    *pt3 = (PTEntry)(K2P(ka) | flags);
    inc_page_cnt(ka);
}

