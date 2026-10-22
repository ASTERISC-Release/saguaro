/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * set_thread_area()/get_thread_area() and 32-bit code, as Wine's new WoW64 uses them (VM PAL;
 * dev docs: proton-vm/design-32bit.md): a per-thread GDT TLS slot through the 64-bit
 * system call and through `int $0x80` (the i386 ABI Wine issues it with); the same entry with a
 * different base in two threads, kept across context switches while %fs holds the selector; a
 * far jump to Linux's 32-bit code selector 0x23 and back; and a signal that interrupts the 32-bit
 * code, handled the way Wine does (restore the TLS base first, reload the 32-bit %fs before the
 * return), with the sigreturn resuming the 32-bit code. The PALs without per-thread GDT slots
 * answer ENOSYS: the test then exits early with "TEST SKIPPED" (the Python side runs it only
 * under saguaro-vm).
 */

#define _GNU_SOURCE
#include <asm/ldt.h>
#include <asm/prctl.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "common.h"

#define MARKER      0xfeedc0deu
#define CS32        0x23
#define ITERATIONS  300

static unsigned long g_tls_base;

static long raw_syscall2(long nr, long a, long b) {
    long ret = nr;
    __asm__ volatile("syscall" : "+a"(ret) : "D"(a), "S"(b) : "rcx", "r11", "memory");
    return ret;
}

static void set_tls_base(unsigned long base) {
    /* raw: libc must not run while %fs is not the TLS */
    raw_syscall2(SYS_arch_prctl, ARCH_SET_FS, base);
}

/* `int $0x80` set_thread_area, Wine's alloc_fs_sel(): the user_desc at an address below 4 GiB */
static long set_thread_area_int80(void* low, struct user_desc* d) {
    memcpy(low, d, 16);
    long ret = 243; /* i386 __NR_set_thread_area */
    __asm__ volatile("int $0x80" : "+a"(ret) : "b"((uint32_t)(uintptr_t)low) : "memory");
    memcpy(d, low, 16);
    return ret;
}

static void* map_low_page(void) {
    void* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED)
        p = mmap((void*)0x33000000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p >= 0x100000000ull)
        errx(1, "no page below 4 GiB");
    return p;
}

static uint32_t read_fs0_with(unsigned sel) {
    uint32_t v;
    __asm__ volatile("movw %w1, %%fs\n\tmovl %%fs:0, %0" : "=r"(v) : "r"(sel) : "memory");
    set_tls_base(g_tls_base);
    return v;
}

/* --- two threads, one entry, %fs kept loaded across yields and sleeps --- */
static int g_entry;
static volatile int g_switch_fail[2];

static void switch_loop(uint32_t* base, int who) {
    struct user_desc d = {.entry_number = g_entry, .base_addr = (uint32_t)(uintptr_t)base,
                          .limit = 0xfff, .seg_32bit = 1, .useable = 1};
    if (set_thread_area_int80(base + 32, &d) < 0) {
        g_switch_fail[who] = -2;
        return;
    }
    unsigned long tls = 0;
    CHECK(syscall(SYS_arch_prctl, ARCH_GET_FS, &tls));
    struct timespec* ts = (struct timespec*)(base + 96);
    ts->tv_sec = 0;
    ts->tv_nsec = 1000000;
    uint32_t want = *base;
    int fail = -1;
    __asm__ volatile("movw %w0, %%fs" :: "r"((unsigned)(d.entry_number << 3) | 3));
    for (int i = 0; i < ITERATIONS; i++) {
        uint32_t v;
        __asm__ volatile("movl %%fs:0, %0" : "=r"(v));
        if (v != want) {
            fail = i;
            break;
        }
        if (i % 16 == 15)
            raw_syscall2(SYS_nanosleep, (long)ts, 0);
        else
            raw_syscall2(SYS_sched_yield, 0, 0);
    }
    set_tls_base(tls);
    g_switch_fail[who] = fail;
}

static void* switch_thread(void* arg) {
    switch_loop(arg, 1);
    return NULL;
}

/* --- 32-bit code and a signal in it --- */
extern void cs32_trampoline(void* page);
__asm__(".text\n"
        ".globl cs32_trampoline\n"
        ".type cs32_trampoline,@function\n"
        "cs32_trampoline:\n"
        "  push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "  mov %rsp, 0xf0(%rdi)\n"          /* the 64-bit stack, restored by the return stub */
        "  lea 0xf80(%rdi), %rsp\n"         /* a stack inside the low page for the 32-bit code */
        "  mov $0x2b, %ecx\n"               /* compat mode checks DS/ES: the user data selector */
        "  mov %ecx, %ds\n"
        "  mov %ecx, %es\n"
        "  pushq $0x23\n"                   /* Linux's 32-bit code selector, hard-coded in Wine */
        "  lea 0x40(%rdi), %rcx\n"
        "  push %rcx\n"
        "  lretq\n"                         /* -> 32-bit code at page+0x40 */
        ".size cs32_trampoline, .-cs32_trampoline\n");

unsigned long g_sig_tls;
uint64_t g_sig_csgsfs;
unsigned char* g_sig_page;
unsigned g_sig_sel;
volatile int g_sig_handled;
static pid_t g_main_tid;

/* Wine's handler protocol: the TLS base first (init_handler), the frame's cs/fs, the flag the
 * 32-bit loop waits for, then %fs back to the 32-bit selector when the signal interrupted 32-bit
 * code (leave_handler) */
extern void sig_handler(int, siginfo_t*, void*);
__asm__(".text\n"
        ".globl sig_handler\n"
        ".type sig_handler,@function\n"
        "sig_handler:\n"
        "  push %rbx\n"
        "  mov 184(%rdx), %rbx\n"             /* uc_mcontext.gregs[REG_CSGSFS] */
        "  mov $158, %eax\n"                  /* arch_prctl(ARCH_SET_FS, tls) */
        "  mov $0x1002, %edi\n"
        "  mov g_sig_tls(%rip), %rsi\n"
        "  syscall\n"
        "  mov %rbx, g_sig_csgsfs(%rip)\n"
        "  movl $1, g_sig_handled(%rip)\n"
        "  mov g_sig_page(%rip), %rax\n"
        "  movl $1, 0xe8(%rax)\n"
        "  cmpw $0x23, %bx\n"
        "  jne 1f\n"
        "  mov g_sig_sel(%rip), %eax\n"
        "  movw %ax, %fs\n"
        "1:\n"
        "  pop %rbx\n"
        "  ret\n"
        ".size sig_handler, .-sig_handler\n");

static void* killer(void* arg) {
    (void)arg;
    usleep(50000);
    CHECK(syscall(SYS_tgkill, getpid(), g_main_tid, SIGUSR1));
    return NULL;
}

static void put32(unsigned char* p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* 32-bit code at +0x40: ecx = 0xffffffff; loop: eax = flag; jnz done; dec ecx; jnz loop;
 * done: page+0xec = ecx; ebx = %fs:0; push cs64; push page+0x80; lret. The 64-bit stub at +0x80:
 * rsp = saved; page+0xe0 = eax; page+0xe4 = ebx; pop r15..rbx; ret */
static void emit_code(unsigned char* page, uint16_t cs64) {
    unsigned char* c = page + 0x40;
    *c++ = 0xb9; put32(c, 0xffffffff); c += 4;
    *c++ = 0xa1; put32(c, (uint32_t)(uintptr_t)(page + 0xe8)); c += 4;
    *c++ = 0x85; *c++ = 0xc0;
    *c++ = 0x75; *c++ = 0x03;
    *c++ = 0x49;
    *c++ = 0x75; *c++ = 0xf4;
    *c++ = 0x89; *c++ = 0x0d; put32(c, (uint32_t)(uintptr_t)(page + 0xec)); c += 4;
    *c++ = 0x64; *c++ = 0x8b; *c++ = 0x1d; put32(c, 0); c += 4;
    *c++ = 0x68; put32(c, cs64); c += 4;
    *c++ = 0x68; put32(c, (uint32_t)(uintptr_t)(page + 0x80)); c += 4;
    *c++ = 0xcb;
    c = page + 0x80;
    *c++ = 0x48; *c++ = 0x8b; *c++ = 0x24; *c++ = 0x25; put32(c, (uint32_t)(uintptr_t)(page + 0xf0)); c += 4;
    *c++ = 0x89; *c++ = 0x04; *c++ = 0x25; put32(c, (uint32_t)(uintptr_t)(page + 0xe0)); c += 4;
    *c++ = 0x89; *c++ = 0x1c; *c++ = 0x25; put32(c, (uint32_t)(uintptr_t)(page + 0xe4)); c += 4;
    const unsigned char pops[] = {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3};
    memcpy(c, pops, sizeof(pops));
}

int main(void) {
    CHECK(syscall(SYS_arch_prctl, ARCH_GET_FS, &g_tls_base));
    uint32_t* base = map_low_page();
    *base = MARKER;

    /* 1. a slot through the 64-bit system call (ENOSYS: nothing to test on this PAL) */
    struct user_desc d = {.entry_number = -1, .base_addr = (uint32_t)(uintptr_t)base, .limit = 0xfff,
                          .seg_32bit = 1, .useable = 1};
    if (syscall(SYS_set_thread_area, &d) < 0) {
        if (errno == ENOSYS) {
            printf("TEST SKIPPED (set_thread_area: ENOSYS)\n");
            return 0;
        }
        err(1, "set_thread_area");
    }
    if (d.entry_number < 12 || d.entry_number > 14)
        errx(1, "entry_number %u outside 12..14", d.entry_number);
    unsigned sel = (d.entry_number << 3) | 3;
    uint32_t v = read_fs0_with(sel);
    if (v != MARKER)
        errx(1, "selector %#x: read %#x through %%fs, want %#x", sel, v, MARKER);

    /* get_thread_area reads it back; an empty descriptor frees it; the next -1 gets it again */
    struct user_desc g = {.entry_number = d.entry_number};
    CHECK(syscall(SYS_get_thread_area, &g));
    if (g.base_addr != d.base_addr || g.limit != d.limit || !g.seg_32bit || g.seg_not_present)
        errx(1, "get_thread_area: base %#x limit %#x 32bit %u present %u", g.base_addr, g.limit,
             g.seg_32bit, !g.seg_not_present);
    struct user_desc bad = d;
    bad.entry_number = 11;
    if (syscall(SYS_set_thread_area, &bad) == 0 || errno != EINVAL)
        errx(1, "entry 11 not refused with EINVAL");
    bad = d;
    bad.seg_32bit = 0;
    if (syscall(SYS_set_thread_area, &bad) == 0 || errno != EINVAL)
        errx(1, "a 16-bit segment not refused with EINVAL");
    struct user_desc empty = {.entry_number = d.entry_number, .read_exec_only = 1, .seg_not_present = 1};
    CHECK(syscall(SYS_set_thread_area, &empty));
    struct user_desc again = d;
    again.entry_number = -1;
    CHECK(syscall(SYS_set_thread_area, &again));
    if (again.entry_number != d.entry_number)
        errx(1, "the freed slot %u was not reused (got %u)", d.entry_number, again.entry_number);

    /* 2. the same through `int $0x80`, as Wine issues it */
    struct user_desc w = {.entry_number = -1, .base_addr = (uint32_t)(uintptr_t)base, .limit = ~0u,
                          .seg_32bit = 1, .useable = 1};
    long ret = set_thread_area_int80(base + 16, &w);
    if (ret < 0)
        errx(1, "int $0x80 set_thread_area: %ld", ret);
    if (w.entry_number < 12 || w.entry_number > 14 || w.entry_number == d.entry_number)
        errx(1, "int $0x80: entry_number %u (the first slot is %u)", w.entry_number, d.entry_number);
    v = read_fs0_with((w.entry_number << 3) | 3);
    if (v != MARKER)
        errx(1, "int $0x80 selector: read %#x, want %#x", v, MARKER);
    /* the registers around an `int $0x80` survive it (Linux preserves all but eax) */
    uint64_t r_di = 0x1111, r_si = 0x2222, r_dx = 0x3333, r_cx = 0x4444, r_8 = 0x5555, r_9 = 0x6666,
             r_10 = 0x7777, r_11 = 0x8888;
    long nr = 244; /* get_thread_area, entry 12..14 given in the low page */
    ((uint32_t*)(base + 16))[0] = w.entry_number;
    __asm__ volatile("int $0x80"
                     : "+a"(nr), "+D"(r_di), "+S"(r_si), "+d"(r_dx), "+c"(r_cx), "+r"(r_8), "+r"(r_9),
                       "+r"(r_10), "+r"(r_11)
                     : "b"((uint32_t)(uintptr_t)(base + 16))
                     : "memory");
    if (nr != 0 || r_di != 0x1111 || r_si != 0x2222 || r_dx != 0x3333 || r_cx != 0x4444 ||
            r_8 != 0x5555 || r_9 != 0x6666 || r_10 != 0x7777 || r_11 != 0x8888)
        errx(1, "int $0x80 get_thread_area: ret %ld, registers di %#lx si %#lx dx %#lx cx %#lx r8 %#lx "
                "r9 %#lx r10 %#lx r11 %#lx", nr, r_di, r_si, r_dx, r_cx, r_8, r_9, r_10, r_11);

    /* 3. two threads, one entry, each its own base, %fs loaded across context switches */
    g_entry = w.entry_number;
    g_switch_fail[0] = g_switch_fail[1] = -3;
    uint32_t* other = base + 256;
    *other = 0x600dbeefu;
    pthread_t th;
    if (pthread_create(&th, NULL, switch_thread, other))
        err(1, "pthread_create");
    switch_loop(base, 0);
    pthread_join(th, NULL);
    if (g_switch_fail[0] != -1 || g_switch_fail[1] != -1)
        errx(1, "two threads, one entry: main %d, thread %d (-1 = every read saw its own base)",
             g_switch_fail[0], g_switch_fail[1]);

    /* 4. 32-bit code at selector 0x23 with %fs loaded, a signal in it, the sigreturn back to it */
    unsigned char* page = map_low_page();
    memset(page, 0xcc, 0x1000);
    *(uint32_t*)page = MARKER;
    uint16_t cs64;
    __asm__("movw %%cs, %0" : "=r"(cs64));
    emit_code(page, cs64);
    *(uint32_t*)(page + 0xe0) = 0;
    *(uint32_t*)(page + 0xe4) = 0;
    *(uint32_t*)(page + 0xe8) = 0;
    *(uint32_t*)(page + 0xec) = 0;
    struct user_desc p = {.entry_number = -1, .base_addr = (uint32_t)(uintptr_t)page, .limit = 0xfff,
                          .seg_32bit = 1, .useable = 1};
    ret = set_thread_area_int80(page + 0x100, &p);
    if (ret < 0)
        errx(1, "int $0x80 set_thread_area for the page: %ld", ret);
    g_sig_sel = (p.entry_number << 3) | 3;
    g_sig_page = page;
    g_sig_tls = g_tls_base;
    g_main_tid = syscall(SYS_gettid);
    static char altstack[65536];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof(altstack)};
    CHECK(sigaltstack(&ss, NULL));
    struct sigaction sa = {0};
    sa.sa_sigaction = sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    CHECK(sigaction(SIGUSR1, &sa, NULL));
    if (pthread_create(&th, NULL, killer, NULL))
        err(1, "pthread_create");
    __asm__ volatile("movw %w0, %%fs" :: "r"(g_sig_sel));
    cs32_trampoline(page);
    set_tls_base(g_tls_base);
    pthread_join(th, NULL);
    uint32_t ebx = *(uint32_t*)(page + 0xe4), by_flag = *(uint32_t*)(page + 0xec);
    unsigned cs = g_sig_csgsfs & 0xffff;
    if (!g_sig_handled)
        errx(1, "the signal handler did not run");
    if (cs != CS32)
        errx(1, "the frame's CS is %#x, want %#x (the signal did not interrupt the 32-bit code)", cs, CS32);
    if (!by_flag)
        errx(1, "the 32-bit loop ran out instead of seeing the handler's flag");
    if (ebx != MARKER)
        errx(1, "after the signal, the 32-bit code read %#x through %%fs, want %#x", ebx, MARKER);

    printf("TEST OK\n");
    return 0;
}
