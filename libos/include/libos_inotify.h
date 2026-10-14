/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Shared declarations of the LibOS-internal inotify implementation (`sys/libos_inotify.c` and
 * `fs/inotify/fs.c`). The `IN_*` constants come from the kernel uapi values, spelled out here
 * because `linux/inotify.h` is not part of the LibOS include chain.
 */

#include "libos_types.h"
#include "libos_utils.h"

/* kernel uapi `linux/inotify.h` (verbatim values) */
#define IN_ACCESS         0x00000001
#define IN_MODIFY         0x00000002
#define IN_ATTRIB         0x00000004
#define IN_CLOSE_WRITE    0x00000008
#define IN_CLOSE_NOWRITE  0x00000010
#define IN_OPEN           0x00000020
#define IN_MOVED_FROM     0x00000040
#define IN_MOVED_TO       0x00000080
#define IN_CREATE         0x00000100
#define IN_DELETE         0x00000200
#define IN_DELETE_SELF    0x00000400
#define IN_MOVE_SELF      0x00000800
#define IN_ALL_EVENTS     0x00000fff
#define IN_UNMOUNT        0x00002000
#define IN_Q_OVERFLOW     0x00004000
#define IN_IGNORED        0x00008000
#define IN_ONLYDIR        0x01000000
#define IN_DONTFOLLOW     0x02000000
#define IN_EXCL_UNLINK    0x04000000
#define IN_MASK_CREATE    0x10000000
#define IN_MASK_ADD       0x20000000
#define IN_ISDIR          0x40000000
#define IN_ONESHOT        0x80000000
#define IN_CLOEXEC        02000000 /* == O_CLOEXEC */
#define IN_NONBLOCK       04000    /* == O_NONBLOCK */

/* the kernel's `max_user_instances` default */
#define INOTIFY_MAX_INSTANCES 128

/* header size of the on-the-wire `struct inotify_event` (no trailing name) */
#define INOTIFY_EVENT_HDRLEN 16

/* the kernel pads an event's name to a multiple of this (verified natively: 2- and 3-character
 * names both report `len = 16`) */
#define INOTIFY_NAME_ALIGN INOTIFY_EVENT_HDRLEN

/* One watched dentry in one inotify instance. The list is global so event generation can find
 * every match without an inode-level marks infrastructure; `dir_dentry` holds a reference. */
struct libos_inotify_watch {
    struct libos_inotify_watch* next;   /* global list (libos_inotify.c) */
    struct libos_dentry* dir_dentry;    /* the watched object itself (file or directory) */
    struct libos_handle* group;         /* the owning inotify instance (not refcounted: the watch
                                         * is dropped on the instance's close, before teardown) */
    unsigned int mask;                  /* the watched events (`IN_ALL_EVENTS` subset) */
    bool oneshot;
    int wd;
};

/*
 * Event generation, called from the filesystem/syscall paths. None of them takes
 * `g_dcache_lock`, but all are safe to call while holding it (the order is always dcache before
 * the inotify spinlocks), and the name is read from the dentry as it is at call time.
 *
 * `file_event` is the kernel's `fsnotify_file()`: the event goes to the parent directory's
 * watches (with the child's name) and to the object's own watch, with `IN_ISDIR` when the object
 * is a directory. `child_event*` are parent-only events (create/delete/move pairs; the `_raw`
 * variant for callers whose dentry name or inode is already gone). `delete_self_locked` is the
 * unlink/rmdir sequence on the victim (attrib/delete-self/ignored + the parent's delete);
 * `move_self` the rename self-event. Callers that hold no dentry lock must hold `g_dcache_lock`
 * themselves around the lookup + emission to keep dentry fields stable (all current callers do).
 */
void libos_inotify_file_event(struct libos_dentry* dent, unsigned int mask);
void libos_inotify_child_event(struct libos_dentry* dir, struct libos_dentry* child,
                               unsigned int mask, unsigned int cookie);
void libos_inotify_child_event_raw(struct libos_dentry* dir, const char* name, unsigned int mask,
                                   bool isdir, unsigned int cookie);
void libos_inotify_delete_self_locked(struct libos_dentry* dent, bool report_delete_parent);
void libos_inotify_move_self(struct libos_dentry* dent);
void libos_inotify_transfer_watches(struct libos_dentry* from, struct libos_dentry* to);

/* drop all watches and queued events of a closing instance */
void libos_inotify_destroy_watches(struct libos_handle* group);

/* the limits exposed through `/proc/sys/fs/inotify/` */
unsigned int libos_inotify_max_queued_events(void);
unsigned int libos_inotify_max_user_watches(void);
