/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Host requests of the VBS-enclave PAL (PAL_VBS_OCALL_HOST, struct pal_vbs_hostreq in
 * pal_vbs_abi.h): host files with Win32 file calls, host sockets with Winsock. The host is
 * untrusted; the vbs PAL decides what it asks for (trusted and allowed files, sockets) and checks
 * what comes back. Values on the wire follow Linux (open flags, socket constants, sockaddr layouts,
 * negative errno); this file translates them.
 *
 * Paths are absolute Linux-style paths below the host root directory (the host's working
 * directory, or the 4th argument of pal_vbs_host.exe): "/data/x" -> "<root>\data\x". A path with a
 * ".." component is refused.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pal_vbs_abi.h"

/* Linux values */
#define L_EPERM        1
#define L_ENOENT       2
#define L_EIO          5
#define L_EBADF        9
#define L_EAGAIN       11
#define L_ENOMEM       12
#define L_EACCES       13
#define L_EEXIST       17
#define L_ENOTDIR      20
#define L_EISDIR       21
#define L_EINVAL       22
#define L_EMFILE       24
#define L_ENOSPC       28
#define L_EPIPE        32
#define L_ENAMETOOLONG 36
#define L_ENOTEMPTY    39
#define L_ENOTSOCK     88
#define L_EMSGSIZE     90
#define L_EOPNOTSUPP   95
#define L_EAFNOSUPPORT 97
#define L_EADDRINUSE   98
#define L_EADDRNOTAVAIL 99
#define L_ENETUNREACH  101
#define L_ECONNABORTED 103
#define L_ECONNRESET   104
#define L_EISCONN      106
#define L_ENOTCONN     107
#define L_ETIMEDOUT    110
#define L_ECONNREFUSED 111
#define L_EHOSTUNREACH 113
#define L_EALREADY     114
#define L_EINPROGRESS  115

#define L_O_ACCMODE  03
#define L_O_WRONLY   01
#define L_O_RDWR     02
#define L_O_CREAT    0100
#define L_O_EXCL     0200
#define L_O_TRUNC    01000
#define L_O_APPEND   02000

#define L_AF_INET  2
#define L_AF_INET6 10
#define L_POLLIN   0x1
#define L_POLLOUT  0x4
#define L_POLLERR  0x8
#define L_POLLHUP  0x10
#define L_POLLNVAL 0x20

/* ---- handle table ---- */

enum { T_FREE = 0, T_FILE, T_SOCK };

#define MAX_IDS 4096
static struct {
    int type;
    HANDLE file;
    SOCKET sock;
} g_ids[MAX_IDS];
static SRWLOCK g_ids_lock = SRWLOCK_INIT;
static wchar_t g_root[MAX_PATH];

void pal_vbs_host_io_init(const char* root) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (root && root[0]) {
        MultiByteToWideChar(CP_UTF8, 0, root, -1, g_root, MAX_PATH);
    } else {
        GetCurrentDirectoryW(MAX_PATH, g_root);
    }
    size_t n = wcslen(g_root);
    if (n && (g_root[n - 1] == L'\\' || g_root[n - 1] == L'/'))
        g_root[n - 1] = 0;
}

static int64_t new_id(int type, HANDLE f, SOCKET s) {
    AcquireSRWLockExclusive(&g_ids_lock);
    for (int i = 3; i < MAX_IDS; i++) {
        if (g_ids[i].type == T_FREE) {
            g_ids[i].type = type;
            g_ids[i].file = f;
            g_ids[i].sock = s;
            ReleaseSRWLockExclusive(&g_ids_lock);
            return i;
        }
    }
    ReleaseSRWLockExclusive(&g_ids_lock);
    return -L_EMFILE;
}

static int get_id(int64_t id, int type, HANDLE* f, SOCKET* s) {
    if (id < 0 || id >= MAX_IDS)
        return -L_EBADF;
    AcquireSRWLockShared(&g_ids_lock);
    int t = g_ids[id].type;
    if (f)
        *f = g_ids[id].file;
    if (s)
        *s = g_ids[id].sock;
    ReleaseSRWLockShared(&g_ids_lock);
    if (t == T_FREE)
        return -L_EBADF;
    if (type && t != type)
        return type == T_SOCK ? -L_ENOTSOCK : -L_EBADF;
    return t;
}

/* ---- errors ---- */

static int64_t win_err(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:  return -L_ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: return -L_EACCES;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return -L_EEXIST;
        case ERROR_DIR_NOT_EMPTY:  return -L_ENOTEMPTY;
        case ERROR_DIRECTORY:      return -L_ENOTDIR;
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER: return -L_EINVAL;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return -L_ENOSPC;
        case ERROR_FILENAME_EXCED_RANGE: return -L_ENAMETOOLONG;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:    return -L_ENOMEM;
        default:                   return -L_EIO;
    }
}

static int64_t wsa_err(int e) {
    switch (e) {
        case WSAEWOULDBLOCK:    return -L_EAGAIN;
        case WSAEINPROGRESS:    return -L_EINPROGRESS;
        case WSAEALREADY:       return -L_EALREADY;
        case WSAEADDRINUSE:     return -L_EADDRINUSE;
        case WSAEADDRNOTAVAIL:  return -L_EADDRNOTAVAIL;
        case WSAECONNREFUSED:   return -L_ECONNREFUSED;
        case WSAECONNRESET:     return -L_ECONNRESET;
        case WSAECONNABORTED:   return -L_ECONNABORTED;
        case WSAENOTCONN:       return -L_ENOTCONN;
        case WSAEISCONN:        return -L_EISCONN;
        case WSAETIMEDOUT:      return -L_ETIMEDOUT;
        case WSAESHUTDOWN:      return -L_EPIPE;
        case WSAENETUNREACH:    return -L_ENETUNREACH;
        case WSAEHOSTUNREACH:   return -L_EHOSTUNREACH;
        case WSAEMSGSIZE:       return -L_EMSGSIZE;
        case WSAEAFNOSUPPORT:   return -L_EAFNOSUPPORT;
        case WSAEACCES:         return -L_EACCES;
        case WSAEINVAL:         return -L_EINVAL;
        case WSAENOTSOCK:       return -L_ENOTSOCK;
        case WSAEOPNOTSUPP:     return -L_EOPNOTSUPP;
        case WSAEMFILE:         return -L_EMFILE;
        case WSAENOBUFS:        return -L_ENOMEM;
        default:                return -L_EIO;
    }
}

/* ---- paths ---- */

static int64_t map_path(const char* path, uint64_t len, wchar_t* out, size_t cap) {
    char p[1024];
    if (len == 0 || len >= sizeof(p) || path[0] != '/')
        return -L_EINVAL;
    memcpy(p, path, len);
    p[len] = 0;
    if (strlen(p) != len)
        return -L_EINVAL;
    /* refuse ".." components */
    for (const char* c = p; (c = strstr(c, "..")) != NULL; c += 2)
        if ((c == p || c[-1] == '/') && (c[2] == 0 || c[2] == '/'))
            return -L_EACCES;
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1024))
        return -L_EINVAL;
    for (wchar_t* c = w; *c; c++)
        if (*c == L'/')
            *c = L'\\';
    if (wcslen(g_root) + wcslen(w) + 1 > cap)
        return -L_ENAMETOOLONG;
    wcscpy(out, g_root);
    wcscat(out, w);
    size_t n = wcslen(out);
    if (n > wcslen(g_root) && out[n - 1] == L'\\')
        out[n - 1] = 0;
    return 0;
}

/* ---- sockaddr ---- */

static int lin_to_win_sockaddr(const uint8_t* in, uint64_t len, struct sockaddr_storage* out,
                               int* out_len) {
    if (len < 2 || len > sizeof(*out))
        return -L_EINVAL;
    memset(out, 0, sizeof(*out));
    memcpy(out, in, len);
    uint16_t fam = (uint16_t)(in[0] | (in[1] << 8));
    if (fam == L_AF_INET && len >= sizeof(struct sockaddr_in)) {
        out->ss_family = AF_INET;
        *out_len = sizeof(struct sockaddr_in);
    } else if (fam == L_AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        out->ss_family = AF_INET6;
        *out_len = sizeof(struct sockaddr_in6);
    } else {
        return -L_EAFNOSUPPORT;
    }
    return 0;
}

static uint64_t win_to_lin_sockaddr(const struct sockaddr_storage* in, uint8_t* out, size_t cap) {
    size_t n = in->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    if (n > cap)
        return 0;
    memcpy(out, in, n);
    uint16_t fam = in->ss_family == AF_INET6 ? L_AF_INET6 : L_AF_INET;
    out[0] = fam & 0xff;
    out[1] = fam >> 8;
    return n;
}

/* ---- files ---- */

static int64_t do_open(const char* path, uint64_t len, int64_t flags) {
    wchar_t w[MAX_PATH * 2];
    int64_t r = map_path(path, len, w, MAX_PATH * 2);
    if (r < 0)
        return r;
    DWORD access = GENERIC_READ;
    if ((flags & L_O_ACCMODE) == L_O_WRONLY)
        access = GENERIC_WRITE;
    else if ((flags & L_O_ACCMODE) == L_O_RDWR)
        access = GENERIC_READ | GENERIC_WRITE;
    DWORD disp = OPEN_EXISTING;
    if ((flags & L_O_CREAT) && (flags & L_O_EXCL))
        disp = CREATE_NEW;
    else if ((flags & L_O_CREAT) && (flags & L_O_TRUNC))
        disp = CREATE_ALWAYS;
    else if (flags & L_O_CREAT)
        disp = OPEN_ALWAYS;
    else if (flags & L_O_TRUNC)
        disp = TRUNCATE_EXISTING;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           disp, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return win_err(GetLastError());
    int64_t id = new_id(T_FILE, h, INVALID_SOCKET);
    if (id < 0)
        CloseHandle(h);
    return id;
}

static int64_t fill_stat(HANDLE h, const wchar_t* w, struct pal_vbs_host_stat* st) {
    memset(st, 0, sizeof(*st));
    if (h) {
        BY_HANDLE_FILE_INFORMATION fi;
        if (!GetFileInformationByHandle(h, &fi))
            return win_err(GetLastError());
        st->size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
        st->is_dir = !!(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        st->mode = st->is_dir ? 0755 : ((fi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    } else {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a))
            return win_err(GetLastError());
        st->size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        st->is_dir = !!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        st->mode = st->is_dir ? 0755 : ((a.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    }
    return 0;
}

static int64_t do_listdir(const char* path, uint64_t len, int64_t first, uint8_t* out,
                          uint64_t cap, uint64_t* out_len) {
    wchar_t w[MAX_PATH * 2];
    int64_t r = map_path(path, len, w, MAX_PATH * 2 - 3);
    if (r < 0)
        return r;
    wcscat(w, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(w, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return win_err(GetLastError());
    int64_t idx = 0, count = 0;
    uint64_t used = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (idx++ < first)
            continue;
        char name[MAX_PATH * 3];
        int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name) - 2, NULL, NULL);
        if (n <= 0)
            continue;
        n--; /* without the terminator */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            name[n++] = '/';
        if (used + n + 1 > cap)
            break;
        memcpy(out + used, name, n);
        out[used + n] = 0;
        used += n + 1;
        count++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    *out_len = used;
    return count;
}

/* ---- sockets ---- */

static int64_t set_nonblock(SOCKET s, int on) {
    u_long v = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &v) == 0 ? 0 : wsa_err(WSAGetLastError());
}

static int64_t do_setsockopt(SOCKET s, int64_t level, int64_t name, int64_t value) {
    int wlevel, wname;
    if (level == 1 /* SOL_SOCKET */) {
        wlevel = SOL_SOCKET;
        switch (name) {
            case 2:  return 0; /* SO_REUSEADDR: Windows' SO_REUSEADDR has unsafe semantics; Linux's
                                * is roughly Windows' default */
            case 7:  wname = SO_SNDBUF; break;
            case 8:  wname = SO_RCVBUF; break;
            case 9:  wname = SO_KEEPALIVE; break;
            case 6:  wname = SO_BROADCAST; break;
            default: return 0; /* not supported: ignored */
        }
    } else if (level == 6 /* IPPROTO_TCP */) {
        wlevel = IPPROTO_TCP;
        if (name == 1)
            wname = TCP_NODELAY;
        else
            return 0;
    } else if (level == 41 /* IPPROTO_IPV6 */ && name == 26 /* IPV6_V6ONLY */) {
        wlevel = IPPROTO_IPV6;
        wname = IPV6_V6ONLY;
    } else {
        return 0;
    }
    int v = (int)value;
    return setsockopt(s, wlevel, wname, (const char*)&v, sizeof(v)) == 0
           ? 0 : wsa_err(WSAGetLastError());
}

static int64_t do_poll(struct pal_vbs_host_pollfd* pf, int64_t n, int64_t timeout_ms) {
    WSAPOLLFD w[256];
    if (n < 0 || n > 256)
        return -L_EINVAL;
    int nsock = 0, ready = 0;
    int map[256];
    for (int64_t i = 0; i < n; i++) {
        pf[i].revents = 0;
        SOCKET s;
        int t = get_id(pf[i].id, 0, NULL, &s);
        if (t == T_FILE) {
            pf[i].revents = pf[i].events & (L_POLLIN | L_POLLOUT);
            ready += !!pf[i].revents;
        } else if (t == T_SOCK) {
            w[nsock].fd = s;
            w[nsock].events = (SHORT)(((pf[i].events & L_POLLIN) ? POLLRDNORM : 0) |
                                      ((pf[i].events & L_POLLOUT) ? POLLWRNORM : 0));
            w[nsock].revents = 0;
            map[nsock++] = (int)i;
        } else {
            pf[i].revents = L_POLLNVAL;
            ready++;
        }
    }
    if (nsock) {
        int r = WSAPoll(w, nsock, ready ? 0 : (int)timeout_ms);
        if (r < 0)
            return wsa_err(WSAGetLastError());
        for (int k = 0; k < nsock; k++) {
            uint32_t re = 0;
            if (w[k].revents & (POLLRDNORM | POLLRDBAND))
                re |= L_POLLIN;
            if (w[k].revents & POLLWRNORM)
                re |= L_POLLOUT;
            if (w[k].revents & POLLERR)
                re |= L_POLLERR;
            if (w[k].revents & POLLHUP)
                re |= L_POLLHUP | L_POLLIN;
            if (w[k].revents & POLLNVAL)
                re |= L_POLLNVAL;
            pf[map[k]].revents = re;
            ready += !!re;
        }
    } else if (!ready && timeout_ms > 0) {
        Sleep((DWORD)timeout_ms);
    }
    return ready;
}

/* ---- dispatcher ---- */

void pal_vbs_host_request(uint8_t* buf) {
    struct pal_vbs_hostreq* r = (struct pal_vbs_hostreq*)buf;
    uint8_t* data = buf + sizeof(*r);
    uint64_t in_len = r->in_len > PAL_VBS_HOST_DATA_MAX ? PAL_VBS_HOST_DATA_MAX : r->in_len;
    int64_t* a = r->a;
    int64_t res = -L_EINVAL;
    uint64_t out_len = 0;
    HANDLE f;
    SOCKET s;

    switch (r->op) {
        case PAL_VBS_HOST_OPEN:
            res = do_open((const char*)data, in_len, a[0]);
            break;
        case PAL_VBS_HOST_PREAD: {
            if ((res = get_id(a[0], T_FILE, &f, NULL)) < 0)
                break;
            DWORD want = (DWORD)(a[2] < 0 ? 0 : (a[2] > (int64_t)PAL_VBS_HOST_DATA_MAX
                                                  ? PAL_VBS_HOST_DATA_MAX : a[2]));
            OVERLAPPED ov = {0};
            ov.Offset = (DWORD)a[1];
            ov.OffsetHigh = (DWORD)((uint64_t)a[1] >> 32);
            DWORD got = 0;
            if (!ReadFile(f, data, want, &got, &ov) && GetLastError() != ERROR_HANDLE_EOF) {
                res = win_err(GetLastError());
                break;
            }
            res = got;
            out_len = got;
            break;
        }
        case PAL_VBS_HOST_PWRITE: {
            if ((res = get_id(a[0], T_FILE, &f, NULL)) < 0)
                break;
            OVERLAPPED ov = {0};
            ov.Offset = (DWORD)a[1];
            ov.OffsetHigh = (DWORD)((uint64_t)a[1] >> 32);
            DWORD put = 0;
            if (!WriteFile(f, data, (DWORD)in_len, &put, &ov)) {
                res = win_err(GetLastError());
                break;
            }
            res = put;
            break;
        }
        case PAL_VBS_HOST_FSTAT:
            if ((res = get_id(a[0], T_FILE, &f, NULL)) < 0)
                break;
            res = fill_stat(f, NULL, (struct pal_vbs_host_stat*)data);
            out_len = res == 0 ? sizeof(struct pal_vbs_host_stat) : 0;
            break;
        case PAL_VBS_HOST_STAT: {
            wchar_t w[MAX_PATH * 2];
            if ((res = map_path((const char*)data, in_len, w, MAX_PATH * 2)) < 0)
                break;
            res = fill_stat(NULL, w, (struct pal_vbs_host_stat*)data);
            out_len = res == 0 ? sizeof(struct pal_vbs_host_stat) : 0;
            break;
        }
        case PAL_VBS_HOST_CLOSE: {
            int t = get_id(a[0], 0, &f, &s);
            if (t < 0) {
                res = t;
                break;
            }
            AcquireSRWLockExclusive(&g_ids_lock);
            g_ids[a[0]].type = T_FREE;
            ReleaseSRWLockExclusive(&g_ids_lock);
            if (t == T_FILE)
                CloseHandle(f);
            else
                closesocket(s);
            res = 0;
            break;
        }
        case PAL_VBS_HOST_FTRUNCATE: {
            if ((res = get_id(a[0], T_FILE, &f, NULL)) < 0)
                break;
            FILE_END_OF_FILE_INFO eof;
            eof.EndOfFile.QuadPart = a[1];
            res = SetFileInformationByHandle(f, FileEndOfFileInfo, &eof, sizeof(eof))
                  ? 0 : win_err(GetLastError());
            break;
        }
        case PAL_VBS_HOST_UNLINK: {
            wchar_t w[MAX_PATH * 2];
            if ((res = map_path((const char*)data, in_len, w, MAX_PATH * 2)) < 0)
                break;
            BOOL ok = a[0] ? RemoveDirectoryW(w) : DeleteFileW(w);
            res = ok ? 0 : win_err(GetLastError());
            break;
        }
        case PAL_VBS_HOST_RENAME: {
            size_t l1 = strnlen((const char*)data, in_len);
            if (l1 >= in_len) {
                res = -L_EINVAL;
                break;
            }
            wchar_t w1[MAX_PATH * 2], w2[MAX_PATH * 2];
            if ((res = map_path((const char*)data, l1, w1, MAX_PATH * 2)) < 0 ||
                (res = map_path((const char*)data + l1 + 1, in_len - l1 - 1, w2, MAX_PATH * 2)) < 0)
                break;
            res = MoveFileExW(w1, w2, MOVEFILE_REPLACE_EXISTING) ? 0 : win_err(GetLastError());
            break;
        }
        case PAL_VBS_HOST_MKDIR: {
            wchar_t w[MAX_PATH * 2];
            if ((res = map_path((const char*)data, in_len, w, MAX_PATH * 2)) < 0)
                break;
            res = CreateDirectoryW(w, NULL) ? 0 : win_err(GetLastError());
            break;
        }
        case PAL_VBS_HOST_LISTDIR: {
            char p[1024];
            uint64_t l = in_len < sizeof(p) ? in_len : sizeof(p) - 1;
            memcpy(p, data, l);
            res = do_listdir(p, l, a[0], data, PAL_VBS_HOST_DATA_MAX, &out_len);
            break;
        }
        case PAL_VBS_HOST_SOCKET: {
            int af = a[0] == L_AF_INET ? AF_INET : a[0] == L_AF_INET6 ? AF_INET6 : -1;
            int type = a[1] == 1 ? SOCK_STREAM : a[1] == 2 ? SOCK_DGRAM : -1;
            if (af < 0 || type < 0) {
                res = -L_EAFNOSUPPORT;
                break;
            }
            SOCKET ns = socket(af, type, 0);
            if (ns == INVALID_SOCKET) {
                res = wsa_err(WSAGetLastError());
                break;
            }
            if (a[2])
                set_nonblock(ns, 1);
            res = new_id(T_SOCK, NULL, ns);
            if (res < 0)
                closesocket(ns);
            break;
        }
        case PAL_VBS_HOST_BIND: {
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            struct sockaddr_storage ss;
            int sl;
            if ((res = lin_to_win_sockaddr(data, in_len, &ss, &sl)) < 0)
                break;
            if (bind(s, (struct sockaddr*)&ss, sl) != 0) {
                res = wsa_err(WSAGetLastError());
                break;
            }
            sl = sizeof(ss);
            if (getsockname(s, (struct sockaddr*)&ss, &sl) == 0)
                out_len = win_to_lin_sockaddr(&ss, data, PAL_VBS_HOST_SOCKADDR_MAX);
            res = 0;
            break;
        }
        case PAL_VBS_HOST_LISTEN:
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            res = listen(s, (int)a[1]) == 0 ? 0 : wsa_err(WSAGetLastError());
            break;
        case PAL_VBS_HOST_ACCEPT: {
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            struct sockaddr_storage ss;
            int sl = sizeof(ss);
            SOCKET c = accept(s, (struct sockaddr*)&ss, &sl);
            if (c == INVALID_SOCKET) {
                res = wsa_err(WSAGetLastError());
                break;
            }
            set_nonblock(c, a[1] ? 1 : 0);
            out_len = win_to_lin_sockaddr(&ss, data, PAL_VBS_HOST_SOCKADDR_MAX);
            res = new_id(T_SOCK, NULL, c);
            if (res < 0)
                closesocket(c);
            break;
        }
        case PAL_VBS_HOST_CONNECT: {
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            struct sockaddr_storage ss;
            int sl;
            if ((res = lin_to_win_sockaddr(data, in_len, &ss, &sl)) < 0)
                break;
            res = 0;
            if (connect(s, (struct sockaddr*)&ss, sl) != 0) {
                res = wsa_err(WSAGetLastError());
                if (res == -L_EAGAIN)
                    res = -L_EINPROGRESS;
                if (res != -L_EINPROGRESS)
                    break;
            }
            sl = sizeof(ss);
            if (getsockname(s, (struct sockaddr*)&ss, &sl) == 0)
                out_len = win_to_lin_sockaddr(&ss, data, PAL_VBS_HOST_SOCKADDR_MAX);
            break;
        }
        case PAL_VBS_HOST_SEND: {
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            if (in_len < 4) {
                res = -L_EINVAL;
                break;
            }
            uint32_t al;
            memcpy(&al, data, 4);
            if (al > PAL_VBS_HOST_SOCKADDR_MAX || 4 + al > in_len) {
                res = -L_EINVAL;
                break;
            }
            const char* bytes = (const char*)data + 4 + al;
            int nbytes = (int)(in_len - 4 - al);
            if (a[1])
                set_nonblock(s, 1);
            int n;
            if (al) {
                struct sockaddr_storage ss;
                int sl;
                if ((res = lin_to_win_sockaddr(data + 4, al, &ss, &sl)) < 0)
                    break;
                n = sendto(s, bytes, nbytes, 0, (struct sockaddr*)&ss, sl);
            } else {
                n = send(s, bytes, nbytes, 0);
            }
            int e = WSAGetLastError();
            if (a[1] && !a[2])
                set_nonblock(s, 0);
            res = n >= 0 ? n : wsa_err(e);
            break;
        }
        case PAL_VBS_HOST_RECV: {
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            int want = (int)(a[1] < 0 ? 0 : (a[1] > (int64_t)(PAL_VBS_HOST_DATA_MAX
                                                             - PAL_VBS_HOST_SOCKADDR_MAX)
                                             ? PAL_VBS_HOST_DATA_MAX - PAL_VBS_HOST_SOCKADDR_MAX
                                             : a[1]));
            if (a[2])
                set_nonblock(s, 1);
            struct sockaddr_storage ss;
            int sl = sizeof(ss);
            memset(&ss, 0, sizeof(ss));
            int n = recvfrom(s, (char*)data + PAL_VBS_HOST_SOCKADDR_MAX, want, 0,
                             (struct sockaddr*)&ss, &sl);
            int e = WSAGetLastError();
            if (a[2] && !a[3])
                set_nonblock(s, 0);
            if (n < 0) {
                res = wsa_err(e);
                break;
            }
            memset(data, 0, PAL_VBS_HOST_SOCKADDR_MAX);
            if (ss.ss_family)
                win_to_lin_sockaddr(&ss, data, PAL_VBS_HOST_SOCKADDR_MAX);
            res = n;
            out_len = PAL_VBS_HOST_SOCKADDR_MAX + n;
            break;
        }
        case PAL_VBS_HOST_SHUTDOWN:
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            res = shutdown(s, (int)a[1]) == 0 ? 0 : wsa_err(WSAGetLastError());
            break;
        case PAL_VBS_HOST_SETNONBLOCK:
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            res = set_nonblock(s, (int)a[1]);
            break;
        case PAL_VBS_HOST_SETSOCKOPT:
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            res = do_setsockopt(s, a[1], a[2], a[3]);
            break;
        case PAL_VBS_HOST_GETSOCKNAME: {
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            struct sockaddr_storage ss;
            int sl = sizeof(ss);
            if (getsockname(s, (struct sockaddr*)&ss, &sl) != 0) {
                res = wsa_err(WSAGetLastError());
                break;
            }
            out_len = win_to_lin_sockaddr(&ss, data, PAL_VBS_HOST_SOCKADDR_MAX);
            res = 0;
            break;
        }
        case PAL_VBS_HOST_FIONREAD: {
            if ((res = get_id(a[0], T_SOCK, NULL, &s)) < 0)
                break;
            u_long v = 0;
            res = ioctlsocket(s, FIONREAD, &v) == 0 ? (int64_t)v : wsa_err(WSAGetLastError());
            break;
        }
        case PAL_VBS_HOST_POLL: {
            int64_t n = a[0];
            if (n < 0 || (uint64_t)n * sizeof(struct pal_vbs_host_pollfd) > in_len) {
                res = -L_EINVAL;
                break;
            }
            res = do_poll((struct pal_vbs_host_pollfd*)data, n, a[1]);
            out_len = (uint64_t)n * sizeof(struct pal_vbs_host_pollfd);
            break;
        }
        default:
            res = -L_EINVAL;
            break;
    }
    r->result = res;
    r->out_len = out_len;
}
