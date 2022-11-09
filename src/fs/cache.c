#include "common/buf.h"
#include "common/checker.h"
#include "common/defines.h"
#include "common/list.h"
#include "common/sem.h"
#include "common/spinlock.h"
#include "fs/defines.h"
#include "kernel/init.h"
#include "kernel/schinfo.h"
#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

static const SuperBlock *sblock;
static const BlockDevice *device;

static SpinLock lock, b_lock; // protects block cache.
static ListNode head;         // the list of all allocated in-memory block.
static LogHeader header;      // in-memory copy of log header block.
static Semaphore s1, s2, s3;

//由于reserve的机制，能够包含的是固定的？
// hint: you may need some other variables. Just add them here.
// xv6: commit
// 时（获得了所有的block信息，在header里：数目、对应的block，然后写到磁盘上，然后在log中标记为）
//读取的时候记得read_header
//怎么又，，group
// commit了。。。因为header只有一个，真正写到磁盘header上的commit只有一次
//

struct LOG {
  /* data */
  // unsigned int log_used;
  SpinLock lock;
  bool committing;
  // bool committed[LOG_MAX_SIZE / OP_MAX_NUM_BLOCKS]; //需要管理transaction
  unsigned int outstanding;
  int op_reserved;
} log;

// read the content from disk.
static INLINE void device_read(Block *block) {
  device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block) {
  device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
  device->read(sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header() {
  device->write(sblock->log_start, (u8 *)&header);
}
// initialize a block struct.
static void init_block(Block *block) {
  block->block_no = 0;
  init_list_node(&block->node);
  block->acquired = false;
  block->pinned = false;

  init_sleeplock(&block->lock);
  block->valid = false;
  memset(block->data, 0, sizeof(block->data));
}
// see `cache.h`.
static usize get_num_cached_blocks() {
  // TODO
  int cnt = 0;
  _for_in_list(p, &head) {
    if (p == &head) {
      continue;
    }
    cnt++;
  }
  return cnt;
}
// see `cache.h`.
static Block *cache_acquire(usize block_no) {
  // TODO
  _acquire_spinlock(&lock);
  _for_in_list(p, &head) {
    if (p == &head) {
      continue;
    }
    auto bp = container_of(p, Block, node);
    if (bp->valid && bp->block_no == block_no) {
      bp->acquired = true;
      _release_spinlock(&lock);
      wait_sem(&bp->lock);
      return bp;
    }
  }

  if (get_num_cached_blocks() >= EVICTION_THRESHOLD) {
    _for_in_list_reverse(p, &head) {
      if (p == &head) {
        continue;
      }
      auto bp = container_of(p, Block, node);
      if (!bp->pinned && !bp->acquired) {
        bp->block_no = block_no;
        bp->acquired = true;
        _release_spinlock(&lock);
        wait_sem(&bp->lock);
        device_read(bp);
        bp->valid = true;
        return bp;
      }
    }
  }

  auto new_block = (Block *)kalloc(sizeof(Block));
  init_block(new_block);
  new_block->block_no = block_no;
  _insert_into_list(&head, &new_block->node);
  new_block->acquired = true;
  _release_spinlock(&lock);
  wait_sem(&new_block->lock);
  device_read(new_block);
  new_block->valid = true;
  return new_block;
}

// see `cache.h`.
static void cache_release(Block *block) {
  // TODO
  _acquire_spinlock(&lock);
  // lru
  block->acquired = false;
  _detach_from_list(&block->node);
  _merge_list(&head, &block->node);
  _release_spinlock(&lock);
  post_sem(&block->lock);
}

void install_trans() {
  for (usize tail = 0; tail < header.num_blocks; tail++) {
    // log在磁盘上有位置，对应
    auto from = cache_acquire(sblock->log_start + tail + 1);
    auto to = cache_acquire(header.block_no[tail]);
    memmove(to->data, from->data, BLOCK_SIZE);
    device_write(to);
    // when recovering,if the head contains合并后的事务
    //只有当一个cacahe中的block被写了，但是还没到磁盘上时，才pin
    _acquire_spinlock(&lock);
    to->pinned = false;
    _release_spinlock(&lock);
    cache_release(from);
    cache_release(to);
  }
}

void recover_from_log() {
  read_header();
  install_trans();
  _acquire_spinlock(&log.lock);
  header.num_blocks = 0;
  _release_spinlock(&log.lock);
  write_header();
}

// initialize block cache.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device) {
  sblock = _sblock;
  device = _device;
  // TODO
  init_spinlock(&lock);
  init_list_node(&head);

  init_spinlock(&log.lock);
  init_sem(&s1, 0);
  init_sem(&s2, 0);
  init_sem(&s3, 0);
  init_spinlock(&b_lock);

  log.committing = false;
  log.outstanding = 0;
  recover_from_log();
}

// see `cache.h`.
//如何确定无commit？read_header，确定之后再看
//可以确保commit不打断begin
//需要一个write log，确实需要把cache搬到log里去。怎么搬呢，就是找到目的地，
//那个buf是通过
static void cache_begin_op(OpContext *ctx) {
  // TODO
  //只有恢复的时候从磁盘读header
  while (1) {
    _acquire_spinlock(&log.lock);
    if (log.committing) {
      _release_spinlock(&log.lock);
      wait_sem(&s3);
    } else if (header.num_blocks + log.op_reserved + OP_MAX_NUM_BLOCKS >
               LOG_MAX_SIZE) { // begin_op时header.num = 0?如果不在committing
      _release_spinlock(&log.lock);
      wait_sem(&s1);
    } else {
      log.outstanding++;
      ctx->rm = OP_MAX_NUM_BLOCKS;
      log.op_reserved += ctx->rm;
      break;
    }
  }
  _release_spinlock(&log.lock);
}

// see `cache.h`.
// 打断了并行的原子？
// 确保没有并行的原子操作即可，对于写log，也就是commit
// commit到底指什么
static void cache_sync(OpContext *ctx, Block *block) {
  // TODO
  if (ctx != NULL) {
    _acquire_spinlock(&log.lock);
    if (header.num_blocks >= LOG_MAX_SIZE) {
      PANIC();
    }
    if (log.outstanding < 1) {
      PANIC();
    }
    bool not_allocated = true;
    for (usize i = 0; i < header.num_blocks; i++) {
      if (header.block_no[i] == block->block_no) {
        not_allocated = false;
        break;
      }
    }
    if (not_allocated) {
      header.block_no[header.num_blocks++] = block->block_no;
      block->pinned = true;
      if (ctx->rm > 0) {
        ctx->rm--;
        log.op_reserved--;
      } else {
        PANIC();
      }
    }
    _release_spinlock(&log.lock);
  } else {
    device_write(block);
  }
}

// see `cache.h`.
void write_log() {
  for (usize tail = 0; tail < header.num_blocks; tail++) {
    auto to = cache_acquire(sblock->log_start + tail + 1);
    auto from = cache_acquire(header.block_no[tail]);
    memmove(to->data, from->data, BLOCK_SIZE);
    device_write(to);
    cache_release(from);
    cache_release(to);
  }
}

void commit() {
  if (header.num_blocks > 0) {
    write_log();
    write_header();
    install_trans();
    _acquire_spinlock(&log.lock);
    header.num_blocks = 0;
    _release_spinlock(&log.lock);
    write_header();
  }
}

static void cache_end_op(OpContext *ctx) {
  // TODO
  _acquire_spinlock(&log.lock);
  log.outstanding--;
  log.op_reserved -= ctx->rm;
  if (log.committing)
    PANIC();
  if (log.outstanding == 0) {
    log.committing = true;
    _release_spinlock(&log.lock);
    commit();
    _acquire_spinlock(&log.lock);
    log.committing = false;
    _release_spinlock(&log.lock);
    post_all_sem(&s1);
    post_all_sem(&s2);
    post_all_sem(&s3);
  } else {
    _release_spinlock(&log.lock);
    post_all_sem(&s1);
    wait_sem(&s2);
  }
  // log.header.block_no =
}

usize BBLOCK(usize b, SuperBlock *sb) {
  _acquire_spinlock(&b_lock);
  auto ret = b / BIT_PER_BLOCK + sb->bitmap_start;
  _release_spinlock(&b_lock);
  return ret;
}
void bzero(OpContext *ctx, u32 block_no) {
  Block *bp = cache_acquire(block_no);
  memset(bp->data, 0, BLOCK_SIZE);
  cache_sync(ctx, bp);
  cache_release(bp);
}
static usize cache_alloc(OpContext *ctx) {
  // TODO
  u32 b, bi, m;
  Block *bp = NULL;
  for (b = 0; b < sblock->num_blocks; b += BIT_PER_BLOCK) {
    bp = cache_acquire(BBLOCK((u32)b, sblock));
    for (bi = 0; bi < BIT_PER_BLOCK && b + bi < sblock->num_blocks; bi++) {
      m = (u32)(1 << (bi % 8));
      if ((bp->data[bi / 8] & (u8)m) == 0) {
        bp->data[bi / 8] |= (u8)m;
        cache_sync(ctx, bp);
        cache_release(bp);
        bzero(ctx, b + bi);
        return b + bi;
      }
    }
    cache_release(bp);
  }
  PANIC();
}

// see `cache.h`.
// hint: you can use `cache_acquire`/`cache_sync` to read/write blocks.
static void cache_free(OpContext *ctx, usize block_no) {
  // TODO
  Block *bp;
  int bi, m;
  bp = cache_acquire(BBLOCK(block_no, sblock));
  bi = block_no % BIT_PER_BLOCK;
  m = 1 << (bi % 8);
  if ((bp->data[bi / 8] & m) == 0) {
    PANIC();
  }
  bp->data[bi / 8] &= ~m;
  cache_sync(ctx, bp);
  cache_release(bp);
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};
