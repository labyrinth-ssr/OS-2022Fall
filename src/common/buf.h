#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/string.h>
#include <common/sem.h>

#define BSIZE 512

#define B_VALID 0x2 /* Buffer has been read from disk. */
#define B_DIRTY 0x4 /* Buffer needs to be written to disk. */

typedef struct buf {
    int flags;
    u32 blockno;
    u8 data[BSIZE];  // 1B*512
    /*
     * Add other necessary elements. It depends on you.
     */
    /* TODO: Lab7 driver. */
    ListNode node;
    Semaphore sl;
} buf;

typedef struct BufQueue {
    ListNode* begin;
    ListNode* end;
    int sz;
    SpinLock lk;
} BufQueue;

void bufqueue_init(BufQueue* x);
void bufqueue_lock(BufQueue* x);
void bufqueue_unlock(BufQueue* x);
void bufqueue_push(BufQueue* x, buf* item);
void bufqueue_pop(BufQueue* x);
buf* bufqueue_front(BufQueue* x);
bool bufqueue_empty(BufQueue* x);


