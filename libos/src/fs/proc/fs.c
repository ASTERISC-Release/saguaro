/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*!
 * \file
 *
 * This file contains the implementation of `/proc` pseudo-filesystem.
 */

#include "api.h"
#include "libos_fs_pseudo.h"
#include "libos_inotify.h"
#include "libos_process.h"

static int proc_tainted_load(struct libos_dentry* dent, char** out_data, size_t* out_size) {
    __UNUSED(dent);

    /* the kernel's taint flags: nothing can taint this LibOS, so the mask is always empty (the
     * LTP timerfd clock-stealing test reads it and requires it to exist) */
    char* buffer = strdup("0\n");
    if (!buffer)
        return -ENOMEM;

    *out_data = buffer;
    *out_size = strlen(buffer);
    return 0;
}

/* `/proc/sys/fs/inotify/{max_queued_events,max_user_instances,max_user_watches}`: the limits the
 * inotify implementation applies (see `sys/libos_inotify.c`); LTP `inotify05` reads
 * `max_queued_events` to size its overflow test */
static int proc_uint_load(unsigned int value, char** out_data, size_t* out_size) {
    char* buffer = malloc(16);
    if (!buffer)
        return -ENOMEM;

    int ret = snprintf(buffer, 16, "%u\n", value);
    if (ret <= 0 || ret >= 16) {
        free(buffer);
        return -EINVAL;
    }
    *out_data = buffer;
    *out_size = strlen(buffer);
    return 0;
}

static int proc_inotify_max_queued_events_load(struct libos_dentry* dent, char** out_data,
                                               size_t* out_size) {
    __UNUSED(dent);
    return proc_uint_load(libos_inotify_max_queued_events(), out_data, out_size);
}

static int proc_inotify_max_user_instances_load(struct libos_dentry* dent, char** out_data,
                                                size_t* out_size) {
    __UNUSED(dent);
    return proc_uint_load(INOTIFY_MAX_INSTANCES, out_data, out_size);
}

static int proc_inotify_max_user_watches_load(struct libos_dentry* dent, char** out_data,
                                              size_t* out_size) {
    __UNUSED(dent);
    return proc_uint_load(libos_inotify_max_user_watches(), out_data, out_size);
}

static int proc_pid_max_load(struct libos_dentry* dent, char** out_data, size_t* out_size) {
    __UNUSED(dent);

    size_t buffer_size = 16; /* enough to hold PID_MAX_LIMIT */
    char* buffer = malloc(buffer_size);
    if (!buffer)
        return -ENOMEM;

    static_assert(PID_MAX_LIMIT <= UINT_MAX, "wrong types");
    int ret = snprintf(buffer, buffer_size, "%u", PID_MAX_LIMIT);
    if (ret < 0) {
        free(buffer);
        return ret;
    }
    assert((size_t)ret < buffer_size);

    *out_data = buffer;
    *out_size = buffer_size;
    return 0;
}

int proc_self_follow_link(struct libos_dentry* dent, char** out_target) {
    __UNUSED(dent);
    IDTYPE pid = g_process.pid;
    char name[11];
    snprintf(name, sizeof(name), "%u", pid);
    if (!(*out_target = strdup(name)))
        return -ENOMEM;
    return 0;
}

/* `/proc/<pid>` and `/proc/<pid>/task/<tid>` for our local process and threads */
static void init_thread_dir(struct pseudo_node* ent) {
    pseudo_add_link(ent, "root", &proc_thread_follow_link);
    pseudo_add_link(ent, "cwd", &proc_thread_follow_link);
    pseudo_add_link(ent, "exe", &proc_thread_follow_link);
    pseudo_add_str(ent, "maps", &proc_thread_maps_load);
    pseudo_add_str(ent, "cmdline", &proc_thread_cmdline_load);
    pseudo_add_str(ent, "status", &proc_thread_status_load);
    pseudo_add_str(ent, "statm", &proc_thread_statm_load);
    pseudo_add_str(ent, "stat", &proc_thread_stat_load);

    struct pseudo_node* fd = pseudo_add_dir(ent, "fd");
    struct pseudo_node* fd_link = pseudo_add_link(fd, /*name=*/NULL, &proc_thread_fd_follow_link);
    fd_link->name_exists = &proc_thread_fd_name_exists;
    fd_link->list_names = &proc_thread_fd_list_names;
}

/* System V message queue limits and table (see `sys/libos_sysv_msg.c`): `/proc/sys/kernel/msgmni`
 * (LTP msgget03 creates that many queues), `msgmax`, `msgmnb`, and `/proc/sysvipc/msg` */

static int proc_msgmni_load(struct libos_dentry* dent, char** out_data, size_t* out_size) {
    __UNUSED(dent);
    return proc_uint_load(sysv_msg_limit(0), out_data, out_size);
}

static int proc_msgmax_load(struct libos_dentry* dent, char** out_data, size_t* out_size) {
    __UNUSED(dent);
    return proc_uint_load(sysv_msg_limit(1), out_data, out_size);
}

static int proc_msgmnb_load(struct libos_dentry* dent, char** out_data, size_t* out_size) {
    __UNUSED(dent);
    return proc_uint_load(sysv_msg_limit(2), out_data, out_size);
}

static int proc_sysvipc_msg_load(struct libos_dentry* dent, char** out_data, size_t* out_size) {
    __UNUSED(dent);
    return sysv_msg_proc_load(out_data, out_size);
}

/* `/proc/<pid>` for other processes, uses IPC */
static void init_ipc_thread_dir(struct pseudo_node* ent) {
    pseudo_add_link(ent, "root", &proc_ipc_thread_follow_link);
    pseudo_add_link(ent, "cwd", &proc_ipc_thread_follow_link);
    pseudo_add_link(ent, "exe", &proc_ipc_thread_follow_link);
}

int init_procfs(void) {
    struct pseudo_node* root = pseudo_add_root_dir("proc");

    struct pseudo_node* sys = pseudo_add_dir(root, "sys");
    struct pseudo_node* kernel = pseudo_add_dir(sys, "kernel");
    pseudo_add_str(kernel, "pid_max", &proc_pid_max_load);
    pseudo_add_str(kernel, "tainted", &proc_tainted_load);
    pseudo_add_str(kernel, "msgmni", &proc_msgmni_load);
    pseudo_add_str(kernel, "msgmax", &proc_msgmax_load);
    pseudo_add_str(kernel, "msgmnb", &proc_msgmnb_load);

    struct pseudo_node* fs_node = pseudo_add_dir(sys, "fs");
    struct pseudo_node* inotify_node = pseudo_add_dir(fs_node, "inotify");
    pseudo_add_str(inotify_node, "max_queued_events", &proc_inotify_max_queued_events_load);
    pseudo_add_str(inotify_node, "max_user_instances", &proc_inotify_max_user_instances_load);
    pseudo_add_str(inotify_node, "max_user_watches", &proc_inotify_max_user_watches_load);

    pseudo_add_str(root, "meminfo", &proc_meminfo_load);
    pseudo_add_str(root, "cpuinfo", &proc_cpuinfo_load);
    pseudo_add_str(root, "stat", &proc_stat_load);

    /* `/proc/sysvipc/{shm,msg}`: the System V shared memory and message queue tables */
    struct pseudo_node* sysvipc = pseudo_add_dir(root, "sysvipc");
    pseudo_add_str(sysvipc, "shm", &proc_sysvipc_shm_load);
    pseudo_add_str(sysvipc, "msg", &proc_sysvipc_msg_load);

    pseudo_add_link(root, "self", &proc_self_follow_link);

    struct pseudo_node* thread_pid = pseudo_add_dir(root, /*name=*/NULL);
    thread_pid->name_exists = &proc_thread_pid_name_exists;
    thread_pid->list_names = &proc_thread_pid_list_names;
    init_thread_dir(thread_pid);

    struct pseudo_node* thread_task = pseudo_add_dir(thread_pid, "task");
    struct pseudo_node* thread_tid = pseudo_add_dir(thread_task, /*name=*/NULL);
    thread_tid->name_exists = &proc_thread_tid_name_exists;
    thread_tid->list_names = &proc_thread_tid_list_names;
    init_thread_dir(thread_tid);

    struct pseudo_node* ipc_thread_pid = pseudo_add_dir(root, /*name=*/NULL);
    ipc_thread_pid->name_exists = &proc_ipc_thread_pid_name_exists;
    /* Listing remote processes is not currently supported. */
    ipc_thread_pid->list_names = NULL;
    init_ipc_thread_dir(ipc_thread_pid);

    return 0;
}
