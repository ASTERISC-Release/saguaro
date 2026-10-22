/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * gfx-fdpass: a graphics host-redirected fd (the render node, opened through the VM graphics
 * redirection: the guest fd is the listener's fd plus GRAPHICS_FS_OFFSET) passed with SCM_RIGHTS
 * to another guest process, as Wine's clients hand their DRM fds to the wineserver. Two receivers:
 * a forked child (it shares the parent's graphics session), and an exec'd child started with
 * EGL_EAGER_SESSION=0 that never touched graphics (the LibOS maps the comm page for it when the
 * fd arrives, as for the wineserver). Each receiver does fstat, DRM_IOCTL_VERSION and close on the
 * received fd. Under saguaro-vm: `make vm-gfx-fdpass.manifest MULTIPROCESS=1 EAGER=1` (the parent's
 * session starts at load), see vm-check.sh. Usage: gfx-fdpass [recv <socket fd>].
 */

#define _GNU_SOURCE
#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

static const char* g_node = "/dev/dri/renderD129";

static int send_fd(int sock, int fd) {
    char byte = 'F';
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof(buf)};
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == 1 ? 0 : -errno;
}

static int recv_fd(int sock) {
    char byte = 0;
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof(buf)};
    if (recvmsg(sock, &msg, 0) != 1)
        return -errno;
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(sizeof(int)))
        return -EBADMSG;
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

/* the receiver's checks on the fd it got: 0 on success, the failing step's -errno otherwise */
static int check_received(const char* who, int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) {
        printf("gfx-fdpass: %s: fstat(%d): %s\n", who, fd, strerror(errno));
        return -errno;
    }
    char name[64] = {0}, date[64] = {0}, desc[64] = {0};
    struct drm_version v = {.name = name, .name_len = sizeof(name) - 1, .date = date,
                            .date_len = sizeof(date) - 1, .desc = desc, .desc_len = sizeof(desc) - 1};
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0) {
        printf("gfx-fdpass: %s: DRM_IOCTL_VERSION(%d): %s\n", who, fd, strerror(errno));
        return -errno;
    }
    printf("gfx-fdpass: %s: fd %d: char device %d:%d, driver '%s' %d.%d.%d\n", who, fd,
           major(st.st_rdev), minor(st.st_rdev), name, v.version_major, v.version_minor,
           v.version_patchlevel);
    if (!S_ISCHR(st.st_mode) || !name[0])
        return -EINVAL;
    if (close(fd) < 0) {
        printf("gfx-fdpass: %s: close(%d): %s\n", who, fd, strerror(errno));
        return -errno;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "recv")) {
        /* the exec'd receiver: no graphics call of its own before the fd arrives */
        int sock = atoi(argv[2]);
        int fd = recv_fd(sock);
        if (fd < 0) {
            printf("gfx-fdpass: exec'd child: recvmsg: %s\n", strerror(-fd));
            return 2;
        }
        return check_received("exec'd child", fd) == 0 ? 0 : 3;
    }

    const char* node = getenv("GFX_FDPASS_NODE");
    if (node)
        g_node = node;
    int drm = open(g_node, O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        printf("gfx-fdpass: open(%s): %s\ngfx-fdpass: RESULT FAIL\n", g_node, strerror(errno));
        return 1;
    }
    printf("gfx-fdpass: parent: %s is fd %d\n", g_node, drm);
    fflush(stdout);
    int failed = 0;

    /* 1. a forked child */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int fd = recv_fd(sv[1]);
        if (fd < 0) {
            printf("gfx-fdpass: forked child: recvmsg: %s\n", strerror(-fd));
            _exit(2);
        }
        _exit(check_received("forked child", fd) == 0 ? 0 : 3);
    }
    close(sv[1]);
    int ret = send_fd(sv[0], drm);
    if (ret < 0)
        printf("gfx-fdpass: parent: sendmsg to the forked child: %s\n", strerror(-ret));
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    printf("gfx-fdpass: forked child exited %d\n", code);
    failed += (ret < 0 || code != 0);
    close(sv[0]);

    /* 2. an exec'd child with no session of its own */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char sockarg[16];
        snprintf(sockarg, sizeof(sockarg), "%d", sv[1]);
        setenv("EGL_EAGER_SESSION", "0", 1);
        execl(argv[0], argv[0], "recv", sockarg, (char*)NULL);
        printf("gfx-fdpass: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(sv[1]);
    ret = send_fd(sv[0], drm);
    if (ret < 0)
        printf("gfx-fdpass: parent: sendmsg to the exec'd child: %s\n", strerror(-ret));
    status = 0;
    waitpid(pid, &status, 0);
    code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    printf("gfx-fdpass: exec'd child exited %d\n", code);
    failed += (ret < 0 || code != 0);
    close(sv[0]);

    /* the parent's own fd still works after the receivers closed theirs */
    char name[64] = {0};
    struct drm_version v = {.name = name, .name_len = sizeof(name) - 1};
    if (ioctl(drm, DRM_IOCTL_VERSION, &v) < 0) {
        printf("gfx-fdpass: parent: DRM_IOCTL_VERSION after the passes: %s\n", strerror(errno));
        failed++;
    }
    close(drm);
    printf("gfx-fdpass: RESULT %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
