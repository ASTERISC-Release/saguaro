/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Host passthrough calls (see pal.h). Only the Linux PAL implements them; the weak defaults below
 * serve every other PAL, so that they need no change.
 */

#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"

__attribute__((weak)) int _PalHostReadlink(const char* uri, char* buf, size_t* size) {
    __UNUSED(uri);
    __UNUSED(buf);
    __UNUSED(size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostReadlink(const char* typed_uri, char* buf, size_t* size) {
    return _PalHostReadlink(typed_uri, buf, size);
}

__attribute__((weak)) int _PalHostFdWrap(int host_fd, PAL_HANDLE* out_handle) {
    __UNUSED(host_fd);
    __UNUSED(out_handle);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostFdWrap(int host_fd, PAL_HANDLE* out_handle) {
    return _PalHostFdWrap(host_fd, out_handle);
}

__attribute__((weak)) int _PalHostFdGet(PAL_HANDLE handle, int* out_host_fd) {
    __UNUSED(handle);
    __UNUSED(out_host_fd);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostFdGet(PAL_HANDLE handle, int* out_host_fd) {
    return _PalHostFdGet(handle, out_host_fd);
}

__attribute__((weak)) int _PalHostClockMonotonic(uint64_t* out_nsec) {
    __UNUSED(out_nsec);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostClockMonotonic(uint64_t* out_nsec) {
    return _PalHostClockMonotonic(out_nsec);
}

__attribute__((weak)) int _PalHostUnixConnect(const char* path, pal_stream_options_t options,
                                              PAL_HANDLE* out_handle) {
    __UNUSED(path);
    __UNUSED(options);
    __UNUSED(out_handle);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostUnixConnect(const char* path, pal_stream_options_t options, PAL_HANDLE* out_handle) {
    return _PalHostUnixConnect(path, options, out_handle);
}

__attribute__((weak)) int _PalHostSendmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len,
                                          const int* host_fds, size_t host_fds_cnt,
                                          bool nonblocking, size_t* out_size) {
    __UNUSED(handle);
    __UNUSED(iov);
    __UNUSED(iov_len);
    __UNUSED(host_fds);
    __UNUSED(host_fds_cnt);
    __UNUSED(nonblocking);
    __UNUSED(out_size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostSendmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, const int* host_fds,
                   size_t host_fds_cnt, bool nonblocking, size_t* out_size) {
    return _PalHostSendmsg(handle, iov, iov_len, host_fds, host_fds_cnt, nonblocking, out_size);
}

__attribute__((weak)) int _PalHostRecvmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len,
                                          int* host_fds, size_t* host_fds_cnt, bool nonblocking,
                                          size_t* out_size) {
    __UNUSED(handle);
    __UNUSED(iov);
    __UNUSED(iov_len);
    __UNUSED(host_fds);
    __UNUSED(host_fds_cnt);
    __UNUSED(nonblocking);
    __UNUSED(out_size);
    return -PAL_ERROR_NOTIMPLEMENTED;
}

int PalHostRecvmsg(PAL_HANDLE handle, struct iovec* iov, size_t iov_len, int* host_fds,
                   size_t* host_fds_cnt, bool nonblocking, size_t* out_size) {
    return _PalHostRecvmsg(handle, iov, iov_len, host_fds, host_fds_cnt, nonblocking, out_size);
}
