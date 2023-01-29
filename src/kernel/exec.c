#include <elf.h>
#include <common/string.h>
#include <common/defines.h>
#include <kernel/console.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/pt.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <aarch64/trap.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <kernel/printk.h>

//static u64 auxv[][2] = {{AT_PAGESZ, PAGE_SIZE}};
extern int fdalloc(struct file* f);

int execve(const char *path, char *const argv[], char *const envp[]) {
	// TODO
	auto this = thisproc();
	struct pgdir pgdir = {0};
	// open file
	OpContext ctx;
	bcache.begin_op(&ctx);
	Inode* ip = namei(path, &ctx);
	if (ip == NULL) {
		goto bad;
	}

	// Step1: read header
	inodes.lock(ip);
	Elf64_Ehdr header;
	if (inodes.read(ip, (u8*)(&header), 0, sizeof(Elf64_Ehdr)) < sizeof(Elf64_Ehdr)) {
		goto bad;
	}
	if (strncmp((const char*)header.e_ident, ELFMAG, strlen(ELFMAG)) != 0) {
		// check magic number
		goto bad;
	}

	// Step2: read program header
	Elf64_Phdr p_header;
	init_pgdir(&pgdir);
	u64 sp = 0;
	for (Elf64_Half i = 0, offset = header.e_phoff; i < header.e_phnum; offset += sizeof(Elf64_Phdr), ++i) {
		if (inodes.read(ip, (u8*)(&p_header), offset, sizeof(Elf64_Phdr)) < sizeof(Elf64_Phdr)) {
			goto bad;
		}
		if (p_header.p_type == PT_LOAD) { // load and create a section
			// set something
			struct section* st = kalloc(sizeof(struct section));
			memset(st, 0, sizeof(struct section));
			if (p_header.p_flags == (PF_R | PF_X)) {
				st->flags = (ST_FILE | ST_RO);
			} else if (p_header.p_flags == (PF_R | PF_W)) {
				st->flags = (ST_FILE);
			} else {
				kfree(st);
				continue;
			}
			init_sleeplock(&(st->sleeplock));
			st->begin = p_header.p_vaddr;
			st->end = p_header.p_vaddr + p_header.p_memsz;
			sp = MAX(sp, st->end);
			init_list_node(&(st->stnode));
			_insert_into_list(&(pgdir.section_head), &(st->stnode));

			// load 
			u64 va = p_header.p_vaddr, va_end = p_header.p_vaddr + p_header.p_filesz;
			while (va < va_end) {
				void* ka = kalloc_page();
				memset(ka, 0, PAGE_SIZE);
				u64 count = MIN(va_end - va, (u64)PAGE_SIZE - (va - PAGE_BASE(va)));
				// printk("%llx, %llx\n", va, count);
				if (inodes.read(ip, (u8*)((u64)ka + (va - PAGE_BASE(va))), p_header.p_offset + (va - p_header.p_vaddr), count) < count) {
					goto bad;
				}
				// printk("%lld\n", *(u64*)(ka + 0x300));
				vmmap(&pgdir, PAGE_BASE(va), ka, (st->flags & ST_RO) ? PTE_USER_DATA | PTE_RO : PTE_USER_DATA);
				va += count;
			}
			// filesz ~ memsz is BSS section
			// COW
			if (va % PAGE_SIZE) {
				va = PAGE_BASE(va) + PAGE_SIZE;
			}
			for (; va < p_header.p_vaddr + p_header.p_memsz; va += PAGE_SIZE) {
				vmmap(&pgdir, va, get_zero_page(), PTE_USER_DATA);
			}
		} else {
			continue;
		}
	}
	inodes.unlock(ip);
	inodes.put(&ctx, ip);
	bcache.end_op(&ctx);

	// Step3: Allocate and initialize user stack.
	// init section
	sp = PAGE_BASE(sp) + PAGE_SIZE;
	u64 sp_top = sp + STACK_SIZE;
	struct section* st = kalloc(sizeof(struct section));
	memset(st, 0, sizeof(struct section));
	st->flags = ST_STACK;
	init_sleeplock(&(st->sleeplock));
	st->begin = sp;
	st->end = sp_top;
	init_list_node(&(st->stnode));
	_insert_into_list(&(pgdir.section_head), &(st->stnode));
	// vmmap
	for (; sp < sp_top; sp += PAGE_SIZE) {
		void* ka = kalloc_page();
		memset(ka, 0, PAGE_SIZE);
		vmmap(&pgdir, sp, ka, PTE_USER_DATA);
	}

	// left some space
	sp -= 128;
	
	// push envp
	int envc = 0;
	char* new_envp[32] = {0};
	if (envp) {
		for (; envp[envc]; envc++) {

		}
		for (int i = envc - 1; i >= 0; --i) {
			sp -= strlen(envp[i]) + 1;
			// sp = sp & (~15);
			ASSERT(copyout(&pgdir, (void*)sp, envp[i], strlen(envp[i]) + 1) == 0);
			new_envp[i] = (char*)sp;
		}
	}

	// push argv
	sp -= 16;
	int argc = 0;
	char* argp[32] = {0};
	if (argv) {
		for (; argv[argc]; argc++) {

		}
		for (int i = argc - 1; i >= 0; --i) {
			sp -= strlen(argv[i]) + 1;
			// sp = sp & (~15);
			ASSERT(copyout(&pgdir, (void*)sp, argv[i], strlen(argv[i]) + 1) == 0);
			argp[i] = (char*)sp;
		}
	}
	
	sp = sp & (~15);
	// push argv's addr and envp's addr 
	sp -= (envc + 1) * sizeof(char*);
	ASSERT(copyout(&pgdir, (void*)sp, &new_envp, (envc + 1) * sizeof(char*)) == 0);
	sp -= (argc + 1) * sizeof(char*);
	ASSERT(copyout(&pgdir, (void*)sp, &argp, (argc + 1) * sizeof(char*)) == 0);

	// push argc
	sp -= 8;
	ASSERT(copyout(&pgdir, (void*)sp, &argc, sizeof(int)) == 0);

	// set trapframe
	this->ucontext->sp = sp;
	this->ucontext->elr = header.e_entry;
	
	// change pgdir
	free_pgdir(&(this->pgdir));
	this->pgdir = pgdir;
	init_list_node(&(this->pgdir.section_head));
	_insert_into_list(&(pgdir.section_head), &(this->pgdir.section_head));
	_detach_from_list(&(pgdir.section_head));
	attach_pgdir(&(this->pgdir));
	
	// auto st_node = this->pgdir.section_head.next;
	// while (st_node != &(this->pgdir.section_head)) {
	// 	auto section = container_of(st_node, struct section, stnode);
	// 	printk("%llx, %llx\n", section->begin, section->end);
	// 	st_node = st_node->next;
	// }

	return 0;

bad:
	printk("ERROR ELF!!!\n");
	if (pgdir.pt != NULL) {
        free_pgdir(&pgdir);
	}
    if (ip != NULL) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
	}
	bcache.end_op(&ctx);
	i64 xxx = (i64)envp;
	xxx = -1;
	return xxx;
}