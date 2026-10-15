/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Host files and sockets of the VBS-enclave PAL (phase 3e). Host layout below the host root
 * directory: data/in.txt (trusted, hash in the manifest), data/bad.txt (trusted, but the host copy
 * does not match its hash), data/secret.txt (not in the manifest), data/out/ (allowed).
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void check(const char* what, int ok, const char* detail) {
    printf("%-48s %s%s%s\n", what, ok ? "ok" : "FAILED", detail ? ": " : "", detail ? detail : "");
    fails += !ok;
}

/* like check(), with the errno text only on failure */
static void checke(const char* what, int ok) {
    check(what, ok, ok ? NULL : strerror(errno));
}

static int g_port;

static void* echo_server(void* arg) {
    int s = *(int*)arg;
    int c = accept(s, NULL, NULL);
    if (c < 0)
        return NULL;
    struct pollfd p = { .fd = c, .events = POLLIN };
    poll(&p, 1, 5000);
    char buf[256];
    ssize_t n = recv(c, buf, sizeof(buf), 0);
    if (n > 0)
        send(c, buf, n, 0);
    close(c);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char buf[512];
    char msg[128];

    /* trusted file */
    int fd = open("/data/in.txt", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (n >= 0)
        buf[n] = 0;
    check("read a trusted file", n > 0 && !strncmp(buf, "trusted data from the host", 26),
          n > 0 ? buf : strerror(errno));
    if (fd >= 0)
        close(fd);
    checke("write to a trusted file refused", open("/data/in.txt", O_WRONLY) < 0);

    /* trusted file whose host copy was changed */
    fd = open("/data/bad.txt", O_RDONLY);
    snprintf(msg, sizeof(msg), "open returned %d (%s)", fd, fd < 0 ? strerror(errno) : "-");
    check("open a trusted file with a wrong hash refused", fd < 0, msg);

    /* not in the manifest */
    struct stat st;
    int r = stat("/data/secret.txt", &st);
    snprintf(msg, sizeof(msg), "stat returned %d (%s)", r, r < 0 ? strerror(errno) : "-");
    check("a host file not in the manifest does not exist", r < 0 && errno == ENOENT, msg);

    /* allowed directory */
    fd = open("/data/out/hello.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    const char* text = "written from inside the enclave\n";
    ssize_t w = fd >= 0 ? write(fd, text, strlen(text)) : -1;
    checke("create and write an allowed file", w == (ssize_t)strlen(text));
    if (fd >= 0) {
        lseek(fd, 0, SEEK_SET);
        n = read(fd, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = 0;
        check("read it back", n == w && !strcmp(buf, text), NULL);
        fstat(fd, &st);
        check("fstat size", st.st_size == w, NULL);
        close(fd);
    }
    checke("mkdir in the allowed directory", mkdir("/data/out/sub", 0755) == 0 || errno == EEXIST);
    checke("rename", rename("/data/out/hello.txt", "/data/out/sub/renamed.txt") == 0);
    DIR* d = opendir("/data/out/sub");
    int found = 0;
    struct dirent* de;
    while (d && (de = readdir(d)))
        found |= !strcmp(de->d_name, "renamed.txt");
    if (d)
        closedir(d);
    check("readdir finds the renamed file", found, NULL);
    checke("unlink", unlink("/data/out/sub/renamed.txt") == 0);
    checke("rmdir", rmdir("/data/out/sub") == 0);

    /* TCP over host loopback: a server thread and a client */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    r = s >= 0 ? bind(s, (struct sockaddr*)&a, sizeof(a)) : -1;
    socklen_t al = sizeof(a);
    if (r == 0)
        r = getsockname(s, (struct sockaddr*)&a, &al);
    if (r == 0)
        r = listen(s, 4);
    g_port = ntohs(a.sin_port);
    snprintf(msg, sizeof(msg), "port %d", g_port);
    check("socket, bind (port 0), getsockname, listen", r == 0 && g_port, r ? strerror(errno) : msg);
    pthread_t t;
    pthread_create(&t, NULL, echo_server, &s);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(c, (struct sockaddr*)&a, sizeof(a));
    checke("connect", r == 0);
    const char* ping = "ping through the host's sockets";
    send(c, ping, strlen(ping), 0);
    struct pollfd p = { .fd = c, .events = POLLIN };
    int pr = poll(&p, 1, 5000);
    n = recv(c, buf, sizeof(buf) - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    snprintf(msg, sizeof(msg), "poll %d, echo \"%s\"", pr, buf);
    check("poll and echo", pr == 1 && !strcmp(buf, ping), msg);
    close(c);
    pthread_join(t, NULL);
    close(s);

    printf("hostio test: %s\n", fails ? "FAILED" : "passed");
    return fails;
}
