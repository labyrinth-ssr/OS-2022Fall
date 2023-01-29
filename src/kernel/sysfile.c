//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "fs/cache.h"
#include "fs/defines.h"
#include "syscall.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fcntl.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <sys/syscall.h>

struct iovec {
  void *iov_base; /* Starting address. */
  usize iov_len;  /* Number of bytes to transfer. */
};

// get the file object by fd
// return null if the fd is invalid
static struct file *fd2file(int fd) {
  // TODO
  struct file *f;
  if (fd < 0 || fd >= NOFILE || (f = thisproc()->oftable.ofile[fd]) == 0)
    return NULL;
  return thisproc()->oftable.ofile[fd];
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f) {
  /* TODO: Lab10 Shell */
  int fd;
  struct proc *p = thisproc();
  for (fd = 0; fd < NOFILE; fd++) {
    if (p->oftable.ofile[fd] == 0) {
      p->oftable.ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

define_syscall(ioctl, int fd, u64 request) {
  ASSERT(request == 0x5413);
  (void)fd;
  return 0;
}

/*
 *	map addr to a file
 */
// define_syscall(mmap, void *addr, int length, int prot, int flags, int fd,
//                int offset) {
//   // TODO
//   (void)
// }

// define_syscall(munmap, void *addr, size_t length) {
//   // TODO
// }

/*
 * Get the parameters and call filedup.
 */
define_syscall(dup, int fd) {
  struct file *f = fd2file(fd);
  if (!f)
    return -1;
  /* int  */ fd = fdalloc(f);
  if (fd < 0)
    return -1;
  filedup(f);
  return fd;
}

/*
 * Get the parameters and call fileread.
 */
define_syscall(read, int fd, char *buffer, int size) {
  struct file *f = fd2file(fd);
  if (!f || size <= 0 || !user_writeable(buffer, size))
    return -1;
  return fileread(f, buffer, size);
}

/*
 * Get the parameters and call filewrite.
 */
define_syscall(write, int fd, char *buffer, int size) {
  // printk("sys write\n");
  struct file *f = fd2file(fd);
  if (!f || size <= 0 || !user_readable(buffer, size))
    return -1;
  return filewrite(f, buffer, size);
}

define_syscall(writev, int fd, struct iovec *iov, int iovcnt) {
  // printk("sys writev\n");

  struct file *f = fd2file(fd);
  struct iovec *p;
  if (!f || iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt))
    return -1;
  usize tot = 0;
  for (p = iov; p < iov + iovcnt; p++) {
    if (!user_readable(p->iov_base, p->iov_len))
      return -1;
    tot += filewrite(f, p->iov_base, p->iov_len);
  }
  return tot;
}

/*
 * Get the parameters and call fileclose.
 * Clear this fd of this process.
 */
define_syscall(close, int fd) {
  /* TODO: Lab10 Shell */
  struct file *f = fd2file(fd);
  thisproc()->oftable.ofile[fd] = 0;
  fileclose(f);
  return 0;
}
/*
 * Get the parameters and call filestat.
 */
define_syscall(fstat, int fd, struct stat *st) {
  struct file *f = fd2file(fd);
  if (!f || !user_writeable(st, sizeof(*st)))
    return -1;
  return filestat(f, st);
}

define_syscall(newfstatat, int dirfd, const char *path, struct stat *st,
               int flags) {
  if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st)))
    return -1;
  if (dirfd != AT_FDCWD) {
    printk("sys_fstatat: dirfd unimplemented\n");
    return -1;
  }
  if (flags != 0) {
    printk("sys_fstatat: flags unimplemented\n");
    return -1;
  }

  Inode *ip;
  OpContext ctx;
  bcache.begin_op(&ctx);
  if ((ip = namei(path, &ctx)) == 0) {
    bcache.end_op(&ctx);
    return -1;
  }
  inodes.lock(ip);
  stati(ip, st);
  inodes.unlock(ip);
  inodes.put(&ctx, ip);
  bcache.end_op(&ctx);

  return 0;
}

// Is the directory dp empty except for "." and ".." ?
static int isdirempty(Inode *dp) {
  usize off;
  DirEntry de;

  for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
    if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
      PANIC();
    if (de.inode_no != 0)
      return 0;
  }
  return 1;
}

define_syscall(unlinkat, int fd, const char *path, int flag) {
  ASSERT(fd == AT_FDCWD && flag == 0);
  Inode *ip, *dp;
  DirEntry de;
  char name[FILE_NAME_MAX_LENGTH];
  usize off;
  if (!user_strlen(path, 256))
    return -1;
  OpContext ctx;
  bcache.begin_op(&ctx);
  if ((dp = nameiparent(path, name, &ctx)) == 0) {
    bcache.end_op(&ctx);
    return -1;
  }

  inodes.lock(dp);

  // Cannot unlink "." or "..".
  if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
      strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
    goto bad;

  usize inumber = inodes.lookup(dp, name, &off);
  if (inumber == 0)
    goto bad;
  ip = inodes.get(inumber);
  inodes.lock(ip);

  if (ip->entry.num_links < 1)
    PANIC();
  if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (inodes.write(&ctx, dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
    PANIC();
  if (ip->entry.type == INODE_DIRECTORY) {
    dp->entry.num_links--;
    inodes.sync(&ctx, dp, true);
  }
  inodes.unlock(dp);
  inodes.put(&ctx, dp);
  ip->entry.num_links--;
  inodes.sync(&ctx, ip, true);
  inodes.unlock(ip);
  inodes.put(&ctx, ip);
  bcache.end_op(&ctx);
  return 0;

bad:
  inodes.unlock(dp);
  inodes.put(&ctx, dp);
  bcache.end_op(&ctx);
  return -1;
}

void iunlockput(OpContext *ctx, Inode *ip) {
  inodes.unlock(ip);
  inodes.put(ctx, ip);
}
/*
 * Create an inode.
 *
 * Example:
 * Path is "/foo/bar/bar1", type is normal file.
 * You should get the inode of "/foo/bar", and
 * create an inode named "bar1" in this directory.
 *
 * If type is directory, you should additionally handle "." and "..".
 */
Inode *create(const char *path, short type, short major, short minor,
              OpContext *ctx) {
  /* TODO: Lab10 Shell */
  Inode *ip, *dp;
  usize ino;
  char name[FILE_NAME_MAX_LENGTH];

  if ((dp = nameiparent(path, name, ctx)) == 0)
    return 0;

  inodes.lock(dp);

  // what is the entry？
  if ((ino = inodes.lookup(dp, name, 0)) != 0) {
    ip = inodes.get(ino);
    iunlockput(ctx, dp);
    inodes.lock(ip);
    if (type == INODE_REGULAR &&
        (ip->entry.type == INODE_REGULAR || ip->entry.type == INODE_DEVICE))
      return ip;
    iunlockput(ctx, ip);
    return 0;
  }

  ip = inodes.get(inodes.alloc(ctx, type));
  if (ip == 0) {
    panic("alloc fail");
  }

  inodes.lock(ip);
  ip->entry.major = major;
  ip->entry.minor = minor;
  ip->entry.num_links = 1;
  ip->valid = true;
  inodes.sync(ctx, ip, true);

  if (type == INODE_DIRECTORY) { // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    dp->entry.num_links++;
    inodes.sync(ctx, dp, true);
    inodes.insert(ctx, ip, ".", ip->inode_no);
    inodes.insert(ctx, ip, "..", dp->inode_no);
    // if (< 0 || < 0)
    //   goto fail;
  }
  inodes.insert(ctx, dp, name, ip->inode_no);
  // if (< 0) goto fail;

  if (type == INODE_DIRECTORY) {
    // now that success is guaranteed:
    dp->entry.num_links++; // for ".."
    inodes.sync(ctx, dp, true);
  }

  iunlockput(ctx, dp);

  return ip;

  // fail:
  //   // something went wrong. de-allocate ip.
  //   ip->entry.num_links = 0;
  //   inodes.sync(ctx, ip, true);
  //   iunlockput(ctx, ip);
  //   iunlockput(ctx, dp);
  //   return 0;
}

define_syscall(openat, int dirfd, const char *path, int omode) {
  int fd;
  struct file *f;
  Inode *ip;

  if (!user_strlen(path, 256))
    return -1;

  if (dirfd != AT_FDCWD) {
    printk("sys_openat: dirfd unimplemented\n");
    return -1;
  }

  OpContext ctx;
  bcache.begin_op(&ctx);
  if (omode & O_CREAT) {
    // FIXME: Support acl mode.
    ip = create(path, INODE_REGULAR, 0, 0, &ctx);
    if (ip == 0) {
      bcache.end_op(&ctx);
      return -1;
    }
  } else {
    if ((ip = namei(path, &ctx)) == 0) {
      bcache.end_op(&ctx);
      return -1;
    }
    inodes.lock(ip);
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return -1;
  }
  inodes.unlock(ip);
  bcache.end_op(&ctx);

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

define_syscall(mkdirat, int dirfd, const char *path, int mode) {
  Inode *ip;
  if (!user_strlen(path, 256))
    return -1;
  if (dirfd != AT_FDCWD) {
    printk("sys_mkdirat: dirfd unimplemented\n");
    return -1;
  }
  if (mode != 0) {
    printk("sys_mkdirat: mode unimplemented\n");
    return -1;
  }
  OpContext ctx;
  bcache.begin_op(&ctx);
  if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
    bcache.end_op(&ctx);
    return -1;
  }
  inodes.unlock(ip);
  inodes.put(&ctx, ip);
  bcache.end_op(&ctx);
  return 0;
}

define_syscall(mknodat, int dirfd, const char *path, int major, int minor) {
  Inode *ip;
  if (!user_strlen(path, 256))
    return -1;
  if (dirfd != AT_FDCWD) {
    printk("sys_mknodat: dirfd unimplemented\n");
    return -1;
  }
  printk("mknodat: path '%s', major:minor %d:%d\n", path, major, minor);
  OpContext ctx;
  bcache.begin_op(&ctx);
  if ((ip = create(path, INODE_DEVICE, major, minor, &ctx)) == 0) {
    bcache.end_op(&ctx);
    return -1;
  }
  inodes.unlock(ip);
  inodes.put(&ctx, ip);
  bcache.end_op(&ctx);
  return 0;
}

define_syscall(chdir, const char *path) {
  // TODO
  // change the cwd (current working dictionary) of current process to 'path'
  // you may need to do some validations
  Inode *ip;
  struct proc *p = thisproc();
  OpContext ctx;
  bcache.begin_op(&ctx);
  if (path == NULL || (ip = namei(path, &ctx)) == 0) {
    bcache.end_op(&ctx);
    return -1;
  }
  inodes.lock(ip);
  if (ip->entry.type != INODE_DIRECTORY) {
    iunlockput(&ctx, ip);
    bcache.end_op(&ctx);
    return -1;
  }
  inodes.unlock(ip);
  inodes.put(&ctx, p->cwd);
  bcache.end_op(&ctx);
  p->cwd = ip;
  return 0;
}

// define_syscall(pipe2, int *fd, int flags) {
// TODO
//   uint64 fdarray; // user pointer to array of two integers
// struct file *rf, *wf;
// int fd0, fd1;
// struct proc *p = thisproc();

// if (pipeAlloc(&rf, &wf) < 0)
//   return -1;
// fd0 = -1;
// if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
//   if (fd0 >= 0)
//     p->ofile[fd0] = 0;
//   fileclose(rf);
//   fileclose(wf);
//   return -1;
// }
// if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
//     copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1))
//     <
//         0) {
//   p->ofile[fd0] = 0;
//   p->ofile[fd1] = 0;
//   fileclose(rf);
//   fileclose(wf);
//   return -1;
// }
// fd[0] = fd0;
// fd[1] = fd1;
//   return 0;
// }
