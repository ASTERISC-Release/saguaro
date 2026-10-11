#include <stdint.h>
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <GL/gl.h>
#include <stdio.h>

#include "common.h"

void enable_syscall_logging(void) {
    long _offset = acquire_libos_lock();
    comm_page_t* c = comm_page(_offset);
    c->req_bit = SYSCALL_LOGGING_ENABLE;
    comm_sync_notify(c);
    relinquish_libos_lock(_offset);
}

void disable_syscall_logging(void) {
    long _offset = acquire_libos_lock();
    comm_page_t* c = comm_page(_offset);
    c->req_bit = SYSCALL_LOGGING_DISABLE;
    comm_sync_notify(c);
    relinquish_libos_lock(_offset);
}

void setup_syscall_comms(void) {
    fprintf(stderr, "\n========COMMS========\n");

    long _offset = acquire_libos_lock();
    /* Setup the communication region. */
    comm_page_t *addr = (comm_page_t*) mmap((void*)SYS_COMMS_ADDR, SYS_COMMS_SIZE, 
            PROT_READ | PROT_WRITE, 
            (MAP_ANONYMOUS | MAP_FIXED | MAP_PRIVATE), -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap COMMS_REGION");
        assert(0);
    }
    memset(addr, 0, SYS_COMMS_SIZE);
    
    addr[0].magic = 0x1234567812345678ULL;
    __sync_synchronize();
    
    while (addr[0].magic != 0x2) {
        __sync_synchronize();
        usleep(1000);
    }
    addr[0].magic = 0x1234567812345678ULL;
    fprintf(stderr, "[*] COMMS: 0x%llx\n", SYS_COMMS_ADDR);

    comm_page_t *c = comm_page(_offset);

    /* Setup the data region. */
    void* data = mmap(DATA_REGION, DATA_SIZE, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed, returning... (data = %p)\n", data);
        perror("mmap DATA_REGION");
        assert(0);
    }
    /* Anonymous mappings are already zero-filled; avoid large memset which is very slow once PAL
     * marks this region as UC/WC. */
    *(uint64_t*)data = 0x1234567812344678ULL;
    __sync_synchronize();
    while (*((uint64_t *)data) != 0x2) {
        __sync_synchronize();
        usleep(1000);
    }
    fprintf(stderr, "[*] DATA: 0x%lx\n", (uint64_t)data);

    /* Setup the hugepages data region. */
    volatile void* hugepage_data = mmap(HUGEPAGE_DATA_REGION, HUGE_DATA_SIZE,
                                        PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (hugepage_data == MAP_FAILED) {
        fprintf(stderr, "mmap failed (hugepages), returning... (data = %p)\n", data);
        perror("mmap HUGEPAGE_DATA_REGION");
        assert(0);
    }
    fprintf(stderr, "[*] HUGEPAGE DATA: 0x%lx\n", (uint64_t)hugepage_data);

    // Send hugepage data region address to listener
    c->p1 = (uint64_t)hugepage_data;
    /* Anonymous mappings are already zero-filled; avoid large memset which is very slow once PAL
     * marks this region as UC/WC. */
    *(volatile uint64_t*)hugepage_data = 0x1234567812344678ULL;
    __sync_synchronize();
    while (*((uint64_t *)hugepage_data) != 0x2) {
        __sync_synchronize();
        usleep(1000);
    }
    fprintf(stderr, "[*] HUGEPAGE DATA: 0x%lx\n", (uint64_t)hugepage_data);

    /* The data handshakes finish before the listener creates its host XCB state and enters the
     * request loop. Wait for that final readiness signal so the first real request cannot be
     * mistaken for setup state and cleared. */
    while (__atomic_load_n(&addr[0].magic, __ATOMIC_ACQUIRE) != 0x3)
        usleep(1000);
    __atomic_store_n(&addr[0].magic, COMM_MAGIC, __ATOMIC_RELEASE);

    /* Now initialize the other 15 comm pages, since QEMU thread 0 has finished setup_comm_data_regions */
    for (int i = 1; i < NUM_COMM_PAGES; i++) {
        addr[i].magic = 0x1234567812345678ULL;
    }
    __sync_synchronize();
    for (int i = 1; i < NUM_COMM_PAGES; i++) {
        while (addr[i].magic != 0x2) {
            __sync_synchronize();
            usleep(1000);
        }
        addr[i].magic = 0x1234567812345678ULL;
    }

    fprintf(stderr, "========COMMS========\n");
    relinquish_libos_lock(_offset);
}

long acquire_libos_lock(void){
    long ret = syscall(__NR_ACQUIRE_LLOCK);
    return ret;
}

long relinquish_libos_lock(long offset){
    long ret = syscall(__NR_RELINQUISH_LLOCK, offset);
    return ret;
}
