#include "aarch64/mmu.h"
#include "kernel/printk.h"
#include <aarch64/trap.h>
#include <common/defines.h>
#include <common/string.h>
#include <elf.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <kernel/console.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

// static u64 auxv[][2] = {{AT_PAGESZ, PAGE_SIZE}};
extern int fdalloc(struct file *f);

static int loaduvm(struct pgdir *pgdir, u64 va, Inode *ip, u32 offset, u32 sz) {
  printk("loaduvm:size:%d", sz);
  u64 n;
  u64 pa, i;
  for (i = 0; i < sz; i += PAGE_SIZE) {
    pa = uva2ka(pgdir, va + i);
    if (pa == 0)
      panic("loadseg: address should exist");
    if (sz - i < PAGE_SIZE)
      n = sz - i;
    else
      n = PAGE_SIZE;
    if (inodes.read(ip, (u8 *)pa, offset + i, n) != n)
      return -1;
  }
  return 0;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
  // TODO
  (void)envp;
  OpContext ctx;
  bcache.begin_op(&ctx);
  Inode *ip = namei(path, &ctx);
  if (!ip)
    return -1;
  inodes.lock(ip);
  Elf64_Ehdr elf;
  struct pgdir *pgdir = &thisproc()->pgdir;
  if (inodes.read(ip, (u8 *)(&elf), 0, sizeof(elf)) < sizeof(elf)) {
    goto bad;
  }
  if (strncmp((const char *)elf.e_ident, ELFMAG, 4)) {
    panic("bad magic");
  }
  init_pgdir(pgdir);
  u64 sz = 0;
  Elf64_Phdr ph;
  for (u64 i = 0, off = elf.e_phoff; i < elf.e_phnum; i++, off += sizeof(ph)) {
    if ((inodes.read(ip, (u8 *)&ph, off, sizeof(ph)) != sizeof(ph)))
      goto bad;
    if (ph.p_type != PT_LOAD)
      continue;
    if (ph.p_memsz < ph.p_filesz)
      goto bad;
    // size is fixed to one page
    // sz = (u64)uvm_alloc(pgdir, 0, 8192, sz, ph.p_vaddr + ph.p_memsz);
    // FIXME:loaduvm
    if (loaduvm(pgdir, (u64)ph.p_vaddr, ip, (u32)ph.p_offset,
                (u32)ph.p_filesz) < 0) {
      goto bad;
    }
  }
  inodes.unlock(ip);
  inodes.put(&ctx, ip);
  bcache.end_op(&ctx);

  // ip = 0;
  // sz = ROUNDUP(sz, PAGE_SIZE);
  // sz = (u64)uvm_alloc(pgdir, 0, 8192, sz, sz + (PAGE_SIZE << 1));
  // if (!sz)
  //   goto bad;
  // clearpteu(pgdir, (char *)(sz - (PAGE_SIZE << 1)));

  u64 sp = sz;
  // printf("sp%x\n", sp);
  int argc = 0;
  u64 ustk[3 + 32 + 1];
  if (argv) {
    for (; argv[argc]; argc++) {
      if (argc > 32)
        goto bad;
      sp -= strlen(argv[argc]) + 1;
      sp = ROUNDDOWN(sp, 16);
      if (copyout(pgdir, (void *)sp, argv[argc], strlen(argv[argc]) + 1) < 0) {
        goto bad;
      }
      ustk[argc] = sp;
    }
  }

  ustk[argc] = 0;
  thisproc()->ucontext->x[0] = (u64)argc;
  if ((argc & 1) == 0)
    sp -= 8;

  u64 auxv[] = {0, AT_PAGESZ, PAGE_SIZE, AT_NULL};
  sp -= sizeof(auxv);
  if (copyout(pgdir, (void *)sp, auxv, sizeof(auxv)) < 0)
    goto bad;

  sp -= 8;
  u64 tmp = 0;
  if (copyout(pgdir, (void *)sp, &tmp, 8) < 0)
    goto bad;

  sp = sp - (u64)(argc + 1) * 8;
  thisproc()->ucontext->x[1] = sp;
  if (copyout(pgdir, (void *)sp, ustk, ((u64)argc + 1) * 8) < 0)
    goto bad;

  sp -= 8;
  if (copyout(pgdir, (void *)sp, &argc, 8) < 0)
    goto bad;

  struct pgdir *oldpgdir = &thisproc()->pgdir;
  strncpy(thisproc()->name, path, strlen(path) + 1);
  // thisproc()->sz = sz;
  thisproc()->ucontext->sp = sp;
  thisproc()->ucontext->elr = elf.e_entry;
  attach_pgdir(pgdir);
  free_pgdir(oldpgdir);
  return 0;

bad:
  if (pgdir)
    free_pgdir(pgdir);
  if (ip) {
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
  }
  return -1;
}
