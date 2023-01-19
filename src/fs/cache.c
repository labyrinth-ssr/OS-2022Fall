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

static SpinLock lock;    // protects block cache.
static ListNode head;    // the list of all allocated in-memory block.
static LogHeader header; // in-memory copy of log header block.
static Semaphore s1, s2, s3;
static bool swap_valid[SWAP_END - SWAP_START];

// hint: you may need some other variables. Just add them here.
struct LOG {
  /* data */
  SpinLock lock;
  bool committing;
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
  block->valid = false;
  init_sleeplock(&block->lock);
  memset(block->data, 0, sizeof(block->data));
}
// see `cache.h`.
static usize get_num_cached_blocks() {
  // Modified
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
  // Modified
  _acquire_spinlock(&lock);
  _for_in_list(p, &head) {
    if (p == &head) {
      continue;
    }
    auto bp = container_of(p, Block, node);
    if (bp->block_no == block_no) {
      bp->acquired = true;
      _release_spinlock(&lock);
      unalertable_wait_sem(&bp->lock);
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
        unalertable_wait_sem(&bp->lock);
        device_read(bp);
        return bp;
      }
    }
  }

  auto new_block = (Block *)kalloc(sizeof(Block));
  init_block(new_block);
  new_block->block_no = block_no;
  _insert_into_list(&head, &new_block->node);
  new_block->acquired = true;
  unalertable_wait_sem(&new_block->lock);
  device_read(new_block);
  new_block->valid = true;
  _release_spinlock(&lock);
  return new_block;
}

// see `cache.h`.
static void cache_release(Block *block) {
  // Modified
  _acquire_spinlock(&lock);
  block->acquired = false;
  _detach_from_list(&block->node);
  _merge_list(&head, &block->node);
  _release_spinlock(&lock);
  post_sem(&block->lock);
}

void install_trans() {
  for (usize tail = 0; tail < header.num_blocks; tail++) {
    auto from = cache_acquire(sblock->log_start + tail + 1);
    auto to = cache_acquire(header.block_no[tail]);
    memmove(to->data, from->data, BLOCK_SIZE);
    device_write(to);
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
  // Modified
  init_spinlock(&lock);
  init_list_node(&head);

  init_spinlock(&log.lock);
  init_sem(&s1, 0);
  init_sem(&s2, 0);
  init_sem(&s3, 0);

  log.committing = false;
  log.outstanding = 0;
  recover_from_log();

  for (auto i = SWAP_START; i < SWAP_END; i++) {
    swap_valid[i] = false;
  }
}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx) {
  // Modified
  while (1) {
    _acquire_spinlock(&log.lock);
    if (log.committing) {
      _release_spinlock(&log.lock);
      unalertable_wait_sem(&s3);
    } else if (header.num_blocks + log.op_reserved + OP_MAX_NUM_BLOCKS >
               LOG_MAX_SIZE) {
      _release_spinlock(&log.lock);
      unalertable_wait_sem(&s1);
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
static void cache_sync(OpContext *ctx, Block *block) {
  // Modified
  if (ctx != NULL) {
    _acquire_spinlock(&log.lock);
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
        printk("exceed OP_MAX_NUM_BLOCKS\n");
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
  // Modified
  _acquire_spinlock(&log.lock);
  log.outstanding--;
  log.op_reserved -= ctx->rm;
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
    unalertable_wait_sem(&s2);
  }
  // log.header.block_no =
}

usize BBLOCK(usize b, const SuperBlock *sb) {
  auto ret = b / BIT_PER_BLOCK + sb->bitmap_start;
  return ret;
}
void bzero(OpContext *ctx, u32 block_no) {
  Block *bp = cache_acquire(block_no);
  memset(bp->data, 0, BLOCK_SIZE);
  cache_sync(ctx, bp);
  cache_release(bp);
}

static usize cache_alloc(OpContext *ctx) {
  // Modified
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
  printk("no free block on disk");
  PANIC();
}

// see `cache.h`.
// hint: you can use `cache_acquire`/`cache_sync` to read/write blocks.
static void cache_free(OpContext *ctx, usize block_no) {
  // Modified
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

void release_8_blocks(u32 bno) {
  for (usize i = bno - SWAP_START;
       i < bno - SWAP_START + 8 && i < SWAP_END - SWAP_START; i++) {
    swap_valid[i] = false;
  }
}

u32 find_and_set_8_blocks() {

  // Modified:在swap分区中找到8个连续的块，并返回第一个块的
  // 块号
  // OpContext ctx;

  u32 left = 0, right = 0;
  bool has_eight = FALSE;
  while (right < SWAP_END - SWAP_START) {
    if (swap_valid[right] == false) {
      right++;
      if (right - left == 8) {
        has_eight = TRUE;
        break;
      }
    } else {
      left = right + 1;
      right = right + 1;
    }
  }
  if (has_eight) {
    for (u32 i = left; i < right; i++) {
      swap_valid[i] = true;
    }
    return SWAP_START + left;
  }

  printk("no continuous 8 blocks on disk");
  for (auto i = 0; i < SWAP_END - SWAP_START; i++) {
    printk("%d ", swap_valid[i]);
  }
  PANIC();
  (void)swap_valid;
  // return 0;
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
