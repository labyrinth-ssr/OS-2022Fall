/* File descriptors */

#include "file.h"
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/inode.h>
#include <common/list.h>
#include <kernel/mem.h>
#include <fs/pipe.h>
#include "fs.h"

static struct ftable ftable;

void init_ftable() {
    // TODO: initialize your ftable
    init_spinlock(&(ftable.lock));
}

void init_oftable(struct oftable *oftable) {
    // TODO: initialize your oftable for a new process
    memset(oftable, 0, sizeof(struct oftable));
}

/* Allocate a file structure. */
struct file* filealloc() {
    /* TODO: Lab10 Shell */
    _acquire_spinlock(&(ftable.lock));
    for (int i = 0; i < NFILE; ++i) {
        if (ftable.table[i].ref == 0) {
            ftable.table[i].ref = 1;
            _release_spinlock(&(ftable.lock));
            return &(ftable.table[i]);
        }
    }
    _release_spinlock(&(ftable.lock));
    return 0;
}

/* Increment ref count for file f. */
struct file* filedup(struct file* f) {
    /* TODO: Lab10 Shell */
    _acquire_spinlock(&(ftable.lock));
    f->ref++;
    _release_spinlock(&(ftable.lock));
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void fileclose(struct file* f) {
    /* TODO: Lab10 Shell */
    _acquire_spinlock(&(ftable.lock));
    f->ref--;
    if (f->ref == 0) {
        File f_close = *f;
        memset(f, 0, sizeof(File));
        _release_spinlock(&(ftable.lock)); // avoid to spin ...
        if (f_close.type == FD_INODE) {
            OpContext ctx;
            bcache.begin_op(&ctx);
            inodes.put(&ctx, f_close.ip);
            bcache.end_op(&ctx);
        }
        if (f_close.type == FD_PIPE) {
            pipeClose(f_close.pipe, f_close.writable);
        }
    } else {
        _release_spinlock(&(ftable.lock));
    }
}

/* Get metadata about file f. */
int filestat(struct file* f, struct stat* st) {
    /* TODO: Lab10 Shell */
    if (f->type == FD_INODE) {
        inodes.lock(f->ip);
        stati(f->ip, st);
        inodes.unlock(f->ip);
        return 0;
    }
    return -1;
}

/* Read from file f. */
isize fileread(struct file* f, char* addr, isize n) {
    /* TODO: Lab10 Shell */
    if (f->readable == 0) {
        return -1;
    }
    if (f->type == FD_NONE) {
        return -1;
    }

    if (f->type == FD_PIPE) {
        return (isize)pipeRead(f->pipe, (u64)addr, n);
    }

    // Inode
    inodes.lock(f->ip);
    usize count = inodes.read(f->ip, (u8*)addr, f->off, n);
    f->off += count;
    inodes.unlock(f->ip);
    return count;
}

/* Write to file f. */
isize filewrite(struct file* f, char* addr, isize n) {
    /* TODO: Lab10 Shell */
    if (f->writable == 0) {
        return -1;
    }
    if (f->type == FD_NONE) {
        return -1;
    }

    if (f->type == FD_PIPE) {
        return (isize)pipeWrite(f->pipe, (u64)addr, n);
    }
    
    // Inode
    isize n1, i = 0, maximum = (OP_MAX_NUM_BLOCKS - 1 - 1 - 2) / 2 * BLOCK_SIZE;
    while (i < n) {
        n1 = n - i;
        if (n1 > maximum) {
            // from xv6:
            // write a few blocks at a time to avoid exceeding
            // the maximum log transaction size, including
            // i-node, indirect block, allocation blocks,
            // and 2 blocks of slop for non-aligned writes.
            // this really belongs lower down, since writei()
            // might be writing a device like the console.
            n1 = maximum;
        }

        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.lock(f->ip);
        usize count = inodes.write(&ctx, f->ip, (u8*)(addr + i), f->off, n1);
        f->off += count;
        inodes.unlock(f->ip);
        bcache.end_op(&ctx);
        i += count;
    }
    return n;
}
