/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/* Cross-process VM PAL qualification: inherited channels and two kinds of shared mappings. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct shared {
    int futex;
    int value;
};

static void must(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "%s: %s\n", what, strerror(errno));
        exit(1);
    }
}

static int wait_for_value(int* word, int wanted) {
    struct timespec timeout = { .tv_sec = 5 };
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != wanted) {
        int seen = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (syscall(SYS_futex, word, FUTEX_WAIT, seen, &timeout) < 0 && errno != EAGAIN)
            return -1;
    }
    return 0;
}

int main(void) {
    int to_child[2], to_parent[2], sock[2];
    must(pipe(to_child) == 0 && pipe(to_parent) == 0, "pipe");
    must(socketpair(AF_UNIX, SOCK_STREAM, 0, sock) == 0, "socketpair");
    struct shared* shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    must(shared != MAP_FAILED, "MAP_SHARED anonymous");

    char path[128];
    snprintf(path, sizeof(path), "/dev/shm/aarch64-mp-shared-%ld", (long)getpid());
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    must(fd >= 0 && ftruncate(fd, 4096) == 0, "/dev/shm create");
    char* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    must(shm != MAP_FAILED, "/dev/shm map");
    shm[0] = 'A';

    pid_t child = fork();
    must(child >= 0, "fork");
    if (!child) {
        char c;
        close(to_child[1]);
        close(to_parent[0]);
        close(sock[0]);
        if (read(to_child[0], &c, 1) != 1 || c != 'P' || write(to_parent[1], "C", 1) != 1)
            _exit(2);
        if (read(sock[1], &c, 1) != 1 || c != 'U' || write(sock[1], "V", 1) != 1)
            _exit(3);
        if (wait_for_value(&shared->futex, 1) < 0 || shared->value != 42)
            _exit(4);
        shared->value = 99;
        __atomic_store_n(&shared->futex, 2, __ATOMIC_RELEASE);
        syscall(SYS_futex, &shared->futex, FUTEX_WAKE, 1);
        int child_fd = open(path, O_RDWR);
        if (child_fd < 0)
            _exit(5);
        char* child_shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, child_fd, 0);
        if (child_shm == MAP_FAILED || child_shm[0] != 'A')
            _exit(6);
        child_shm[0] = 'B';
        _exit(0);
    }

    close(to_child[0]);
    close(to_parent[1]);
    close(sock[1]);
    char c;
    must(write(to_child[1], "P", 1) == 1 && read(to_parent[0], &c, 1) == 1 && c == 'C',
         "pipe across fork");
    must(write(sock[0], "U", 1) == 1 && read(sock[0], &c, 1) == 1 && c == 'V',
         "Unix socketpair across fork");
    shared->value = 42;
    __atomic_store_n(&shared->futex, 1, __ATOMIC_RELEASE);
    must(syscall(SYS_futex, &shared->futex, FUTEX_WAKE, 1) >= 0, "futex wake");
    must(wait_for_value(&shared->futex, 2) == 0 && shared->value == 99,
         "MAP_SHARED and futex across fork");
    int status;
    must(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "child completion");
    must(shm[0] == 'B', "/dev/shm visibility through a separate mapping");
    printf("shared: 5/5 passed (pipes, Unix sockets, MAP_SHARED, futex, /dev/shm)\n");
    unlink(path);
    return 0;
}
