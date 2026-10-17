/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * unixsock -- named AF_UNIX sockets between processes (the `shared` probe only has a socketpair):
 * the parent binds and listens on a path (stream) and on an abstract name, a forked child
 * connects to each and exchanges a message. Prints "unixsock: 2/2 passed". (A `SOCK_DGRAM`
 * AF_UNIX socket is not supported on the VM PAL: bind fails with "Protocol not supported", so it
 * is not tested.)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass;

static socklen_t make_addr(struct sockaddr_un* a, const char* name, int abstract) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    if (abstract) {
        strcpy(a->sun_path + 1, name);
        return offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);
    }
    strcpy(a->sun_path, name);
    return sizeof(*a);
}

static int stream_case(const char* name, int abstract) {
    struct sockaddr_un a;
    socklen_t len = make_addr(&a, name, abstract);
    if (!abstract)
        unlink(name);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0 || bind(ls, (struct sockaddr*)&a, len) < 0 || listen(ls, 4) < 0) {
        printf("unixsock: %s: bind/listen: %s\n", name, strerror(errno));
        return 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        char buf[16] = {0};
        if (c < 0 || connect(c, (struct sockaddr*)&a, len) < 0 || write(c, "ping", 4) != 4 ||
                read(c, buf, sizeof(buf) - 1) != 4 || strcmp(buf, "pong"))
            _exit(1);
        _exit(0);
    }
    int s = accept(ls, NULL, NULL);
    char buf[16] = {0};
    int ok = s >= 0 && read(s, buf, sizeof(buf) - 1) == 4 && !strcmp(buf, "ping") &&
             write(s, "pong", 4) == 4;
    int status = 0;
    waitpid(pid, &status, 0);
    ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("unixsock: stream %s %s\n", abstract ? "abstract" : "path", ok ? "ok" : "FAIL");
    close(ls);
    if (!abstract)
        unlink(name);
    return ok;
}

int main(void) {
    g_pass += stream_case("/tmp/unixsock-probe.sock", 0);
    g_pass += stream_case("unixsock-probe-abstract", 1);
    printf("unixsock: %d/2 passed\n", g_pass);
    return g_pass == 2 ? 0 : 1;
}
