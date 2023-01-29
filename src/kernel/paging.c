#include <kernel/proc.h>
#include <aarch64/mmu.h>
#include <fs/block_device.h>
#include <fs/cache.h> 
#include <kernel/paging.h>
#include <common/defines.h>
#include <kernel/pt.h>
#include <common/sem.h>
#include <common/list.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/init.h>

// define_rest_init(paging) {
// 	//TODO init
// 	init_block_device();
// 	init_bcache(get_super_block(), &block_device);
// }

static struct section* create_heap(ListNode* section_head, u64 begin) {
	struct section* heap = (struct section*)kalloc(sizeof(struct section));
	heap->flags = ST_HEAP;
	init_sleeplock(&(heap->sleeplock));
	heap->begin = begin;
	heap->end = heap->begin;
	init_list_node(&(heap->stnode));
	_insert_into_list(section_head, &(heap->stnode));
	return heap;
}

u64 sbrk(i64 size) {
	//TODO
	auto this = thisproc();
	auto section_node = this->pgdir.section_head.next;
	auto section = container_of(section_node, struct section, stnode);
	u64 begin = 0;
	while (!(section->flags & ST_HEAP)) {
		section_node = section_node->next;
		section = container_of(section_node, struct section, stnode);
		begin = MAX(begin, section->end);
		if (section_node == &(this->pgdir.section_head)) {
			break;
		}
	}
	if (section_node == &(this->pgdir.section_head)) {
		// if there is no heap, create it
		section = create_heap(&(this->pgdir.section_head), begin + PAGE_SIZE);
	}

	auto end = section->end;
	if (size >= 0) {
		section->end += size;
	} else {
		size = -size;
		if ((u64)size <= section->end - section->begin) {
			if (section->flags & ST_SWAP) {
				swapin(&(this->pgdir), section);
			}
			u64 va_begin = (section->end - size) % PAGE_SIZE ? PAGE_BASE(section->end - size) + PAGE_SIZE: PAGE_BASE(section->end - size);
			for (u64 i = va_begin; i < section->end; i += PAGE_SIZE) {
				auto pte_ptr = get_pte(&(this->pgdir), i, false);
				if ((pte_ptr != NULL) && (PTE_FLAGS(*pte_ptr) & PTE_VALID)) {
					u64 ka = P2K(PTE_ADDRESS(*pte_ptr));
					kfree_page((void*)ka);
					*pte_ptr = 0;
				}
			}
			section->end -= size;
		} else {
			printk("%lld, %lld, %lld\n", section->begin, section->end, size);
			PANIC();
		}
	}
	arch_tlbi_vmalle1is();
	return end;
}	

void* alloc_page_for_user() {
	while(left_page_cnt() <= REVERSED_PAGES){ //this is a soft limit
		//TODO
		auto swp_proc = get_offline_proc();
		if (swp_proc == NULL) {
			break;
		}
		auto section_node = swp_proc->pgdir.section_head.next;
		auto section = container_of(section_node, struct section, stnode);
		while (!(section->flags & ST_HEAP)) {
			// we ensure that there is heap section
			section_node = section_node->next;
			section = container_of(section_node, struct section, stnode);
		}
		swapout(&(swp_proc->pgdir), section);
	}
	return kalloc_page();
}

//caller must have the pd->lock
void swapout(struct pgdir* pd, struct section* st) {
	ASSERT(!(st->flags & ST_SWAP));
	st->flags |= ST_SWAP;
	//TODO
	u64 begin = st->begin, end = st->end;
	for (u64 i = begin; i < end; i += PAGE_SIZE) {
		// set pte_entry invalid
		auto pte_ptr = get_pte(pd, i, false);
		if (pte_ptr != NULL && (*pte_ptr & PTE_VALID)) {
			*pte_ptr &= (~PTE_VALID);
		}
	}
	unalertable_wait_sem(&(st->sleeplock));
	pd->online = true;
	_release_spinlock(&(pd->lock));
	if (st->flags & ST_FILE) {
		// do nothing
	} else {
		for (u64 i = begin; i < end; i += PAGE_SIZE) {
			// to disk
			auto pte_ptr = get_pte(pd, i, false);
			if (pte_ptr != NULL && *pte_ptr != 0 && !(*pte_ptr & PTE_VALID)) {
				u64 ka = P2K(PTE_ADDRESS(*pte_ptr));
				*pte_ptr = (write_page_to_disk((void*)ka) << 12);
				kfree_page((void*)ka);
			}
		}
	}
	post_sem(&(st->sleeplock));
}

void swapin(struct pgdir* pd, struct section* st) {
	ASSERT(st->flags & ST_SWAP);
	//TODO
	unalertable_wait_sem(&(st->sleeplock));
	u64 begin = st->begin, end = st->end;
	if (st->flags & ST_FILE) {
		// do nothing
	} else {
		for (u64 i = begin; i < end; i += PAGE_SIZE) {
			// from disk
			auto pte_ptr = get_pte(pd, i, false);
			if (pte_ptr != NULL && *pte_ptr != 0 && !(*pte_ptr & PTE_VALID)) {
				u64 ka = (u64)kalloc_page();
				u32 bno = (u32)(PTE_ADDRESS(*pte_ptr) >> 12);
				read_page_from_disk((void*)ka, bno);
				vmmap(pd, i, (void*)ka, PTE_USER_DATA);
			}
		}
	}
	post_sem(&(st->sleeplock));
	st->flags &= ~ST_SWAP;
}

int pgfault(u64 iss) {
	struct proc* p = thisproc();
	struct pgdir* pd = &p->pgdir;
	u64 addr = arch_get_far();
	//TODO
	// find the corresponding section
	auto section_node = p->pgdir.section_head.next;
	auto section = container_of(section_node, struct section, stnode);
	while (!(section->begin <= addr && addr < section->end)) {
		section_node = section_node->next;
		ASSERT(section_node != &(p->pgdir.section_head));
		section = container_of(section_node, struct section, stnode);
	}

	PTEntry pa = *get_pte(pd, addr, true);
	if (pa == 0) {
		// lazy allocation
		// if (section->flags & ST_FILE) {
		// 	// if it is file
		// 	// read from disk
		// 	void* new_page = alloc_page_for_user();
		// 	u64 offset = section->offset + PAGE_BASE(addr - section->begin); // offset should aligns to PAGE_SIZE
		// 	inodes.lock(section->fp->ip);
		// 	inodes.read(section->fp->ip, new_page, offset, MIN((u64)PAGE_SIZE, section->offset + section->length - offset));
		// 	inodes.unlock(section->fp->ip);
		// 	vmmap(pd, addr, new_page, (section->flags & ST_RO) ? PTE_USER_DATA | PTE_RO : PTE_USER_DATA);
		// } else 
		if (section->flags & ST_SWAP) { // if swapout
			swapin(pd, section);
			void* new_page = alloc_page_for_user();
			vmmap(pd, addr, new_page, PTE_USER_DATA);
		} else {
			void* new_page = alloc_page_for_user();
			vmmap(pd, addr, new_page, PTE_USER_DATA);
		}
	} else if (pa & PTE_RO) {
		// copy on write
		void* old_page = (void*)P2K(PTE_ADDRESS(pa));
		void* new_page = alloc_page_for_user();
		memcpy(new_page, old_page, PAGE_SIZE);
		vmmap(pd, addr, new_page, PTE_USER_DATA);
		kfree_page(old_page);
	} else if (!(pa & PTE_VALID)) {
		swapin(pd, section);
	} else {
		PANIC();
	}
	arch_tlbi_vmalle1is();
	return (int)iss;
}

void init_sections(ListNode* section_head) {
	struct section* heap = kalloc(sizeof(struct section));
	heap->flags = ST_HEAP;
	init_sleeplock(&(heap->sleeplock));
	heap->begin = 0x0;
	heap->end = 0x0;
	init_list_node(&(heap->stnode));
	_insert_into_list(section_head, &(heap->stnode));
}

void free_sections(struct pgdir* pd) {
	auto section_node = pd->section_head.next;
	while (section_node != &(pd->section_head)) {
		auto section = container_of(section_node, struct section, stnode);
		if (section->flags & ST_SWAP) {
			swapin(pd, section);
		}
		for (u64 i = PAGE_BASE(section->begin); i < section->end; i += PAGE_SIZE) {
			auto pte_ptr = get_pte(pd, i, false);
			if (pte_ptr == NULL) {
				continue;
			}
			if (*pte_ptr & PTE_VALID) {
				u64 ka = P2K(PTE_ADDRESS(*pte_ptr)); 
				kfree_page((void*)ka);
			}
		}
		section_node = section_node->next;
		_detach_from_list(&(section->stnode));
		kfree((void*)section);
	}
}

void copy_sections(ListNode* from_head, ListNode* to_head) {
	ListNode* a = from_head->next, *b = to_head;
	while (a != from_head) {
		// init
		auto st = container_of(a, struct section, stnode);
		struct section* new_st = kalloc(sizeof(struct section));
		new_st->flags = st->flags;
		init_sleeplock(&(st->sleeplock));
		new_st->begin = st->begin;
		new_st->end = st->end;
		if (st->fp) {
			new_st->fp = filedup(st->fp);
		}
		new_st->offset = st->offset;
		new_st->length = st->length;
		// add to list
		_insert_into_list(b, &(new_st->stnode));
		a = a->next;
		b = b->next;
	}
}