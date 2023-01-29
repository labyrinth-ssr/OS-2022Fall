#pragma once

#include <kernel/proc.h>
#include <kernel/schinfo.h>
#include <common/spinlock.h>
#include <common/bitmap.h>

#define MAX_PID 256

typedef struct pid_bitmap_t {
    BitmapCell bitmap[MAX_PID / 64];
    SpinLock pid_lock;
    int last_pid;
    int size;
} pid_bitmap_t;

struct container
{
    struct container* parent;
    struct proc* rootproc;

    struct schinfo schinfo;
    struct schqueue schqueue;

    // TODO: namespace (local pid?)
    pid_bitmap_t local_pid;
};

struct container* create_container(void (*root_entry)(), u64 arg);
void set_container_to_this(struct proc*);
