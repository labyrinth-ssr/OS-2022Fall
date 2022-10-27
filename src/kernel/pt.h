#pragma once

#include <aarch64/mmu.h>

struct pgdir
{
    PTEntriesPtr pt;//init = NULL
};

void init_pgdir(struct pgdir* pgdir);
PTEntriesPtr get_pte(struct pgdir* pgdir, u64 va, bool alloc);//alloc = true : alloc for null va , else return NULL
void free_pgdir(struct pgdir* pgdir);
void attach_pgdir(struct pgdir* pgdir);
