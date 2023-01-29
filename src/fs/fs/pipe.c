#include <kernel/mem.h>
#include <kernel/sched.h>
#include <fs/pipe.h>
#include <common/string.h>

int pipeAlloc(File** f0, File** f1) {
    // TODO
    Pipe* pipe = kalloc(sizeof(Pipe));
    *f0 = filealloc();
    *f1 = filealloc();
    if (pipe == NULL || *f0 == NULL || *f1 == NULL) {
        kfree(pipe);
        if (*f0) {
            fileclose(*f0);
        }
        if (*f1) {
            fileclose(*f1);
        }
        return -1;
    }

    // init pipe
    init_spinlock(&(pipe->lock));
    init_sem(&(pipe->wlock), 0);
    init_sem(&(pipe->rlock), 0);
    pipe->nread = 0;
    pipe->nwrite = 0;
    pipe->readopen = 1;
    pipe->writeopen = 1;

    // init file
    (*f0)->type = FD_PIPE;
    (*f0)->readable = 1;
    (*f0)->writable = 0;
    (*f0)->pipe = pipe;
    (*f1)->type = FD_PIPE;
    (*f1)->readable = 0;
    (*f1)->writable = 1;
    (*f1)->pipe = pipe;
    return 1;
}

void pipeClose(Pipe* pi, int writable) {
    // TODO
    _acquire_spinlock(&(pi->lock));
    if (writable == 0) {
        pi->readopen = 0;
        post_all_sem(&(pi->wlock));
    }
    if (writable == 1) {
        pi->writeopen = 0;
        post_all_sem(&(pi->rlock));
    }

    if (pi->readopen == 0 && pi->writeopen == 0) {
        _release_spinlock(&(pi->lock));
        kfree(pi);
    } else {
        _release_spinlock(&(pi->lock)); 
    }
}

int pipeWrite(Pipe* pi, u64 addr, int n) {
    // TODO
    char* src = (char*)addr;
    
    _acquire_spinlock(&(pi->lock));
    for (int i = 0; i < n; ++i) {
        while (pi->nread + PIPESIZE == pi->nwrite) {
            // pipe is full
            if (pi->readopen == 0 || thisproc()->killed) {
                _release_spinlock(&(pi->lock));
                return i;
            }

            // sleep and wait
            post_all_sem(&(pi->rlock));
            _lock_sem(&(pi->wlock));
            _release_spinlock(&(pi->lock));
            if (_wait_sem(&(pi->wlock), true) == FALSE) {
                return i;
            }
            _acquire_spinlock(&(pi->lock));
        }
        pi->data[pi->nwrite % PIPESIZE] = src[i];
        pi->nwrite += 1;
    }
    
    post_all_sem(&(pi->rlock));
    _release_spinlock(&(pi->lock));
    return n;
}

int pipeRead(Pipe* pi, u64 addr, int n) {
    // TODO
    char* dst = (char*)addr;
    
    _acquire_spinlock(&(pi->lock));
    for (int i = 0; i < n; ++i) {
        while (pi->nread == pi->nwrite) {
            // pipe is empty
            if (pi->writeopen == 0 || thisproc()->killed) {
                _release_spinlock(&(pi->lock));
                return i;
            }

            // sleep and wait
            post_all_sem(&(pi->wlock));
            _lock_sem(&(pi->rlock));
            _release_spinlock(&(pi->lock));
            if (_wait_sem(&(pi->rlock), true) == FALSE) {
                return i;
            }
            _acquire_spinlock(&(pi->lock));
        }
        dst[i] = pi->data[pi->nread % PIPESIZE];
        pi->nread += 1;
    }

    post_all_sem(&(pi->wlock));
    _release_spinlock(&(pi->lock));
    return n;
}