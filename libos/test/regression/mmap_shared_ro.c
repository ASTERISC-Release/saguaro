/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * A read-only MAP_SHARED mapping of a file sees the stores another process makes through its
 * writable MAP_SHARED mapping of the same file, without msync (Linux: one page cache). This is how
 * Wine's clients read the wineserver's session block (a memfd mapped PAGE_READONLY in every GUI
 * process, written by the server). Under the VM PAL with multiprocess support, writable shared
 * mappings of a host file share one cache of frames; a read-only one used to be a copy of the host
 * file taken at mmap() time, so it never saw the writer (Documentation/devel/proton-vm/FINDINGS.md,
 * 2026-10-16).
 *
 * Checked, each with a fork and a pipe ordering the two processes: the reader maps before the
 * writer, the writer maps (and stores) before the reader, a memfd passed over a socketpair with
 * the reader mapping it read-only (the server case), and the MAP_PRIVATE forms Wine really uses
 * for a read-only view on Linux (PROT_READ MAP_PRIVATE, and PROT_READ|PROT_WRITE MAP_PRIVATE then
 * mprotect(PROT_READ)): Linux shares the page cache with a private mapping until it is written,
 * so they see the writer too; a store through the private mapping then stays private
 * (copy-on-write) and the writer does not see it. The private forms are checked with the writer
 * mapping first, as the wineserver does (under the VM PAL a private mapping made before any
 * writer exists is still a copy, Documentation/devel/proton-vm/TODO.md).
 */

#define _GNU_SOURCE

#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 4

static long page_size;

static int send_fd(int sock, int fd) {
    char c = 'f';
    struct iovec iov = { .iov_base = &c, .iov_len = 1 };
    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                          .msg_controllen = sizeof(cbuf) };
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

static int recv_fd(int sock) {
    char c;
    struct iovec iov = { .iov_base = &c, .iov_len = 1 };
    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                          .msg_controllen = sizeof(cbuf) };
    if (recvmsg(sock, &msg, 0) != 1)
        return -1;
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_type != SCM_RIGHTS)
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    return fd;
}

static void wait_byte(int fd) {
    char c;
    if (read(fd, &c, 1) != 1)
        err(1, "read sync");
}

static void send_byte(int fd) {
    if (write(fd, "x", 1) != 1)
        err(1, "write sync");
}

enum reader_kind { READER_SHARED, READER_PRIVATE, READER_PRIVATE_RW_THEN_RO };

/* The writer stores a pattern through its writable mapping; the reader checks it through its own
 * mapping (`kind`). `reader_first`: the reader maps before the writer maps and stores. */
static void check_order(const char* name, int fd, bool reader_first, enum reader_kind kind) {
    size_t size = PAGES * page_size;
    int to_child[2], to_parent[2];
    if (pipe(to_child) < 0 || pipe(to_parent) < 0)
        err(1, "pipe");

    pid_t pid = fork();
    if (pid < 0)
        err(1, "fork");
    if (pid == 0) {
        /* the writer; with `reader_first` it waits until the reader has mapped */
        if (reader_first)
            wait_byte(to_child[0]);
        char* w = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (w == MAP_FAILED)
            err(1, "%s: writer mmap", name);
        for (int p = 0; p < PAGES; p++)
            memset(w + p * page_size, 'A' + p, page_size);
        send_byte(to_parent[1]); /* stored */
        wait_byte(to_child[0]);  /* the reader has checked */
        /* a second round: a later store through the same mapping */
        for (int p = 0; p < PAGES; p++)
            memset(w + p * page_size, 'a' + p, page_size);
        send_byte(to_parent[1]);
        wait_byte(to_child[0]);
        if (w[0] != 'a')
            errx(1, "%s: the reader's private store reached the writer (%#x)", name, w[0]);
        if (munmap(w, size) < 0)
            err(1, "writer munmap");
        exit(0);
    }

    /* the reader */
    char* r;
    int rprot = kind == READER_PRIVATE_RW_THEN_RO ? PROT_READ | PROT_WRITE : PROT_READ;
    int rflags = kind == READER_SHARED ? MAP_SHARED : MAP_PRIVATE;
    if (!reader_first)
        wait_byte(to_parent[0]);
    r = mmap(NULL, size, rprot, rflags, fd, 0);
    if (r == MAP_FAILED)
        err(1, "%s: reader mmap", name);
    if (kind == READER_PRIVATE_RW_THEN_RO && mprotect(r, size, PROT_READ) < 0)
        err(1, "%s: reader mprotect", name);
    if (reader_first) {
        send_byte(to_child[1]);
        wait_byte(to_parent[0]);
    }
    for (int p = 0; p < PAGES; p++) {
        if (r[p * page_size] != 'A' + p || r[(p + 1) * page_size - 1] != 'A' + p)
            errx(1, "%s: page %d reads %#x/%#x, expected %#x (first store)", name, p,
                 r[p * page_size], r[(p + 1) * page_size - 1], 'A' + p);
    }
    send_byte(to_child[1]);
    wait_byte(to_parent[0]);
    for (int p = 0; p < PAGES; p++) {
        if (r[p * page_size] != 'a' + p)
            errx(1, "%s: page %d reads %#x, expected %#x (second store)", name, p,
                 r[p * page_size], 'a' + p);
    }
    if (kind != READER_SHARED) {
        /* copy-on-write: a store through the private mapping is ours alone */
        if (mprotect(r, size, PROT_READ | PROT_WRITE) < 0)
            err(1, "%s: reader mprotect to rw", name);
        r[0] = 'Z';
        if (r[0] != 'Z' || r[page_size] != 'b')
            errx(1, "%s: private store not seen by its own mapping (%#x %#x)", name, r[0],
                 r[page_size]);
        char* again = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (again == MAP_FAILED)
            err(1, "%s: reader second mmap", name);
        if (again[0] != 'a')
            errx(1, "%s: private store leaked to the file (%#x)", name, again[0]);
        if (munmap(again, size) < 0)
            err(1, "munmap again");
    }
    send_byte(to_child[1]);
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        errx(1, "%s: writer exited with %d", name, status);
    if (munmap(r, size) < 0)
        err(1, "reader munmap");
    close(to_child[0]); close(to_child[1]); close(to_parent[0]); close(to_parent[1]);
    printf("%s: ok\n", name);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* lines are printed around fork() */
    page_size = sysconf(_SC_PAGESIZE);
    size_t size = PAGES * page_size;

    /* a host file */
    const char* path = "tmp/mmap_shared_ro.data";
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        err(1, "open %s", path);
    if (ftruncate(fd, size) < 0)
        err(1, "ftruncate");
    check_order("host file, reader first", fd, /*reader_first=*/true, READER_SHARED);
    check_order("host file, writer first", fd, /*reader_first=*/false, READER_SHARED);
    check_order("host file, private reader", fd, /*reader_first=*/false, READER_PRIVATE);
    check_order("host file, private rw-then-ro reader", fd, /*reader_first=*/false,
                READER_PRIVATE_RW_THEN_RO);
    close(fd);
    unlink(path);

    /* a memfd received over a socketpair (the wineserver's session block) */
    int mfd = (int)syscall(SYS_memfd_create, "mmap_shared_ro", 0);
    if (mfd < 0)
        err(1, "memfd_create");
    if (ftruncate(mfd, size) < 0)
        err(1, "ftruncate memfd");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        err(1, "socketpair");
    if (send_fd(sv[0], mfd) < 0)
        err(1, "send_fd");
    int rfd = recv_fd(sv[1]);
    if (rfd < 0)
        err(1, "recv_fd");
    close(mfd);
    check_order("memfd over SCM_RIGHTS, reader first", rfd, /*reader_first=*/true, READER_SHARED);
    check_order("memfd over SCM_RIGHTS, writer first", rfd, /*reader_first=*/false, READER_SHARED);
    check_order("memfd over SCM_RIGHTS, private rw-then-ro reader", rfd, /*reader_first=*/false,
                READER_PRIVATE_RW_THEN_RO);
    close(rfd);
    close(sv[0]);
    close(sv[1]);

    puts("TEST OK");
    return 0;
}
