#pragma once

#include <stdint.h>
#include <stddef.h>
#include "libos_lock.h"
#include "log.h"

#define SG_HOST_FD_OFFSET 500

#define SG_GEM_DATA_START      0x100000000ULL
#define SG_GEM_DATA_SIZE       (3ULL * 1024 * 1024 * 1024)
#define SG_GEM_HUGEPAGE_START  0x200000000ULL
#define SG_GEM_HUGEPAGE_SIZE   (1ULL * 1024 * 1024 * 1024)

#define NUM_COMM_PAGES 1

typedef struct {
    volatile uint64_t magic;
    volatile uint64_t req_bit;
    volatile uint64_t ret;
    volatile uint64_t p1;
    volatile uint64_t p2;
    volatile uint64_t p3;
    volatile uint64_t p4;
    volatile uint64_t p5;
} comm_page_t;

_Static_assert(offsetof(comm_page_t, ret) == 16, "SG response must stay in the hot cache line");
_Static_assert(offsetof(comm_page_t, p5) == 56, "common SG payload must fit one cache line");
_Static_assert(sizeof(comm_page_t) == 64, "SG communication ABI must fit one cache line");

extern struct libos_lock sg_comm_lock;

#define ENABLE_SG

#define COMM_ADDR  0xf00000ULL
#define COMM_MAGIC 0x1234567812345678ULL
static uint64_t LOG_MMAP_EVENT = 1;
static uint64_t SETUP_DATA = 2;
static uint64_t GEM_ALLOCATION = 3;
static uint64_t FSTAT = 4;
static uint64_t IOCTL = 5;
static uint64_t OPEN = 6;
static uint64_t FCNTL = 7;
static uint64_t READLINK = 8;
static uint64_t NEWFSTAT = 9;
static uint64_t GETDENT = 10;
static uint64_t DUP = 11;
static uint64_t DUP_IDENTITY = 30;
static uint64_t X11_SETUP = 12;
static uint64_t X11_PRESENT = 13;
static uint64_t CLOSE = 14;

static uint64_t LSEEK = 22;
static uint64_t READ = 24;
static uint64_t WRITE = 25;
static uint64_t SYSV_SEMGET = 34;
static uint64_t SYSV_SEMCTL = 35;
static uint64_t SYSV_SEMOP = 36;
static uint64_t HOST_FLOCK = 37;
static uint64_t HOST_FTRUNCATE = 38;

static size_t special_allocation = 0x123456789ULL;
static uint64_t fs_offset = 500;
/* Both Integrated and Discrete GPUs should be supported */
#define iGPUNODE_PATH "/dev/dri/renderD128"
#define dGPUNODE_PATH "/dev/dri/renderD129"

static void* DATA_REGION = (void*)SG_GEM_DATA_START;
static const size_t DATA_SIZE = SG_GEM_DATA_SIZE;
static inline comm_page_t* comm_page(long offset) {
    return (comm_page_t*)(uintptr_t)(COMM_ADDR + offset);
}

static uint64_t comm_sync_notify(comm_page_t* c) {
    if (!c || c->magic != COMM_MAGIC) {
        return 0;
    }
    /* Publish the request payload and request bit before polling for host
     * completion. The host accesses the same memory through a separate mapping;
     * volatile alone does not define an inter-thread/process synchronization
     * relationship. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* The dedicated host listener normally completes these shared-memory requests immediately.
     * A minimal load/test loop avoids AMD KVM's PAUSE-filter path and minimizes round-trip
     * latency; this path is used only while an SG host request is outstanding. */
    while (__atomic_load_n(&c->req_bit, __ATOMIC_ACQUIRE) != 0) {
    }

    return c->ret;
}

static inline void ensure_lock_ready(struct libos_lock *lock){
    if(lock_created(lock))
        return;
    if(!(create_lock(lock))){
        assert(false);
    }else {
        // log_always("[Gramine] Global lock initialized\n");
    }
}
