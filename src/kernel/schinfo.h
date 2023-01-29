#pragma once

#include <common/list.h>
#include <common/rbtree.h>
struct proc; // dont include proc.h here

// embedded data for cpus
struct sched {
    // TODO: customize your sched info
    struct proc* thisproc;
    struct proc* idle;
};

// embeded data for procs
struct schinfo {
    // TODO: customize your sched info
    u64 vruntime;
    int prio;
    int weight;
    bool group;
    struct rb_node_ node;
    bool is_in_queue;
};

// embedded data for containers
struct schqueue {
    // TODO: customize your sched queue
    struct rb_root_ rq;
    u64 weight_sum;
    u64 sched_latency;
    bool unused;
};
