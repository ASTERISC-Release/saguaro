/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * execve("/proc/self/fd/N") of a memfd, the way programs run an in-memory executable.
 *
 * Linux semantics checked here: a memfd created with MFD_NOEXEC_SEAL has no execute permission
 * (EACCES) and carries F_SEAL_EXEC; F_SEAL_EXEC by itself only freezes the mode, so a sealed
 * executable memfd still runs; MFD_EXEC together with MFD_NOEXEC_SEAL is EINVAL. The last step
 * execs a copy of this binary from a memfd, which prints "TEST OK".
 *
 * Usage: memfd_exec            (the child is started with the argument "child")
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

static int memfd(const char* name, unsigned int flags) {
    return (int)syscall(SYS_memfd_create, name, flags);
}

/* a memfd holding a copy of this executable */
static int copy_self(unsigned int flags) {
    int src = open("/proc/self/exe", O_RDONLY);
    CHECK(src >= 0);
    int fd = memfd("exe", flags);
    CHECK(fd >= 0);
    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0)
        CHECK(write(fd, buf, (size_t)n) == n);
    CHECK(n == 0);
    close(src);
    return fd;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        puts("TEST OK");
        return 0;
    }

    char path[64];
    char* child_argv[] = {argv[0], "child", NULL};
    char* envp[] = {NULL};

    CHECK(memfd("bad", MFD_EXEC | MFD_NOEXEC_SEAL) == -1 && errno == EINVAL);

    /* MFD_NOEXEC_SEAL: F_SEAL_EXEC is set, and the memfd cannot be executed */
    int noexec = copy_self(MFD_NOEXEC_SEAL);
    CHECK(fcntl(noexec, F_GET_SEALS) & F_SEAL_EXEC);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", noexec);
    CHECK(execve(path, child_argv, envp) == -1 && errno == EACCES);
    close(noexec);

    /* F_SEAL_EXEC on an executable memfd does not stop it from running */
    int fd = copy_self(MFD_ALLOW_SEALING | MFD_CLOEXEC);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_EXEC) == 0);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    fflush(stdout);
    execve(path, child_argv, envp);
    fprintf(stderr, "execve(%s): %s\n", path, strerror(errno));
    return 1;
}
