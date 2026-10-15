#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "rw_file.h"

#define SRV_ADDR_NONEXISTING "/tmp/nonexisting/nonexisting"
#define SRV_ADDR_DUMMY       "tmp/dummy"
#define SRV_ADDR             "tmp/unix_socket"

static const char g_buffer[] = "Hello from UDS server!";

static void check_nonexisting_socket(void) {
    int s = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));

    struct sockaddr_un sa = {
        .sun_family = AF_UNIX,
        .sun_path = SRV_ADDR_NONEXISTING,
    };

    int ret = connect(s, (void*)&sa, sizeof(sa));
    if (ret == 0 || errno != ENOENT) {
        errx(1, "nonexisting-socket connect didn't fail with ENOENT");
    }

    CHECK(close(s));
}

static void create_dummy_socket(void) {
    int s = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));

    struct sockaddr_un sa = {
        .sun_family = AF_UNIX,
        .sun_path = SRV_ADDR_DUMMY,
    };
    CHECK(bind(s, (void*)&sa, sizeof(sa)));
    CHECK(listen(s, 5));
    /* do not close this socket to test two sockets in parallel */
}

static void server(int pipefd) {
    int s = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));

    struct sockaddr_un sa = {
        .sun_family = AF_UNIX,
        .sun_path = SRV_ADDR,
    };
    CHECK(bind(s, (void*)&sa, sizeof(sa)));
    CHECK(listen(s, 5));

    char c = 0;
    ssize_t x = CHECK(write(pipefd, &c, sizeof(c)));
    if (x != sizeof(c)) {
        errx(1, "client terminated unexpectedly");
    }
    CHECK(close(pipefd));

    int client = CHECK(accept(s, NULL, NULL));

    CHECK(close(s));

    x = CHECK(posix_fd_write(client, g_buffer, sizeof(g_buffer)));
    if (x != sizeof(g_buffer)) {
        errx(1, "partial write to client");
    }

    CHECK(close(client));
}

static void client(int pipefd) {
    char c = 0;
    ssize_t x = CHECK(read(pipefd, &c, sizeof(c)));
    if (x != sizeof(c)) {
        errx(1, "server terminated unexpectedly");
    }
    CHECK(close(pipefd));

    /* named UNIX domain sockets create FS nodes that another process sees, verify it */
    struct stat statbuf;
    CHECK(lstat(SRV_ADDR, &statbuf));
    if (statbuf.st_uid != getuid() || statbuf.st_gid != getgid()) {
        errx(1, "unexpected UID/GID of file `%s`", SRV_ADDR);
    }
    if ((statbuf.st_mode & S_IFMT) != S_IFSOCK) {
        errx(1, "file `%s` is not a UNIX domain socket (mode %#o)", SRV_ADDR, statbuf.st_mode);
    }
    if (statbuf.st_mode & 022) {
        errx(1, "unexpected permissions %#o of `%s` (umask 022)", statbuf.st_mode, SRV_ADDR);
    }
    int fd = open(SRV_ADDR, O_RDONLY);
    if (fd != -1 || errno != ENXIO) {
        errx(1, "open of a socket node didn't fail with ENXIO");
    }

    /* the same node, named relative to another directory (the pipe name is canonical) */
    CHECK(chdir("tmp"));
    struct stat statbuf2;
    CHECK(lstat("unix_socket", &statbuf2));
    if (statbuf2.st_ino != statbuf.st_ino && (statbuf2.st_mode & S_IFMT) != S_IFSOCK) {
        errx(1, "`tmp/unix_socket` seen from `tmp` is not the socket");
    }
    CHECK(chdir(".."));

    int s = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));

    struct sockaddr_un sa = {
        .sun_family = AF_UNIX,
        .sun_path = SRV_ADDR,
    };
    CHECK(connect(s, (void*)&sa, sizeof(sa)));

    char buf[sizeof(g_buffer) + 1] = { 0 };
    x = CHECK(posix_fd_read(s, buf, sizeof(buf) - 1));
    if (x != sizeof(g_buffer)) {
        errx(1, "partial read from server");
    }

    if (strcmp(buf, g_buffer)) {
        errx(1, "unexpected message from server: expected %s, got %s", g_buffer, buf);
    }

    CHECK(close(s));
}

int main(void) {
    check_nonexisting_socket();
    create_dummy_socket();

    int pipefds[2];
    CHECK(pipe(pipefds));

    pid_t p_s = CHECK(fork());
    if (p_s == 0) {
        CHECK(close(pipefds[0]));
        server(pipefds[1]);
        return 0;
    }

    pid_t p_c = CHECK(fork());
    if (p_c == 0) {
        CHECK(close(pipefds[1]));
        client(pipefds[0]);
        return 0;
    }

    CHECK(close(pipefds[0]));
    CHECK(close(pipefds[1]));

    for (size_t i = 0; i < 2; i++) {
        int status = 0;
        CHECK(wait(&status));
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            errx(1, "child wait status: %#x", status);
        }
    }

    /* a second bind on the node: EADDRINUSE */
    int s2 = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));
    struct sockaddr_un sa_dummy = {
        .sun_family = AF_UNIX,
        .sun_path = SRV_ADDR_DUMMY,
    };
    if (bind(s2, (void*)&sa_dummy, sizeof(sa_dummy)) == 0 || errno != EADDRINUSE) {
        errx(1, "second bind on `%s` didn't fail with EADDRINUSE", SRV_ADDR_DUMMY);
    }

    /* the server of SRV_ADDR is gone, its node stays: connect gives ECONNREFUSED */
    struct sockaddr_un sa_stale = {
        .sun_family = AF_UNIX,
        .sun_path = SRV_ADDR,
    };
    if (connect(s2, (void*)&sa_stale, sizeof(sa_stale)) == 0 || errno != ECONNREFUSED) {
        errx(1, "connect to the stale node `%s` didn't fail with ECONNREFUSED (%s)", SRV_ADDR,
             strerror(errno));
    }
    CHECK(close(s2));

    /* a regular file is not a socket: ECONNREFUSED */
    int f = CHECK(open("tmp/not_a_socket", O_WRONLY | O_CREAT | O_TRUNC, 0600));
    CHECK(close(f));
    int s3 = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));
    struct sockaddr_un sa_file = {
        .sun_family = AF_UNIX,
        .sun_path = "tmp/not_a_socket",
    };
    if (connect(s3, (void*)&sa_file, sizeof(sa_file)) == 0 || errno != ECONNREFUSED) {
        errx(1, "connect to a regular file didn't fail with ECONNREFUSED (%s)", strerror(errno));
    }
    CHECK(close(s3));
    CHECK(unlink("tmp/not_a_socket"));

    /* the nodes are removed with unlink, and connect then says ENOENT */
    CHECK(unlink(SRV_ADDR_DUMMY));
    CHECK(unlink(SRV_ADDR));
    struct stat statbuf;
    if (lstat(SRV_ADDR, &statbuf) == 0 || errno != ENOENT) {
        errx(1, "`%s` still exists after unlink", SRV_ADDR);
    }
    int s4 = CHECK(socket(AF_UNIX, SOCK_STREAM, 0));
    if (connect(s4, (void*)&sa_stale, sizeof(sa_stale)) == 0 || errno != ENOENT) {
        errx(1, "connect after unlink didn't fail with ENOENT (%s)", strerror(errno));
    }
    CHECK(close(s4));

    puts("TEST OK");
    return 0;
}
