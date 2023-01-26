#include "aarch64/intrinsic.h"
#include "aarch64/mmu.h"
#include "fs/fs.h"
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
  printk("loaduvm:size:%d\n", sz);
  int n;
  u64 pa, va0;
  while (sz > 0) {
    va0 = PAGE_BASE(va);
    pa = (u64)uva2ka(pgdir, va0);
    if (pa == 0)
      panic("addr not exist");
    n = MIN(PAGE_SIZE - (va - va0), sz);
    printk("inode read:dest:%p,offset:%llx,size:%d", (u8 *)(pa + (va - va0)),
           (u64)offset, n);
    if (inodes.read(ip, (u8 *)(pa + (va - va0)), offset, (usize)n) != (usize)n)
      return -1;
    offset += n;
    sz -= n;
    va += n;
  }
  return 0;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
  // TODO
  printk("in execve\n");
  u64 argc, sz = 0, sp, ustack[7], stackbase;

  // init_filesystem();
  (void)envp;
  OpContext ctx;
  bcache.begin_op(&ctx);
  printk("path:%s\n", path);
  Inode *ip = namei(path, &ctx);
  if (!ip)
    return -1;
  inodes.lock(ip);
  printk("ip:%p\n", ip);

  Elf64_Ehdr elf;
  struct proc *p = thisproc();
  struct pgdir *pgdir = kalloc(sizeof(struct pgdir));
  init_pgdir(pgdir);

  // &thisproc()->pgdir;
  if (inodes.read(ip, (u8 *)(&elf), 0, sizeof(elf)) < sizeof(elf)) {
    goto bad;
  }
  if (strncmp((const char *)elf.e_ident, ELFMAG, 4)) {
    panic("bad magic");
  }

  Elf64_Phdr ph;
  printk("elf.e_phoff:%lld,elf.e_phnum:%lld\n", (u64)elf.e_phoff,
         (u64)elf.e_phnum);
  for (u64 i = 0, off = elf.e_phoff; i < elf.e_phnum; i++, off += sizeof(ph)) {
    if ((inodes.read(ip, (u8 *)&ph, off, sizeof(ph)) != sizeof(ph)))
      goto bad;
    sz = ph.p_vaddr;
    if (ph.p_type != PT_LOAD)
      continue;
    if (ph.p_memsz < ph.p_filesz)
      goto bad;
    printk("vaddr:%llx\n", (u64)ph.p_vaddr);
    printk("sz:%llx\n", (u64)ph.p_memsz);
    sz = (u64)uvm_alloc(pgdir, sz, sz + ph.p_memsz);
    printk("offset:%llx", (u64)ph.p_offset);
    if (loaduvm(pgdir, (u64)ph.p_vaddr, ip, (u32)ph.p_offset,
                (u32)ph.p_filesz) < 0) {
      goto bad;
    }
  }
  printk("done\n");
  inodes.unlock(ip);
  inodes.put(&ctx, ip);
  bcache.end_op(&ctx);
  ip = 0;

  p = thisproc();
  sz = ROUNDUP(sz, PAGE_SIZE) + PAGE_SIZE;
  u64 sz1;
  if ((sz1 = uvm_alloc(pgdir, sz, sz + 2 * PAGE_SIZE)) == 0)
    goto bad;
  // clearpteu(pgdir, (char *)(sz - (PAGE_SIZE << 1)));
  sz = sz1;
  uvmclear(pgdir, sz - 2 * PAGE_SIZE);
  sp = sz;
  stackbase = sp - PAGE_SIZE;

  argc = 0;
  if (argv) {
    for (; argv[argc]; argc++) {
      if (argc > 6)
        goto bad;
      sp -= strlen(argv[argc]) + 1;
      sp = ROUNDDOWN(sp, 16);
      if (sp < stackbase)
        goto bad;
      if (copyout(pgdir, (void *)sp, argv[argc], strlen(argv[argc]) + 1) < 0) {
        goto bad;
      }
      ustack[argc] = sp;
    }
  }
  ustack[argc] = 0;

  sp -= (argc + 1) * sizeof(u64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pgdir, (void *)sp, (char *)ustack, (argc + 1) * sizeof(u64)) < 0)
    goto bad;

  p->ucontext->x[0] = (u64)argc;
  p->ucontext->x[1] = sp;

  sp -= 8;
  u64 tmp = 0;
  if (copyout(pgdir, (void *)sp, &tmp, 8) < 0)
    goto bad;
  sp -= 8;
  if (copyout(pgdir, (void *)sp, &argc, 8) < 0)
    goto bad;

  // struct pgdir *oldpgdir = &p->pgdir;
  // strncpy(p->name, path, strlen(path) + 1);
  // p->sz = sz;
  printk("%p", p->pgdir.pt);
  auto old_pt = p->pgdir.pt;
  p->pgdir.pt = pgdir->pt;
  kfree_page(old_pt);
  p->ucontext->sp = sp;
  p->ucontext->elr = elf.e_entry;
  p->sz = sz;

  attach_pgdir(&p->pgdir);
  arch_tlbi_vmalle1is();

  printk("entry:%p\n", (void *)elf.e_entry);
  printk("pte:%llx\n", *get_pte(pgdir, elf.e_entry, 0));
  printk("data:%x\n", *(int *)(0x40014c));
  // free_pgdir(oldpgdir);
  // 0x407000

  return 0;

bad:
  printk("bad\n");
  PANIC();
  if (pgdir)
    free_pgdir(pgdir);
  if (ip) {
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
  }
  return -1;
}
