#pragma once

#include <common/list.h>
struct proc; // dont include proc.h here

// embedded data for cpus
struct sched
{
    // TODO: customize your sched info
    struct proc* thisproc;
    struct proc* idle; //always exists
};

// embeded data for procs
struct schinfo
{
    // TODO: customize your sched info
    ListNode rq;
    int prio;
    // int time_counter;
    //nice value?
};
