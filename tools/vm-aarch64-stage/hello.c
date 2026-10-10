/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Bring-up application for the aarch64 vm-PAL (tools/run-vm-aarch64-dev.sh).
 * Linked -static-pie: ET_DYN so LibOS mmaps it at an ASLR'd address inside DRAM (a non-PIE
 * 0x400680 link address has no physical backing on arm-virt), and no dynamic loader so the
 * B3 handoff exercises only the PAL/LibOS syscall paths. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <ucontext.h>

#define NTHREADS 4
#define ITERS 2000

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long counter;
static __thread unsigned long tid_local; /* lives in the *thread's* TLS block (TPIDR_EL0) */

/* B4-tail: EL0 fault -> LibOS memfault upcall -> SIGSEGV frame on the app stack -> handler
 * returns via glibc's restorer `rt_sigreturn` -> pal_sigreturn_return eret. The handler moves
 * ucontext.pc past the faulting store, so a correct sigreturn resume lands exactly on the next
 * instruction -- a pc/sp/GPR-fidelity test for the whole delivery+return chain.
 *
 * The bad address is an *opaque global* rather than the literal NULL: with a constant 0 address
 * GCC compiles `*(volatile int*)NULL = 42` into a store followed by `brk #0x3e8` (it proves the
 * store always traps, so the fall-through is unreachable), and the handler's `pc += 4` would land
 * on that BRK (EC=0x3c) instead of the continuation. An opaque pointer defeats that and leaves a
 * normal 4-byte store followed by the real next instruction. */
static volatile unsigned long faults;
static volatile int* bad_addr; /* set to NULL at runtime, opaque to the compiler (see above) */
static void segv_handler(int sig, siginfo_t* info, void* uc) {
    (void)sig;
    if (info->si_addr == NULL) {
        ((ucontext_t*)uc)->uc_mcontext.pc += 4; /* skip the faulting instruction */
        faults++;
    }
}

static void* worker(void* arg) {
    long id = (long)arg;
    tid_local = (unsigned long)id * 1000;
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&mtx);
        counter += tid_local + 1;
        pthread_mutex_unlock(&mtx);
    }
    return (void*)(tid_local + ITERS);
}

int main(int argc, char** argv) {
    (void)argv;
    /* Opaque NULL (the compiler cannot fold `argc < 0`): a plain store + real continuation, no
     * constant-NULL trap folding (see bad_addr's comment). */
    bad_addr = argc < 0 ? (volatile int*)&counter : NULL;
    printf("Hello world from Gramine VM-PAL on aarch64!\n");
    fflush(stdout);

    /* B3/B4 acceptance: spend some time executing in EL0 so the 100ms periodic timer tick
     * arrives while the *user* thread is running -- the el0_irq preemption path
     * (save_userland_context -> scheduler -> isr_eret_to_userland). clock_gettime in the
     * loop re-crosses the svc/eret boundary between spins, so a register-clobber
     * regression corrupts the loop, not just a one-shot print. */
    volatile unsigned long spins = 0;
    for (int i = 0; i < 2; i++) {
        struct timespec ts = {0};
        unsigned long target = spins + 40000000UL; /* ~1 tick under TCG */
        while (spins < target) {
            spins++;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        printf("spin %d done, ts.tv_nsec=%ld\n", i, ts.tv_nsec);
        fflush(stdout);
    }

    /* B6 acceptance: multiple EL0 threads round-robining on one CPU through clone, futex
     * lock contention (sched_thread_wait/wakeup under ticks), per-thread TPIDR_EL0
     * installs, and per-thread syscall_scratch slots. The expected counter value proves
     * every mutex handoff preserved each thread's TLS (tid_local read at +1 per add). */
    pthread_t th[NTHREADS];
    for (long i = 1; i <= NTHREADS; i++) {
        if (pthread_create(&th[i - 1], NULL, worker, (void*)i) != 0) {
            printf("pthread_create failed\n");
            return 1;
        }
    }
    unsigned long sums = 0;
    for (int i = 0; i < NTHREADS; i++) {
        void* rv = NULL;
        pthread_join(th[i], &rv);
        sums += (unsigned long)rv;
    }
    unsigned long want = 0;
    for (long id = 1; id <= NTHREADS; id++) {
        want += ITERS * (id * 1000 + 1);
    }
    printf("threads done, counter=%lu want=%lu sums=%lu\n", counter, want, sums);
    fflush(stdout);

    /* B4-tail acceptance: fault delivery and sigreturn (see segv_handler above). */
    struct sigaction sa = {
        .sa_sigaction = segv_handler,
        .sa_flags = SA_SIGINFO,
    };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        printf("sigaction failed\n");
        return 3;
    }
    *bad_addr = 42; /* SIGSEGV through an opaque NULL pointer; handler skips it */
    printf("sigreturn recovered: faults=%lu\n", faults);
    fflush(stdout);

    return (counter == want && faults == 1) ? 0 : 2;
}
