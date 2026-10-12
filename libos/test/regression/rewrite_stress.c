/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Stress test for lazy direct-PAL syscall-site rewriting (`sys.rewrite_syscalls`, aarch64/riscv64).
 * Every scenario starts
 * from a cold process (no sites patched yet) and checks results, not performance. Run one scenario
 * per process: `rewrite_stress <scenario>`; prints "OK <scenario>" on success.
 *
 * Scenarios: threads, entry, fork, exec, signals, restart, blob, jit, mprotect, threadstorm, all
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
#define SYSCALL_NR_GETPID 172
#define SC_INSN_NR        0xd2801588u /* mov x8, #172 */
#define SC_INSN_CALL      0xd4000001u /* svc #0 */
#define SC_INSN_ADD_A0(i) (0x91000000u | ((i & 0xfff) << 10))
#define SC_INSN_RET       0xd65f03c0u
#elif defined(__riscv)
#define SC_INSN_NR        0x0ac00893u /* addi a7, zero, 172 */
#define SC_INSN_CALL      0x00000073u /* ecall */
#define SC_INSN_ADD_A0(i) (0x00050513u | ((i & 0xfff) << 20))
#define SC_INSN_RET       0x00008067u /* jalr zero, ra, 0 */
#elif defined(__x86_64__)
/* No syscall-site rewriting on x86_64 (the bundled glibc is patched instead; see
 * Documentation/devel/syscall-interception/PLAN.md), and test_603 is skipped on this
 * architecture. These encodings exist to keep the binary building on x86_64, as before:
 * x86's 2-byte `syscall` does not fit the one-instruction-per-word layout. `mov al, 39;
 * syscall` is exactly 4 bytes, so the NR word performs the whole call, the CALL word is a
 * nop pad, and the `jit` scenario is correct here. The `blob` scenario's add cannot be
 * encoded in 4 words (add eax, imm32 is 5 bytes), so it stays a nop pad: its result check
 * fails for i > 0 on a manual run -- visibly, where main failed with SIGILL on foreign code. */
#define SC_INSN_NR        0x050F27B0u /* mov al, 39 (getpid); syscall */
#define SC_INSN_CALL      0x90909090u /* nop; nop; nop; nop (pad) */
#define SC_INSN_ADD_A0(i) 0x90909090u /* pad: add eax, imm32 is 5 bytes */
#define SC_INSN_RET       0x000000C3u /* ret */
#else
#error "rewrite_stress needs a direct-PAL syscall ISA"
#endif

#define CHK(cond)                                                                           \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #cond, errno); \
            _exit(1);                                                                       \
        }                                                                                   \
    } while (0)

static int g_zero = -1, g_null = -1;

/* A mix of raw-`svc` users: glibc wrappers and the syscall() trampoline. Each iteration checks
 * the result so that a mis-routed or clobbering syscall is noticed immediately. */
static void hammer(long iters) {
    pid_t pid = getpid();
    long last = 0;
    for (long i = 0; i < iters; i++) {
        CHK(getpid() == pid);
        CHK(syscall(SYS_getpid) == pid);
        char c = 0x55;
        CHK(read(g_zero, &c, 1) == 1 && c == 0);
        CHK(write(g_null, &c, 1) == 1);
        struct timespec ts;
        CHK(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts) == 0);
        long now = ts.tv_sec * 1000000000L + ts.tv_nsec;
        CHK(now >= last);
        last = now;
        uint32_t f = 0;
        CHK(syscall(SYS_futex, &f, FUTEX_WAKE, 1, NULL, NULL, 0) == 0);
        CHK(sched_yield() == 0);
    }
}

static void open_fds(void) {
    g_zero = open("/dev/zero", O_RDONLY);
    g_null = open("/dev/null", O_WRONLY);
    CHK(g_zero >= 0 && g_null >= 0);
}

#if defined(__riscv)
/* Force a file-backed, 2-mod-4 ecall and keep t3/t4 live across it. The first call takes SIGSYS
 * and publishes the site's stub; the second call must preserve both caller-saved registers on
 * the patched-entry path. */
static __attribute__((noinline)) long raw_getpid_preserving_t3_t4(void) {
    const long expect_t3 = 0x13579bdf2468ace0;
    const long expect_t4 = 0x02468ace13579bdf;
    register long a0 __asm__("a0") = 0;
    register long a7 __asm__("a7") = SYS_getpid;
    register long t3 __asm__("t3") = expect_t3;
    register long t4 __asm__("t4") = expect_t4;

    __asm__ volatile(".balign 4\n\tc.nop\n\tecall"
                     : "+r"(a0), "+r"(t3), "+r"(t4)
                     : "r"(a7)
                     : "memory");
    if (t3 != expect_t3 || t4 != expect_t4) {
        fprintf(stderr, "FAIL t3=%#lx (expected %#lx), t4=%#lx (expected %#lx)\n",
                t3, expect_t3, t4, expect_t4);
        _exit(1);
    }
    return a0;
}

static void sc_entry(void) {
    long pid = getpid();
    CHK(raw_getpid_preserving_t3_t4() == pid); /* initial SIGSYS publishes the stub */
    CHK(raw_getpid_preserving_t3_t4() == pid); /* patched-entry path, with no other threads */
}
#endif

/* ---- threads: many threads race to hit (and patch) the same cold sites ---- */
static pthread_barrier_t g_bar;
static void* thr_hammer(void* arg) {
    pid_t tid = syscall(SYS_gettid);
#if defined(__riscv)
    /* All workers start together below. The first call at this exact unaligned site patches it;
     * the immediate second call exercises the patched stub with t3/t4 live. */
    long pid = getpid();
    CHK(raw_getpid_preserving_t3_t4() == pid);
    CHK(raw_getpid_preserving_t3_t4() == pid);
#endif
    pthread_barrier_wait(&g_bar);
    hammer((long)arg);
    CHK(syscall(SYS_gettid) == tid);
    return NULL;
}
static void sc_threads(void) {
    enum { N = 16 };
    pthread_t t[N];
    CHK(pthread_barrier_init(&g_bar, NULL, N) == 0);
    for (int i = 0; i < N; i++)
        CHK(pthread_create(&t[i], NULL, thr_hammer, (void*)3000L) == 0);
    for (int i = 0; i < N; i++)
        CHK(pthread_join(t[i], NULL) == 0);
}

/* ---- fork: children start from a (parent-patched) image; both sides keep running ---- */
static void sc_fork(void) {
    hammer(200); /* patch some sites in the parent first */
    for (int round = 0; round < 10; round++) {
        pid_t kids[6];
        for (int k = 0; k < 6; k++) {
            pid_t p = fork();
            CHK(p >= 0);
            if (p == 0) {
                hammer(2000);
                pid_t gc = fork();
                CHK(gc >= 0);
                if (gc == 0) {
                    hammer(500);
                    _exit(7);
                }
                int st;
                CHK(waitpid(gc, &st, 0) == gc && WIFEXITED(st) && WEXITSTATUS(st) == 7);
                _exit(3);
            }
            kids[k] = p;
        }
        hammer(1000); /* parent keeps going while children run */
        for (int k = 0; k < 6; k++) {
            int st;
            CHK(waitpid(kids[k], &st, 0) == kids[k]);
            CHK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
        }
    }
}

/* ---- exec: fresh image each time, then hammer cold ---- */
static void sc_exec(char** argv) {
    for (int round = 0; round < 15; round++) {
        pid_t p = fork();
        CHK(p >= 0);
        if (p == 0) {
            execl("/rewrite_stress", "/rewrite_stress", "hammer", NULL);
            _exit(99);
        }
        int st;
        CHK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    for (int round = 0; round < 10; round++) { /* vfork + exec */
        pid_t p = vfork();
        CHK(p >= 0);
        if (p == 0) {
            execl("/rewrite_stress", "/rewrite_stress", "hammer", NULL);
            _exit(99);
        }
        int st;
        CHK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
}

/* ---- signals: asynchronous signals landing at arbitrary points, including inside the stub and
 * the LibOS entry sequence, with the handler itself issuing syscalls ---- */
static atomic_long g_sig_cnt;
static void on_sig(int sig) {
    (void)sig;
    char c = 1;
    atomic_fetch_add(&g_sig_cnt, 1);
    (void)syscall(SYS_getpid);
    (void)write(g_null, &c, 1);
}
static pthread_t g_main_thread;
static atomic_int g_stop;
static void* killer(void* arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        pthread_kill(g_main_thread, SIGUSR1);
        struct timespec ts = {0, 30000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}
static void signals_impl(bool timer, bool killer_thread) {
    struct sigaction sa = {.sa_handler = on_sig};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    CHK(sigaction(SIGUSR1, &sa, NULL) == 0);
    CHK(sigaction(SIGALRM, &sa, NULL) == 0);
    if (timer) {
        struct itimerval it = {{0, 200}, {0, 200}};
        CHK(setitimer(ITIMER_REAL, &it, NULL) == 0);
    }
    g_main_thread = pthread_self();
    pthread_t k;
    if (killer_thread)
        CHK(pthread_create(&k, NULL, killer, NULL) == 0);
    hammer(30000);
    atomic_store(&g_stop, 1);
    if (killer_thread)
        CHK(pthread_join(k, NULL) == 0);
    if (timer) {
        struct itimerval off = {{0, 0}, {0, 0}};
        CHK(setitimer(ITIMER_REAL, &off, NULL) == 0);
    }
    CHK(atomic_load(&g_sig_cnt) > 0);
}
static void sc_signals(void) { signals_impl(true, true); }

/* ---- restart: blocking syscalls interrupted by signals (-ERESTART* path re-enters the stub) --- */
static int g_pipe[2];
static void* slow_writer(void* arg) {
    (void)arg;
    for (int i = 0; i < 3000; i++) {
        unsigned char b = i & 0xff;
        CHK(write(g_pipe[1], &b, 1) == 1);
        struct timespec ts = {0, 20000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}
static void sc_restart(void) {
    struct sigaction sa = {.sa_handler = on_sig};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    CHK(sigaction(SIGALRM, &sa, NULL) == 0);
    CHK(pipe(g_pipe) == 0);
    struct itimerval it = {{0, 300}, {0, 300}};
    CHK(setitimer(ITIMER_REAL, &it, NULL) == 0);
    pthread_t w;
    CHK(pthread_create(&w, NULL, slow_writer, NULL) == 0);
    for (int i = 0; i < 3000; i++) {
        unsigned char b;
        CHK(read(g_pipe[0], &b, 1) == 1); /* blocks; SA_RESTART restarts it after each signal */
        CHK(b == (i & 0xff));
    }
    CHK(pthread_join(w, NULL) == 0);
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    CHK(atomic_load(&g_sig_cnt) > 0);
}

/* ---- blob: file-backed executable mappings that come and go, including re-mapping different
 * code at the same address (a stale patch there would return the wrong value) ---- */
static void sc_blob(void) {
    const char* path = "/mnt/tmpfs/rewrite_stress_blob";
    pid_t pid = getpid();
    void* prev = NULL;
    for (int i = 0; i < 300; i++) {
        uint32_t code[5];
        code[0] = SC_INSN_NR;
        code[1] = SC_INSN_CALL;
        code[2] = SC_INSN_ADD_A0(i);
        code[3] = SC_INSN_RET;
        code[4] = 0;
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0700);
        CHK(fd >= 0);
        CHK(write(fd, code, sizeof(code)) == (ssize_t)sizeof(code));
        CHK(ftruncate(fd, 4096) == 0);
        int flags = MAP_PRIVATE | (prev ? MAP_FIXED_NOREPLACE : 0);
        void* m = mmap(prev, 4096, PROT_READ | PROT_EXEC, flags, fd, 0);
        CHK(m != MAP_FAILED);
        close(fd);
        long (*fn)(void) = (long (*)(void))m;
        for (int k = 0; k < 50; k++)
            CHK(fn() == pid + (i & 0xfff));
        if (i % 25 == 0) { /* a forked child must also work with this mapping */
            pid_t p = fork();
            CHK(p >= 0);
            if (p == 0) {
                for (int k = 0; k < 50; k++)
                    CHK(fn() == getpid() + (i & 0xfff));
                _exit(0);
            }
            int st;
            CHK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        }
        CHK(munmap(m, 4096) == 0);
        prev = m; /* next iteration maps different code at the same address */
    }
    unlink(path);
}

#if defined(__riscv)
/* ---- unaligned: compressed code places the ecall at 2 mod 4. Threads race both the
 * c.ebreak publication protocol and repeated execution of the resulting unaligned jal. ---- */
static long (*g_unaligned_fn)(void);
static void* unaligned_user(void* arg) {
    (void)arg;
    pid_t pid = getpid();
    pthread_barrier_wait(&g_bar);
    for (int i = 0; i < 5000; i++)
        CHK(g_unaligned_fn() == pid);
    return NULL;
}

static void sc_unaligned(void) {
    const char* path = "/mnt/tmpfs/rewrite_stress_unaligned";
    unsigned char code[16] = {0};
    uint32_t nr = SC_INSN_NR;
    uint16_t c_nop = 0x0001;
    uint32_t call = SC_INSN_CALL;
    uint32_t ret = SC_INSN_RET;
    memcpy(code + 0, &nr, sizeof(nr));
    memcpy(code + 4, &c_nop, sizeof(c_nop));
    memcpy(code + 6, &call, sizeof(call));
    memcpy(code + 10, &ret, sizeof(ret));

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0700);
    CHK(fd >= 0);
    CHK(write(fd, code, sizeof(code)) == (ssize_t)sizeof(code));
    CHK(ftruncate(fd, 4096) == 0);
    void* mapping = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    CHK(mapping != MAP_FAILED);
    close(fd);
    g_unaligned_fn = mapping;

    enum { N = 16 };
    pthread_t threads[N];
    CHK(pthread_barrier_init(&g_bar, NULL, N) == 0);
    for (int i = 0; i < N; i++)
        CHK(pthread_create(&threads[i], NULL, unaligned_user, NULL) == 0);
    for (int i = 0; i < N; i++)
        CHK(pthread_join(threads[i], NULL) == 0);
    CHK(munmap(mapping, 4096) == 0);
    unlink(path);
}
#endif

/* ---- jit: anonymous executable memory must keep working (never patched) ---- */
static atomic_long g_jit_err;
static void* jit_user(void* arg) {
    long (*fn)(void) = arg;
    pid_t pid = getpid();
    for (int i = 0; i < 20000; i++)
        if (fn() != pid)
            atomic_fetch_add(&g_jit_err, 1);
    return NULL;
}
static void sc_jit(void) {
    uint32_t* m = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHK(m != MAP_FAILED);
    m[0] = SC_INSN_NR;
    m[1] = SC_INSN_CALL;
    m[2] = SC_INSN_RET;
    __builtin___clear_cache((char*)m, (char*)(m + 4));
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        CHK(pthread_create(&t[i], NULL, jit_user, m) == 0);
    for (int i = 0; i < 4; i++)
        CHK(pthread_join(t[i], NULL) == 0);
    CHK(atomic_load(&g_jit_err) == 0);
    pid_t p = fork(); /* anonymous code is copied into the child; it must not branch to stubs */
    CHK(p >= 0);
    if (p == 0) {
        long (*fn)(void) = (long (*)(void))m;
        for (int i = 0; i < 1000; i++)
            CHK(fn() == getpid());
        _exit(0);
    }
    int st;
    CHK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* ---- mprotect: the application changes protections on the text it is executing while other
 * threads are patching it. KNOWN LIMITATION: the patcher briefly makes the page RWX and restores
 * the protection it read from the VMA; an app mprotect in that window can be lost. ---- */
static atomic_int g_mp_stop;
static void* mp_toggler(void* arg) {
    (void)arg;
    uintptr_t page = (uintptr_t)&hammer & ~4095UL;
    while (!atomic_load(&g_mp_stop)) {
        CHK(mprotect((void*)page, 4096, PROT_READ | PROT_EXEC | PROT_WRITE) == 0);
        CHK(mprotect((void*)page, 4096, PROT_READ | PROT_EXEC) == 0);
    }
    return NULL;
}
static void sc_mprotect(void) {
    pthread_t tg, w[4];
    CHK(pthread_create(&tg, NULL, mp_toggler, NULL) == 0);
    CHK(pthread_barrier_init(&g_bar, NULL, 4) == 0);
    for (int i = 0; i < 4; i++)
        CHK(pthread_create(&w[i], NULL, thr_hammer, (void*)3000L) == 0);
    for (int i = 0; i < 4; i++)
        CHK(pthread_join(w[i], NULL) == 0);
    atomic_store(&g_mp_stop, 1);
    CHK(pthread_join(tg, NULL) == 0);
}

/* ---- threadstorm: thread creation/exit (clone + exit sites) in bulk ---- */
static void* thr_short(void* arg) {
    (void)arg;
    hammer(5);
    return NULL;
}
static void sc_threadstorm(void) {
    for (int round = 0; round < 40; round++) {
        pthread_t t[50];
        for (int i = 0; i < 50; i++)
            CHK(pthread_create(&t[i], NULL, thr_short, NULL) == 0);
        for (int i = 0; i < 50; i++)
            CHK(pthread_join(t[i], NULL) == 0);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        return 2;
    }
    const char* sc = argv[1];
    open_fds();
    if (strcmp(sc, "hammer") == 0) {
        hammer(5000);
        return 0;
    }
    alarm(240); /* safety net against hangs */

    int ran = 0;
#define RUN(name, call)                       \
    if (!strcmp(sc, name) || !strcmp(sc, "all")) { \
        call;                                 \
        printf("OK %s\n", name);              \
        ran++;                                \
    }
#if defined(__riscv)
    RUN("entry", sc_entry());
#endif
    RUN("threads", sc_threads());
    RUN("fork", sc_fork());
    RUN("exec", sc_exec(argv));
    RUN("signals", sc_signals());
    if (!strcmp(sc, "signals_timer")) { signals_impl(true, false); printf("OK signals_timer\n"); ran++; }
    if (!strcmp(sc, "signals_kill")) { signals_impl(false, true); printf("OK signals_kill\n"); ran++; }
    RUN("restart", sc_restart());
    RUN("blob", sc_blob());
#if defined(__riscv)
    RUN("unaligned", sc_unaligned());
#endif
    RUN("jit", sc_jit());
    RUN("mprotect", sc_mprotect());
    RUN("threadstorm", sc_threadstorm());
    if (!ran) {
        fprintf(stderr, "unknown scenario %s\n", sc);
        return 2;
    }
    return 0;
}
