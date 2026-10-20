/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * The LibOS's NETLINK_ROUTE emulation (libos/src/net/netlink.c): glibc's if_nameindex() and
 * getifaddrs() must return the loopback interface (index 1, "lo", 127.0.0.1/8, ::1 and a packet
 * entry), a raw RTM_GETLINK dump must carry the request's sequence number and the socket's port
 * id and end with NLMSG_DONE, an unknown request must get NLMSG_ERROR, and other netlink
 * protocols must be refused. Wine's nsiproxy.sys dereferences if_nameindex()'s result without a
 * NULL check (dev docs: proton-vm/FINDINGS.md, 2026-10-16).
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    /* if_nameindex */
    struct if_nameindex* ix = if_nameindex();
    if (!ix)
        err(1, "if_nameindex");
    bool lo_found = false;
    int count = 0;
    for (struct if_nameindex* e = ix; e->if_index; e++) {
        count++;
        if (e->if_index == 1 && !strcmp(e->if_name, "lo"))
            lo_found = true;
    }
    if_freenameindex(ix);
    if (!lo_found)
        errx(1, "if_nameindex: no lo with index 1 (%d entries)", count);
    printf("if_nameindex: %d interface(s), lo is index 1\n", count);
    /* not checked: if_nametoindex()/if_indextoname() use SIOCGIFINDEX/SIOCGIFNAME on an inet
     * socket, which the LibOS does not emulate (dev docs: proton-vm/TODO.md) */

    /* getifaddrs */
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) < 0)
        err(1, "getifaddrs");
    bool v4 = false, v6 = false, packet = false;
    for (struct ifaddrs* a = ifa; a; a = a->ifa_next) {
        if (strcmp(a->ifa_name, "lo") || !a->ifa_addr)
            continue;
        if (!(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_LOOPBACK))
            errx(1, "lo flags %#x", a->ifa_flags);
        if (a->ifa_addr->sa_family == AF_INET) {
            char buf[64];
            inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, buf, sizeof(buf));
            if (strcmp(buf, "127.0.0.1"))
                errx(1, "lo inet %s", buf);
            v4 = true;
        } else if (a->ifa_addr->sa_family == AF_INET6) {
            char buf[64];
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)a->ifa_addr)->sin6_addr, buf, sizeof(buf));
            if (strcmp(buf, "::1"))
                errx(1, "lo inet6 %s", buf);
            v6 = true;
        } else if (a->ifa_addr->sa_family == AF_PACKET) {
            packet = true;
        }
    }
    freeifaddrs(ifa);
    if (!v4 || !v6 || !packet)
        errx(1, "getifaddrs: lo inet %d inet6 %d packet %d", v4, v6, packet);
    puts("getifaddrs: lo has inet, inet6 and packet entries");

    /* a raw dump: sequence number, port id, NLMSG_DONE */
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        err(1, "socket(AF_NETLINK)");
    struct sockaddr_nl nladdr = {.nl_family = AF_NETLINK};
    if (bind(fd, (struct sockaddr*)&nladdr, sizeof(nladdr)) < 0)
        err(1, "bind");
    socklen_t alen = sizeof(nladdr);
    if (getsockname(fd, (struct sockaddr*)&nladdr, &alen) < 0 || nladdr.nl_pid == 0)
        err(1, "getsockname");
    struct {
        struct nlmsghdr nlh;
        struct rtgenmsg g;
    } req = {
        .nlh = {.nlmsg_len = sizeof(req), .nlmsg_type = RTM_GETLINK,
                .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 0x1234},
        .g = {.rtgen_family = AF_UNSPEC},
    };
    if (send(fd, &req, sizeof(req), 0) != sizeof(req))
        err(1, "send RTM_GETLINK");
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0)
        err(1, "recv");
    bool link = false, done = false;
    size_t remaining = n;
    for (struct nlmsghdr* h = (struct nlmsghdr*)buf; NLMSG_OK(h, remaining); h = NLMSG_NEXT(h, remaining)) {
        if (h->nlmsg_seq != 0x1234 || h->nlmsg_pid != nladdr.nl_pid)
            errx(1, "reply seq %#x pid %u (expected %#x %u)", h->nlmsg_seq, h->nlmsg_pid, 0x1234,
                 nladdr.nl_pid);
        if (h->nlmsg_type == RTM_NEWLINK)
            link = true;
        if (h->nlmsg_type == NLMSG_DONE)
            done = true;
    }
    if (!link || !done)
        errx(1, "dump: link %d done %d (%zd bytes)", link, done, n);
    printf("RTM_GETLINK dump: %zd bytes, RTM_NEWLINK and NLMSG_DONE, seq and pid as requested\n", n);

    /* an unknown request: NLMSG_ERROR */
    req.nlh.nlmsg_type = RTM_GETROUTE;
    req.nlh.nlmsg_seq = 7;
    if (send(fd, &req, sizeof(req), 0) != sizeof(req))
        err(1, "send RTM_GETROUTE");
    n = recv(fd, buf, sizeof(buf), 0);
    if (n < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)))
        err(1, "recv error reply (%zd)", n);
    struct nlmsghdr* h = (struct nlmsghdr*)buf;
    if (h->nlmsg_type != NLMSG_ERROR || ((struct nlmsgerr*)NLMSG_DATA(h))->error != -EOPNOTSUPP)
        errx(1, "RTM_GETROUTE: type %u error %d", h->nlmsg_type,
             ((struct nlmsgerr*)NLMSG_DATA(h))->error);
    puts("RTM_GETROUTE: NLMSG_ERROR EOPNOTSUPP");
    close(fd);

    /* other protocols are refused */
    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (fd >= 0 || errno != EPROTONOSUPPORT)
        errx(1, "NETLINK_KOBJECT_UEVENT: fd %d errno %d", fd, errno);
    puts("NETLINK_KOBJECT_UEVENT: EPROTONOSUPPORT");

    puts("TEST OK");
    return 0;
}
