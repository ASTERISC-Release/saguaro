/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * 32-bit syscall arguments with something else in the upper half of the register. The kernel
 * narrows each argument to its declared type (SYSCALL_DEFINE), so only the low 32 bits count; the
 * LibOS must do the same. On RISC-V the C calling convention lets compiled code assume that a
 * 32-bit argument is sign-extended, so a zero-extended `uid_t -1` (as glibc passes it), a request
 * with bit 31 set, or garbage from a raw syscall() used to reach the LibOS as a different 64-bit
 * value.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/random.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

#define HI (1ul << 40)                   /* garbage in the upper half */
#define ZX(v) ((unsigned long)(uint32_t)(v)) /* a 32-bit value zero-extended */

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    alarm(30); /* a timeout misread as huge must not hang the test */
    char path[512];
    snprintf(path, sizeof(path), "%s/syscall_args_file", argv[1]);
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0);

    /* descriptors (int / unsigned int) */
    CHECK(syscall(SYS_write, (unsigned long)fd | HI, "abcd", 4ul) == 4);
    CHECK(syscall(SYS_lseek, (unsigned long)fd, 0l, (unsigned long)SEEK_SET | HI) == 0);
    char buf[8] = {0};
    CHECK(syscall(SYS_read, (unsigned long)fd | HI, buf, 4ul) == 4 && !memcmp(buf, "abcd", 4));
    CHECK(syscall(SYS_lseek, (unsigned long)fd | HI, 0l, (unsigned long)SEEK_END) == 4);
    long nfd = syscall(SYS_dup3, (unsigned long)fd | HI, 77ul | HI, (unsigned long)O_CLOEXEC);
    CHECK(nfd == 77);
    CHECK(syscall(SYS_fcntl, 77ul, (unsigned long)F_GETFD | HI) == FD_CLOEXEC);
    CHECK(syscall(SYS_close, 77ul | HI) == 0);

    /* AT_FDCWD (-100) zero-extended, as a 32-bit int stored in an unsigned long register */
    long ofd = syscall(SYS_openat, ZX(AT_FDCWD), path, (unsigned long)O_RDONLY, 0ul);
    CHECK(ofd >= 0);
    close((int)ofd);

    /* uid_t/gid_t -1 means "unchanged"; glibc passes it zero-extended */
    uid_t r0, e0, s0, r1, e1, s1;
    CHECK(getresuid(&r0, &e0, &s0) == 0);
    CHECK(syscall(SYS_setresuid, ZX(-1), ZX(-1), ZX(-1)) == 0);
    CHECK(getresuid(&r1, &e1, &s1) == 0 && r1 == r0 && e1 == e0 && s1 == s0);
    gid_t gr0, ge0, gs0, gr1, ge1, gs1;
    CHECK(getresgid(&gr0, &ge0, &gs0) == 0);
    CHECK(syscall(SYS_setresgid, ZX(-1), ZX(-1), ZX(-1)) == 0);
    CHECK(getresgid(&gr1, &ge1, &gs1) == 0 && gr1 == gr0 && ge1 == ge0 && gs1 == gs0);
    CHECK(syscall(SYS_fchown, (unsigned long)fd, ZX(-1), ZX(-1)) == 0);

    /* pid_t, signal number, poll timeout (int) */
    CHECK(syscall(SYS_kill, (unsigned long)getpid() | HI, 0ul | HI) == 0);
    int p[2];
    CHECK(pipe(p) == 0);
    struct pollfd pfd = {.fd = p[0], .events = POLLIN};
#ifdef SYS_poll
    CHECK(syscall(SYS_poll, &pfd, 1ul, 0ul | HI) == 0); /* timeout 0, not 2^40 ms */
#else
    struct timespec zero = {0, 0};
    CHECK(syscall(SYS_ppoll, &pfd, 1ul, &zero, NULL, 8ul) == 0);
#endif

    /* an ioctl request with bit 31 set (_IOR), zero-extended */
    int rnd = open("/dev/urandom", O_RDONLY);
    CHECK(rnd >= 0);
    int ent = -1;
    CHECK(syscall(SYS_ioctl, (unsigned long)rnd, ZX(RNDGETENTCNT), &ent) == 0 && ent >= 0);
    close(rnd);

    close(fd);
    unlink(path);
    puts("TEST OK");
    return 0;
}
