//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fcntl.h>

#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <sys/syscall.h>
#include <kernel/mem.h>
#include "syscall.h"
#include <fs/pipe.h>
#include <common/string.h>
#include <fs/inode.h>
#include <kernel/printk.h>

struct iovec {
    void* iov_base; /* Starting address. */
    usize iov_len; /* Number of bytes to transfer. */
};


// get the file object by fd
// return null if the fd is invalid
static struct file* fd2file(int fd) {
    // TODO
    if (fd < 0 || fd >= NOFILE) {
        return NULL;
    }
    return thisproc()->oftable.otable[fd];
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file* f) {
    /* TODO: Lab10 Shell */
    auto this = thisproc();
    for (int i = 0; i < NOFILE; ++i) {
        if (this->oftable.otable[i] == NULL) {
            this->oftable.otable[i] = f;
            return i;
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
define_syscall(mmap, void* addr, int length, int prot, int flags, int fd, int offset) {
    // TODO
    addr = addr;
    length = length;
    prot = prot;
    flags = flags;
    fd = fd;
    offset = offset;
    return 0;
}

define_syscall(munmap, void *addr, u64 length) {
    // TODO
    addr = addr;
    length = length;
    return 0;
}

/*
 * Get the parameters and call filedup.
 */
define_syscall(dup, int fd) {
    struct file* f = fd2file(fd);
    if (!f)
        return -1;
    int new_fd = fdalloc(f);
    if (new_fd < 0)
        return -1;
    filedup(f);
    return new_fd;
}

/*
 * Get the parameters and call fileread.
 */
define_syscall(read, int fd, char* buffer, int size) {
    struct file* f = fd2file(fd);
    if (!f || size <= 0 || !user_writeable(buffer, size))
        return -1;
    return fileread(f, buffer, size);
}

/*
 * Get the parameters and call filewrite.
 */
define_syscall(write, int fd, char* buffer, int size) {
    struct file* f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size))
        return -1;
    return filewrite(f, buffer, size);
}

define_syscall(writev, int fd, struct iovec *iov, int iovcnt) {
    struct file* f = fd2file(fd);
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
    auto f = fd2file(fd);
    if (f == NULL) {
        return -1;
    }
    thisproc()->oftable.otable[fd] = NULL;
    fileclose(f);
    return 0;
}

/*
 * Get the parameters and call filestat.
 */
define_syscall(fstat, int fd, struct stat* st) {
    struct file* f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st)))
        return -1;
    return filestat(f, st);
}

define_syscall(newfstatat, int dirfd, const char* path, struct stat* st, int flags) {
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

    Inode* ip;
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
static int isdirempty(Inode* dp) {
    usize off;
    DirEntry de;

    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8*)&de, off, sizeof(de)) != sizeof(de))
            PANIC();
        if (de.inode_no != 0)
            return 0;
    }
    return 1;
}

define_syscall(unlinkat, int fd, const char* path, int flag) {
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
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0
        || strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
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
    if (inodes.write(&ctx, dp, (u8*)&de, off, sizeof(de)) != sizeof(de))
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
Inode* create(const char* path, short type, short major, short minor, OpContext* ctx) {
    /* TODO: Lab10 Shell */
    char name[FILE_NAME_MAX_LENGTH] = {0};
    
    // get the parent inode
    auto ip_parent = nameiparent(path, name, ctx);
    if (ip_parent == NULL) {
        return NULL;
    }

    // lookup name
    inodes.lock(ip_parent);
    u32 ip_no = inodes.lookup(ip_parent, name, NULL);
    if (ip_no != 0) {
        // if find, return 
        auto ip = inodes.get(ip_no);
        inodes.unlock(ip_parent);
        inodes.put(ctx, ip_parent);
        inodes.lock(ip);
        if (ip->entry.type == INODE_REGULAR && type == INODE_REGULAR) {     
            return ip;
        }
        inodes.unlock(ip);
        inodes.put(ctx, ip);
        return NULL;
    }
    // else create
    ip_no = inodes.alloc(ctx, type);
    auto ip = inodes.get(ip_no);
    
    // insert "." and ".."
    if (type == INODE_DIRECTORY) {
        ip_parent->entry.num_links++;
        inodes.sync(ctx, ip_parent, TRUE);
        // insert ".." and '.'
        inodes.insert(ctx, ip, "..", ip_parent->inode_no);
        inodes.insert(ctx, ip, ".", ip->inode_no);
    }

    // set
    inodes.lock(ip);
    ip->entry.major = major;
    ip->entry.minor = minor;
    ip->entry.type = type;
    ip->entry.num_links = 1;
    inodes.sync(ctx, ip, TRUE);
    inodes.insert(ctx, ip_parent, name, ip_no);

    inodes.unlock(ip_parent);
    inodes.put(ctx, ip_parent);
    return ip;
}

define_syscall(openat, int dirfd, const char* path, int omode) {
    int fd;
    struct file* f;
    Inode* ip;

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

define_syscall(mkdirat, int dirfd, const char* path, int mode) {
    Inode* ip;
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

define_syscall(mknodat, int dirfd, const char* path, int major, int minor) {
    Inode* ip;
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

define_syscall(chdir, const char* path) {
    // TODO
    // change the cwd (current working dictionary) of current process to 'path'
    // you may need to do some validations
    OpContext ctx;
    bcache.begin_op(&ctx);
    auto cwd = namei(path, &ctx);
    // if not find
    if (cwd == NULL) {
        bcache.end_op(&ctx);
        return -1;
    }

    // if not a valid directory
    inodes.lock(cwd);
    if (cwd->entry.type != INODE_DIRECTORY) {
        inodes.unlock(cwd);
        inodes.put(&ctx, cwd);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(cwd);
    inodes.put(&ctx, thisproc()->cwd);
    thisproc()->cwd = cwd;
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(pipe2, int *fd, int flags) {
    // TODO
    flags = flags;
    File* f0, *f1;
    if (pipeAlloc(&f0, &f1) == 1) {
        fd[0] = fdalloc(f0);
        fd[1] = fdalloc(f1);
        // if alloc failed
        if (fd[0] == -1 || fd[1] == -1) {
            if (fd[0] != -1) {
                thisproc()->oftable.otable[fd[0]] = NULL;
            }
            fileclose(f0);
            fileclose(f1);
            return -1;
        }
        return 0;
    } else {
        return -1;
    }
}
