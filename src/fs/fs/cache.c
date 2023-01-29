#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

static const SuperBlock* sblock;
static const BlockDevice* device;

static SpinLock lock;     // protects block cache.
static ListNode head;     // the list of all allocated in-memory block.
static LogHeader header;  // in-memory copy of log header block.

static u32 cached_blocks_num;

// manage swap_bitmap
static SpinLock swap_lock;
static char swap_bitmap[SWAP_BIT_END - SWAP_BIT_START];

// hint: you may need some other variables. Just add them here.
struct LOG {
    /* data */
    SpinLock lock;
    usize outstanding;
    usize used;
    usize log_max_num;
    bool committing;
    Semaphore sem;
    Semaphore outstanding_sem;
} log;

// read the content from disk.
static INLINE void device_read(Block* block) {
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block* block) {
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8*)&header);
}

// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8*)&header);
}

// initialize a block struct.
static void init_block(Block* block) {
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
    return cached_blocks_num;
}

// see `cache.h`.
static Block* cache_acquire(usize block_no) {
    // TODO
    bool in_cache = false;
    Block* b;
    ListNode* p;
find_in_cache:
    in_cache = false;
    _acquire_spinlock(&lock);
    p = head.next;
    while(p != &head) {
        b = container_of(p, Block, node);
        if (b->block_no == block_no) {
            in_cache = true;
            break;
        }
        p = p->next;
    }

    if (in_cache) {
        if (b->acquired == true) {
            _release_spinlock(&lock);
            unalertable_wait_sem(&b->lock);
            goto find_in_cache; // find again to guarantee it is valid
        }

        get_sem(&b->lock);
        _detach_from_list(&b->node);
        _insert_into_list(&head, &b->node);
        b->acquired = true;
        _release_spinlock(&lock);
        return b;
    }

    // LRU
    p = head.prev;
    while (cached_blocks_num >= EVICTION_THRESHOLD && p != &head) {
        Block* block = container_of(p, Block, node);
        if (block->acquired != true && block->pinned != true) {
            p = _detach_from_list(p);
            kfree(block);
            cached_blocks_num--;
        } else {
            p = p->prev;
        }
    }
    // else from sd card
    b = (Block*)kalloc(sizeof(Block));
    // init it
    init_block(b);
    b->block_no = block_no;
    _insert_into_list(&head, &b->node);
    cached_blocks_num++;
    
    // hold the sleep lock so it's safe
    b->acquired = true;
    get_sem(&b->lock);
    _release_spinlock(&lock);

    // read from disk
    device_read(b);
    b->valid = true;
    return b;
}

// see `cache.h`.
static void cache_release(Block* block) {
    // TODO
    _acquire_spinlock(&lock);
    block->acquired = false;
    post_sem(&(block->lock));
    _release_spinlock(&lock);
}

void log_to_disk() {
    for (u64 i = 0; i < header.num_blocks; ++i) {
        Block log_block;
        init_block(&log_block);
        log_block.block_no = sblock->log_start + i + 1;
        device_read(&log_block);
        log_block.block_no = header.block_no[i];
        device_write(&log_block);
    }
}

// initialize block cache.
void init_bcache(const SuperBlock* _sblock, const BlockDevice* _device) {
    sblock = _sblock;
    device = _device;

    // TODO
    init_spinlock(&lock);
    init_spinlock(&swap_lock);
    init_list_node(&head);
    cached_blocks_num = 0;

    // init log struct
    log.used = 0;
    log.log_max_num = MIN(sblock->num_log_blocks - 1, LOG_MAX_SIZE);
    init_spinlock(&(log.lock));
    init_sem(&(log.sem), 0);
    init_sem(&(log.outstanding_sem), 0);
    log.committing = false;

    // recover from log
    read_header();
    log_to_disk();
    header.num_blocks = 0;
    write_header();
}

// see `cache.h`.
static void cache_begin_op(OpContext* ctx) {
    // TODO
    _acquire_spinlock(&(log.lock));
    while (true) {
        if (log.committing) {
            _lock_sem(&(log.sem));
            _release_spinlock(&(log.lock));
            // unalertablewait
            if (_wait_sem(&(log.sem), false) == false) {
                PANIC();
            }
            _acquire_spinlock(&(log.lock));
        } else if (log.used + OP_MAX_NUM_BLOCKS > log.log_max_num) {
            _lock_sem(&(log.sem));
            _release_spinlock(&(log.lock));
            // unalertablewait
            if (_wait_sem(&(log.sem), false) == false) {
                PANIC();
            }
            _acquire_spinlock(&(log.lock));
        } else {
            log.outstanding++;
            log.used += OP_MAX_NUM_BLOCKS;
            ctx->rm = OP_MAX_NUM_BLOCKS;
            ctx->block_num = 0;
            break;
        }
    }
    _release_spinlock(&(log.lock));
}

// see `cache.h`.
static void cache_sync(OpContext* ctx, Block* block) {
    // TODO
    if (ctx == NULL) {
        device_write(block);
        return;
    }

    _acquire_spinlock(&(log.lock));
    for (u64 i = 0; i < header.num_blocks; ++i) {
        if (header.block_no[i] == block->block_no) {
            // local absorption
            for (usize j = 0; j < ctx->block_num; ++j) {
                if (ctx->op_block[j] == block->block_no) {
                    _release_spinlock(&(log.lock));
                    return;
                }
            }
            // global absorption
            ctx->op_block[ctx->block_num] = block->block_no;
            ctx->block_num++;
            ctx->rm--;
            log.used--;
            post_all_sem(&(log.sem));
            _release_spinlock(&(log.lock));
            return;
        }
    }

    // if not in log, add a new block
    if (ctx->rm == 0) {
        PANIC();
    }
    header.num_blocks++;
    header.block_no[header.num_blocks - 1] = block->block_no;
    ctx->op_block[ctx->block_num] = block->block_no;
    ctx->block_num++;
    ctx->rm--;
    block->pinned = true;
    _release_spinlock(&(log.lock));
}

// see `cache.h`.
static void cache_end_op(OpContext* ctx) {
    // TODO
    _acquire_spinlock(&(log.lock));
    log.outstanding--;
    log.used -= ctx->rm;
    post_all_sem(&(log.sem));
    // if outstanding > 0
    // sleep until checkpointed
    if (log.outstanding > 0) {
        _lock_sem(&(log.outstanding_sem));
        _release_spinlock(&(log.lock));
        // unalertablewait
        if (_wait_sem(&(log.outstanding_sem), false) == false) {
            PANIC();
        };
        return;
    }
    if (log.outstanding == 0) { // checkpoints
        log.committing = true;
        _release_spinlock(&(log.lock));
        // 1.Write blocks to log area
        for (u64 i = 0; i < header.num_blocks; ++i) {
            Block* b = cache_acquire(header.block_no[i]);
            device->write(sblock->log_start + i + 1, b->data);
            b->pinned = false;
            cache_release(b);
        }
        write_header(); // 2.Write log header
        log_to_disk();  // 3.Copy blocks to original locations
        header.num_blocks = 0;
        write_header(); // 4.reset log
        _acquire_spinlock(&(log.lock));
        log.used = 0;
        log.committing = false;
        post_all_sem(&(log.sem));
        post_all_sem(&(log.outstanding_sem));
    }
    _release_spinlock(&(log.lock));
}

// see `cache.h`.
// hint: you can use `cache_acquire`/`cache_sync` to read/write blocks.
static usize cache_alloc(OpContext* ctx) {
    // TODO
    for (u32 i = 0; i < SWAP_START; i += BIT_PER_BLOCK) {
        Block* bitmap_block = cache_acquire(i / BIT_PER_BLOCK + sblock->bitmap_start);
        for (u32 j = 0; j < BIT_PER_BLOCK && i + j < SWAP_START; ++j) {
            if (bitmap_get((BitmapCell*)bitmap_block->data, j) == false) {
                Block* b = cache_acquire(i + j);
                memset(b->data, 0, BLOCK_SIZE);
                cache_sync(ctx, b);
                bitmap_set((BitmapCell*)bitmap_block->data, j);
                cache_sync(ctx, bitmap_block);

                cache_release(b);
                cache_release(bitmap_block);
                return i + j;
            }
        }
        cache_release(bitmap_block);
    }
    PANIC();
}

// see `cache.h`.
// hint: you can use `cache_acquire`/`cache_sync` to read/write blocks.
static void cache_free(OpContext* ctx, usize block_no) {
    // TODO
    Block* bitmap_block = cache_acquire(block_no / BIT_PER_BLOCK + sblock->bitmap_start);
    bitmap_clear((BitmapCell*)bitmap_block->data, block_no % BIT_PER_BLOCK);
    cache_sync(ctx, bitmap_block);
    cache_release(bitmap_block);
}

void release_8_blocks(u32 bno) {
    _acquire_spinlock(&swap_lock);
    swap_bitmap[(bno - SWAP_START) / 8] = (char)0;
    _release_spinlock(&swap_lock);
}

u32 find_and_set_8_blocks() {
    _acquire_spinlock(&swap_lock);
    for (int i = 0; i < SWAP_BIT_END - SWAP_BIT_START; ++i) {
        if (swap_bitmap[i] == (char)0) {
            swap_bitmap[i] = (char)0xff;
            _release_spinlock(&swap_lock);
            return i * 8 + SWAP_START;
        }
    }
    PANIC();
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
