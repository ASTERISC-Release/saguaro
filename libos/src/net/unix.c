/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2022 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

/*
 * Implementation of UNIX domain sockets.
 * Currently only stream-oriented sockets are supported (i.e. `SOCK_STREAM`).
 */

/*
 * Pathname UNIX domain sockets are visible on the Gramine filesystem: `bind` creates a node at the
 * path (a marker link that the file systems report as `S_IFSOCK`, see UNIX_SOCKET_NODE_MARKER in
 * `libos_fs.h`), `connect` and `stat` find it from any process of the session on a shared mount,
 * `unlink` removes it. The PAL pipe behind a named socket is named after the canonical absolute
 * path of the node, so relative names, the current directory and ".." resolve as on Linux.
 * Abstract sockets (a leading NUL byte) have no node and are named after their bytes. (Formerly
 * pathname sockets had no dentry and were named after the bytes of the path as given.)
 */

#include "crypto.h"
#include "hex.h"
#include "libos_fs.h"
#ifdef ENABLE_GRAPHICS
#include "libos_graphics.h"
#endif
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_process.h"
#include "libos_socket.h"
#include "linux_socket.h"
#include "pal.h"
#include "perm.h"
#include "stat.h"
#include "toml_utils.h"

/*
 * Host UNIX sockets (gramine-direct). Gramine's UNIX sockets connect Gramine processes only. The
 * manifest can list host socket paths (`sys.insecure__host_unix_sockets`, for example the X
 * server's `/tmp/.X11-unix/X0`); a `connect()` to such a path connects to the host socket itself,
 * through the Linux PAL, and file descriptors passed with `SCM_RIGHTS` are translated between
 * LibOS fds and host fds in both directions.
 */

/* fds per message, as in the PAL (HOST_MSG_MAX_FDS) */
#define HOST_UNIX_MAX_FDS 64

static bool is_host_unix_socket(const char* path, size_t pathlen) {
    toml_table_t* sys = toml_table_in(g_manifest_root, "sys");
    if (!sys)
        return false;
    toml_array_t* paths = toml_array_in(sys, "insecure__host_unix_sockets");
    if (!paths)
        return false;
    for (int i = 0; i < toml_array_nelem(paths); i++) {
        toml_datum_t entry = toml_string_at(paths, i);
        if (!entry.ok)
            continue;
        bool match = strlen(entry.u.s) == pathlen && !memcmp(entry.u.s, path, pathlen);
        free(entry.u.s);
        if (match)
            return true;
    }
    return false;
}

/* Whether the manifest lists any host UNIX socket (sys.insecure__host_unix_sockets). */
static bool host_unix_sockets_configured(void) {
    static int configured = -1;
    int val = __atomic_load_n(&configured, __ATOMIC_RELAXED);
    if (val < 0) {
        toml_table_t* sys = toml_table_in(g_manifest_root, "sys");
        toml_array_t* paths = sys ? toml_array_in(sys, "insecure__host_unix_sockets") : NULL;
        val = paths && toml_array_nelem(paths) > 0;
        __atomic_store_n(&configured, val, __ATOMIC_RELAXED);
    }
    return val;
}

static int host_unix_send(struct libos_handle* handle, PAL_HANDLE pal_handle, struct iovec* iov,
                          size_t iov_len, void* msg_control, size_t msg_controllen,
                          size_t* out_size, bool force_nonblocking) {
    int host_fds[HOST_UNIX_MAX_FDS];
    struct libos_handle* hdls[HOST_UNIX_MAX_FDS];
    size_t fds_cnt = 0;
    int ret = 0;

    struct cmsghdr* cmsg = (struct cmsghdr*)msg_control;
    size_t rest = msg_controllen;
    while (cmsg && rest >= sizeof(struct cmsghdr)) {
        /* the last header may end without padding (libxcb sets `msg_controllen` to CMSG_LEN()),
         * which Linux accepts */
        if (cmsg->cmsg_len < sizeof(struct cmsghdr) || cmsg->cmsg_len > rest) {
            ret = -EINVAL;
            goto out;
        }
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < n; i++) {
                if (fds_cnt == HOST_UNIX_MAX_FDS) {
                    ret = -ETOOMANYREFS;
                    goto out;
                }
                int fd;
                memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
                if (!hdl) {
                    ret = -EBADF;
                    goto out;
                }
                hdls[fds_cnt] = hdl;
                PAL_HANDLE ph = hdl->type == TYPE_SOCK ? hdl->info.sock.pal_handle
                                                       : hdl->pal_handle;
                if (!ph || PalHostFdGet(ph, &host_fds[fds_cnt]) < 0) {
                    log_warning("sendmsg to a host socket: fd %d has no host fd", fd);
                    fds_cnt++;
                    ret = -EBADF;
                    goto out;
                }
                fds_cnt++;
            }
        } else if (cmsg->cmsg_level == SOL_SOCKET) {
            ret = -EINVAL; /* SCM_CREDENTIALS: not supported */
            goto out;
        }
        if (CMSG_ALIGN(cmsg->cmsg_len) >= rest)
            break;
        rest -= CMSG_ALIGN(cmsg->cmsg_len);
        cmsg = (struct cmsghdr*)((char*)cmsg + CMSG_ALIGN(cmsg->cmsg_len));
    }

    lock(&handle->lock);
    bool nonblocking = force_nonblocking || (handle->flags & O_NONBLOCK);
    unlock(&handle->lock);

    ret = PalHostSendmsg(pal_handle, iov, iov_len, host_fds, fds_cnt, nonblocking, out_size);
    ret = pal_to_unix_errno(ret);
out:
    for (size_t i = 0; i < fds_cnt; i++)
        put_handle(hdls[i]);
    return ret;
}

static int host_unix_recv(struct libos_handle* handle, PAL_HANDLE pal_handle, struct iovec* iov,
                          size_t iov_len, void* msg_control, size_t* msg_controllen_ptr,
                          size_t* out_size, bool force_nonblocking) {
    size_t space = (msg_control && msg_controllen_ptr) ? *msg_controllen_ptr : 0;
    size_t max_fds = space >= CMSG_SPACE(sizeof(int))
                         ? MIN((space - CMSG_LEN(0)) / sizeof(int), (size_t)HOST_UNIX_MAX_FDS)
                         : 0;
    int host_fds[HOST_UNIX_MAX_FDS];
    size_t fds_cnt = max_fds;

    lock(&handle->lock);
    bool nonblocking = force_nonblocking || (handle->flags & O_NONBLOCK);
    unlock(&handle->lock);

    int ret = PalHostRecvmsg(pal_handle, iov, iov_len, host_fds, &fds_cnt, nonblocking, out_size);
    if (ret < 0)
        return pal_to_unix_errno(ret);

    size_t installed = 0;
    int fds[HOST_UNIX_MAX_FDS];
    for (size_t i = 0; i < fds_cnt; i++) {
        PAL_HANDLE ph = NULL;
        ret = PalHostFdWrap(host_fds[i], &ph);
        if (ret < 0) {
            log_warning("recvmsg from a host socket: cannot wrap host fd %d: %s", host_fds[i],
                        pal_strerror(ret));
            continue; /* the host fd is leaked open; nothing better to do */
        }
        int fd = hostfd_install(ph, /*cloexec=*/false);
        if (fd < 0)
            continue;
        fds[installed++] = fd;
    }

    if (msg_control && msg_controllen_ptr) {
        if (installed) {
            struct cmsghdr* cmsg = (struct cmsghdr*)msg_control;
            cmsg->cmsg_len = CMSG_LEN(installed * sizeof(int));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            memcpy(CMSG_DATA(cmsg), fds, installed * sizeof(int));
            *msg_controllen_ptr = CMSG_SPACE(installed * sizeof(int));
        } else {
            *msg_controllen_ptr = 0;
        }
    }
    return 0;
}

static int host_unix_connect(struct libos_handle* handle, const char* path, size_t pathlen,
                             pal_stream_options_t options, PAL_HANDLE* out_pal_handle) {
    char host_path[sizeof(((struct sockaddr_un*)0)->sun_path) + 1];
    if (pathlen >= sizeof(host_path))
        return -EINVAL;
    memcpy(host_path, path, pathlen);
    host_path[pathlen] = '\0';

    static bool warned = false;
    if (!__atomic_exchange_n(&warned, true, __ATOMIC_RELAXED)) {
        log_warning("Connecting to host UNIX sockets (sys.insecure__host_unix_sockets), first: %s; "
                    "this is insecure and must not be used in production.", host_path);
    }

    int ret = PalHostUnixConnect(host_path, options, out_pal_handle);
    if (ret < 0)
        return ret == -PAL_ERROR_NOTIMPLEMENTED ? -ENOENT : pal_to_unix_errno(ret);
    handle->info.sock.host_unix = true;
    return 0;
}

/*!
 * \brief Verify UNIX socket address and convert it to a unique socket name.
 *
 * \param         addr            The socket address to convert.
 * \param[in,out] addrlen_ptr     Pointer to the size of \p addr. Always updated to the actual size
 *                                of the address (but it's never extended).
 * \param[out]    sock_name       Buffer for the output socket name. On success contains a null
 *                                terminated string.
 * \param         sock_name_size  Size of \p sock_name.
 */
bool unix_socket_node_perm(const char* link_target, mode_t* out_perm) {
    if (!strstartswith(link_target, UNIX_SOCKET_NODE_MARKER))
        return false;
    const char* digits = link_target + static_strlen(UNIX_SOCKET_NODE_MARKER);
    mode_t perm = 0;
    for (; *digits; digits++) {
        if (*digits < '0' || *digits > '7')
            return false;
        perm = perm * 8 + (*digits - '0');
    }
    *out_perm = perm & 07777;
    return true;
}

/* "<instance id>/<SHA-256 of name>": the name of the PAL pipe behind a UNIX socket */
static int sockname_from_bytes(const char* bytes, size_t len, char* sock_name,
                               size_t sock_name_size) {
    uint8_t hash[32];
    LIB_SHA256_CONTEXT hash_context;
    int ret = lib_SHA256Init(&hash_context);
    if (ret < 0) {
        return -ENOMEM;
    }
    ret = lib_SHA256Update(&hash_context, (const uint8_t*)bytes, len);
    if (ret < 0) {
        return -ENOMEM;
    }
    ret = lib_SHA256Final(&hash_context, hash);
    if (ret < 0) {
        return -ENOMEM;
    }

    char hash_str[64 + 1] = {0};
    bytes2hex(hash, sizeof(hash), hash_str, sizeof(hash_str));

    size_t out_len = snprintf(sock_name, sock_name_size, "%lu/%s", g_pal_public_state->instance_id,
                              hash_str);
    if (out_len >= sock_name_size)
        return -ERANGE;
    return 0;
}

/*
 * The node of a named UNIX socket. With `create` (bind): the path must not exist (EADDRINUSE), its
 * parent must be writable, and the node is created; without (connect): the path must exist and be
 * a socket node (ENOENT, ECONNREFUSED as on Linux). On success `sock_name` names the PAL pipe
 * after the canonical absolute path.
 */
static int named_socket_node(const char* path, bool create, char* sock_name,
                             size_t sock_name_size) {
    struct libos_dentry* dent = NULL;
    char* abs_path = NULL;
    size_t abs_size = 0;
    int ret;

    lock(&g_dcache_lock);
    ret = path_lookupat(/*start=*/NULL, path, LOOKUP_NO_FOLLOW | (create ? LOOKUP_CREATE : 0),
                        &dent);
    if (ret < 0)
        goto out;
    if (create) {
        if (dent->inode || !dent->parent) {
            ret = -EADDRINUSE;
            goto out;
        }
        ret = check_permissions(dent->parent, MAY_WRITE | MAY_EXEC);
        if (ret < 0)
            goto out;
        struct libos_d_ops* d_ops = dent->mount->fs->d_ops;
        if (!d_ops || !d_ops->symlink) {
            ret = -EOPNOTSUPP;
            goto out;
        }
        mode_t perm = PERM_rwxrwxrwx & ~g_process.umask;
        char target[static_strlen(UNIX_SOCKET_NODE_MARKER) + 8];
        snprintf(target, sizeof(target), UNIX_SOCKET_NODE_MARKER "%04o", perm);
        ret = d_ops->symlink(target, dent);
        if (ret < 0) {
            if (ret == -EEXIST)
                ret = -EADDRINUSE;
            goto out;
        }
        /* from now on the node is a socket; the link target is only its on-host encoding */
        struct libos_inode* inode = dent->inode;
        lock(&inode->lock);
        inode->type = S_IFSOCK;
        inode->perm = perm;
        inode->size = 0;
        free(inode->data);
        inode->data = NULL;
        unlock(&inode->lock);
    } else {
        assert(dent->inode);
        if (dent->inode->type != S_IFSOCK) {
            ret = -ECONNREFUSED;
            goto out;
        }
    }
    ret = dentry_abs_path(dent, &abs_path, &abs_size);
    if (ret < 0)
        goto out;
    ret = sockname_from_bytes(abs_path, strlen(abs_path), sock_name, sock_name_size);
out:
    unlock(&g_dcache_lock);
    if (dent)
        put_dentry(dent);
    free(abs_path);
    return ret;
}

/* removes the node that `named_socket_node(..., create=true, ...)` made (bind failed after it) */
static void named_socket_node_remove(const char* path) {
    struct libos_dentry* dent = NULL;
    lock(&g_dcache_lock);
    int ret = path_lookupat(/*start=*/NULL, path, LOOKUP_NO_FOLLOW, &dent);
    if (ret == 0 && dent->inode && dent->inode->type == S_IFSOCK && dent->mount->fs->d_ops &&
            dent->mount->fs->d_ops->unlink) {
        ret = dent->mount->fs->d_ops->unlink(dent);
        if (ret == 0) {
            put_inode(dent->inode);
            dent->inode = NULL;
        }
    }
    unlock(&g_dcache_lock);
    if (dent)
        put_dentry(dent);
}

static int unaddr_to_sockname(void* addr, size_t* addrlen_ptr, bool create, char* sock_name,
                              size_t sock_name_size) {
    if (*addrlen_ptr > sizeof(struct sockaddr_un)) {
        /* Cap the address at the maximal possible size - rest of the input buffer (if any) is
         * ignored. */
        *addrlen_ptr = sizeof(struct sockaddr_un);
    }
    if (*addrlen_ptr < offsetof(struct sockaddr_un, sun_path) + 1) {
        return -EINVAL;
    }
    static_assert(offsetof(struct sockaddr_un, sun_family) < offsetof(struct sockaddr_un, sun_path),
                  "ops");
    unsigned short family;
    memcpy(&family, (char*)addr + offsetof(struct sockaddr_un, sun_family), sizeof(family));
    if (family != AF_UNIX) {
        return -EAFNOSUPPORT;
    }

    const char* path = (char*)addr + offsetof(struct sockaddr_un, sun_path);
    size_t pathlen = *addrlen_ptr - offsetof(struct sockaddr_un, sun_path);
    assert(pathlen >= 1);
    if (!path[0]) {
        /* Abstract UNIX socket: named after its bytes, no node. */
        return sockname_from_bytes(path, pathlen, sock_name, sock_name_size);
    }

    /* Named UNIX socket: named after the canonical path of its node. */
    pathlen = strnlen(path, pathlen);
    char* path_copy = alloc_substr(path, pathlen);
    if (!path_copy)
        return -ENOMEM;
    int ret = named_socket_node(path_copy, create, sock_name, sock_name_size);
    free(path_copy);
    return ret;
}

/* the node's path of a named socket address, NUL-terminated, or NULL for an abstract one */
static char* unaddr_node_path(void* addr, size_t addrlen) {
    const char* path = (char*)addr + offsetof(struct sockaddr_un, sun_path);
    size_t pathlen = addrlen - offsetof(struct sockaddr_un, sun_path);
    if (!path[0])
        return NULL;
    return alloc_substr(path, strnlen(path, pathlen));
}

static void fixup_sockaddr_un_path(struct sockaddr_storage* ss_addr, size_t* addrlen_ptr) {
    /* We know the addr is valid, but it might not contain the ending nullbyte or contain some
     * unnecessary garbage after it. */
    assert(*addrlen_ptr <= sizeof(struct sockaddr_un));
    assert(offsetof(struct sockaddr_un, sun_path) < *addrlen_ptr);
    assert(sizeof(struct sockaddr_un) < sizeof(*ss_addr));
    assert(*addrlen_ptr < sizeof(*ss_addr));

    char* path = (char*)ss_addr + offsetof(struct sockaddr_un, sun_path);
    size_t pathlen = *addrlen_ptr - offsetof(struct sockaddr_un, sun_path);
    assert(pathlen >= 1);
    if (!path[0]) {
        /* Abstract UNIX socket - nothing to do. */
        return;
    }

    pathlen = strnlen(path, pathlen);

    /* Clean unnecessary garbage, if any. */
    assert(sizeof(*ss_addr) - offsetof(struct sockaddr_un, sun_path) - pathlen > 0);
    memset(path + pathlen, 0, sizeof(*ss_addr) - offsetof(struct sockaddr_un, sun_path) - pathlen);

    *addrlen_ptr = offsetof(struct sockaddr_un, sun_path) + pathlen + 1;
    assert(*addrlen_ptr <= sizeof(*ss_addr));
}

static int create(struct libos_handle* handle) {
    assert(handle->info.sock.domain == AF_UNIX);
    assert(handle->info.sock.type == SOCK_STREAM || handle->info.sock.type == SOCK_DGRAM);

    if (handle->info.sock.type == SOCK_DGRAM) {
        /* We use PAL pipes to emulate UNIX sockets. Pipes are streams by their nature, so we have
         * no infrastructure to preserve message (datagram) boundaries - hence datagram UNIX sockets
         * are not supported. */
        return -EPROTONOSUPPORT;
    }
    if (handle->info.sock.protocol != 0) {
        return -EPROTONOSUPPORT;
    }

    handle->info.sock.pal_handle = NULL;
    return 0;
}

static int bind(struct libos_handle* handle, void* addr, size_t addrlen) {
    struct libos_sock_handle* sock = &handle->info.sock;
    assert(locked(&sock->lock));

    char pipe_name[static_strlen(URI_PREFIX_PIPE_SRV) + 96 + 1] = URI_PREFIX_PIPE_SRV;
    int ret = unaddr_to_sockname(addr, &addrlen, /*create=*/true,
                                 pipe_name + static_strlen(URI_PREFIX_PIPE_SRV),
                                 sizeof(pipe_name) - static_strlen(URI_PREFIX_PIPE_SRV));
    if (ret < 0) {
        return ret;
    }

    lock(&handle->lock);
    /* `setflags` in "fs/socket/fs.c" is the only way to change this flag and it takes `sock->lock`,
     * so using `options` after releasing the lock below is race-free. */
    pal_stream_options_t options = handle->flags & O_NONBLOCK ? PAL_OPTION_NONBLOCK : 0;
    unlock(&handle->lock);

    PAL_HANDLE pal_handle = NULL;
    ret = PalStreamOpen(pipe_name, PAL_ACCESS_RDWR, /*share_flags=*/0, PAL_CREATE_IGNORED, options,
                        &pal_handle);
    if (ret < 0) {
        char* node_path = unaddr_node_path(addr, addrlen);
        if (node_path) {
            named_socket_node_remove(node_path);
            free(node_path);
        }
        return (ret == -PAL_ERROR_STREAMEXIST) ? -EADDRINUSE : pal_to_unix_errno(ret);
    }

    __atomic_store_n(&handle->info.sock.pal_handle, pal_handle, __ATOMIC_RELEASE);

    static_assert(sizeof(struct sockaddr_un) < sizeof(sock->local_addr),
                  "need additional space for a nullbyte");
    sock->local_addrlen = addrlen;
    memcpy(&sock->local_addr, addr, sock->local_addrlen);
    /* The address was verified in `unaddr_to_sockname`, so this is safe to call. */
    fixup_sockaddr_un_path(&sock->local_addr, &sock->local_addrlen);

    interrupt_epolls(handle);
    return 0;
}

static int listen(struct libos_handle* handle, unsigned int backlog) {
    /* PAL pipes don't have changeable wait queue size. */
    __UNUSED(backlog);
    struct libos_sock_handle* sock = &handle->info.sock;
    assert(locked(&sock->lock));

    if (sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    /* This socket is already listening - it must have been bound before. */
    assert(sock->state == SOCK_BOUND || sock->state == SOCK_LISTENING);
    return 0;
}

static int accept(struct libos_handle* handle, bool is_nonblocking,
                  struct libos_handle** out_client) {
    pal_stream_options_t options = is_nonblocking ? PAL_OPTION_NONBLOCK : 0;
    PAL_HANDLE pal_handle = __atomic_load_n(&handle->info.sock.pal_handle, __ATOMIC_ACQUIRE);
    /* Since this socket is listening, it must have a PAL handle. */
    assert(pal_handle);
    PAL_HANDLE client_pal_handle;
    int ret = PalStreamWaitForClient(pal_handle, &client_pal_handle, options);
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }

    struct libos_handle* client_handle = get_new_socket_handle(handle->info.sock.domain,
                                                               handle->info.sock.type,
                                                               handle->info.sock.protocol,
                                                               is_nonblocking);
    if (!client_handle) {
        PalObjectDestroy(client_pal_handle);
        return -ENOMEM;
    }

    struct libos_sock_handle* client_sock = &client_handle->info.sock;
    client_sock->state = SOCK_CONNECTED;
    client_sock->pal_handle = client_pal_handle;
    client_sock->can_be_read = true;
    client_sock->can_be_written = true;
    assert(client_sock->ops == &sock_unix_ops);

    client_sock->remote_addr.ss_family = AF_UNIX;
    client_sock->remote_addrlen = sizeof(client_sock->remote_addr.ss_family);

    lock(&handle->info.sock.lock);
    client_sock->local_addrlen = handle->info.sock.local_addrlen;
    memcpy(&client_sock->local_addr, &handle->info.sock.local_addr, client_sock->local_addrlen);
    unlock(&handle->info.sock.lock);

    *out_client = client_handle;
    return 0;
}

static int connect(struct libos_handle* handle, void* addr, size_t addrlen, bool* out_inprogress) {
    struct libos_sock_handle* sock = &handle->info.sock;
    assert(locked(&sock->lock));

    if (sock->state != SOCK_NEW) {
        log_warning("Gramine does not support connect on already bound UNIX socket");
        return -EINVAL;
    }

    lock(&handle->lock);
    /* `setflags` in "fs/socket/fs.c" is the only way to change this flag and it takes `sock->lock`,
     * so using `options` after releasing the lock below is race-free. */
    pal_stream_options_t options = handle->flags & O_NONBLOCK ? PAL_OPTION_NONBLOCK : 0;
    unlock(&handle->lock);

    int ret;
    PAL_HANDLE pal_handle = NULL;
    if (addrlen > sizeof(struct sockaddr_un))
        addrlen = sizeof(struct sockaddr_un);
    if (addrlen < offsetof(struct sockaddr_un, sun_path) + 1)
        return -EINVAL;
    unsigned short family;
    memcpy(&family, (char*)addr + offsetof(struct sockaddr_un, sun_family), sizeof(family));
    if (family != AF_UNIX)
        return -EAFNOSUPPORT;
    const char* path = (char*)addr + offsetof(struct sockaddr_un, sun_path);
    size_t pathlen = addrlen - offsetof(struct sockaddr_un, sun_path);
    if (path[0] && is_host_unix_socket(path, strnlen(path, pathlen))) {
        /* a host socket (manifest list): no node of ours, the host resolves the path */
        ret = host_unix_connect(handle, path, strnlen(path, pathlen), options, &pal_handle);
        if (ret < 0)
            return ret;
        addrlen = offsetof(struct sockaddr_un, sun_path) + strnlen(path, pathlen) + 1;
    } else {
        char pipe_name[static_strlen(URI_PREFIX_PIPE) + 96 + 1] = URI_PREFIX_PIPE;
        ret = unaddr_to_sockname(addr, &addrlen, /*create=*/false,
                                 pipe_name + static_strlen(URI_PREFIX_PIPE),
                                 sizeof(pipe_name) - static_strlen(URI_PREFIX_PIPE));
        if (ret < 0) {
            return ret;
        }
        ret = PalStreamOpen(pipe_name, PAL_ACCESS_RDWR, /*share_flags=*/0, PAL_CREATE_IGNORED,
                            options, &pal_handle);
        if (ret < 0) {
            /* the node exists but nobody listens behind it (or an abstract name is unbound) */
            return (ret == -PAL_ERROR_CONNFAILED) ? -ECONNREFUSED : pal_to_unix_errno(ret);
        }
    }

    assert(sock->pal_handle == NULL);
    __atomic_store_n(&sock->pal_handle, pal_handle, __ATOMIC_RELEASE);

    static_assert(sizeof(struct sockaddr_un) < sizeof(sock->remote_addr),
                  "need additional space for a nullbyte");
    sock->remote_addrlen = addrlen;
    memcpy(&sock->remote_addr, addr, sock->remote_addrlen);
    /* The address was verified in `unaddr_to_sockname`, so this is safe to call. */
    fixup_sockaddr_un_path(&sock->remote_addr, &sock->remote_addrlen);

    if (sock->state != SOCK_BOUND) {
        assert(sock->state == SOCK_NEW);
        sock->local_addr.ss_family = AF_UNIX;
        sock->local_addrlen = sizeof(sock->local_addr.ss_family);
    }

    interrupt_epolls(handle);
    *out_inprogress = false;
    return 0;
}

static int disconnect(struct libos_handle* handle) {
    __UNUSED(handle);
    /* We do not support disconnecting UNIX sockets. */
    return -EINVAL;
}

static int set_socket_option(struct libos_handle* handle, int optname, void* optval, size_t len) {
    /* All currently supported options use `int`. */
    int val;
    if (len < sizeof(val)) {
        return -EINVAL;
    }
    memcpy(&val, optval, sizeof(val));

    switch (optname) {
        case SO_REUSEADDR:
            /* This option has no effect on UNIX sockets (we just save the value). */
            handle->info.sock.reuseaddr = !!val;
            break;
        default:
            return -ENOPROTOOPT;
    }
    return 0;
}

static int setsockopt(struct libos_handle* handle, int level, int optname, void* optval,
                      size_t len) {
    assert(locked(&handle->info.sock.lock));

    switch (level) {
        case SOL_SOCKET:
            return set_socket_option(handle, optname, optval, len);
        default:
            return -ENOPROTOOPT;
    }
}

static int getsockopt(struct libos_handle* handle, int level, int optname, void* optval,
                      size_t* len) {
    /* Nothing to do here. */
    __UNUSED(handle);
    __UNUSED(level);
    __UNUSED(optname);
    __UNUSED(optval);
    __UNUSED(len);
    return -ENOPROTOOPT;
}

static int maybe_force_nonblocking_wrapper(bool force_nonblocking, struct libos_handle* handle,
                                           PAL_HANDLE pal_handle,
                                           int (*func)(PAL_HANDLE, uint64_t, size_t*, void*),
                                           void* buf, size_t* size) {
    /*
     * There are 3 kinds of operations that can race here:
     * 1) operation with force_nonblocking set,
     * 2) operation with force_nonblocking not set,
     * 3) changing blockingness of the handle,
     * and any combination of any of them should yield correct results.
     *
     * 1-1, 2-2, 3-3 races are "normal" and proper locking orders them correctly.
     * 1-2 races - operation 2 does not affect 1; operation 2 will be repeated if handle is
     *             temporarily nonblocking (concurrent operation 1 happening).
     * 1-3 races - we count threads using the handle in temporary nonblocking mode. The first thread
     *             to use it does the change on PAL level and the last thread restores the correct
     *             (currently set in LibOS) mode. Operation 3 takes that into account - sets flags
     *             only in LibOS if `handle->info.sock.force_nonblocking_users_count` is nonzero
     *             (see "libos/src/fs/socket/fs.c"). `handle->lock` provides proper ordering.
     * 2-3 races - this is inherently racy - operation 2 will be either in the old or new mode,
     *             depending on how it ends up ordered vs operation 3. This works exactly the same
     *             on normal Linux - doing operations 2 and 3 concurrently, user app cannot know
     *             whether operation 2 ends up being blocking or not.
     */
    int ret;
    if (force_nonblocking) {
        /* We already have `pal_handle` set, so there is no need for taking `sock->lock`. */
        lock(&handle->lock);
        handle->info.sock.force_nonblocking_users_count += 1;
        if (!(handle->flags & O_NONBLOCK) && handle->info.sock.force_nonblocking_users_count == 1) {
            /* Temporarily set `pal_handle` in nonblocking mode. */
            PAL_STREAM_ATTR attr;
            ret = PalStreamAttributesQueryByHandle(pal_handle, &attr);
            if (ret < 0) {
                unlock(&handle->lock);
                return pal_to_unix_errno(ret);
            }
            assert(!attr.nonblocking);
            attr.nonblocking = true;
            ret = PalStreamAttributesSetByHandle(pal_handle, &attr);
            if (ret < 0) {
                unlock(&handle->lock);
                return pal_to_unix_errno(ret);
            }
        }
        unlock(&handle->lock);
    }

again:
    ret = func(pal_handle, /*offset=*/0, size, buf);
    if (ret < 0) {
        ret = (ret == -PAL_ERROR_TOOLONG) ? -EMSGSIZE : pal_to_unix_errno(ret);
        if (ret == -EAGAIN && !force_nonblocking) {
            lock(&handle->lock);
            bool handle_is_blocking = !(handle->flags & O_NONBLOCK);
            unlock(&handle->lock);
            if (handle_is_blocking) {
                /* Spurious `EAGAIN`, retry. */
                goto again;
            }
        }
    }

    if (force_nonblocking) {
        lock(&handle->lock);
        handle->info.sock.force_nonblocking_users_count -= 1;
        if (!(handle->flags & O_NONBLOCK) && handle->info.sock.force_nonblocking_users_count == 0) {
            /* `pal_handle` was temporarily in nonblocking mode, fix it. */
            PAL_STREAM_ATTR attr;
            int tmp_ret = PalStreamAttributesQueryByHandle(pal_handle, &attr);
            if (tmp_ret < 0) {
                unlock(&handle->lock);
                log_error("nonblocking restore: failed to get handle attrs: %s", pal_strerror(tmp_ret));
                PalProcessExit(1);
            }
            assert(attr.nonblocking);
            attr.nonblocking = false;
            tmp_ret = PalStreamAttributesSetByHandle(pal_handle, &attr);
            if (tmp_ret < 0) {
                unlock(&handle->lock);
                log_error("nonblocking restore: failed to set handle attrs: %s", pal_strerror(tmp_ret));
                PalProcessExit(1);
            }
        }
        unlock(&handle->lock);
    }

    return ret;
}

/*
 * SCM_RIGHTS between Gramine processes. The PAL handle travels with the data
 * (PalStreamWriteWithHandles); the LibOS side of the handle goes as the blob next to it: the kind,
 * the open flags and position, and what the kind needs to rebuild a `struct libos_handle` in the
 * receiver (the path of a file, the address state of a socket, the eventfd flags). The receiver
 * gets a copy of the open file description (Gramine does not share offsets between processes, as
 * after fork). Supported: files, directories and devices of chroot mounts, pipes, sockets with a
 * PAL handle, host eventfds, host fds; others give EOPNOTSUPP.
 */
#define FDPASS_MAGIC 0x53444650u /* "PFDS" */

enum fdpass_kind {
    FDPASS_FILE = 1, FDPASS_PIPE, FDPASS_SOCK, FDPASS_EVENTFD, FDPASS_HOSTFD,
    /* a graphics host-redirected handle (TYPE_HOST/TYPE_HOST_IDENTITY, VM graphics): the fd is the
     * listener's fd plus GRAPHICS_HOST_FD_OFFSET and there is no PAL stream, so the entry carries
     * no cargo; the receiver asks the listener for a dup and installs it at the new number */
    FDPASS_GFXHOST,
};

struct fdpass_hdr {
    uint32_t magic;
    uint32_t count;
};

struct fdpass_entry {
    uint32_t kind;
    int flags;
    int acc_mode;
    uint64_t pos;
    /* FDPASS_SOCK */
    int domain, type, protocol;
    int state;
    bool can_be_read, can_be_written;
    struct sockaddr_storage local_addr, remote_addr;
    size_t local_addrlen, remote_addrlen;
    /* FDPASS_EVENTFD */
    bool is_semaphore;
    /* FDPASS_PIPE */
    struct libos_pipe_handle pipe;
    /* FDPASS_FILE: the absolute path follows, NUL-terminated */
    uint32_t path_len;
    /* whether a PAL handle travels with this entry (all kinds but FDPASS_GFXHOST) */
    bool has_cargo;
    /* FDPASS_GFXHOST: the sender's descriptor (host fd + offset) and the handle type */
    int gfx_fd;
    int gfx_type;
};

/* describes `hdl` (the sender's descriptor `fd`) into `*cur` (advancing it, bounded by `end`);
 * returns its PAL handle, NULL for a kind that travels without one */
static int fdpass_describe(struct libos_handle* hdl, int fd, char** cur, char* end,
                           PAL_HANDLE* out_pal) {
    struct fdpass_entry e = {0};
    PAL_HANDLE pal = NULL;
    char* path = NULL;
    size_t path_size = 0;
    int ret;

    lock(&hdl->lock);
    e.flags = hdl->flags;
    e.acc_mode = hdl->acc_mode;
    unlock(&hdl->lock);
    lock(&hdl->pos_lock);
    e.pos = hdl->pos;
    unlock(&hdl->pos_lock);

    switch (hdl->type) {
        case TYPE_CHROOT:
        case TYPE_DEV:
            /* a file of a chroot mount, or a device node (/dev/null, the console): by path */
            if (!hdl->dentry || !hdl->pal_handle)
                return -EOPNOTSUPP;
            lock(&g_dcache_lock);
            ret = dentry_abs_path(hdl->dentry, &path, &path_size);
            unlock(&g_dcache_lock);
            if (ret < 0)
                return ret;
            e.kind = FDPASS_FILE;
            e.path_len = strlen(path) + 1;
            pal = hdl->pal_handle;
            break;
        case TYPE_PIPE:
            if (!hdl->pal_handle)
                return -EOPNOTSUPP;
            e.kind = FDPASS_PIPE;
            e.pipe = hdl->info.pipe;
            pal = hdl->pal_handle;
            break;
        case TYPE_SOCK: {
            struct libos_sock_handle* sock = &hdl->info.sock;
            pal = __atomic_load_n(&sock->pal_handle, __ATOMIC_ACQUIRE);
            if (!pal || sock->host_unix)
                return -EOPNOTSUPP;
            e.kind = FDPASS_SOCK;
            lock(&sock->lock);
            e.domain = sock->domain;
            e.type = sock->type;
            e.protocol = sock->protocol;
            e.state = sock->state;
            e.can_be_read = sock->can_be_read;
            e.can_be_written = sock->can_be_written;
            e.local_addr = sock->local_addr;
            e.local_addrlen = sock->local_addrlen;
            e.remote_addr = sock->remote_addr;
            e.remote_addrlen = sock->remote_addrlen;
            unlock(&sock->lock);
            break;
        }
        case TYPE_EVENTFD:
            /* only the host eventfd (sys.insecure__allow_eventfd) lives in its PAL handle; the
             * secure one keeps its count in the LibOS of one process */
            if (!hdl->pal_handle || !g_eventfd_passthrough_mode)
                return -EOPNOTSUPP;
            e.kind = FDPASS_EVENTFD;
            e.is_semaphore = hdl->info.eventfd.is_semaphore;
            pal = hdl->pal_handle;
            break;
        case TYPE_HOSTFD:
            if (!hdl->pal_handle)
                return -EOPNOTSUPP;
            e.kind = FDPASS_HOSTFD;
            pal = hdl->pal_handle;
            break;
#ifdef ENABLE_GRAPHICS
        case TYPE_HOST:
        case TYPE_HOST_IDENTITY:
            /* Wine's clients hand the server their DRM device fds (DXVK on the GPU) */
            if (!graphics_redirect_enabled() || fd <= (int)GRAPHICS_HOST_FD_OFFSET)
                return -EOPNOTSUPP;
            e.kind = FDPASS_GFXHOST;
            e.gfx_fd = fd;
            e.gfx_type = hdl->type;
            break;
#endif
        default:
            __UNUSED(fd);
            log_warning("sendmsg: an fd of a handle of type %d cannot be passed to another process",
                        hdl->type);
            return -EOPNOTSUPP;
    }
    e.has_cargo = pal != NULL;
    if ((size_t)(end - *cur) < sizeof(e) + e.path_len) {
        free(path);
        return -EMSGSIZE;
    }
    memcpy(*cur, &e, sizeof(e));
    *cur += sizeof(e);
    if (e.path_len) {
        memcpy(*cur, path, e.path_len);
        *cur += e.path_len;
    }
    free(path);
    *out_pal = pal;
    return 0;
}

/* a chroot file handle for a file that has no path in the receiver (unlinked, or anonymous):
 * a dentry outside the tree named after the last path component, on the mount of the nearest
 * directory that still exists, with an inode from the PAL handle's attributes */
static int fdpass_rebuild_orphan_file(struct libos_handle* hdl, const char* path, PAL_HANDLE pal) {
    PAL_STREAM_ATTR attr;
    int ret = PalStreamAttributesQueryByHandle(pal, &attr);
    if (ret < 0)
        return pal_to_unix_errno(ret);
    mode_t type;
    switch (attr.handle_type) {
        case PAL_TYPE_FILE: type = S_IFREG; break;
        case PAL_TYPE_DEV:  type = S_IFCHR; break;
        default: return -EOPNOTSUPP;
    }

    const char* base = path;
    for (const char* c = path; *c; c++)
        if (*c == '/')
            base = c + 1;
    char* dir_path = alloc_substr(path, base - path);
    if (!dir_path)
        return -ENOMEM;

    lock(&g_dcache_lock);
    struct libos_dentry* dir = NULL;
    struct libos_mount* mount = g_dentry_root->mount;
    char* probe = dir_path;
    while (*probe) {
        if (path_lookupat(/*start=*/NULL, probe, LOOKUP_DIRECTORY, &dir) == 0) {
            mount = dir->mount;
            put_dentry(dir);
            break;
        }
        char* slash = NULL;
        for (char* c = probe; *c; c++)
            if (*c == '/')
                slash = c;
        if (!slash || slash == probe)
            break;
        *slash = '\0';
    }
    struct libos_dentry* dent = get_new_dentry(mount, /*parent=*/NULL, base, strlen(base));
    struct libos_inode* inode = dent ? get_new_inode(mount, type, attr.share_flags) : NULL;
    if (!dent || !inode) {
        if (dent)
            put_dentry(dent);
        unlock(&g_dcache_lock);
        free(dir_path);
        return -ENOMEM;
    }
    inode->size = attr.pending_size;
    dent->inode = inode;
    get_inode(inode);
    hdl->dentry = dent; /* the new dentry's reference */
    hdl->inode = inode;
    hdl->fs = inode->fs;
    unlock(&g_dcache_lock);
    free(dir_path);

    hdl->type = TYPE_CHROOT;
    hdl->seekable = true;
    hdl->pal_handle = pal;
    return 0;
}

/* rebuilds the handle of a description; takes over `pal` (destroyed on failure) */
static int fdpass_rebuild(const struct fdpass_entry* e, const char* path, PAL_HANDLE pal,
                          struct libos_handle** out_hdl, int* out_fixed_fd) {
    struct libos_handle* hdl = NULL;
    int ret;
    *out_fixed_fd = -1;
    switch (e->kind) {
        case FDPASS_FILE: {
            hdl = get_new_handle();
            if (!hdl) {
                ret = -ENOMEM;
                goto fail;
            }
            /* the receiver's own dentry and inode for the path; the PAL handle is the sender's
             * (the same host file) */
            ret = open_namei(hdl, /*start=*/NULL, path,
                             e->flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOFOLLOW), /*mode=*/0,
                             /*found=*/NULL);
            if (ret == -ENOENT || ret == -ENXIO) {
                /* no such path any more (an unlinked temporary file, a memfd: what wineserver
                 * passes for its mappings): a dentry of its own, outside the tree, over the
                 * sender's handle */
                ret = fdpass_rebuild_orphan_file(hdl, path, pal);
                if (ret < 0)
                    goto fail;
                pal = NULL;
                break;
            }
            if (ret < 0)
                goto fail;
            if (hdl->pal_handle && hdl->pal_handle != pal)
                PalObjectDestroy(hdl->pal_handle);
            hdl->pal_handle = pal;
            pal = NULL;
            break;
        }
        case FDPASS_PIPE:
            hdl = get_new_handle();
            if (!hdl) {
                ret = -ENOMEM;
                goto fail;
            }
            hdl->type = TYPE_PIPE;
            hdl->fs = &pipe_builtin_fs;
            hdl->info.pipe = e->pipe;
            hdl->info.pipe.peer_fd = -1;
            hdl->pal_handle = pal;
            pal = NULL;
            break;
        case FDPASS_SOCK: {
            hdl = get_new_socket_handle(e->domain, e->type, e->protocol,
                                        /*is_nonblocking=*/!!(e->flags & O_NONBLOCK));
            if (!hdl) {
                ret = -ENOMEM;
                goto fail;
            }
            struct libos_sock_handle* sock = &hdl->info.sock;
            lock(&sock->lock);
            sock->state = e->state;
            sock->can_be_read = e->can_be_read;
            sock->can_be_written = e->can_be_written;
            sock->local_addr = e->local_addr;
            sock->local_addrlen = e->local_addrlen;
            sock->remote_addr = e->remote_addr;
            sock->remote_addrlen = e->remote_addrlen;
            __atomic_store_n(&sock->pal_handle, pal, __ATOMIC_RELEASE);
            unlock(&sock->lock);
            pal = NULL;
            break;
        }
        case FDPASS_EVENTFD:
            hdl = get_new_handle();
            if (!hdl) {
                ret = -ENOMEM;
                goto fail;
            }
            if (!g_eventfd_passthrough_mode) {
                ret = -EOPNOTSUPP;
                goto fail;
            }
            hdl->type = TYPE_EVENTFD;
            hdl->fs = &eventfd_builtin_fs;
            hdl->info.eventfd.is_semaphore = e->is_semaphore;
            hdl->info.eventfd.broken_in_child = false;
            spinlock_init(&hdl->info.eventfd.lock);
            hdl->pal_handle = pal;
            pal = NULL;
            break;
        case FDPASS_HOSTFD:
            hdl = get_new_handle();
            if (!hdl) {
                ret = -ENOMEM;
                goto fail;
            }
            hdl->type = TYPE_HOSTFD;
            hdl->fs = &hostfd_builtin_fs;
            hdl->pal_handle = pal;
            pal = NULL;
            break;
#ifdef ENABLE_GRAPHICS
        case FDPASS_GFXHOST: {
            /* this process talks to the listener from now on (the wineserver never ran the
             * graphics library); then its own dup of the host fd, at the number the host chose */
            /* (graphics_redirect_enabled() is "comm page mapped and a VM/TDX PAL": the receiver
             * may not have the page yet, so the PAL kind is tested here and the page mapped) */
            const char* host = g_pal_public_state->host_type;
            if (pal || (strcmp(host, "VM") && strcmp(host, "TDX"))) {
                ret = -EOPNOTSUPP;
                goto fail;
            }
            ret = graphics_ensure_comm_mapped();
            if (ret < 0)
                goto fail;
            long dup_fd = graphics_host_dup_request(e->gfx_fd, e->gfx_type);
            if (dup_fd < 0) {
                ret = (int)dup_fd;
                goto fail;
            }
            hdl = get_new_handle();
            if (!hdl) {
                ret = -ENOMEM;
                goto fail;
            }
            hdl->type = e->gfx_type; /* as libos_dup.c builds a dup'ed host handle */
            *out_fixed_fd = (int)dup_fd;
            break;
        }
#endif
        default:
            ret = -EOPNOTSUPP;
            goto fail;
    }
    lock(&hdl->lock);
    hdl->flags = e->flags;
    hdl->acc_mode = e->acc_mode;
    unlock(&hdl->lock);
    lock(&hdl->pos_lock);
    hdl->pos = e->pos;
    unlock(&hdl->pos_lock);
    *out_hdl = hdl;
    return 0;
fail:
    if (hdl)
        put_handle(hdl);
    if (pal)
        PalObjectDestroy(pal);
    return ret;
}

/* installs the received handles as fds and fills the user's SCM_RIGHTS control message */
static void fdpass_deliver(PAL_HANDLE* cargo, size_t cargo_count, const char* meta,
                           size_t meta_size, void* msg_control, size_t* msg_controllen_ptr,
                           bool cmsg_cloexec) {
    size_t space = (msg_control && msg_controllen_ptr) ? *msg_controllen_ptr : 0;
    size_t max_fds = space >= CMSG_LEN(sizeof(int)) ? (space - CMSG_LEN(0)) / sizeof(int) : 0;
    int fds[PAL_STREAM_MAX_HANDLES];
    size_t installed = 0;
    size_t used = 0;

    const char* cur = meta;
    const char* end = meta + meta_size;
    struct fdpass_hdr hdr = {0};
    if (meta_size >= sizeof(hdr))
        memcpy(&hdr, cur, sizeof(hdr));
    cur += sizeof(hdr);
    if (hdr.magic != FDPASS_MAGIC || hdr.count < cargo_count) {
        log_warning("recvmsg: %zu handle(s) came without a usable description; dropped",
                    cargo_count);
        hdr.count = 0;
    }
    /* entries and cargo pair up in order; an entry without cargo (a graphics host handle) takes
     * none, so `used` counts cargo, not entries */
    for (size_t i = 0; i < hdr.count && (size_t)(end - cur) >= sizeof(struct fdpass_entry); i++) {
        struct fdpass_entry e;
        memcpy(&e, cur, sizeof(e));
        cur += sizeof(e);
        const char* path = NULL;
        if (e.path_len) {
            if ((size_t)(end - cur) < e.path_len)
                break;
            path = cur;
            cur += e.path_len;
        }
        PAL_HANDLE pal = NULL;
        if (e.has_cargo) {
            if (used >= cargo_count)
                break;
            pal = cargo[used++];
        }
        if (installed >= max_fds) {
            /* no room in the control message: Linux sets MSG_CTRUNC and closes the surplus */
            if (pal)
                PalObjectDestroy(pal);
            continue;
        }
        struct libos_handle* hdl = NULL;
        int fixed_fd = -1;
        int ret = fdpass_rebuild(&e, path, pal, &hdl, &fixed_fd);
        if (ret < 0) {
            log_warning("recvmsg: cannot rebuild a passed handle of kind %u: %s", e.kind,
                        unix_strerror(ret));
            continue;
        }
        int fd;
        if (fixed_fd >= 0) {
#ifdef ENABLE_GRAPHICS
            ret = graphics_drop_stale_host_handle(fixed_fd);
            if (ret == 0)
                ret = set_new_fd_handle_by_fd(fixed_fd, hdl, cmsg_cloexec ? FD_CLOEXEC : 0, NULL);
#endif
            fd = ret < 0 ? ret : fixed_fd;
        } else {
            fd = set_new_fd_handle(hdl, cmsg_cloexec ? FD_CLOEXEC : 0, NULL);
        }
        put_handle(hdl);
        if (fd < 0)
            continue;
        fds[installed++] = fd;
    }
    for (size_t i = used; i < cargo_count; i++)
        PalObjectDestroy(cargo[i]);

    if (msg_control && msg_controllen_ptr) {
        if (installed) {
            struct cmsghdr* cmsg = (struct cmsghdr*)msg_control;
            cmsg->cmsg_len = CMSG_LEN(installed * sizeof(int));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            memcpy(CMSG_DATA(cmsg), fds, installed * sizeof(int));
            *msg_controllen_ptr = MIN(CMSG_SPACE(installed * sizeof(int)), space);
        } else {
            *msg_controllen_ptr = 0;
        }
    }
}

/* the data plus the described handles through the PAL; honours the nonblocking state like
 * maybe_force_nonblocking_wrapper() (one attempt, EAGAIN surfaces as on Linux) */
static int send_with_handles(bool force_nonblocking, struct libos_handle* handle,
                             PAL_HANDLE pal_handle, void* buf, size_t* size,
                             struct libos_handle** hdls, const int* fds, size_t count) {
    size_t meta_cap = sizeof(struct fdpass_hdr) + count * (sizeof(struct fdpass_entry) + PATH_MAX);
    char* meta = malloc(meta_cap);
    if (!meta)
        return -ENOMEM;
    PAL_HANDLE cargo[PAL_STREAM_MAX_HANDLES];
    size_t cargo_count = 0;
    char* cur = meta + sizeof(struct fdpass_hdr);
    int ret;
    for (size_t i = 0; i < count; i++) {
        PAL_HANDLE pal = NULL;
        ret = fdpass_describe(hdls[i], fds[i], &cur, meta + meta_cap, &pal);
        if (ret < 0)
            goto out;
        if (pal)
            cargo[cargo_count++] = pal;
    }
    struct fdpass_hdr hdr = {.magic = FDPASS_MAGIC, .count = count};
    memcpy(meta, &hdr, sizeof(hdr));

    bool nonblocking = force_nonblocking;
    if (!nonblocking) {
        lock(&handle->lock);
        nonblocking = !!(handle->flags & O_NONBLOCK);
        unlock(&handle->lock);
    }
    PAL_STREAM_ATTR attr;
    bool toggled = false;
    if (nonblocking) {
        /* the PAL handle may be blocking while a forced-nonblocking send is asked for */
        ret = PalStreamAttributesQueryByHandle(pal_handle, &attr);
        if (ret == 0 && !attr.nonblocking) {
            attr.nonblocking = true;
            toggled = PalStreamAttributesSetByHandle(pal_handle, &attr) == 0;
        }
    }
    ret = PalStreamWriteWithHandles(pal_handle, buf, size, cargo, cargo_count, meta, cur - meta);
    if (toggled) {
        attr.nonblocking = false;
        (void)PalStreamAttributesSetByHandle(pal_handle, &attr);
    }
    if (ret < 0)
        ret = ret == -PAL_ERROR_NOTIMPLEMENTED ? -ENOSYS : pal_to_unix_errno(ret);
out:
    free(meta);
    return ret;
}

static int send(struct libos_handle* handle, struct iovec* iov, size_t iov_len, void* msg_control,
                size_t msg_controllen, size_t* out_size, void* addr, size_t addrlen,
                bool force_nonblocking) {
    __UNUSED(addr);
    __UNUSED(addrlen);

    /* Connected SOCK_DGRAM pairs use this stream-oriented PAL pipe path. General UNIX datagram
     * sockets are still rejected by create(). */

    if (handle->info.sock.host_unix) {
        PAL_HANDLE host_pal_handle = __atomic_load_n(&handle->info.sock.pal_handle,
                                                     __ATOMIC_ACQUIRE);
        return host_unix_send(handle, host_pal_handle, iov, iov_len, msg_control, msg_controllen,
                              out_size, force_nonblocking);
    }

    /* the fds of SCM_RIGHTS control messages go with the data (fdpass_describe) */
    struct libos_handle* pass_hdls[PAL_STREAM_MAX_HANDLES];
    int pass_fds[PAL_STREAM_MAX_HANDLES];
    size_t pass_cnt = 0;
    int ret = 0;
    struct cmsghdr* cmsg = (struct cmsghdr*)msg_control;
    size_t rest_msg_controllen = msg_controllen;
    while (cmsg && rest_msg_controllen >= sizeof(struct cmsghdr)) {
        /* the last header may end without padding, which Linux accepts */
        if (cmsg->cmsg_len < sizeof(struct cmsghdr) || cmsg->cmsg_len > rest_msg_controllen) {
            ret = -EINVAL;
            goto out_pass;
        }
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < n; i++) {
                if (pass_cnt == PAL_STREAM_MAX_HANDLES) {
                    ret = -ETOOMANYREFS;
                    goto out_pass;
                }
                int fd;
                memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
                if (!hdl) {
                    ret = -EBADF;
                    goto out_pass;
                }
                pass_fds[pass_cnt] = fd;
                pass_hdls[pass_cnt++] = hdl;
            }
        } else if (cmsg->cmsg_level == SOL_SOCKET) {
            ret = -EINVAL; /* SCM_CREDENTIALS and the rest: not supported */
            goto out_pass;
        }
        /* Linux ignores non-SOL-SOCKET cmsgs instead of erroring out, let's do the same */
        if (CMSG_ALIGN(cmsg->cmsg_len) >= rest_msg_controllen)
            break;
        rest_msg_controllen -= CMSG_ALIGN(cmsg->cmsg_len);
        cmsg = (struct cmsghdr*)((char*)cmsg + CMSG_ALIGN(cmsg->cmsg_len));
    }

    PAL_HANDLE pal_handle = __atomic_load_n(&handle->info.sock.pal_handle, __ATOMIC_ACQUIRE);
    if (!pal_handle) {
        ret = -ENOTCONN;
        goto out_pass;
    }

    void* buf;
    size_t size;
    char* backing_buf = NULL;
    if (iov_len == 1) {
        /* Common case - no need for copying. */
        buf = iov[0].iov_base;
        size = iov[0].iov_len;
    } else {
        size = 0;
        for (size_t i = 0; i < iov_len; i++) {
            size += iov[i].iov_len;
        }
        backing_buf = malloc(size);
        if (!backing_buf) {
            ret = -ENOMEM;
            goto out_pass;
        }
        size = 0;
        for (size_t i = 0; i < iov_len; i++) {
            memcpy(backing_buf + size, iov[i].iov_base, iov[i].iov_len);
            size += iov[i].iov_len;
        }
        buf = backing_buf;
        /* `size` is already correct. */
    }

    if (pass_cnt == 0) {
        ret = maybe_force_nonblocking_wrapper(force_nonblocking, handle, pal_handle, PalStreamWrite,
                                              buf, &size);
    } else {
        ret = send_with_handles(force_nonblocking, handle, pal_handle, buf, &size, pass_hdls, pass_fds,
                                pass_cnt);
    }
    free(backing_buf);
    if (ret == 0)
        *out_size = size;
out_pass:
    for (size_t i = 0; i < pass_cnt; i++)
        put_handle(pass_hdls[i]);
    return ret;
}

static int recv(struct libos_handle* handle, struct iovec* iov, size_t iov_len, void* msg_control,
                size_t* msg_controllen_ptr, size_t* out_size, void* addr, size_t* addrlen_ptr,
                bool force_nonblocking, bool cmsg_cloexec) {
    __UNUSED(addr);
    __UNUSED(addrlen_ptr);

    /* Connected SOCK_DGRAM pairs use this stream-oriented PAL pipe path. General UNIX datagram
     * sockets are still rejected by create(). */

    PAL_HANDLE pal_handle = __atomic_load_n(&handle->info.sock.pal_handle, __ATOMIC_ACQUIRE);
    if (!pal_handle) {
        return -ENOTCONN;
    }

    if (handle->info.sock.host_unix) {
        return host_unix_recv(handle, pal_handle, iov, iov_len, msg_control, msg_controllen_ptr,
                              out_size, force_nonblocking);
    }

    /* A socketpair end whose peer end was sent to a host process (for example the X server, see
     * host_unix_send()) can receive fds from it. With host UNIX sockets configured, a receive that
     * asks for control data goes through the host path when the PAL exposes the host socket. */
    int unused_host_fd;
    if (msg_control && msg_controllen_ptr && *msg_controllen_ptr && host_unix_sockets_configured()
            && PalHostFdGet(pal_handle, &unused_host_fd) == 0) {
        return host_unix_recv(handle, pal_handle, iov, iov_len, msg_control, msg_controllen_ptr,
                              out_size, force_nonblocking);
    }

    void* buf;
    size_t size;
    char* backing_buf = NULL;
    if (iov_len == 1) {
        /* Common simple case. */
        buf = iov[0].iov_base;
        size = iov[0].iov_len;
    } else {
        size = 0;
        for (size_t i = 0; i < iov_len; i++) {
            size += iov[i].iov_len;
        }
        backing_buf = malloc(size);
        if (!backing_buf) {
            return -ENOMEM;
        }
        buf = backing_buf;
        /* `size` is already correct. */
    }

    int ret;
    /* room for at least one fd in the control buffer (CMSG_LEN, as Linux: CMSG_SPACE of one fd
     * would already hold two on x86_64) */
    bool want_handles = msg_control && msg_controllen_ptr
                        && *msg_controllen_ptr >= CMSG_LEN(sizeof(int));
    if (!want_handles) {
        /* a plain read drops handles that came with these bytes, as Linux without a control
         * buffer */
        ret = maybe_force_nonblocking_wrapper(force_nonblocking, handle, pal_handle, PalStreamRead,
                                              buf, &size);
    } else {
        PAL_HANDLE cargo[PAL_STREAM_MAX_HANDLES];
        size_t cargo_count = PAL_STREAM_MAX_HANDLES;
        size_t meta_size = sizeof(struct fdpass_hdr)
                           + PAL_STREAM_MAX_HANDLES * (sizeof(struct fdpass_entry) + PATH_MAX);
        char* meta = malloc(meta_size);
        if (!meta) {
            free(backing_buf);
            return -ENOMEM;
        }
        bool truncated = false;
        bool nonblocking = force_nonblocking;
        if (!nonblocking) {
            lock(&handle->lock);
            nonblocking = !!(handle->flags & O_NONBLOCK);
            unlock(&handle->lock);
        }
        PAL_STREAM_ATTR attr;
        bool toggled = false;
        if (nonblocking) {
            ret = PalStreamAttributesQueryByHandle(pal_handle, &attr);
            if (ret == 0 && !attr.nonblocking) {
                attr.nonblocking = true;
                toggled = PalStreamAttributesSetByHandle(pal_handle, &attr) == 0;
            }
        }
        ret = PalStreamReadWithHandles(pal_handle, buf, &size, cargo, &cargo_count, meta,
                                       &meta_size, &truncated);
        if (toggled) {
            attr.nonblocking = false;
            (void)PalStreamAttributesSetByHandle(pal_handle, &attr);
        }
        if (ret == -PAL_ERROR_NOTIMPLEMENTED) {
            /* a PAL without the feature: the plain read, no handles */
            ret = maybe_force_nonblocking_wrapper(force_nonblocking, handle, pal_handle,
                                                  PalStreamRead, buf, &size);
            cargo_count = 0;
            *msg_controllen_ptr = 0;
        } else if (ret < 0) {
            ret = pal_to_unix_errno(ret);
        } else if (cargo_count || meta_size) {
            /* handles, or a description alone (a graphics host handle carries no PAL handle) */
            fdpass_deliver(cargo, cargo_count, meta, meta_size, msg_control, msg_controllen_ptr,
                           cmsg_cloexec);
        } else {
            *msg_controllen_ptr = 0;
        }
        free(meta);
    }
    if (ret == 0) {
        if (backing_buf) {
            /* Need to copy back to user buffers. */
            size_t copied = 0;
            for (size_t i = 0; i < iov_len && copied < size; i++) {
                size_t this_size = MIN(size - copied, iov[i].iov_len);
                memcpy(iov[i].iov_base, buf + copied, this_size);
                copied += this_size;
            }
            assert(copied == size);
        }
        *out_size = size;
    }
    free(backing_buf);

    if (!want_handles && msg_control && msg_controllen_ptr) {
        *msg_controllen_ptr = 0;
    }

    return ret;
}

struct libos_sock_ops sock_unix_ops = {
    .create = create,
    .bind = bind,
    .listen = listen,
    .accept = accept,
    .connect = connect,
    .disconnect = disconnect,
    .getsockopt = getsockopt,
    .setsockopt = setsockopt,
    .send = send,
    .recv = recv,
};
