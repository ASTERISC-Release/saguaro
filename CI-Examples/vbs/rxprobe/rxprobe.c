/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Research probe (Documentation/devel/vbs-enclave/PLAN-rwx.md, "Runtime executable memory: options"):
 * each way to get executable memory at run time, one call at a time, with the errno and the line of
 * /proc/self/maps for the region afterwards. Generated code is called only when the call that should
 * have made it executable returned success. It reports; it makes no verdict and no claim of security.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

#if defined(__x86_64__)
static const unsigned char code[] = { 0xb8, 42, 0, 0, 0, 0xc3 }; /* mov eax, 42; ret */
#else
static const unsigned char code[] = { 0x40, 0x05, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6 }; /* mov w0,#42; ret */
#endif

#define RW  (PROT_READ | PROT_WRITE)
#define RX  (PROT_READ | PROT_EXEC)
#define RWX (PROT_READ | PROT_WRITE | PROT_EXEC)
#define ANON (MAP_PRIVATE | MAP_ANONYMOUS)

/* "lo-hi perms ..." of a /proc/self/maps line (no scanf: the static libc's long double code uses x18) */
static int parse_line(const char* l, unsigned long* lo, unsigned long* hi, char* perm) {
    unsigned long v[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        int n = 0;
        for (;; l++, n++) {
            int c = *l, d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else break;
            v[i] = v[i] * 16 + d;
        }
        if (!n || *l != (i ? ' ' : '-'))
            return 0;
        l++;
    }
    for (int i = 0; i < 4; i++)
        if (!l[i])
            return 0;
    memcpy(perm, l, 4);
    perm[4] = 0;
    *lo = v[0];
    *hi = v[1];
    return 1;
}

static void maps_of(const void* p) {
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    if (!f) {
        printf("    maps: cannot open\n");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        char perm[8];
        if (parse_line(line, &lo, &hi, perm) && lo <= (unsigned long)p && (unsigned long)p < hi) {
            printf("    maps: %s", line);
            fclose(f);
            return;
        }
    }
    printf("    maps: no line for %p\n", p);
    fclose(f);
}

static void* rep_map(const char* what, void* r) {
    if (r == MAP_FAILED) {
        printf("%-58s -> FAILED: %s\n", what, strerror(errno));
        return NULL;
    }
    printf("%-58s -> ok %p\n", what, r);
    return r;
}

static int rep_int(const char* what, int r) {
    if (r < 0)
        printf("%-58s -> FAILED: %s\n", what, strerror(errno));
    else
        printf("%-58s -> ok\n", what);
    return r;
}

/* called only after the call that should have made `p` executable succeeded */
static void try_run(void* p) {
    memcpy(p, code, sizeof(code));
    __builtin___clear_cache((char*)p, (char*)p + sizeof(code));
    printf("    calling the generated function (a hang here = the page is not executable)\n");
    fflush(stdout);
    printf("    generated function returned %d\n", ((int (*)(void))p)());
}

static void* new_rw(size_t sz) {
    void* p = mmap(NULL, sz, RW, ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t small = 4096, large = 32ul << 20;
    void* p;

    printf("== option 1: anonymous RW, write code, then execute-read\n");
    for (int i = 0; i < 2; i++) {
        size_t sz = i ? large : small;
        printf("-- size %zu\n", sz);
        p = rep_map("mmap(RW, anonymous)", mmap(NULL, sz, RW, ANON, -1, 0));
        if (!p)
            continue;
        memcpy(p, code, sizeof(code));
        if (rep_int("mprotect(R|X)", mprotect(p, sz, RX)) == 0)
            try_run(p);
        maps_of(p);
        munmap(p, sz);
    }
    printf("-- RW, then mprotect(R|W|X), then PROT_EXEC only\n");
    if ((p = new_rw(small))) {
        if (rep_int("mprotect(R|W|X)", mprotect(p, small, RWX)) == 0)
            try_run(p);
        maps_of(p);
        if (rep_int("mprotect(PROT_EXEC)", mprotect(p, small, PROT_EXEC)) == 0)
            printf("    (execute-only accepted)\n");
        munmap(p, small);
    }

    printf("== option 2: RWX (or any execute) from the start\n");
    for (int i = 0; i < 2; i++) {
        size_t sz = i ? large : small;
        printf("-- size %zu\n", sz);
        if ((p = rep_map("mmap(R|W|X, anonymous)", mmap(NULL, sz, RWX, ANON, -1, 0)))) {
            try_run(p);
            maps_of(p);
        }
        if ((p = rep_map("mmap(R|X, anonymous)", mmap(NULL, sz, RX, ANON, -1, 0))))
            maps_of(p);
        if ((p = rep_map("mmap(PROT_EXEC, anonymous)", mmap(NULL, sz, PROT_EXEC, ANON, -1, 0))))
            maps_of(p);
    }
    if ((p = new_rw(small))) {
        void* q = rep_map("mmap(MAP_FIXED over the RW page, R|W|X)",
                          mmap(p, small, RWX, ANON | MAP_FIXED, -1, 0));
        if (q)
            try_run(q);
    }

    printf("== option 3: mremap\n");
    if ((p = new_rw(small))) {
        memcpy(p, code, sizeof(code));
        void* q = rep_map("mremap(RW page, grow to 8 KiB, MAYMOVE)", mremap(p, small, 2 * small, MREMAP_MAYMOVE));
        if (q == MAP_FAILED || !q)
            q = p;
        if (rep_int("  then mprotect(R|X) on the result", mprotect(q, 2 * small, RX)) == 0)
            try_run(q);
        maps_of(q);
        /* the LibOS records the protection before the PAL answers: what does a remap of this mapping do? */
        void* r = rep_map("  mremap of the (recorded r-x) region, grow 8->12 KiB, MAYMOVE",
                            mremap(q, 2 * small, 3 * small, MREMAP_MAYMOVE));
        if (r)
            maps_of(r);
        void* d = rep_map("  mremap(MAYMOVE|DONTUNMAP) of the same", mremap(r ? r : q, r ? 3 * small : 2 * small, r ? 3 * small : 2 * small,
                                                                          MREMAP_MAYMOVE | MREMAP_DONTUNMAP));
        if (d)
            maps_of(d);
    }
    if ((p = new_rw(small))) {
        void* target = mmap(NULL, 4 * small, PROT_NONE, ANON | MAP_NORESERVE, -1, 0);
        if (target != MAP_FAILED) {
            void* q = rep_map("mremap(RW page, MAYMOVE|FIXED into a PROT_NONE reservation)",
                              mremap(p, small, small, MREMAP_MAYMOVE | MREMAP_FIXED, target));
            if (q) {
                memcpy(q, code, sizeof(code));
                if (rep_int("  then mprotect(R|X)", mprotect(q, small, RX)) == 0)
                    try_run(q);
            }
        }
    }
    printf("== option 4: memfd and file-backed mappings\n");
    int fd = memfd_create("code", 0);
    if (rep_int("memfd_create", fd) >= 0) {
        rep_int("ftruncate(4096)", ftruncate(fd, small));
        rep_int("write(code)", (int)write(fd, code, sizeof(code)));
        void* w = rep_map("mmap(memfd, RW, MAP_SHARED)", mmap(NULL, small, RW, MAP_SHARED, fd, 0));
        void* x = rep_map("mmap(memfd, R|X, MAP_SHARED) while mapped RW", mmap(NULL, small, RX, MAP_SHARED, fd, 0));
        if (x)
            try_run(x);
        x = rep_map("mmap(memfd, R|X, MAP_PRIVATE)", mmap(NULL, small, RX, MAP_PRIVATE, fd, 0));
        if (x)
            try_run(x);
        x = rep_map("mmap(memfd, R|W|X, MAP_SHARED)", mmap(NULL, small, RWX, MAP_SHARED, fd, 0));
        if (x)
            try_run(x);
        /* an executable mapping of a handle at offset 0 is taken by the VBS hook of libos_mmap.c (see
         * FINDINGS.md): the same mappings at another offset, and a larger one, reach the PAL */
        rep_int("ftruncate(8 MiB)", ftruncate(fd, 8ul << 20));
        pwrite(fd, code, sizeof(code), small);
        x = rep_map("mmap(memfd, R|X, MAP_SHARED, offset 4096)", mmap(NULL, small, RX, MAP_SHARED, fd, small));
        if (x)
            try_run(x);
        x = rep_map("mmap(memfd, R|X, MAP_PRIVATE, offset 4096)", mmap(NULL, small, RX, MAP_PRIVATE, fd, small));
        if (x)
            try_run(x);
        x = rep_map("mmap(memfd, R|X, MAP_PRIVATE, 8 MiB at offset 0)", mmap(NULL, 8ul << 20, RX, MAP_PRIVATE, fd, 0));
        if (x)
            try_run(x);
        void* pr = rep_map("mmap(memfd, RW, MAP_PRIVATE)", mmap(NULL, small, RW, MAP_PRIVATE, fd, 0));
        if (pr && rep_int("  then mprotect(R|X)", mprotect(pr, small, RX)) == 0)
            try_run(pr);
        if (w)
            munmap(w, small);
        close(fd);
    }
    int tf = open("/tmp/rxprobe.bin", O_RDWR | O_CREAT | O_TRUNC, 0700);
    if (rep_int("open(/tmp/rxprobe.bin) tmpfs", tf) >= 0) {
        rep_int("write(code)", (int)write(tf, code, sizeof(code)));
        rep_int("ftruncate(4096)", ftruncate(tf, small));
        void* x = rep_map("mmap(tmpfs file, R|X, MAP_PRIVATE)", mmap(NULL, small, RX, MAP_PRIVATE, tf, 0));
        if (x)
            try_run(x);
        x = rep_map("mmap(tmpfs file, R|X, MAP_SHARED)", mmap(NULL, small, RX, MAP_SHARED, tf, 0));
        if (x)
            try_run(x);
        rep_int("ftruncate(8192)", ftruncate(tf, 2 * small));
        x = rep_map("mmap(tmpfs file, R|X, MAP_PRIVATE, offset 4096)", mmap(NULL, small, RX, MAP_PRIVATE, tf, small));
        if (x)
            try_run(x);
        close(tf);
    }

    printf("== option 5: pages of the packed image that are not code\n");
    {
        static char data_page[8192] __attribute__((aligned(4096))) = { 1 };
        void* d = data_page;
        printf("-- a writable global of the program (packed data page)\n");
        maps_of(d);
        rep_int("mprotect(data page, R|X)", mprotect(d, small, RX));
        maps_of(d);
        rep_int("mprotect(data page, R|W|X)", mprotect(d, small, RWX));
        rep_map("mmap(MAP_FIXED over the data page, R|W|X)", mmap(d, small, RWX, ANON | MAP_FIXED, -1, 0));
        /* a range that straddles the end of the code and the start of the data */
        FILE* f = fopen("/proc/self/maps", "r");
        char line[512];
        unsigned long plo = 0, phi = 0, nlo = 0, nhi = 0;
        char pperm[8] = "", nperm[8];
        while (f && fgets(line, sizeof(line), f)) {
            if (!parse_line(line, &nlo, &nhi, nperm))
                continue;
            if (pperm[2] == 'x' && nlo == phi && nperm[0] == 'r' && nperm[1] == 'w') {
                printf("-- the end of the text and the start of the data are adjacent: %lx\n", nlo);
                rep_int("mprotect(last text page + first data page, R|X)",
                        mprotect((void*)(nlo - small), 2 * small, RX));
                break;
            }
            plo = nlo; phi = nhi; memcpy(pperm, nperm, sizeof(pperm));
        }
        (void)plo;
        if (f)
            fclose(f);
    }
    {
        /* the program's own code: move or grow a mapping that is executable */
        extern char __executable_start;
        void* t = &__executable_start;
        printf("== option 3b: mremap of the program's own text mapping (last: a failed attempt leaves the LibOS without the text VMA)\n");
        maps_of(t);
        FILE* f = fopen("/proc/self/maps", "r");
        char line[512];
        unsigned long lo = 0, hi = 0;
        while (f && fgets(line, sizeof(line), f)) {
            char perm[8];
            if (parse_line(line, &lo, &hi, perm) && perm[2] == 'x' && lo <= (unsigned long)&main &&
                    (unsigned long)&main < hi)
                break;
        }
        if (f)
            fclose(f);
        if (lo) {
            size_t len = hi - lo;
            void* m = rep_map("mremap(own text VMA, same size, MAYMOVE|DONTUNMAP)",
                              mremap((void*)lo, len, len, MREMAP_MAYMOVE | MREMAP_DONTUNMAP));
            if (m)
                maps_of(m);
            m = rep_map("mremap(own text VMA, grow by 4 KiB, MAYMOVE)",
                        mremap((void*)lo, len, len + small, MREMAP_MAYMOVE));
            if (m && m != (void*)lo)
                maps_of(m);
            maps_of(&main);
        }
    }

    printf("== done\n");
    return 0;
}
