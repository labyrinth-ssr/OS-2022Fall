#include <common/string.h>
#include <fs/inode.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <sys/stat.h>
#include <common/sem.h>
#include <kernel/sched.h>
#include <kernel/console.h>

// this lock mainly prevents concurrent access to inode list `head`, reference
// count increment and decrement.
static SpinLock lock;
static ListNode head;

static const SuperBlock* sblock;
static const BlockCache* cache;

// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry* get_entry(Block* block, usize inode_no) {
    return ((InodeEntry*)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32* get_addrs(Block* block) {
    return ((IndirectBlock*)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock* _sblock, const BlockCache* _cache) {
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes)
        inodes.root = inodes.get(ROOT_INODE_NO);
    else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode* inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext* ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);
    // TODO
    for (u32 i = 1; i < sblock->num_inodes; ++i) {
        u32 block_no = to_block_no(i);
        Block* block = cache->acquire(block_no);
        auto entry_ptr = get_entry(block, i);
        if (entry_ptr->type == INODE_INVALID) {
            memset(entry_ptr, 0, sizeof(InodeEntry));
            entry_ptr->type = type;
            cache->sync(ctx, block);
            cache->release(block);
            return i;
        }
        cache->release(block);
    }
    PANIC();
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    unalertable_wait_sem(&(inode->lock));
}

// see `inode.h`.
static void inode_unlock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    post_sem(&(inode->lock));
}

// see `inode.h`.
static void inode_sync(OpContext* ctx, Inode* inode, bool do_write) {
    // TODO
    Block* block = cache->acquire(to_block_no(inode->inode_no));
    if (do_write && inode->valid) {
        InodeEntry* entry_ptr = get_entry(block, inode->inode_no);
        memcpy(entry_ptr, &(inode->entry), sizeof(InodeEntry));
        cache->sync(ctx, block);
    } else if (inode->valid == FALSE) {
        InodeEntry* entry_ptr = get_entry(block, inode->inode_no);
        memcpy(&(inode->entry), entry_ptr, sizeof(InodeEntry));
        inode->valid = TRUE;
    }
    cache->release(block);
}

// see `inode.h`.
static Inode* inode_get(usize inode_no) {
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    _acquire_spinlock(&lock);
    // TODO
    ListNode* p = head.next;
    Inode* inode;
    while (p != &head) {
        inode = container_of(p, Inode, node);
        if (inode->inode_no == inode_no && inode->rc.count > 0 && inode->valid) {
            _increment_rc(&(inode->rc));
            _release_spinlock(&lock);
            return inode;
        }
        p = p->next;
    }
    // if not find, get from disk
    // put it in empty
    inode = kalloc(sizeof(Inode));
    init_inode(inode);
    inode->inode_no = inode_no;
    _increment_rc(&(inode->rc));
    _insert_into_list(&head, &(inode->node));
    inode_lock(inode); // we can make sure to get the lock
    _release_spinlock(&lock);
    inode_sync(NULL, inode, FALSE);
    inode_unlock(inode);
    return inode;
}

// see `inode.h`.
static void inode_clear(OpContext* ctx, Inode* inode) {
    // TODO
    auto entry_ptr = &(inode->entry);
    for (int i = 0; i < INODE_NUM_DIRECT; ++i) {
        if (entry_ptr->addrs[i] != 0) {
            cache->free(ctx, entry_ptr->addrs[i]);
            entry_ptr->addrs[i] = 0;
        }
    }
    if (entry_ptr->indirect != 0) {
        Block* block = cache->acquire(entry_ptr->indirect);
        for (usize i = 0; i < INODE_NUM_INDIRECT; ++i) {
            u32* addr = (u32*)(block->data);
            if (addr[i] != 0) {
                cache->free(ctx, addr[i]);
                addr[i] = 0;
            }
        }
        cache->release(block);
        cache->free(ctx, entry_ptr->indirect);
        entry_ptr->indirect = 0;
    }
    entry_ptr->num_bytes = 0;
    inode_sync(ctx, inode, TRUE);
}

// see `inode.h`.
static Inode* inode_share(Inode* inode) {
    // TODO
    _acquire_spinlock(&lock);
    _increment_rc(&(inode->rc));
    _release_spinlock(&lock);
    return inode;
}

// see `inode.h`.
static void inode_put(OpContext* ctx, Inode* inode) {
    // TODO
    _acquire_spinlock(&lock);
    if (inode->rc.count == 1 && inode->entry.num_links == 0 && inode->valid) {
        inode_lock(inode); // we can make sure to get the lock
        inode->valid = FALSE;
        _release_spinlock(&lock);

        // clear disk
        inode_clear(ctx, inode);
        inode->entry.type = INODE_INVALID;
        inode_sync(ctx, inode, true);

        // free
        _acquire_spinlock(&lock);
        inode_unlock(inode);
        _detach_from_list(&(inode->node));
        _release_spinlock(&lock);
        kfree(inode);
        return;
    }
    _decrement_rc(&(inode->rc));
    _release_spinlock(&lock);
}

// this function is private to inode layer, because it can allocate block
// at arbitrary offset, which breaks the usual file abstraction.
//
// retrentry_ptrve the block in `inode` where offset lives. If the block is not
// allocated, `inode_map` will allocate a new block and update `inode`, at
// which time, `*modifentry_ptrd` will be set to true.
// the block number is returned.
//
// NOTE: caller must hold the lock of `inode`.
static usize inode_map(OpContext* ctx,
                       Inode* inode,
                       usize offset,
                       bool* modified) {
    // TODO
    ASSERT(offset < INODE_NUM_DIRECT + INODE_NUM_INDIRECT);
    InodeEntry* entry_ptr = &(inode->entry);
    // find in 'direct'
    if (offset < INODE_NUM_DIRECT) {
        if (entry_ptr->addrs[offset] == 0) {
            entry_ptr->addrs[offset] = cache->alloc(ctx);
            *modified = TRUE;
        }
        return entry_ptr->addrs[offset];
    }

    // find in 'indirect'
    offset -= INODE_NUM_DIRECT;
    if (entry_ptr->indirect == 0) { // alloc indrect block
        entry_ptr->indirect = cache->alloc(ctx);
    }

    Block* block = cache->acquire(entry_ptr->indirect);
    u32* addrs = get_addrs(block);
    if (addrs[offset] == 0) {
        addrs[offset] = cache->alloc(ctx);
        cache->sync(ctx, block);
        *modified = TRUE;
    } 
    cache->release(block);
    return addrs[offset];
}

// see `inode.h`.
static usize inode_read(Inode* inode, u8* dest, usize offset, usize count) {
    InodeEntry* entry = &inode->entry;
    if (inode->entry.type == INODE_DEVICE) {
        ASSERT(inode->entry.major == 1);
        return console_read(inode, (char*)dest, count);
    }
    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);
    
    // TODO
    bool modified = FALSE;
    u8* src;
    for (usize i = 0, cnt = 0; i < count; i += cnt, dest += cnt, offset += cnt) {
        usize block_no = inode_map(NULL, inode, offset / BLOCK_SIZE, &modified);
        Block* block = cache->acquire(block_no);
        if (i == 0) {
            cnt = MIN(BLOCK_SIZE - offset % (usize)BLOCK_SIZE, (count));
            src = block->data + offset % (usize)BLOCK_SIZE;
        } else {
            cnt = MIN((usize)BLOCK_SIZE, count - i);
            src = block->data;
        }
        memcpy(dest, src, cnt);
        cache->release(block);
    }
    
    return count;
}

// see `inode.h`.
static usize inode_write(OpContext* ctx,
                         Inode* inode,
                         u8* src,
                         usize offset,
                         usize count) {
    InodeEntry* entry = &inode->entry;
    if (entry->type == INODE_DEVICE) {
        ASSERT(inode->entry.major == 1);
        return console_write(inode, (char*)src, count);
    }
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    // TODO
    bool modified = FALSE;
    u8* dest;
    for (usize i = 0, cnt = 0; i < count; i += cnt, src += cnt, offset += cnt) {
        usize block_no = inode_map(ctx, inode, offset / BLOCK_SIZE, &modified);
        Block* block = cache->acquire(block_no);
        if (i == 0) {
            cnt = MIN(BLOCK_SIZE - offset % (usize)BLOCK_SIZE, (count));
            dest = block->data + offset % (usize)BLOCK_SIZE;
        } else {
            cnt = MIN((usize)BLOCK_SIZE, count - i);
            dest = block->data;
        }
        memcpy(dest, src, cnt);
        cache->sync(ctx, block);
        cache->release(block);
    }
    if (inode->entry.num_bytes < end) {
        inode->entry.num_bytes = end;
        inode_sync(ctx, inode, TRUE);
    }
    return count;
}

// see `inode.h`.
static usize inode_lookup(Inode* inode, const char* name, usize* index) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    for (usize i = 0; i < entry->num_bytes; i += sizeof(DirEntry)) {
        DirEntry dir_entry;
        inode_read(inode, (void*)&dir_entry, i, sizeof(DirEntry));
        if (dir_entry.inode_no != 0 && strncmp(dir_entry.name, name, FILE_NAME_MAX_LENGTH) == 0) {
            if (index != NULL) {
                *index = i;
            }
            return dir_entry.inode_no;
        }
    }
    return 0;
}

// see `inode.h`.
static usize inode_insert(OpContext* ctx,
                          Inode* inode,
                          const char* name,
                          usize inode_no) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    usize index = 0;
    //if not find
    if (inode_lookup(inode, name, &index) != 0) {
        return -1;
    }

    DirEntry dir_entry;
    // find the appropriate position
    for (index = 0; index < entry->num_bytes; index += sizeof(DirEntry)) {
        inode_read(inode, (void*)&dir_entry, index, sizeof(DirEntry));
        if (dir_entry.inode_no == 0) {
            break;
        }
    }
    // wirte
    memcpy(dir_entry.name, name, FILE_NAME_MAX_LENGTH);
    dir_entry.inode_no = inode_no;
    inode_write(ctx, inode, (void*)&dir_entry, index, sizeof(DirEntry));
    return index;
}

// see `inode.h`.
static void inode_remove(OpContext* ctx, Inode* inode, usize index) {
    // TODO
    if (index < inode->entry.num_bytes) {
        char zero[sizeof(DirEntry)] = {0};
        inode_write(ctx, inode, (void*)zero, index, sizeof(DirEntry));
    }
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};
/* Paths. */

/* Copy the next path element from path into name.
 *
 * Return a pointer to the element following the copied one.
 * The returned path has no leading slashes,
 * so the caller can check *path=='\0' to see if the name is the last one.
 * If no name to remove, return 0.
 *
 * Examples:
 *   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
 *   skipelem("///a//bb", name) = "bb", setting name = "a"
 *   skipelem("a", name) = "", setting name = "a"
 *   skipelem("", name) = skipelem("////", name) = 0
 */
static const char* skipelem(const char* path, char* name) {
    const char* s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/* Look up and return the inode for a path name.
 *
 * If parent != 0, return the inode for the parent and copy the final
 * path element into name, which must have room for DIRSIZ bytes.
 * Must be called inside a transaction since it calls iput().
 */
static Inode* namex(const char* path,
                    int nameiparent,
                    char* name,
                    OpContext* ctx) {
    /* TODO: Lab10 Shell */

    Inode* ip;
    if (path[0] == '/') {
        ip = inode_share(inodes.root);
    } else {
        ip = inode_share(thisproc()->cwd);
    }

    path = skipelem(path, name);
    while (path) {
        inode_lock(ip);
        if (ip->entry.type != INODE_DIRECTORY) {
            inode_unlock(ip);
            inode_put(ctx, ip);
            return 0;
        }
        if (nameiparent != 0 && path[0] == '\0') {
            inode_unlock(ip);
            return ip;
        }
        usize inode_no = inode_lookup(ip, name, NULL);
        if (inode_no == 0) {
            inode_unlock(ip);
            inode_put(ctx, ip);
            return 0;
        }
        Inode* next_ip = inode_get(inode_no);
        inode_unlock(ip);
        inode_put(ctx, ip);
        ip = next_ip;
        path = skipelem(path, name);
    }

    if (nameiparent) {
        inode_put(ctx, ip);
        return 0;
    }
    return ip;
}

Inode* namei(const char* path, OpContext* ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, 0, name, ctx);
}

Inode* nameiparent(const char* path, char* name, OpContext* ctx) {
    return namex(path, 1, name, ctx);
}

/*
 * Copy stat information from inode.
 * Caller must hold ip->lock.
 */
void stati(Inode* ip, struct stat* st) {
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
        case INODE_REGULAR:
            st->st_mode = S_IFREG;
            break;
        case INODE_DIRECTORY:
            st->st_mode = S_IFDIR;
            break;
        case INODE_DEVICE:
            st->st_mode = 0;
            break;
        default:
            PANIC();
    }
}
