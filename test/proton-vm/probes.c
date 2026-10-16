/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Probes for the facts that decide whether Wine can run under gramine-vm (phase 0 of
 * Documentation/devel/proton-vm/PLAN.md). Each probe prints `PROBE <name>: <result>` lines and
 * exits; `probes all` runs every probe in a forked child (a crash or hang in one probe does not
 * stop the others; without fork they run inline). Run natively first (every probe passes on Linux),
 * then under gramine-direct and gramine-vm (test/proton-vm/run-probes.sh).
 *
 * What Wine does (dlls/ntdll/unix/signal_x86_64.c, virtual.c, loader/preloader.c, checked
 * 2026-10-15) and the probe that checks it:
 *   gs        arch_prctl(ARCH_SET_GS, teb) per thread; NtCurrentTeb() is `mov %gs:0x30`
 *   fs32      the 32-bit TEB selector for new WoW64: set_thread_area / modify_ldt, loaded into %fs
 *   cs32      new WoW64 runs 32-bit code through a far jump to selector 0x23 (hard-coded on Linux)
 *   fixed     the preloader's reservations at fixed addresses (0x10000, 0x110000 to 0x200000000)
 *             and ntdll's top-down area 0x7ff000000000-0x7ff600000000, with MAP_FIXED_NOREPLACE
 *   huge      PROT_NONE MAP_NORESERVE reservations of many GiB; commit by touching
 *   noreplace MAP_FIXED_NOREPLACE semantics (EEXIST on a mapped range)
 *   futexv    futex_waitv (fsync)
 *   eventfd   eventfd with EFD_SEMAPHORE, shared with a child (esync)
 *   scm       SCM_RIGHTS over a Unix socketpair to a child (wineserver hands fds to clients), and
 *             a memfd shared through it (the server's shared memory)
 *   sud       prctl(PR_SET_SYSCALL_USER_DISPATCH) and SIGSYS (Wine warns and goes on without it)
 *   signals   SIGSEGV/SIGILL/SIGTRAP/SIGFPE with SA_SIGINFO|SA_ONSTACK on an alternate stack,
 *             resuming at a changed RIP; SIGUSR1 to another thread blocked in a syscall, to a
 *             spinning thread, and to a thread of another process (the server suspends threads
 *             with tgkill); SIGCHLD
 *   threads   how many threads a process can create
 *   procs     how many processes
 *   proc      the /proc and /sys files Wine reads
 *   misc      prctl, rlimits, personality, affinity, clocks, getrandom, pipe2, epoll, memfd, mremap,
 *             madvise, sched_yield, membarrier
 */

#define _GNU_SOURCE
#include <asm/ldt.h>
#include <asm/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/membarrier.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#define PR_SYS_DISPATCH_OFF 0
#define PR_SYS_DISPATCH_ON 1
#endif
#ifndef __NR_futex_waitv
#define __NR_futex_waitv 449
#endif
#ifndef SYSCALL_DISPATCH_FILTER_ALLOW
#define SYSCALL_DISPATCH_FILTER_ALLOW 0
#define SYSCALL_DISPATCH_FILTER_BLOCK 1
#endif

#define P(name, ...) do { printf("PROBE " name ": " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define E() strerror(errno)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long rss_kib(void) {
    FILE* f = fopen("/proc/self/status", "r");
    char line[256];
    long kib = -1;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "VmRSS: %ld", &kib) == 1) break;
    fclose(f);
    return kib;
}

/* ---- a SIGSEGV/SIGILL/SIGBUS catcher for the probes that may fault ---- */
static sigjmp_buf g_jmp;
static volatile int g_fault_sig;
static void fault_jump(int sig, siginfo_t* si, void* uc) {
    (void)si; (void)uc;
    g_fault_sig = sig;
    siglongjmp(g_jmp, 1);
}
static void catch_faults(int on) {
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_jump;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    if (!on) { sa.sa_handler = SIG_DFL; sa.sa_flags = 0; }
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
static void setup_altstack(void) {
    static char stack[65536];
    stack_t ss = {.ss_sp = stack, .ss_size = sizeof(stack)};
    sigaltstack(&ss, NULL);
}

/* ------------------------------------------------------------------------------------------- */
/* gs: arch_prctl(ARCH_SET_GS) and reading through %gs, also after syscalls, a context switch and
 * in a signal handler; wrgsbase/rdgsbase as the alternative (needs CR4.FSGSBASE) */
static uint64_t read_gs0(void) {
    uint64_t v;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(v));
    return v;
}
static uint64_t g_gs_in_handler;
static void gs_handler(int sig) {
    (void)sig;
    g_gs_in_handler = read_gs0();
}
static void* gs_thread(void* arg) {
    uint64_t* block = arg;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, block) < 0) return (void*)-1L;
    usleep(1000);
    return (void*)(uintptr_t)(read_gs0() == block[0]);
}
static void probe_gs(void) {
    static uint64_t block[8] = {0x1122334455667788ull};
    static uint64_t block2[8] = {0x2222222222222222ull};
    uint64_t old = 0;
    syscall(SYS_arch_prctl, ARCH_GET_GS, &old);
    P("gs", "ARCH_GET_GS before: base %#lx", (unsigned long)old);
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, block) < 0) {
        P("gs", "FAIL arch_prctl(ARCH_SET_GS): %s", E());
    } else {
        int ok1 = read_gs0() == block[0];
        usleep(2000);                          /* a syscall and a chance for a context switch */
        int ok2 = read_gs0() == block[0];
        signal(SIGUSR2, gs_handler);
        raise(SIGUSR2);
        int ok3 = g_gs_in_handler == block[0];
        int ok4 = read_gs0() == block[0];
        pthread_t t;
        void* ret = NULL;
        pthread_create(&t, NULL, gs_thread, block2);
        pthread_join(t, &ret);
        int ok5 = read_gs0() == block[0];
        P("gs", "%s ARCH_SET_GS: read %d, after syscall %d, in handler %d, after handler %d, other thread %ld, main after %d",
          ok1 && ok2 && ok3 && ok4 && ok5 && ret == (void*)1 ? "PASS" : "FAIL",
          ok1, ok2, ok3, ok4, (long)(intptr_t)ret, ok5);
        syscall(SYS_arch_prctl, ARCH_SET_GS, old);
    }
}
/* wrgsbase: Linux allows it in user mode on CPUs with FSGSBASE (kernel 5.9+); a #UD otherwise */
static void probe_wrgsbase(void) {
    static uint64_t block[8] = {0x1122334455667788ull};
    setup_altstack();
    catch_faults(1);
    if (sigsetjmp(g_jmp, 1) == 0) {
        uint64_t saved;
        __asm__ volatile("rdgsbase %0" : "=r"(saved));
        __asm__ volatile("wrgsbase %0" ::"r"(block));
        int ok = read_gs0() == block[0];
        __asm__ volatile("wrgsbase %0" ::"r"(saved));
        P("gs", "%s wrgsbase/rdgsbase in user mode: works (read %d)", ok ? "PASS" : "FAIL", ok);
    } else {
        P("gs", "INFO wrgsbase in user mode: signal %d (CR4.FSGSBASE off)", g_fault_sig);
    }
    catch_faults(0);
}

/* ------------------------------------------------------------------------------------------- */
/* fs32: a 32-bit data segment for the WoW64 TEB through set_thread_area (GDT) and modify_ldt
 * (LDT), loaded into %fs, read through %fs:0, then the TLS base restored with ARCH_SET_FS */
static int try_fs_selector(unsigned sel, uint32_t* base, const char* how) {
    unsigned long fsbase = 0;
    uint32_t v = 0;
    int ok;
    syscall(SYS_arch_prctl, ARCH_GET_FS, &fsbase);
    if (sigsetjmp(g_jmp, 1) == 0) {
        __asm__ volatile("movw %w0, %%fs" ::"r"(sel));
        __asm__ volatile("movl %%fs:0, %0" : "=r"(v));
        /* restore the TLS base with a raw syscall: libc must not run while %fs is not the TLS */
        __asm__ volatile("syscall" ::"a"(SYS_arch_prctl), "D"(ARCH_SET_FS), "S"(fsbase) : "rcx", "r11", "memory");
        ok = v == *base;
        P("fs32", "%s %s: selector %#x loaded into %%fs, read %#x (want %#x)", ok ? "PASS" : "FAIL", how, sel, v, *base);
        return ok;
    }
    syscall(SYS_arch_prctl, ARCH_SET_FS, fsbase);
    P("fs32", "FAIL %s: loading selector %#x into %%fs raised signal %d", how, sel, g_fault_sig);
    return 0;
}
static void probe_fs32(void) {
    uint32_t* base = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (base == MAP_FAILED) {
        base = mmap((void*)0x30000000, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        P("fs32", "INFO MAP_32BIT failed, fixed low page %s", base == MAP_FAILED ? E() : "ok");
    }
    if (base == MAP_FAILED || (uintptr_t)base >= 0x100000000ull) {
        P("fs32", "FAIL no page below 4 GiB for the segment base");
        return;
    }
    *base = 0xfeedc0de;
    struct user_desc d = {.entry_number = -1, .base_addr = (uint32_t)(uintptr_t)base, .limit = 0xfff,
                          .seg_32bit = 1, .read_exec_only = 0, .limit_in_pages = 0, .seg_not_present = 0, .useable = 1};
    setup_altstack();
    catch_faults(1);
    if (syscall(SYS_set_thread_area, &d) < 0)
        P("fs32", "FAIL set_thread_area: %s", E());
    else
        try_fs_selector((d.entry_number << 3) | 3, base, "set_thread_area");
    struct user_desc l = d;
    l.entry_number = 0;
    if (syscall(SYS_modify_ldt, 0x11, &l, sizeof(l)) < 0)
        P("fs32", "FAIL modify_ldt: %s", E());
    else
        try_fs_selector((0 << 3) | 7, base, "modify_ldt");
    catch_faults(0);
}

/* ------------------------------------------------------------------------------------------- */
/* cs32: far-return into a 32-bit code segment, run a few instructions, far-return back */
extern void cs32_trampoline(void* page);
__asm__(".text\n"
        ".globl cs32_trampoline\n"
        ".type cs32_trampoline,@function\n"
        "cs32_trampoline:\n"
        "  push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "  mov %rsp, 0xf0(%rdi)\n"          /* the 64-bit stack, restored by the return stub */
        "  lea 0xf80(%rdi), %rsp\n"         /* a stack inside the low page for the 32-bit code */
        "  movzwl 0xf8(%rdi), %ecx\n"       /* the 32-bit code selector under test */
        "  push %rcx\n"
        "  lea 0x40(%rdi), %rcx\n"
        "  push %rcx\n"
        "  lretq\n"                         /* -> 32-bit code at page+0x40 */
        ".size cs32_trampoline, .-cs32_trampoline\n");
static void put32(unsigned char* p, uint32_t v) { memcpy(p, &v, 4); }
static int try_cs32(unsigned char* page, unsigned sel) {
    uint16_t cs64;
    __asm__("movw %%cs, %0" : "=r"(cs64));
    memset(page, 0xcc, 0x1000);
    /* 32-bit code at +0x40: mov $0x1234,%eax; mov %cs,%ebx; push $cs64; push $(page+0x80); lret */
    unsigned char* c = page + 0x40;
    *c++ = 0xb8; put32(c, 0x1234); c += 4;
    *c++ = 0x8c; *c++ = 0xcb;
    *c++ = 0x68; put32(c, cs64); c += 4;
    *c++ = 0x68; put32(c, (uint32_t)(uintptr_t)(page + 0x80)); c += 4;
    *c++ = 0xcb;
    /* 64-bit code at +0x80: mov page+0xf0,%rsp; mov %eax,page+0xe0; mov %ebx,page+0xe4;
     * pop r15..rbx; ret */
    c = page + 0x80;
    *c++ = 0x48; *c++ = 0x8b; *c++ = 0x24; *c++ = 0x25; put32(c, (uint32_t)(uintptr_t)(page + 0xf0)); c += 4;
    *c++ = 0x89; *c++ = 0x04; *c++ = 0x25; put32(c, (uint32_t)(uintptr_t)(page + 0xe0)); c += 4;
    *c++ = 0x89; *c++ = 0x1c; *c++ = 0x25; put32(c, (uint32_t)(uintptr_t)(page + 0xe4)); c += 4;
    const unsigned char pops[] = {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3};
    memcpy(c, pops, sizeof(pops));
    *(uint16_t*)(page + 0xf8) = (uint16_t)sel;
    *(uint32_t*)(page + 0xe0) = 0;
    *(uint32_t*)(page + 0xe4) = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        cs32_trampoline(page);
        uint32_t eax = *(uint32_t*)(page + 0xe0), ebx = *(uint32_t*)(page + 0xe4);
        int ok = eax == 0x1234 && (ebx & 0xffff) == sel;
        P("cs32", "%s selector %#x: ran 32-bit code (eax %#x, cs read back %#x), returned to 64-bit cs %#x",
          ok ? "PASS" : "FAIL", sel, eax, ebx, cs64);
        return ok;
    }
    P("cs32", "FAIL selector %#x: far return raised signal %d", sel, g_fault_sig);
    return 0;
}
static void probe_cs32(void) {
    uint16_t cs64, ss;
    __asm__("movw %%cs, %0" : "=r"(cs64));
    __asm__("movw %%ss, %0" : "=r"(ss));
    P("cs32", "INFO 64-bit cs %#x ss %#x (Linux: cs 0x33 ss 0x2b, 32-bit cs 0x23)", cs64, ss);
    unsigned char* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (page == MAP_FAILED)
        page = mmap((void*)0x31000000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (page == MAP_FAILED || (uintptr_t)page >= 0x100000000ull) {
        P("cs32", "FAIL no executable page below 4 GiB: %s", E());
        return;
    }
    setup_altstack();
    catch_faults(1);
    if (!try_cs32(page, 0x23)) {            /* Linux's selector, hard-coded in Wine */
        try_cs32(page, 0x1b);               /* the Gramine VM kernel's 32-bit user code entry */
        try_cs32(page, 0x2b);
    }
    catch_faults(0);
}

/* ------------------------------------------------------------------------------------------- */
/* fixed: Wine's fixed-address reservations */
static void try_fixed(const char* what, uintptr_t addr, size_t size, int touch) {
    void* p = mmap((void*)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        P("fixed", "FAIL %s: mmap(%#lx, %#lx, MAP_FIXED_NOREPLACE) %s", what, (unsigned long)addr, (unsigned long)size, E());
        return;
    }
    if (p != (void*)addr) {
        P("fixed", "FAIL %s: asked %#lx got %p (old kernel semantics)", what, (unsigned long)addr, p);
        munmap(p, size);
        return;
    }
    int ok = 1;
    if (touch) {
        size_t chunk = size >= 2 << 20 ? 1 << 20 : 4096;
        char* mid = (char*)addr + (size / 2 & ~0xfffUL);
        if (mprotect(mid, chunk, PROT_READ | PROT_WRITE) < 0) {
            P("fixed", "FAIL %s: mprotect in the middle: %s", what, E());
            ok = 0;
        } else {
            mid[0] = 1;
            mid[chunk - 1] = 2;
            ok = mid[0] == 1 && mid[chunk - 1] == 2;
        }
    }
    P("fixed", "%s %s: reserved %#lx+%#lx%s", ok ? "PASS" : "FAIL", what, (unsigned long)addr, (unsigned long)size, touch ? ", committed a piece inside" : "");
    munmap(p, size);
}
static void probe_fixed(void) {
    try_fixed("DOS area (preloader)", 0x10000, 0xf0000, 1);
    try_fixed("low 8G (preloader)", 0x110000, 0x1ffef0000ull, 1);
    try_fixed("low 4G only", 0x110000, 0xfeef0000ull, 1);
    try_fixed("top-down area (ntdll)", 0x7ff000000000ull, 0x600000000ull, 1);
    try_fixed("address space limit probe", 0x7fffffff0000ull, 0x1000, 0);
    try_fixed("user shared data + dispatcher", 0x7ffe0000, 0x2000, 1);
    void* p = mmap((void*)0x7ffe0000, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    P("fixed", "%s MAP_FIXED at 0x7ffe0000: %p", p == (void*)0x7ffe0000 ? "PASS" : "FAIL", p);
    if (p != MAP_FAILED) munmap(p, 0x1000);
}

/* ------------------------------------------------------------------------------------------- */
/* huge: big PROT_NONE reservations and commit by touching */
static void probe_huge(void) {
    static const size_t gib = 1ull << 30;
    size_t sizes[] = {4, 16, 64, 256, 1024};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        double t0 = now_s();
        void* p = mmap(NULL, sizes[i] * gib, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        double t1 = now_s();
        if (p != MAP_FAILED) munmap(p, sizes[i] * gib);
        double t2 = now_s();
        P("huge", "%s reserve %zu GiB PROT_NONE: %s (mmap %.3f s, munmap %.3f s)", p == MAP_FAILED ? "FAIL" : "PASS", sizes[i], p == MAP_FAILED ? E() : "ok", t1 - t0, t2 - t1);
    }
    /* commit: 4 GiB RW NORESERVE, touch a byte per 2 MiB; then 1 GiB a byte per 4 KiB */
    double tm0 = now_s();
    char* p = mmap(NULL, 4 * gib, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        P("huge", "FAIL mmap 4 GiB RW: %s", E());
        return;
    }
    P("huge", "PASS mmap 4 GiB RW NORESERVE in %.3f s", now_s() - tm0);
    long rss0 = rss_kib();
    double t0 = now_s();
    for (size_t off = 0; off < 4 * gib; off += 2 << 20) p[off] = 1;
    double t1 = now_s();
    P("huge", "PASS 4 GiB RW: touched one byte per 2 MiB (2048 pages) in %.3f s, RSS %ld -> %ld KiB", t1 - t0, rss0, rss_kib());
    t0 = now_s();
    for (size_t off = 0; off < gib; off += 4096) p[off] = 1;
    t1 = now_s();
    P("huge", "PASS 1 GiB of it: touched every 4 KiB page (262144 faults) in %.3f s (%.2f us/page), RSS %ld KiB", t1 - t0, (t1 - t0) / 262144 * 1e6, rss_kib());
    t0 = now_s();
    int r = madvise(p, gib, MADV_DONTNEED);
    t1 = now_s();
    P("huge", "%s madvise(MADV_DONTNEED) 1 GiB: %s in %.3f s, RSS %ld KiB, byte now %d", r == 0 ? "PASS" : "FAIL", r == 0 ? "ok" : E(), t1 - t0, rss_kib(), p[0]);
    t0 = now_s();
    munmap(p, 4 * gib);
    P("huge", "PASS munmap 4 GiB in %.3f s", now_s() - t0);
}

/* ------------------------------------------------------------------------------------------- */
static void probe_noreplace(void) {
    void* a = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* b = mmap(a, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    int e = errno;
    P("noreplace", "%s on a mapped range: %s%s", b == MAP_FAILED && e == EEXIST ? "PASS" : "FAIL",
      b == MAP_FAILED ? strerror(e) : "mapped", b != MAP_FAILED && b != a ? " at another address (old kernel semantics)" : "");
    if (b != MAP_FAILED && b != a) munmap(b, 4096);
    munmap(a, 8192);
    void* c = mmap(a, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    P("noreplace", "%s on a free range: got %p (asked %p)", c == a ? "PASS" : "FAIL", c, a);
    if (c != MAP_FAILED) {
        void* d = mmap(a, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        P("noreplace", "%s MAP_FIXED over it: %p", d == a ? "PASS" : "FAIL", d);
        munmap(a, 8192);
    }
    /* hint without flags, as Wine's anon_mmap_tryfixed fallback */
    void* h = mmap((void*)0x400000000000ull, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    P("noreplace", "INFO hint 0x400000000000 without MAP_FIXED: got %p", h);
    if (h != MAP_FAILED) munmap(h, 4096);
}

/* ------------------------------------------------------------------------------------------- */
struct futex_waitv_entry { uint64_t val; uint64_t uaddr; uint32_t flags; uint32_t reserved; };
static uint32_t g_fw[2];
static void* futexv_waker(void* arg) {
    (void)arg;
    usleep(20000);
    __atomic_store_n(&g_fw[1], 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &g_fw[1], FUTEX_WAKE, 1, NULL, NULL, 0);
    return NULL;
}
static void probe_futexv(void) {
    struct futex_waitv_entry w[2] = {{0, (uintptr_t)&g_fw[0], FUTEX_32, 0}, {0, (uintptr_t)&g_fw[1], FUTEX_32, 0}};
    g_fw[0] = 1;
    long r = syscall(__NR_futex_waitv, w, 2, 0, NULL, CLOCK_MONOTONIC);
    int e = errno;
    if (r < 0 && e == ENOSYS) {
        P("futexv", "FAIL futex_waitv: ENOSYS");
        return;
    }
    P("futexv", "%s value mismatch: %s", r < 0 && e == EAGAIN ? "PASS" : "FAIL", r < 0 ? strerror(e) : "returned");
    g_fw[0] = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 20000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    r = syscall(__NR_futex_waitv, w, 2, 0, &ts, CLOCK_MONOTONIC);
    e = errno;
    P("futexv", "%s timeout (absolute, 20 ms): %s", r < 0 && e == ETIMEDOUT ? "PASS" : "FAIL", r < 0 ? strerror(e) : "returned");
    pthread_t t;
    pthread_create(&t, NULL, futexv_waker, NULL);
    r = syscall(__NR_futex_waitv, w, 2, 0, NULL, CLOCK_MONOTONIC);
    e = errno;
    pthread_join(t, NULL);
    P("futexv", "%s woken on the second futex: returned %ld (%s)", r == 1 ? "PASS" : "FAIL", r, r < 0 ? strerror(e) : "index");
}

/* ------------------------------------------------------------------------------------------- */
static void probe_eventfd(void) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    if (fd < 0) {
        P("eventfd", "FAIL eventfd(EFD_SEMAPHORE): %s", E());
        return;
    }
    uint64_t v = 3, got = 0;
    write(fd, &v, 8);
    read(fd, &got, 8);
    struct pollfd pfd = {fd, POLLIN, 0};
    int pr = poll(&pfd, 1, 0);
    P("eventfd", "%s semaphore mode: wrote 3, read %lu, poll after one read %d", got == 1 && pr == 1 ? "PASS" : "FAIL", (unsigned long)got, pr);
    read(fd, &got, 8);
    read(fd, &got, 8);
    pr = poll(&pfd, 1, 0);
    pid_t pid = fork();
    if (pid == 0) {
        usleep(20000);
        v = 1;
        write(fd, &v, 8);
        _exit(0);
    }
    if (pid < 0) {
        P("eventfd", "INFO fork failed (%s): no cross-process test", E());
        return;
    }
    int pr2 = poll(&pfd, 1, 2000);
    got = 0;
    read(fd, &got, 8);
    waitpid(pid, NULL, 0);
    P("eventfd", "%s across fork: empty poll %d, child's write seen by poll %d, read %lu", pr == 0 && pr2 == 1 && got == 1 ? "PASS" : "FAIL", pr, pr2, (unsigned long)got);
    close(fd);
}

/* ------------------------------------------------------------------------------------------- */
static int send_fds(int sock, int* fds, int n) {
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    char buf[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}
static int recv_fds(int sock, int* fds, int n) {
    char byte;
    struct iovec iov = {&byte, 1};
    char buf[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    if (recvmsg(sock, &msg, 0) != 1) return -1;
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -2;
    int got = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    memcpy(fds, CMSG_DATA(c), sizeof(int) * (got < n ? got : n));
    return got;
}
static void probe_scm(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        P("scm", "FAIL socketpair: %s", E());
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        P("scm", "INFO fork failed (%s): no cross-process test", E());
        return;
    }
    if (pid == 0) {
        close(sv[0]);
        int pfd[2];
        pipe2(pfd, O_CLOEXEC);
        int mfd = memfd_create("probe", MFD_CLOEXEC);
        int efd = eventfd(0, EFD_CLOEXEC);
        int shmfd = shm_open("/probe-scm", O_RDWR | O_CREAT, 0600);
        if (mfd < 0) mfd = shmfd;
        ftruncate(mfd, 4096);
        uint32_t* m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (m != MAP_FAILED) m[0] = 0xabcdef01;
        int fds[3] = {pfd[0], mfd, efd};
        int r = send_fds(sv[1], fds, 3);
        if (r != 0) P("scm", "FAIL child: sendmsg(SCM_RIGHTS) %s", E());
        write(pfd[1], "ping", 4);
        uint64_t one = 1;
        write(efd, &one, 8);
        usleep(50000);
        if (m != MAP_FAILED) m[1] = 0x12345678;   /* after the parent mapped it */
        _exit(r == 0 ? 0 : 1);
    }
    close(sv[1]);
    int fds[3] = {-1, -1, -1};
    int n = recv_fds(sv[0], fds, 3);
    if (n != 3) {
        P("scm", "FAIL recvmsg SCM_RIGHTS: got %d (%s)", n, n < 0 ? E() : "fewer fds");
        waitpid(pid, NULL, 0);
        return;
    }
    char buf[8] = {0};
    int rn = read(fds[0], buf, 4);
    uint32_t* m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fds[1], 0);
    uint32_t first = m != MAP_FAILED ? m[0] : 0;
    uint64_t ev = 0;
    read(fds[2], &ev, 8);
    int status = 0;
    waitpid(pid, &status, 0);
    uint32_t second = m != MAP_FAILED ? m[1] : 0;
    P("scm", "%s 3 fds over SCM_RIGHTS: pipe read %d '%s', memfd mapped %s first %#x second (written after our map) %#x, eventfd %lu, child exit %d",
      rn == 4 && first == 0xabcdef01 && second == 0x12345678 && ev == 1 ? "PASS" : "FAIL", rn, buf,
      m == MAP_FAILED ? E() : "ok", first, second, (unsigned long)ev, WEXITSTATUS(status));
    shm_unlink("/probe-scm");
}

/* ------------------------------------------------------------------------------------------- */
/* unixsock: what wineserver and its clients do: the server process binds a Unix socket by a
 * relative name after chdir into the server directory and listens; a client process chdirs there,
 * lstat()s the name (it must be S_IFSOCK) and connects */
#include <sys/stat.h>
#include <sys/un.h>
static void probe_unixsock(void) {
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/probe-sock-%d", getpid());
    mkdir(dir, 0700);
    char path[128];
    snprintf(path, sizeof(path), "%s/socket", dir);
    unlink(path);
    int ready[2];
    pipe2(ready, O_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        P("unixsock", "INFO fork failed (%s)", E());
        return;
    }
    if (pid == 0) {
        chdir(dir);
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        strcpy(a.sun_path, "socket");
        int rb = bind(s, (struct sockaddr*)&a, offsetof(struct sockaddr_un, sun_path) + 7);
        int eb = errno;
        int rl = listen(s, 5);
        if (rb != 0) P("unixsock", "FAIL server: bind: %s", strerror(eb));
        struct stat st;
        int rs = lstat("socket", &st);
        P("unixsock", "%s server: bind %d listen %d, own lstat(\"socket\") %d%s", rb == 0 && rl == 0 ? "PASS" : "FAIL", rb, rl, rs,
          rs == 0 ? (S_ISSOCK(st.st_mode) ? " S_IFSOCK" : " not a socket") : "");
        write(ready[1], "r", 1);
        struct pollfd pfd = {s, POLLIN, 0};
        if (poll(&pfd, 1, 3000) == 1) {
            int c = accept(s, NULL, NULL);
            char b;
            if (read(c, &b, 1) == 1) write(c, "k", 1);
            close(c);
        }
        _exit(0);
    }
    char r;
    read(ready[0], &r, 1);
    chdir(dir);
    struct stat st;
    int rs = lstat("socket", &st);
    int rs2 = lstat(path, &st);
    P("unixsock", "%s client: lstat(\"socket\") %s, lstat(absolute) %s%s", rs == 0 && S_ISSOCK(st.st_mode) ? "PASS" : "FAIL",
      rs == 0 ? "ok" : E(), rs2 == 0 ? "ok" : E(), rs2 == 0 ? (S_ISSOCK(st.st_mode) ? " S_IFSOCK" : " not S_IFSOCK") : "");
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, "socket");
    int rc = connect(s, (struct sockaddr*)&a, offsetof(struct sockaddr_un, sun_path) + 7);
    int e1 = errno;
    if (rc != 0) {
        close(s);
        s = socket(AF_UNIX, SOCK_STREAM, 0);
        strcpy(a.sun_path, path);
        rc = connect(s, (struct sockaddr*)&a, sizeof(a));
        e1 = errno;
        P("unixsock", "INFO client: relative connect failed, absolute connect %s", rc == 0 ? "ok" : strerror(e1));
    }
    char k = 0;
    if (rc == 0) {
        write(s, "p", 1);
        struct pollfd pfd = {s, POLLIN, 0};
        if (poll(&pfd, 1, 3000) == 1) read(s, &k, 1);
    }
    P("unixsock", "%s client: connect %s, echo %s", rc == 0 && k == 'k' ? "PASS" : "FAIL", rc == 0 ? "ok" : strerror(e1), k == 'k' ? "ok" : "none");
    close(s);
    waitpid(pid, NULL, 0);
    if (unlink(path) != 0) P("unixsock", "FAIL unlink of the node: %s", E());
    if (rmdir(dir) != 0) P("unixsock", "FAIL rmdir: %s", E());
}

/* unixsock2: the data path of a by-name connection between two processes, step by step with
 * timings: the server accept()s blocking (no poll), reads one byte, writes one back */
static void probe_unixsock2(void) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/probe-sock2-%d", getpid());
    unlink(path);
    int ready[2];
    pipe2(ready, O_CLOEXEC);
    double t0 = now_s();
    pid_t pid = fork();
    if (pid == 0) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        strcpy(a.sun_path, path);
        if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) { P("unixsock2", "FAIL server bind: %s", E()); _exit(1); }
        if (listen(s, 5) != 0) { P("unixsock2", "FAIL server listen: %s", E()); _exit(1); }
        write(ready[1], "r", 1);
        int c = accept(s, NULL, NULL);
        P("unixsock2", "%s server: accept returned %d at %.3f s%s%s", c >= 0 ? "PASS" : "FAIL", c, now_s() - t0, c < 0 ? ": " : "", c < 0 ? E() : "");
        if (c < 0) _exit(1);
        char b = 0;
        long r = read(c, &b, 1);
        P("unixsock2", "%s server: read %ld '%c' at %.3f s", r == 1 ? "PASS" : "FAIL", r, b ? b : '?', now_s() - t0);
        long w = write(c, "k", 1);
        P("unixsock2", "%s server: write %ld at %.3f s", w == 1 ? "PASS" : "FAIL", w, now_s() - t0);
        usleep(200000);
        close(c);
        _exit(0);
    }
    char rr;
    read(ready[0], &rr, 1);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, path);
    int rc = connect(s, (struct sockaddr*)&a, sizeof(a));
    P("unixsock2", "%s client: connect %d at %.3f s%s%s", rc == 0 ? "PASS" : "FAIL", rc, now_s() - t0, rc ? ": " : "", rc ? E() : "");
    long w = write(s, "p", 1);
    P("unixsock2", "%s client: write %ld at %.3f s", w == 1 ? "PASS" : "FAIL", w, now_s() - t0);
    struct pollfd pfd = {s, POLLIN, 0};
    int pr = poll(&pfd, 1, 3000);
    char k = 0;
    long r = pr == 1 ? read(s, &k, 1) : -1;
    P("unixsock2", "%s client: poll %d (revents %#x), read %ld '%c' at %.3f s", k == 'k' ? "PASS" : "FAIL", pr, pfd.revents, r, k ? k : '?', now_s() - t0);
    close(s);
    int st = 0;
    waitpid(pid, &st, 0);
    unlink(path);
}

/* ------------------------------------------------------------------------------------------- */
static volatile int g_sigsys;
static char g_sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
static void sigsys_handler(int sig, siginfo_t* si, void* ucv) {
    (void)sig; (void)si;
    ucontext_t* uc = ucv;
    g_sigsys++;
    uc->uc_mcontext.gregs[REG_RAX] = -ENOSYS;   /* fake a result and go on after the syscall */
    /* the handler's own return is a syscall (rt_sigreturn): allow it, as Wine does by excluding
     * libc's address range when it turns the dispatch on */
    g_sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
}
static volatile int g_sud_ready, g_sud_go, g_sud_done;
static void* sud_thread(void* arg) {
    /* a raw system call from code outside any allowed range while the creator's selector says
     * BLOCK (the creator flips it only once this thread runs, so its own clone() is not blocked) */
    g_sud_ready = 1;
    while (!g_sud_go)
        __asm__ volatile("pause" ::: "memory");
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(SYS_getpid) : "rcx", "r11", "memory");
    *(long*)arg = r;
    g_sud_done = 1;
    return NULL;
}
static void probe_sud(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    /* allow everything but the raw syscall below (libc lives outside the [0, 0) range: all
     * syscalls go through the selector, which we flip around the test) */
    long r = prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, 0, 0, &g_sud_selector);
    if (r < 0) {
        P("sud", "FAIL prctl(PR_SET_SYSCALL_USER_DISPATCH, ON): %s (Wine warns and continues)", E());
        return;
    }
    g_sud_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(SYS_getpid) : "rcx", "r11", "memory");
    g_sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
    int sigs = g_sigsys;
    P("sud", "%s syscall user dispatch: blocked getpid raised SIGSYS %d time(s), result %ld", sigs == 1 && ret == -ENOSYS ? "PASS" : "FAIL", sigs, ret);

    /* Linux starts every new task with the dispatch off ("any fork or exec of the existing
     * process resets the mechanism"; kernel/fork.c clears it for threads too). Wine's new threads
     * make system calls from libc before they set up their own dispatch, while the parent's
     * selector may say BLOCK. */
    pthread_t t;
    long child_ret = 0;
    int before = g_sigsys;
    if (pthread_create(&t, NULL, sud_thread, &child_ret) == 0) {
        while (!g_sud_ready)
            __asm__ volatile("pause" ::: "memory");
        g_sud_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
        g_sud_go = 1;
        while (!g_sud_done) /* no system call of our own while the selector says BLOCK */
            __asm__ volatile("pause" ::: "memory");
        g_sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
        pthread_join(t, NULL);
    }
    P("sud", "%s a new thread starts with the dispatch off: its getpid returned %ld, SIGSYS %d time(s)",
      child_ret > 0 && g_sigsys == before ? "PASS" : "FAIL", child_ret, g_sigsys - before);

    pid_t pid = fork();
    if (pid == 0) {
        /* the child's copy of the selector: BLOCK before its first system call */
        g_sud_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
        long r2;
        __asm__ volatile("syscall" : "=a"(r2) : "0"(SYS_getpid) : "rcx", "r11", "memory");
        g_sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
        _exit(r2 > 0 && g_sigsys == before ? 0 : 1);
    }
    int st = -1;
    if (pid > 0)
        waitpid(pid, &st, 0);
    prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_OFF, 0, 0, 0);
    P("sud", "%s a forked child starts with the dispatch off: child exit %d",
      pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "PASS" : "FAIL", pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

/* ------------------------------------------------------------------------------------------- */
static volatile int g_sig_count, g_on_altstack, g_sig_code;
static volatile void* g_sig_addr;
static char g_altstack[65536];
static volatile unsigned long g_sig_err;
extern char fault_resume[];
static void resume_handler(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = ucv;
    char local;
    g_sig_count++;
    g_sig_code = si->si_code;
    g_sig_addr = si->si_addr;
    g_sig_err = uc->uc_mcontext.gregs[REG_ERR];
    g_on_altstack = &local >= g_altstack && &local < g_altstack + sizeof(g_altstack);
    (void)sig;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)fault_resume;
}
static __attribute__((noinline)) int do_fault(int kind) {
    int ret = 0;
    switch (kind) {
        case 0: __asm__ volatile("movl $1, 0\n" "fault_resume:\n" ::: "memory"); break;   /* SIGSEGV write to 0 */
        case 1: __asm__ volatile("ud2\n" "jmp fault_resume\n" ::: "memory"); break;
        case 2: __asm__ volatile("int3\n" "jmp fault_resume\n" ::: "memory"); break;
        case 3: __asm__ volatile("xor %%edx, %%edx\n" "xor %%ecx, %%ecx\n" "mov $1, %%eax\n" "div %%ecx\n" "jmp fault_resume\n" ::: "eax", "ecx", "edx", "memory"); break;
    }
    return ret;
}
static volatile int g_usr1_count;
static volatile pid_t g_usr1_tid;
static void usr1_handler(int sig) {
    (void)sig;
    g_usr1_count++;
    g_usr1_tid = syscall(SYS_gettid);
}
static int g_pipe[2];
static volatile pid_t g_thread_tid;
static volatile int g_spin;
static void* blocked_thread(void* arg) {
    (void)arg;
    g_thread_tid = syscall(SYS_gettid);
    char c;
    long r = read(g_pipe[0], &c, 1);
    return (void*)(r < 0 ? (long)errno : 0);
}
static void* spinning_thread(void* arg) {
    (void)arg;
    g_thread_tid = syscall(SYS_gettid);
    double t0 = now_s();
    while (!g_spin && now_s() - t0 < 3) ;
    return (void*)(long)g_spin;
}
static void probe_signals(void) {
    stack_t ss = {.ss_sp = g_altstack, .ss_size = sizeof(g_altstack)};
    int r = sigaltstack(&ss, NULL);
    P("signals", "%s sigaltstack: %s", r == 0 ? "PASS" : "FAIL", r == 0 ? "ok" : E());
    struct sigaction sa = {0};
    sa.sa_sigaction = resume_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    const char* names[] = {"SIGSEGV (write to 0)", "SIGILL (ud2)", "SIGTRAP (int3)", "SIGFPE (div 0)"};
    for (int k = 0; k < (getenv("PROBES_TRAPS") ? 4 : 1); k++) {
        g_sig_count = 0;
        g_on_altstack = 0;
        do_fault(k);
        P("signals", "%s %s: handler ran %d, on the alternate stack %d, si_code %d, si_addr %p, err %#lx, resumed at changed RIP",
          g_sig_count == 1 ? "PASS" : "FAIL", names[k], g_sig_count, g_on_altstack, g_sig_code, (void*)g_sig_addr, g_sig_err);
    }
}
static void probe_sig_block(void) {
    /* SIGUSR1 to a thread blocked in read() (no SA_RESTART): the thread's handler runs, read -> EINTR */
    struct sigaction su = {0};
    su.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &su, NULL);
    pipe2(g_pipe, O_CLOEXEC);
    pthread_t t;
    void* ret = NULL;
    g_thread_tid = 0;
    pthread_create(&t, NULL, blocked_thread, NULL);
    while (!g_thread_tid) usleep(1000);
    usleep(20000);
    g_usr1_count = 0;
    double t0 = now_s();
    syscall(SYS_tgkill, getpid(), g_thread_tid, SIGUSR1);
    pthread_join(t, &ret);
    P("signals", "%s tgkill(SIGUSR1) to a thread blocked in read: handler ran %d on tid %s, read returned %s after %.1f ms",
      g_usr1_count == 1 && g_usr1_tid == g_thread_tid && (long)ret == EINTR ? "PASS" : "FAIL", g_usr1_count,
      g_usr1_tid == g_thread_tid ? "the target" : "ANOTHER thread", (long)ret == EINTR ? "EINTR" : "something else", (now_s() - t0) * 1e3);
}
static void probe_sig_spin(void) {
    struct sigaction su = {0};
    su.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &su, NULL);
    pthread_t t;
    void* ret = NULL;
    double t0;
    /* SIGUSR1 to a thread spinning in user mode */
    g_thread_tid = 0;
    g_spin = 0;
    pthread_create(&t, NULL, spinning_thread, NULL);
    while (!g_thread_tid) usleep(1000);
    g_usr1_count = 0;
    t0 = now_s();
    syscall(SYS_tgkill, getpid(), g_thread_tid, SIGUSR1);
    while (!g_usr1_count && now_s() - t0 < 3) usleep(1000);
    double dt = now_s() - t0;
    g_spin = 1;
    pthread_join(t, &ret);
    P("signals", "%s tgkill(SIGUSR1) to a spinning thread: handler ran %d on %s after %.1f ms", g_usr1_count == 1 && g_usr1_tid == g_thread_tid ? "PASS" : "FAIL",
      g_usr1_count, g_usr1_tid == g_thread_tid ? "the target" : "another thread", dt * 1e3);
}
static void probe_sig_proc(void) {
    struct sigaction su = {0};
    su.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &su, NULL);
    pipe2(g_pipe, O_CLOEXEC);
    pthread_t t;
    void* ret = NULL;
    double t0;
    /* from another process, to a specific thread (what wineserver does) */
    g_thread_tid = 0;
    pthread_create(&t, NULL, blocked_thread, NULL);
    while (!g_thread_tid) usleep(1000);
    usleep(20000);
    g_usr1_count = 0;
    pid_t child = fork();
    if (child == 0) {
        long rr = syscall(SYS_tgkill, getppid(), g_thread_tid, SIGUSR1);
        _exit(rr == 0 ? 0 : errno);
    }
    if (child > 0) {
        int status = 0;
        waitpid(child, &status, 0);
        t0 = now_s();
        while (!g_usr1_count && now_s() - t0 < 3) usleep(1000);
        if (!g_usr1_count) write(g_pipe[1], "x", 1);
        pthread_join(t, &ret);
        P("signals", "%s tgkill(SIGUSR1) from another process to a blocked thread: child tgkill exit %d, handler ran %d on %s, read -> %s",
          WEXITSTATUS(status) == 0 && g_usr1_count == 1 && g_usr1_tid == g_thread_tid && (long)ret == EINTR ? "PASS" : "FAIL",
          WEXITSTATUS(status), g_usr1_count, g_usr1_tid == g_thread_tid ? "the target" : "another thread", (long)ret == EINTR ? "EINTR" : "other");
    } else {
        write(g_pipe[1], "x", 1);
        pthread_join(t, &ret);
        P("signals", "INFO fork failed: no cross-process signal test");
    }
}
static void probe_sig_chld(void) {
    pid_t child;
    /* SIGCHLD */
    g_usr1_count = 0;
    struct sigaction sc = {0};
    sc.sa_handler = usr1_handler;
    sigaction(SIGCHLD, &sc, NULL);
    child = fork();
    if (child == 0) _exit(0);
    if (child > 0) {
        waitpid(child, NULL, 0);
        P("signals", "%s SIGCHLD delivered: %d", g_usr1_count >= 1 ? "PASS" : "FAIL", g_usr1_count);
    }
}

/* ------------------------------------------------------------------------------------------- */
static void probe_traps(void) {
    setenv("PROBES_TRAPS", "1", 1);
    stack_t ss = {.ss_sp = g_altstack, .ss_size = sizeof(g_altstack)};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = resume_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    const char* names[] = {"SIGSEGV (write to 0)", "SIGILL (ud2)", "SIGTRAP (int3)", "SIGFPE (div 0)"};
    /* PROBES_TRAP=1|2|3 runs one of them (each may take the VM guest down) */
    int only = getenv("PROBES_TRAP") ? atoi(getenv("PROBES_TRAP")) : 0;
    for (int k = 1; k < 4; k++) {
        if (only && k != only) continue;
        g_sig_count = 0;
        g_on_altstack = 0;
        do_fault(k);
        P("traps", "%s %s: handler ran %d, on the alternate stack %d, si_code %d, si_addr %p, resumed at changed RIP",
          g_sig_count == 1 ? "PASS" : "FAIL", names[k], g_sig_count, g_on_altstack, g_sig_code, (void*)g_sig_addr);
    }
}
static void* sleeper(void* arg) {
    (void)arg;
    usleep(300000);
    return NULL;
}
static void probe_threads(void) {
    int max = 2048, made = 0;
    pthread_t* ts = calloc(max, sizeof(*ts));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int err = 0;
    double t0 = now_s();
    for (made = 0; made < max; made++) {
        err = pthread_create(&ts[made], &attr, sleeper, NULL);
        if (err) break;
    }
    double t1 = now_s();
    for (int i = 0; i < made; i++) pthread_join(ts[i], NULL);
    P("threads", "%s created %d threads (256 KiB stacks) in %.3f s%s%s, RSS %ld KiB", made >= 512 ? "PASS" : "FAIL", made, t1 - t0,
      err ? ", then: " : "", err ? strerror(err) : "", rss_kib());
    free(ts);
}

/* ------------------------------------------------------------------------------------------- */
static int fork_chain(int depth) {
    if (depth == 0) return 0;
    pid_t p = fork();
    if (p < 0) return depth;
    if (p == 0) _exit(fork_chain(depth - 1));
    int st = 0;
    waitpid(p, &st, 0);
    return WEXITSTATUS(st);
}
static void probe_procs(void) {
    int n = 64, made = 0;
    pid_t pids[64];
    double t0 = now_s();
    for (made = 0; made < n; made++) {
        pids[made] = fork();
        if (pids[made] < 0) break;
        if (pids[made] == 0) {
            usleep(100000);
            _exit(0);
        }
    }
    int e = errno;
    double t1 = now_s();
    for (int i = 0; i < made; i++) waitpid(pids[i], NULL, 0);
    P("procs", "%s %d children alive at once (%.3f s to fork)%s%s", made == n ? "PASS" : "FAIL", made, t1 - t0, made < n ? ": " : "", made < n ? strerror(e) : "");
    int left = fork_chain(8);
    P("procs", "%s a chain of 8 nested forks: %s", left == 0 ? "PASS" : "FAIL", left == 0 ? "ok" : "failed before the end");
    /* execve of a child, as Wine starts wineserver and new processes */
    pid_t p = fork();
    if (p == 0) {
        execl("/bin/true", "true", (char*)NULL);
        execl("/usr/bin/true", "true", (char*)NULL);
        _exit(127);
    }
    int st = -1;
    if (p > 0) waitpid(p, &st, 0);
    P("procs", "%s fork+execve /bin/true: exit %d", p > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "PASS" : "FAIL", p > 0 ? WEXITSTATUS(st) : -1);
}

/* ------------------------------------------------------------------------------------------- */
static void probe_proc(void) {
    const char* files[] = {"/proc/self/maps", "/proc/self/status", "/proc/self/exe", "/proc/self/cmdline", "/proc/self/auxv",
                           "/proc/self/stat", "/proc/self/statm", "/proc/cpuinfo", "/proc/meminfo", "/proc/version",
                           "/proc/sys/vm/max_map_count", "/proc/sys/kernel/random/boot_id", "/proc/self/fd", "/proc/self/task",
                           "/proc/sys/fs/file-max", "/proc/mounts", "/proc/self/mountinfo", "/proc/modules", "/proc/loadavg", "/proc/uptime",
                           "/sys/devices/system/cpu/online", "/sys/devices/system/cpu/cpu0/topology/core_id", "/sys/devices/system/cpu/cpu0/cache/index0/size",
                           "/sys/class/dmi/id/product_name", "/etc/machine-id", "/dev/urandom", "/dev/null", "/dev/shm", "/tmp", "/dev/ntsync"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char buf[4096];
        ssize_t n = -1;
        int fd = open(files[i], O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            n = read(fd, buf, sizeof(buf));
            close(fd);
        }
        if (fd < 0 && errno == EISDIR) n = 0;
        char link[256];
        ssize_t ln = readlink(files[i], link, sizeof(link) - 1);
        if (ln >= 0) link[ln] = 0;
        P("proc", "%s %s: %s%s%s", fd >= 0 || ln >= 0 ? "PASS" : "FAIL", files[i], fd >= 0 ? "open ok, " : E(),
          fd >= 0 ? (n >= 0 ? "read ok" : "read failed") : "", ln >= 0 ? " (symlink)" : "");
    }
    FILE* f = fopen("/proc/self/maps", "r");
    int lines = 0;
    char line[512];
    if (f) {
        while (fgets(line, sizeof(line), f)) lines++;
        fclose(f);
    }
    P("proc", "INFO /proc/self/maps: %d lines", lines);
}

/* ------------------------------------------------------------------------------------------- */
static void probe_misc(void) {
    int r = prctl(PR_SET_NAME, "probe-thread");
    P("misc", "%s prctl(PR_SET_NAME): %s", r == 0 ? "PASS" : "FAIL", r == 0 ? "ok" : E());
    r = prctl(PR_GET_DUMPABLE);
    P("misc", "%s prctl(PR_GET_DUMPABLE): %d%s", r >= 0 ? "PASS" : "FAIL", r, r < 0 ? E() : "");
    struct rlimit rl;
    r = getrlimit(RLIMIT_NOFILE, &rl);
    P("misc", "%s RLIMIT_NOFILE: %lu/%lu (Proton wants hard >= 524288)", r == 0 ? "PASS" : "FAIL", (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);
    rl.rlim_cur = rl.rlim_max;
    r = setrlimit(RLIMIT_NOFILE, &rl);
    P("misc", "%s setrlimit(RLIMIT_NOFILE, hard): %s", r == 0 ? "PASS" : "FAIL", r == 0 ? "ok" : E());
    getrlimit(RLIMIT_STACK, &rl);
    P("misc", "INFO RLIMIT_STACK: %lu", (unsigned long)rl.rlim_cur);
    r = personality(0xffffffff);
    P("misc", "%s personality(query): %#x%s", r >= 0 ? "PASS" : "FAIL", r, r < 0 ? E() : "");
    cpu_set_t set;
    r = sched_getaffinity(0, sizeof(set), &set);
    P("misc", "%s sched_getaffinity: %d cpus", r == 0 ? "PASS" : "FAIL", r == 0 ? CPU_COUNT(&set) : -1);
    struct timespec ts;
    r = clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    int r2 = clock_gettime(CLOCK_BOOTTIME, &ts);
    P("misc", "%s clock_gettime MONOTONIC_RAW %d BOOTTIME %d", r == 0 && r2 == 0 ? "PASS" : "FAIL", r, r2);
    char rnd[16];
    r = getrandom(rnd, sizeof(rnd), 0);
    P("misc", "%s getrandom: %d", r == 16 ? "PASS" : "FAIL", r);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int pf[2];
    r = pipe2(pf, O_CLOEXEC | O_NONBLOCK);
    struct epoll_event ev = {.events = EPOLLIN};
    int r3 = ep >= 0 ? epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) : -1;
    write(pf[1], "x", 1);
    int r4 = ep >= 0 ? epoll_wait(ep, &ev, 1, 100) : -1;
    P("misc", "%s epoll_create1 %d pipe2 %d epoll_ctl %d epoll_wait %d", ep >= 0 && r == 0 && r3 == 0 && r4 == 1 ? "PASS" : "FAIL", ep, r, r3, r4);
    int mfd = memfd_create("probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    P("misc", "%s memfd_create(MFD_ALLOW_SEALING): %s", mfd >= 0 ? "PASS" : "FAIL", mfd >= 0 ? "ok" : E());
    if (mfd >= 0) {
        ftruncate(mfd, 8192);
        void* m1 = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        void* m2 = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        int ok = 0;
        if (m1 != MAP_FAILED && m2 != MAP_FAILED) {
            *(int*)m1 = 42;
            ok = *(int*)m2 == 42;
        }
        P("misc", "%s two MAP_SHARED mappings of a memfd see each other's stores: %d", ok ? "PASS" : "FAIL", ok);
        close(mfd);
    }
    void* a = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* b = mremap(a, 8192, 16384, MREMAP_MAYMOVE);
    P("misc", "%s mremap grow: %s", b != MAP_FAILED ? "PASS" : "FAIL", b != MAP_FAILED ? "ok" : E());
    r = sched_yield();
    P("misc", "%s sched_yield: %d", r == 0 ? "PASS" : "FAIL", r);
    r = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
    P("misc", "%s membarrier(QUERY): %d%s", r >= 0 ? "PASS" : "FAIL", r, r < 0 ? E() : "");
    r = syscall(SYS_rseq, NULL, 0, 0, 0);
    P("misc", "INFO rseq(NULL): %d %s", r, r < 0 ? E() : "");
    r = sysconf(_SC_NPROCESSORS_ONLN);
    P("misc", "INFO sysconf(NPROCESSORS_ONLN) %d, page size %ld", r, sysconf(_SC_PAGESIZE));
    r = setpriority(PRIO_PROCESS, 0, 0);
    P("misc", "%s setpriority(0): %s", r == 0 ? "PASS" : "FAIL", r == 0 ? "ok" : E());
    r = prctl(PR_SET_PDEATHSIG, SIGTERM);
    P("misc", "%s prctl(PR_SET_PDEATHSIG): %s", r == 0 ? "PASS" : "FAIL", r == 0 ? "ok" : E());
}

/* ------------------------------------------------------------------------------------------- */
struct probe { const char* name; void (*fn)(void); };
static const struct probe probes[] = {
    /* the ones that may take the guest down come last */
    {"fixed", probe_fixed},   {"huge", probe_huge},     {"noreplace", probe_noreplace}, {"futexv", probe_futexv},
    {"eventfd", probe_eventfd}, {"scm", probe_scm},     {"unixsock", probe_unixsock}, {"unixsock2", probe_unixsock2}, {"sud", probe_sud},       {"signals", probe_signals},
    {"sig_chld", probe_sig_chld}, {"sig_spin", probe_sig_spin}, {"sig_block", probe_sig_block}, {"sig_proc", probe_sig_proc},
    {"threads", probe_threads}, {"procs", probe_procs}, {"proc", probe_proc},       {"misc", probe_misc},
    {"gs", probe_gs},         {"fs32", probe_fs32},     {"cs32", probe_cs32},       {"traps", probe_traps},
    {"wrgsbase", probe_wrgsbase},
};

static void run_probe(const struct probe* p, int inline_only) {
    pid_t pid = inline_only ? -1 : fork();
    if (pid == 0 || (pid < 0 && errno == ENOSYS) || inline_only) {
        if (pid == 0) alarm(60);
        p->fn();
        if (pid == 0) _exit(0);
        return;
    }
    if (pid < 0) {
        P("run", "fork failed (%s), running %s inline", E(), p->name);
        p->fn();
        return;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        P("run", "FAIL %s: killed by signal %d", p->name, WTERMSIG(status));
    else if (WEXITSTATUS(status))
        P("run", "FAIL %s: exit %d", p->name, WEXITSTATUS(status));
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int inline_only = getenv("PROBES_INLINE") != NULL;
    if (argc < 2 || !strcmp(argv[1], "all")) {
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) run_probe(&probes[i], inline_only);
        printf("PROBES DONE\n");
        return 0;
    }
    for (int a = 1; a < argc; a++) {
        int found = 0;
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
            if (!strcmp(argv[a], probes[i].name)) { run_probe(&probes[i], inline_only); found = 1; }
        if (!found) fprintf(stderr, "unknown probe %s\n", argv[a]);
    }
    printf("PROBES DONE\n");
    return 0;
}
