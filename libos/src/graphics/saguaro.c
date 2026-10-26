/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

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

/* Parse a size string like "1G", "512M" or a plain byte count; returns the
 * fallback when unset or malformed. */
static size_t parse_graphics_size(const char* value, size_t fallback) {
    if (!value || !*value) {
        return fallback;
    }
    char* end;
    unsigned long long v = strtoull(value, &end, 0);
    if (end == value || v == 0) {
        fprintf(stderr, "[.] bad SAGUARO_DATA_SIZE '%s'; using the default %lu\n",
                value, (unsigned long)fallback);
        return fallback;
    }
    if (*end == 'G' || *end == 'g') {
        v *= 1024ULL * 1024 * 1024;
        end++;
    } else if (*end == 'M' || *end == 'm') {
        v *= 1024ULL * 1024;
        end++;
    } else if (*end == 'K' || *end == 'k') {
        v *= 1024ULL;
        end++;
    }
    if (*end != '\0') {
        fprintf(stderr, "[.] bad SAGUARO_DATA_SIZE '%s'; using the default %lu\n",
                value, (unsigned long)fallback);
        return fallback;
    }
    return (size_t)v;
}

/* Parse a plain decimal or 0x-prefixed address; returns the fallback when unset
 * or malformed. */
static uint64_t parse_graphics_u64(const char* value, uint64_t fallback) {
    if (!value || !*value) {
        return fallback;
    }
    char* end;
    uint64_t v = (uint64_t)strtoull(value, &end, 0);
    if (end == value || *end != '\0' || v == 0) {
        fprintf(stderr, "[.] bad SAGUARO_HUGEPAGE_ADDR '%s'; ignoring it\n", value);
        return fallback;
    }
    return v;
}

void setup_syscall_comms(void) {
    fprintf(stderr, "\n========COMMS========\n");

    /* Geometry, from the launcher environment: SAGUARO_DATA_SIZE sizes the data
     * arena (the launcher also passes it to the PAL over fw_cfg, so the PAL's
     * reservation matches), SAGUARO_DATA_ADDR is where it starts (default: the
     * compiled 4G anchor), and SAGUARO_HUGEPAGE_ADDR gives the hugepage
     * arena's guest physical address directly -- a hugepages deployment points
     * it at the dedicated hugetlb bucket backend; when unset the hugepage
     * arena follows the data arena. */
    size_t data_size = parse_graphics_size(getenv("SAGUARO_DATA_SIZE"), DATA_SIZE);
    uint64_t data_addr = parse_graphics_u64(getenv("SAGUARO_DATA_ADDR"), (uint64_t)(uintptr_t)DATA_REGION);
    DATA_REGION = (void *)data_addr;
    uint64_t huge_addr = parse_graphics_u64(getenv("SAGUARO_HUGEPAGE_ADDR"), 0);
    if (huge_addr && huge_addr >= (uint64_t)(uintptr_t)DATA_REGION + data_size) {
        HUGEPAGE_DATA_REGION = (void *)huge_addr;
    } else {
        HUGEPAGE_DATA_REGION = (void *)((uintptr_t)DATA_REGION + data_size);
    }

    /* The communication region first: it is one physical page shared by every process of the
     * guest, and its guest-private part says whether another process has already set the session
     * up (graphics_comm_abi.h). Not cleared here: a second process would wipe the first one's
     * slots and the state. */
    comm_page_t *addr = (comm_page_t*) mmap((void*)SYS_COMMS_ADDR, SYS_COMMS_SIZE,
            PROT_READ | PROT_WRITE,
            (MAP_ANONYMOUS | MAP_FIXED | MAP_PRIVATE), -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap COMMS_REGION");
        assert(0);
    }
    graphics_guest_state_t *state =
        (graphics_guest_state_t*)((char*)addr + GRAPHICS_GUEST_STATE_OFFSET);
    uint64_t expected = GRAPHICS_SESSION_NONE;
    if (!__atomic_compare_exchange_n(&state->session_state, &expected,
                                     GRAPHICS_SESSION_SETTING_UP, /*weak=*/false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* another process owns the session: wait until it is up, take its geometry and map the
         * arenas (fixed physical ranges, no handshake: the listener knows them already) */
        while (__atomic_load_n(&state->session_state, __ATOMIC_ACQUIRE) != GRAPHICS_SESSION_UP)
            usleep(1000);
        DATA_REGION = (void *)(uintptr_t)state->data_addr;
        HUGEPAGE_DATA_REGION = (void *)(uintptr_t)state->huge_addr;
        data_size = state->data_size;
        void* data = mmap(DATA_REGION, data_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        void* hugepage_data = mmap(HUGEPAGE_DATA_REGION, state->huge_size,
                                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                   -1, 0);
        if (data == MAP_FAILED || hugepage_data == MAP_FAILED) {
            perror("mmap of the arenas of the running session");
            assert(0);
        }
        fprintf(stderr, "[*] COMMS: joined the guest's graphics session (data 0x%lx, hugepage "
                "0x%lx)\n========COMMS========\n", (uint64_t)data, (uint64_t)hugepage_data);
        return;
    }

    long _offset = acquire_libos_lock();
    /* the first process: the request slots start clean (the state area keeps its word) */
    memset(addr, 0, NUM_COMM_PAGES * sizeof(comm_page_t));

    /* the ABI version goes with the magic; the listener answers with its own in p5 */
    addr[0].p4 = GRAPHICS_COMM_ABI_VERSION;
    addr[0].magic = 0x1234567812345678ULL;
    __sync_synchronize();

    while (addr[0].magic != 0x2) {
        __sync_synchronize();
        usleep(1000);
    }
    if (addr[0].p5 != GRAPHICS_COMM_ABI_VERSION) {
        fprintf(stderr, "[!] graphics: the host listener speaks comm ABI version %llu, this guest "
                "version %llu (graphics_comm_abi.h): rebuild the graphics QEMU against this tree "
                "(scripts/qemu.sh build graphics)\n", (unsigned long long)addr[0].p5,
                (unsigned long long)GRAPHICS_COMM_ABI_VERSION);
        abort();
    }
    addr[0].p4 = 0;
    addr[0].p5 = 0;
    addr[0].magic = 0x1234567812345678ULL;
    fprintf(stderr, "[*] COMMS: 0x%llx\n", SYS_COMMS_ADDR);

    comm_page_t *c = comm_page(_offset);

    /* Setup the data region. */
    void* data = mmap(DATA_REGION, data_size,
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
    fprintf(stderr, "[*] HUGEPAGE DATA: 0x%lx (data arena %lu MiB)\n",
            (uint64_t)hugepage_data, (unsigned long)(data_size / 1024 / 1024));

    /* Send the geometry to the listener: p1 = hugepage guest address, p2 =
     * data-arena size, p3 = hugepage-arena size. p1 is written last; the
     * listener waits for it before reading the sizes. */
    c->p2 = (uint64_t)data_size;
    c->p3 = (uint64_t)HUGE_DATA_SIZE;
    __sync_synchronize();
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

    /* the session is up: publish the geometry for the processes that join it */
    state->data_addr = (uint64_t)(uintptr_t)DATA_REGION;
    state->data_size = (uint64_t)data_size;
    state->huge_addr = (uint64_t)hugepage_data;
    state->huge_size = (uint64_t)HUGE_DATA_SIZE;
    __atomic_store_n(&state->session_state, GRAPHICS_SESSION_UP, __ATOMIC_RELEASE);

    fprintf(stderr, "========COMMS========\n");
    relinquish_libos_lock(_offset);
}

void* graphics_physmem(size_t size, uint64_t* out_pa) {
    long ret = syscall(__NR_GRAPHICS_PHYSMEM, size, out_pa);
    if (ret < 0 && ret > -4096)
        return NULL;
    return (void*)ret;
}

long acquire_libos_lock(void){
    long ret = syscall(__NR_ACQUIRE_LLOCK);
    return ret;
}

long relinquish_libos_lock(long offset){
    long ret = syscall(__NR_RELINQUISH_LLOCK, offset);
    return ret;
}
