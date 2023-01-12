#include "aarch64/intrinsic.h"
#include "common/defines.h"
#include "common/list.h"
#include "common/rc.h"
#include "common/sem.h"
#include "common/spinlock.h"
#include "fs/cache.h"
#include "fs/defines.h"
#include "kernel/console.h"
#include "kernel/proc.h"
#include <common/string.h>
#include <fs/inode.h>
#include <kernel/console.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <sys/stat.h>

// this lock mainly prevents concurrent access to inode list `head`, reference
// count increment and decrement.
static Semaphore lock;
static ListNode head;

static const SuperBlock *sblock;
static const BlockCache *cache;

// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
  return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry *get_entry(Block *block, usize inode_no) {
  return ((InodeEntry *)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32 *get_addrs(Block *block) {
  return ((IndirectBlock *)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock *_sblock, const BlockCache *_cache) {
  init_sleeplock(&lock);
  init_list_node(&head);
  sblock = _sblock;
  cache = _cache;

  if (ROOT_INODE_NO < sblock->num_inodes)
    inodes.root = inodes.get(ROOT_INODE_NO);
  else
    printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode *inode) {
  init_sleeplock(&inode->lock);
  init_rc(&inode->rc);
  init_list_node(&inode->node);
  inode->inode_no = 0;
  inode->valid = false;
}

// TODO
// see `inode.h`.
//遍历，在磁盘上找invalid的dinode
static usize inode_alloc(OpContext *ctx, InodeType type) {
  (void)ctx;
  ASSERT(type != INODE_INVALID);

  for (usize inode_no = 1; inode_no <= sblock->num_inodes; inode_no++) {
    Block *bp = cache->acquire(to_block_no(inode_no));
    InodeEntry *dip = get_entry(bp, inode_no);
    if (dip->type == INODE_INVALID) {
      memset(dip, 0, sizeof(InodeEntry));
      dip->type = type;
      cache->sync(ctx, bp);
      cache->release(bp);
      return inode_no;
    }
    cache->release(bp);
  }
  printk("no free inode on disk\n");
  PANIC();
}

// TODO
// see `inode.h`.
static void inode_lock(Inode *inode) {
  ASSERT(inode->rc.count > 0);
  ASSERT(wait_sem(&inode->lock));
}

// TODO
// see `inode.h`.
static void inode_unlock(Inode *inode) {
  ASSERT(inode->rc.count > 0);
  post_sem(&inode->lock);
}

// TODO
// see `inode.h`.
static void inode_sync(OpContext *ctx, Inode *inode, bool do_write) {
  usize inode_no = inode->inode_no;
  Block *bp = cache->acquire(to_block_no(inode_no));
  InodeEntry *dip = get_entry(bp, inode_no);
  if (inode->valid && do_write) {
    *dip = inode->entry;
    cache->sync(ctx, bp);
  } else if (!inode->valid) {
    inode->entry = *dip;
    inode->valid = true;
  }
  cache->release(bp);
}

// see `inode.h`.
// 直接分配inode。
// 新alloc的entry或empty的entry没有对应inode_no
// TODO
static Inode *inode_get(usize inode_no) {
  ASSERT(inode_no > 0);
  ASSERT(inode_no < sblock->num_inodes);
  ASSERT((wait_sem(&lock)));
  Inode *ip = NULL;
  ListNode *empty = NULL;
  _for_in_list(p, &head) {
    if (p == &head) {
      continue;
    }
    ip = container_of(p, Inode, node);
    if (ip->inode_no == inode_no && ip->rc.count > 0) {
      _increment_rc(&ip->rc);
      post_sem(&lock);
      return ip;
    }
    if (empty == NULL && ip->rc.count == 0) {
      empty = &ip->node;
    }
  }
  if (empty != NULL) {
    ip = container_of(empty, Inode, node);
  } else {
    ip = (Inode *)kalloc(sizeof(Inode));
    init_inode(ip);
    _merge_list(&head, &ip->node);
  }
  ip->inode_no = inode_no;
  _increment_rc(&ip->rc);
  ip->valid = false;
  inode_lock(ip);
  inode_sync(NULL, ip, false);
  inode_unlock(ip);
  post_sem(&lock);
  return ip;
}
// TODO
// see `inode.h`.
static void inode_clear(OpContext *ctx, Inode *inode) {
  InodeEntry *entry = &inode->entry;
  for (int i = 0; i < INODE_NUM_DIRECT; i++) {
    if (entry->addrs[i]) {
      cache->free(ctx, entry->addrs[i]);
      entry->addrs[i] = 0; // make addr entry invalid
    }
  }
  if (entry->indirect != NULL) {
    Block *bp = cache->acquire(entry->indirect);
    // block_no is usize
    u32 *addr = (void *)(bp->data);
    for (usize i = 0; i < INODE_NUM_INDIRECT; i++) {
      if (addr[i]) {
        cache->free(ctx, addr[i]);
      }
    }
    cache->release(bp);
    cache->free(ctx, entry->indirect);
    entry->indirect = 0;
  }
  entry->num_bytes = 0;
  inode_sync(ctx, inode, true);
}

// TODO
// see `inode.h`.
static Inode *inode_share(Inode *inode) {
  ASSERT(wait_sem(&lock));
  _increment_rc(&inode->rc);
  post_sem(&lock);
  return inode;
}

// TODO
// see `inode.h`.
static void inode_put(OpContext *ctx, Inode *inode) {
  ASSERT(inode->valid);
  // lock to protect rc read ?
  ASSERT(wait_sem(&lock));
  // num_links计算硬链接的数目，目前没用。
  if (inode->rc.count == 1 && inode->entry.num_links == 0) {
    inode_lock(inode);
    post_sem(&lock);
    inode_clear(ctx, inode);
    inode->entry.type = INODE_INVALID;
    inode_sync(ctx, inode, true);
    inode->valid = false;
    inode_unlock(inode);

    ASSERT(wait_sem(&lock));
    _detach_from_list(&inode->node);
    post_sem(&lock);
    _decrement_rc(&inode->rc);
    kfree(inode);
    return;
  }
  _decrement_rc(&inode->rc);
  post_sem(&lock);
}

// this function is private to inode layer, because it can allocate block
// at arbitrary offset, which breaks the usual file abstraction.
//
// retrieve the block in `inode` where offset lives. If the block is not
// allocated, `inode_map` will allocate a new block and update `inode`, at
// which time, `*modified` will be set to true.
// the block number is returned.
//
// NOTE: caller must hold the lock of `inode`.
// TODO
static usize inode_map(OpContext *ctx, Inode *inode, usize offset,
                       bool *modified) {
  InodeEntry *entry = &inode->entry;
  usize addr = 0;
  if (offset < INODE_NUM_DIRECT) {
    addr = entry->addrs[offset];
    if (addr == 0) {
      addr = cache->alloc(ctx);
      entry->addrs[offset] = addr;
      *modified = true;
    }
    return addr;
  }
  offset -= INODE_NUM_DIRECT;
  if (offset < INODE_NUM_INDIRECT) {
    addr = entry->indirect;
    if (addr == 0) {
      addr = cache->alloc(ctx);
      entry->indirect = addr;
    }
    Block *bp = cache->acquire(addr);
    u32 *a = (void *)bp->data;
    addr = a[offset];
    if (addr == 0) {
      addr = cache->alloc(ctx);
      a[offset] = addr;
      *modified = true;
      cache->sync(ctx, bp);
    }
    cache->release(bp);
    return addr;
  }
  printk("offset out of range\n");
  PANIC();
}

// TODO
// see `inode.h`.
static usize inode_read(Inode *inode, u8 *dest, usize offset, usize count) {
  InodeEntry *entry = &inode->entry;
  if (inode->entry.type == INODE_DEVICE) {
    ASSERT(inode->entry.major == 1);
    return console_read(inode, (void *)dest, count);
  }
  if (count + offset > entry->num_bytes)
    count = entry->num_bytes - offset;
  usize end = offset + count;
  ASSERT(offset <= entry->num_bytes);
  ASSERT(end <= entry->num_bytes);
  ASSERT(offset <= end);

  bool modified = false;
  Block *bp = NULL;
  usize m = 0;
  for (usize tot = 0; tot < count; tot += m, offset += m, dest += m) {
    // 处理开头或结尾数据长度不足BLOCK_SIZE的情况
    usize block_no = inode_map(NULL, inode, offset / BLOCK_SIZE, &modified);
    bp = cache->acquire(block_no);
    m = MIN(count - tot, BLOCK_SIZE - offset % BLOCK_SIZE);
    memmove(dest, bp->data + offset % BLOCK_SIZE, m); // z最后一次的offset
    cache->release(bp);
  }

  return 0;
}

// TODO
// see `inode.h`.
static usize inode_write(OpContext *ctx, Inode *inode, u8 *src, usize offset,
                         usize count) {
  InodeEntry *entry = &inode->entry;
  usize end = offset + count;
  if (inode->entry.type == INODE_DEVICE) {
    // what is a major device?
    ASSERT(inode->entry.major == 1);
    return console_write(inode, (char *)src, count);
  }
  ASSERT(offset <= entry->num_bytes);
  ASSERT(end <= INODE_MAX_BYTES);
  ASSERT(offset <= end);
  bool modified = false;
  Block *bp = NULL;
  usize m = 0;
  for (usize tot = 0; tot < count; tot += m, offset += m, src += m) {
    usize block_no = inode_map(ctx, inode, offset / BLOCK_SIZE, &modified);
    bp = cache->acquire(block_no);
    m = MIN(count - tot, BLOCK_SIZE - offset % BLOCK_SIZE);
    memmove(bp->data + offset % BLOCK_SIZE, src, m);
    cache->sync(ctx, bp);
    cache->release(bp);
  }
  if (count > 0 && offset > entry->num_bytes) {
    entry->num_bytes = offset;
    inode_sync(ctx, inode, true);
  }

  return count;
}

// TODO
// see `inode.h`.
static usize inode_lookup(Inode *inode, const char *name, usize *index) {
  InodeEntry *entry = &inode->entry;
  ASSERT(entry->type == INODE_DIRECTORY);

  DirEntry de;
  usize inum;
  for (usize off = 0; off < entry->num_bytes; off += sizeof(DirEntry)) {
    if (inode_read(inode, (void *)&de, off, sizeof(DirEntry))) {
      printk("dir look up fail");
      PANIC();
    }

    if (de.inode_no == 0) {
      continue;
    }
    if (strncmp(de.name, name, FILE_NAME_MAX_LENGTH) == 0) {
      if (index) {
        *index = off;
      }
      inum = de.inode_no;
      return inum;
    }
  }

  return 0;
}

// see `inode.h`.
static usize inode_insert(OpContext *ctx, Inode *inode, const char *name,
                          usize inode_no) {
  InodeEntry *entry = &inode->entry;
  ASSERT(entry->type == INODE_DIRECTORY);

  usize index = 0;
  //同名文件
  if (inode_lookup(inode, name, &index) != 0) {
    return -1;
  }
  usize off;
  DirEntry de;
  for (off = 0; off < entry->num_bytes; off += sizeof(de)) {
    inode_read(inode, (void *)&de, off, sizeof(de));
    if (de.inode_no == 0) {
      break;
    }
  }
  strncpy(de.name, name, FILE_NAME_MAX_LENGTH);
  de.inode_no = inode_no;
  inode_write(ctx, inode, (void *)&de, off, sizeof(de));
  // TODO
  return off;
}

// see `inode.h`.
static void inode_remove(OpContext *ctx, Inode *inode, usize index) {
  InodeEntry *entry = &inode->entry;
  ASSERT(entry->type == INODE_DIRECTORY);
  DirEntry de;
  memset(&de, 0, sizeof(de));
  inode_write(ctx, inode, (void *)&de, index, sizeof(de));
  // TODO
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
static const char *skipelem(const char *path, char *name) {
  const char *s;
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
static Inode *namex(const char *path, int nameiparent, char *name,
                    OpContext *ctx) {
  /* TODO: Lab10 Shell */
  return 0;
}

Inode *namei(const char *path, OpContext *ctx) {
  char name[FILE_NAME_MAX_LENGTH];
  return namex(path, 0, name, ctx);
}

Inode *nameiparent(const char *path, char *name, OpContext *ctx) {
  return namex(path, 1, name, ctx);
}

/*
 * Copy stat information from inode.
 * Caller must hold ip->lock.
 */
void stati(Inode *ip, struct stat *st) {
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
