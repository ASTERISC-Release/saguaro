/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "unlink", "unlinkat", "mkdir", "mkdirat", "rmdir", "umask",
 * "chmod", "fchmod", "fchmodat", "rename", "renameat" and "sendfile".
 */

#include "libos_fs.h"
#include "libos_handle.h"
#include "libos_inotify.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_table.h"
#include "linux_abi/errors.h"
#include "linux_abi/fs.h"
#include "perm.h"
#include "stat.h"

#define UTIME_NOW  ((1L << 30) - 1)
#define UTIME_OMIT ((1L << 30) - 2)


/*
 * Read/write in 64KB chunks in the sendfile() syscall. This syscall also has an optimization of
 * using a statically allocated buffer instead of allocating on the heap (as our internal malloc()
 * has subpar performance). To prevent data races of multiple threads executing sendfile() at the
 * same time and thus potentially corrupting a single static buffer, we optimize for a common case:
 * only the first thread uses the static buffer whereas other threads fall back to a slower heap
 * allocation.
 */
#define BUF_SIZE (64 * 1024)
static char g_sendfile_buf[BUF_SIZE];
static bool g_sendfile_buf_in_use = false;

/* The kernel would look up the parent directory, and remove the child from the inode. But we are
 * working with the PAL, so we open the file, truncate and close it. */
long libos_syscall_unlink(const char* file) {
    return libos_syscall_unlinkat(AT_FDCWD, file, 0);
}

static int rmdir_check_empty_callback(const char* name, void* arg) {
    bool* found = arg;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    *found = true;
    return 0;
}

/* `ENOTEMPTY` for a directory with entries, checked before the filesystem's unlink: the PAL delete
 * reports "directory not empty" only as a generic error. Shared by rmdir() and
 * unlinkat(AT_REMOVEDIR) (aarch64's glibc implements rmdir() with the latter, which once returned
 * EACCES here). Filesystems without a readdir hook are left to their own unlink. */
static int check_dir_empty(struct libos_dentry* dent) {
    struct libos_fs* fs = dent->inode->fs;
    if (!fs || !fs->d_ops || !fs->d_ops->readdir)
        return 0;
    bool not_empty = false;
    int ret = fs->d_ops->readdir(dent, &rmdir_check_empty_callback, &not_empty);
    if (ret < 0)
        return ret;
    return not_empty ? -ENOTEMPTY : 0;
}

static long do_unlinkat(int dfd, const char* pathname, int flag, bool user_pointer) {
    if (user_pointer && !is_user_string_readable(pathname))
        return -EFAULT;

    if (flag & ~AT_REMOVEDIR)
        return -EINVAL;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    int ret;

    if (*pathname != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    lock(&g_dcache_lock);
    ret = path_lookupat(dir, pathname, LOOKUP_NO_FOLLOW, &dent);
    if (ret < 0)
        goto out;

    if (!dent->parent) {
        ret = -EACCES;
        goto out;
    }

    if (flag & AT_REMOVEDIR) {
        if (dent->inode->type != S_IFDIR) {
            ret = -ENOTDIR;
            goto out;
        }
    } else {
        if (dent->inode->type == S_IFDIR) {
            ret = -EISDIR;
            goto out;
        }
    }

    struct libos_fs* fs = dent->inode->fs;
    if ((flag & AT_REMOVEDIR) && fs->d_ops && fs->d_ops->unlink) {
        ret = check_dir_empty(dent);
        if (ret < 0)
            goto out;
    }
    if (fs->d_ops && fs->d_ops->unlink) {
        ret = fs->d_ops->unlink(dent);
        if (ret < 0) {
            goto out;
        }
    } else if (flag & AT_REMOVEDIR) {
        /* Same rule as libos_syscall_rmdir(): a dentry whose fs cannot unlink -- e.g. the
         * synthetic directory Gramine creates in the process of mounting, which the `synthetic`
         * regression test checks -- is not removable. This matters specifically on aarch64,
         * where glibc's rmdir() *is* unlinkat(AT_REMOVEDIR) (the architecture has no rmdir
         * syscall): without this branch the two ABIs disagree, x86's rmdir() returning EACCES
         * while aarch64 silently deletes the mount point. */
        ret = -EACCES;
        goto out;
    }

    lock(&dent->inode->lock);
    if (dent->inode->nlink > 0)
        dent->inode->nlink--;
    unlock(&dent->inode->lock);

    /* inotify: the full removal sequence (attrib/delete-self/ignored on the victim's watches,
     * then `IN_DELETE` on the parent's watches); the victim's inode is still attached */
    libos_inotify_delete_self_locked(dent, /*report_delete_parent=*/true);

    put_inode(dent->inode);
    dent->inode = NULL;
    ret = 0;
out:
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    if (dent)
        put_dentry(dent);
    return ret;
}

long libos_syscall_unlinkat(int dfd, const char* pathname, int flag) {
    return do_unlinkat(dfd, pathname, flag, true);
}

long libos_unlink_internal(const char* pathname) {
    return do_unlinkat(AT_FDCWD, pathname, 0, false);
}

long libos_syscall_mkdir(const char* pathname, int mode) {
    return libos_syscall_mkdirat(AT_FDCWD, pathname, mode);
}

long libos_syscall_mkdirat(int dfd, const char* pathname, int mode) {
    if (!is_user_string_readable(pathname))
        return -EFAULT;

    lock(&g_process.fs_lock);
    mode_t umask = g_process.umask;
    unlock(&g_process.fs_lock);

    /* In addition to permission bits, Linux `mkdirat` honors the sticky bit (see man page) */
    mode &= (PERM_rwxrwxrwx | S_ISVTX);

    mode &= ~umask;

    struct libos_dentry* dir = NULL;
    int ret = 0;

    if (*pathname != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    /* Linux mkdir reports EEXIST for a final symlink, even when it is dangling. */
    struct libos_dentry* existing = NULL;
    lock(&g_dcache_lock);
    ret = path_lookupat(dir, pathname, LOOKUP_NO_FOLLOW | LOOKUP_CREATE, &existing);
    if (ret == 0 && existing->inode && existing->inode->type == S_IFLNK)
        ret = -EEXIST;
    unlock(&g_dcache_lock);
    if (existing)
        put_dentry(existing);
    if (ret < 0)
        goto out;

    ret = open_namei(NULL, dir, pathname, O_CREAT | O_EXCL | O_DIRECTORY, mode, NULL);

out:
    if (dir)
        put_dentry(dir);
    return ret;
}

long libos_syscall_rmdir(const char* pathname) {
    int ret = 0;
    struct libos_dentry* dent = NULL;

    if (!is_user_string_readable(pathname))
        return -EFAULT;

    lock(&g_dcache_lock);
    ret = path_lookupat(/*start=*/NULL, pathname, LOOKUP_NO_FOLLOW | LOOKUP_DIRECTORY, &dent);
    if (ret < 0) {
        goto out;
    }

    if (!dent->parent) {
        ret = -EACCES;
        goto out;
    }

    if (dent->inode->type != S_IFDIR) {
        ret = -ENOTDIR;
        goto out;
    }

    struct libos_fs* fs = dent->inode->fs;
    if (!fs || !fs->d_ops || !fs->d_ops->unlink) {
        ret = -EACCES;
        goto out;
    }

    /* Check emptiness here: the PAL delete reports "directory not empty" only as a generic
     * error, and `ENOTEMPTY` is the answer Linux gives. Filesystems without a readdir hook are
     * left to their own unlink implementation. */
    ret = check_dir_empty(dent);
    if (ret < 0)
        goto out;

    ret = fs->d_ops->unlink(dent);
    if (ret < 0)
        goto out;

    /* inotify: the removal sequence, as in `do_unlinkat` (no link-count `IN_ATTRIB` for
     * directories — the kernel does not report one for `rmdir`) */
    libos_inotify_delete_self_locked(dent, /*report_delete_parent=*/true);

    put_inode(dent->inode);
    dent->inode = NULL;
    ret = 0;
out:
    unlock(&g_dcache_lock);
    if (dent)
        put_dentry(dent);
    return ret;
}

long libos_syscall_umask(mode_t mask) {
    lock(&g_process.fs_lock);
    mode_t old = g_process.umask;
    g_process.umask = mask & 0777;
    unlock(&g_process.fs_lock);
    return old;
}

long libos_syscall_chmod(const char* path, mode_t mode) {
    return libos_syscall_fchmodat(AT_FDCWD, path, mode);
}

long libos_syscall_fchmodat(int dfd, const char* filename, mode_t mode) {
    if (!is_user_string_readable(filename))
        return -EFAULT;

    /* This isn't documented, but that's what Linux does. */
    mode_t perm = mode & 07777;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    int ret = 0;

    if (*filename != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    lock(&g_dcache_lock);
    ret = path_lookupat(dir, filename, LOOKUP_FOLLOW, &dent);
    if (ret < 0)
        goto out;

    struct libos_fs* fs = dent->inode->fs;
    if (fs && fs->d_ops && fs->d_ops->chmod) {
        if ((ret = fs->d_ops->chmod(dent, perm)) < 0)
            goto out_dent;
    }

    lock(&dent->inode->lock);
    dent->inode->perm = perm;
    unlock(&dent->inode->lock);

    /* inotify: the kernel reports a permission change on any watched type (a directory gets
     * `IN_ATTRIB | IN_ISDIR` on both the parent's and its own watch) */
    libos_inotify_file_event(dent, IN_ATTRIB);

out_dent:
    put_dentry(dent);
out:
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    return ret;
}

long libos_syscall_fchmod(int fd, mode_t mode) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    /* This isn't documented, but that's what Linux does. */
    mode_t perm = mode & 07777;

    int ret;
    if (!hdl->inode) {
        ret = -ENOENT;
        goto out;
    }

    struct libos_fs* fs = hdl->inode->fs;
    if (fs && fs->fs_ops && fs->fs_ops->fchmod) {
        ret = fs->fs_ops->fchmod(hdl, perm);
        if (ret < 0)
            goto out;
    }

    lock(&hdl->inode->lock);
    hdl->inode->perm = perm;
    unlock(&hdl->inode->lock);

    if (hdl->dentry && hdl->dentry->inode)
        libos_inotify_file_event(hdl->dentry, IN_ATTRIB); /* as for `chmod` */

    ret = 0;
out:
    put_handle(hdl);
    return ret;
}

long libos_syscall_chown(const char* path, uid_t uid, gid_t gid) {
    return libos_syscall_fchownat(AT_FDCWD, path, uid, gid, 0);
}

long libos_syscall_fchownat(int dfd, const char* filename, uid_t uid, gid_t gid, int flags) {
    __UNUSED(flags);

    if (!is_user_string_readable(filename))
        return -EFAULT;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    int ret = 0;

    if (*filename != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    lock(&g_dcache_lock);
    ret = path_lookupat(dir, filename, LOOKUP_FOLLOW, &dent);
    if (ret < 0)
        goto out;

    lock(&dent->inode->lock);
    dent->inode->uid = (uid == (uid_t)-1) ? dent->inode->uid : uid;
    dent->inode->gid = (gid == (gid_t)-1) ? dent->inode->gid : gid;
    unlock(&dent->inode->lock);

    put_dentry(dent);
out:
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    return ret;
}

long libos_syscall_fchown(int fd, uid_t uid, gid_t gid) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    int ret;
    if (!hdl->inode) {
        ret = -ENOENT;
        goto out;
    }

    lock(&hdl->inode->lock);
    hdl->inode->uid = (uid == (uid_t)-1) ? hdl->inode->uid : uid;
    hdl->inode->gid = (gid == (gid_t)-1) ? hdl->inode->gid : gid;
    unlock(&hdl->inode->lock);

    ret = 0;
out:
    put_handle(hdl);
    return ret;
}

static int do_rename(struct libos_dentry* old_dent, struct libos_dentry* new_dent) {
    assert(locked(&g_dcache_lock));
    assert(old_dent->inode);

    if ((old_dent->inode->type != S_IFREG && old_dent->inode->type != S_IFLNK &&
         old_dent->inode->type != S_IFDIR) ||
        (new_dent->inode && new_dent->inode->type != S_IFREG &&
         new_dent->inode->type != S_IFLNK && new_dent->inode->type != S_IFDIR)) {
        /* Current implementation allows regular files, symlinks and directories. */
        return -ENOSYS;
    }

    if (old_dent->mount != new_dent->mount) {
        /* Disallow cross mount renames */
        return -EXDEV;
    }

    struct libos_fs* fs = old_dent->inode->fs;
    if (!fs || !fs->d_ops || !fs->d_ops->rename) {
        return -EPERM;
    }

    if (old_dent->inode->type == S_IFDIR) {
        if (new_dent->inode) {
            if (new_dent->inode->type != S_IFDIR) {
                return -ENOTDIR;
            }
            if (new_dent->nchildren > 0) {
                return -ENOTEMPTY;
            }
        }
    } else if (new_dent->inode && new_dent->inode->type == S_IFDIR) {
        return -EISDIR;
    }

    if (dentry_is_ancestor(old_dent, new_dent) || dentry_is_ancestor(new_dent, old_dent)) {
        return -EINVAL;
    }

    /* TODO: Add appropriate checks for hardlinks once they get implemented. */

    /* inotify: capture the move's identity before the rename reshapes the dentries */
    struct libos_dentry* old_parent = old_dent->parent;
    bool moved_is_dir = old_dent->inode->type == S_IFDIR;
    static unsigned int rename_cookie = 0;
    unsigned int cookie = ++rename_cookie;

    int ret = fs->d_ops->rename(old_dent, new_dent);
    if (ret < 0)
        return ret;

    if (moved_is_dir) {
        /* directory `nlink` bookkeeping as in the kernel: the old parent loses one child, a
         * different new parent gains one, and an overwritten empty directory loses one */
        struct libos_inode* old_dir = new_dent->inode ? new_dent->inode : NULL; /* replaced dir */
        if (old_dent->parent) {
            lock(&old_dent->parent->inode->lock);
            if (old_dent->parent->inode->nlink > 0)
                old_dent->parent->inode->nlink--;
            unlock(&old_dent->parent->inode->lock);
        }
        if (new_dent->parent && new_dent->parent != old_dent->parent && new_dent->parent->inode) {
            lock(&new_dent->parent->inode->lock);
            new_dent->parent->inode->nlink++;
            unlock(&new_dent->parent->inode->lock);
        }
        if (old_dir && new_dent->parent && new_dent->parent->inode) {
            /* the replaced empty directory leaves the (same or other) new parent */
            lock(&new_dent->parent->inode->lock);
            if (new_dent->parent->inode->nlink > 0)
                new_dent->parent->inode->nlink--;
            unlock(&new_dent->parent->inode->lock);
        }
    }

    if (moved_is_dir) {
        /* The LibOS moves the inode (the kernel moves the dentry), so cached children of the old
         * dentry must follow it to the new one, and the process cwd (which the kernel keeps with
         * the moved directory) has to be rebound. */
        while (!LISTP_EMPTY(&old_dent->children)) {
            struct libos_dentry* child =
                LISTP_FIRST_ENTRY(&old_dent->children, struct libos_dentry, siblings);
            LISTP_DEL_INIT(child, &old_dent->children, siblings);
            child->parent = new_dent;
            LISTP_ADD_TAIL(child, &new_dent->children, siblings);
        }
        new_dent->nchildren = old_dent->nchildren;
        old_dent->nchildren = 0;

        lock(&g_process.fs_lock);
        if (g_process.cwd == old_dent) {
            get_dentry(new_dent);
            put_dentry(g_process.cwd);
            g_process.cwd = new_dent;
        }
        unlock(&g_process.fs_lock);
    }

    /* inotify: `IN_MOVED_FROM` (old parent, old name), `IN_MOVED_TO` (new parent, new name) with
     * a shared cookie, then `IN_MOVE_SELF` on the moved object; then the watches follow the
     * inode to the new dentry (the LibOS moves inodes, the kernel moves dentries) */
    if (old_parent)
        libos_inotify_child_event_raw(old_parent, old_dent->name, IN_MOVED_FROM, moved_is_dir,
                                      cookie);
    if (new_dent->parent && new_dent->parent != old_parent)
        libos_inotify_child_event_raw(new_dent->parent, new_dent->name, IN_MOVED_TO, moved_is_dir,
                                      cookie);
    else if (old_parent)
        libos_inotify_child_event_raw(old_parent, new_dent->name, IN_MOVED_TO, moved_is_dir,
                                      cookie);
    libos_inotify_move_self(old_dent);
    libos_inotify_transfer_watches(old_dent, new_dent);

    if (new_dent->inode)
        put_inode(new_dent->inode);
    new_dent->inode = old_dent->inode;
    old_dent->inode = NULL;
    return 0;
}

static int do_exchange(struct libos_dentry* old_dent, struct libos_dentry* new_dent) {
    assert(locked(&g_dcache_lock));
    assert(old_dent->inode && new_dent->inode);

    mode_t old_type = old_dent->inode->type;
    mode_t new_type = new_dent->inode->type;
    if ((old_type != S_IFREG && old_type != S_IFLNK && old_type != S_IFDIR) ||
        (new_type != S_IFREG && new_type != S_IFLNK && new_type != S_IFDIR)) {
        /* Same rule as `do_rename()`: regular files, symlinks and directories. */
        return -ENOSYS;
    }

    if (old_dent->mount != new_dent->mount || old_dent->inode->fs != new_dent->inode->fs) {
        return -EXDEV;
    }

    if (dentry_is_ancestor(old_dent, new_dent) || dentry_is_ancestor(new_dent, old_dent)) {
        return -EINVAL;
    }

    struct libos_fs* fs = old_dent->inode->fs;
    if (!fs || !fs->d_ops || !fs->d_ops->exchange) {
        return -EOPNOTSUPP;
    }

    int ret = fs->d_ops->exchange(old_dent, new_dent);
    if (ret < 0)
        return ret;

    /* The LibOS swaps the inodes of the two dentries (the kernel swaps the dentries), so what the
     * kernel carries along with a dentry has to be swapped here: the cached children of a
     * directory, the process cwd, and the inotify watches. */
    if (old_type == S_IFDIR || new_type == S_IFDIR) {
        LISTP_TYPE(libos_dentry) old_children = LISTP_INIT;
        LISTP_TYPE(libos_dentry) new_children = LISTP_INIT;
        LISTP_SPLICE_INIT(&old_dent->children, &old_children, siblings, libos_dentry);
        LISTP_SPLICE_INIT(&new_dent->children, &new_children, siblings, libos_dentry);
        struct libos_dentry* child;
        LISTP_FOR_EACH_ENTRY(child, &old_children, siblings)
            child->parent = new_dent;
        LISTP_FOR_EACH_ENTRY(child, &new_children, siblings)
            child->parent = old_dent;
        LISTP_SPLICE_INIT(&old_children, &new_dent->children, siblings, libos_dentry);
        LISTP_SPLICE_INIT(&new_children, &old_dent->children, siblings, libos_dentry);
        size_t nchildren = old_dent->nchildren;
        old_dent->nchildren = new_dent->nchildren;
        new_dent->nchildren = nchildren;

        lock(&g_process.fs_lock);
        struct libos_dentry* cwd = g_process.cwd;
        if (cwd == old_dent || cwd == new_dent) {
            struct libos_dentry* moved = cwd == old_dent ? new_dent : old_dent;
            get_dentry(moved);
            put_dentry(cwd);
            g_process.cwd = moved;
        }
        unlock(&g_process.fs_lock);

        /* a directory that changes parents moves its ".." link: when a directory and a
         * non-directory trade places across two parents, the parents' `nlink` follow */
        if (old_type != new_type && old_dent->parent != new_dent->parent && old_dent->parent &&
                new_dent->parent) {
            struct libos_inode* loses = (old_type == S_IFDIR ? old_dent : new_dent)->parent->inode;
            struct libos_inode* gains = (old_type == S_IFDIR ? new_dent : old_dent)->parent->inode;
            lock(&loses->lock);
            if (loses->nlink > 0)
                loses->nlink--;
            unlock(&loses->lock);
            lock(&gains->lock);
            gains->nlink++;
            unlock(&gains->lock);
        }
    }
    libos_inotify_swap_watches(old_dent, new_dent);

    struct libos_inode* tmp = old_dent->inode;
    old_dent->inode = new_dent->inode;
    new_dent->inode = tmp;
    return 0;
}

static int do_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
                       unsigned int flags) {
    struct libos_dentry* old_dir_dent = NULL;
    struct libos_dentry* old_dent     = NULL;
    struct libos_dentry* new_dir_dent = NULL;
    struct libos_dentry* new_dent     = NULL;
    int ret = 0;

    if (!is_user_string_readable(oldpath) || !is_user_string_readable(newpath)) {
        return -EFAULT;
    }

    lock(&g_dcache_lock);

    if (strcmp(oldpath, newpath) == 0) {
        goto out;
    }

    if (*oldpath != '/' && (ret = get_dirfd_dentry(olddirfd, &old_dir_dent)) < 0) {
        goto out;
    }

    ret = path_lookupat(old_dir_dent, oldpath, LOOKUP_NO_FOLLOW, &old_dent);
    if (ret < 0) {
        goto out;
    }

    if (!old_dent->inode) {
        ret = -ENOENT;
        goto out;
    }

    if (*newpath != '/' && (ret = get_dirfd_dentry(newdirfd, &new_dir_dent)) < 0) {
        goto out;
    }

    ret = path_lookupat(new_dir_dent, newpath, LOOKUP_NO_FOLLOW | LOOKUP_CREATE, &new_dent);
    if (ret < 0)
        goto out;

    // Both dentries should have a ref count of at least 2 at this point
    assert(refcount_get(&old_dent->ref_count) >= 2);
    assert(refcount_get(&new_dent->ref_count) >= 2);

    if (flags & RENAME_EXCHANGE) {
        if (!new_dent->inode) {
            ret = -ENOENT;
            goto out;
        }
        ret = do_exchange(old_dent, new_dent);
        goto out;
    }

    if ((flags & RENAME_NOREPLACE) && new_dent->inode) {
        ret = -EEXIST;
        goto out;
    }

    ret = do_rename(old_dent, new_dent);

out:
    unlock(&g_dcache_lock);
    if (old_dir_dent)
        put_dentry(old_dir_dent);
    if (old_dent)
        put_dentry(old_dent);
    if (new_dir_dent)
        put_dentry(new_dir_dent);
    if (new_dent)
        put_dentry(new_dent);
    return ret;
}

long libos_syscall_rename(const char* oldpath, const char* newpath) {
    return do_renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath, /*flags=*/0);
}

long libos_syscall_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
    return do_renameat(olddirfd, oldpath, newdirfd, newpath, /*flags=*/0);
}

long libos_syscall_renameat2(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
                             unsigned int flags) {
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -EINVAL;
    /* Linux rejects these combinations too (see tmpfs_rename_prepare()). */
    if ((flags & RENAME_EXCHANGE) && (flags & (RENAME_NOREPLACE | RENAME_WHITEOUT)))
        return -EINVAL;
    if (flags & RENAME_WHITEOUT)
        return -EOPNOTSUPP; /* no overlayfs whiteout support in the LibOS filesystems */
    return do_renameat(olddirfd, oldpath, newdirfd, newpath, flags);
}

static int validate_times(const struct __kernel_timespec times[2]) {
    for (size_t i = 0; i < 2; i++) {
        if (times[i].tv_nsec != UTIME_NOW && times[i].tv_nsec != UTIME_OMIT
                && (times[i].tv_nsec < 0 || times[i].tv_nsec >= 1000000000L))
            return -EINVAL;
    }
    return 0;
}

static int set_dentry_times(struct libos_dentry* dent, const struct __kernel_timespec times[2]) {
    if (!dent->inode)
        return -ENOENT;
    if (!dent->inode->fs || !dent->inode->fs->d_ops || !dent->inode->fs->d_ops->set_times)
        return -EOPNOTSUPP;
    return dent->inode->fs->d_ops->set_times(dent, times);
}

/* Set the times of the file referenced by `fd` through its dentry. Shared by the
 * `AT_EMPTY_PATH` / `futimesat(dirfd, NULL, ...)` paths. */
static int set_fd_times(int fd, const struct __kernel_timespec times[2]) {
    struct libos_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;
    if (!hdl->dentry) {
        put_handle(hdl);
        return -EOPNOTSUPP; /* no path to set times on (e.g. pipes and sockets) */
    }
    lock(&g_dcache_lock);
    int ret = set_dentry_times(hdl->dentry, times);
    unlock(&g_dcache_lock);
    put_handle(hdl);
    return ret;
}

/*
 * The shared core of the timestamp syscalls. `times` is a LibOS-internal (already copied and
 * validated) array; `pathname` is a LibOS-internal string and must be non-NULL.
 */
static int do_utimensat(int dirfd, const char* pathname,
                        const struct __kernel_timespec times[2], int flags) {
    struct libos_dentry* dir = NULL;
    struct libos_dentry* dent = NULL;
    int ret;

    if (*pathname != '/' && (ret = get_dirfd_dentry(dirfd, &dir)) < 0)
        return ret;

    lock(&g_dcache_lock);
    int lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NO_FOLLOW : LOOKUP_FOLLOW;
    ret = path_lookupat(dir, pathname, lookup_flags, &dent);
    if (ret == 0)
        ret = set_dentry_times(dent, times);
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    if (dent)
        put_dentry(dent);
    return ret;
}

long libos_syscall_utimensat(int dirfd, const char* pathname,
                             const struct __kernel_timespec* user_times,
                             int flags) {
    struct __kernel_timespec times[2];
    if (!is_user_string_readable(pathname))
        return -EFAULT;
    if (user_times && !is_user_memory_readable(user_times, sizeof(times)))
        return -EFAULT;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;

    if (user_times) {
        memcpy(times, user_times, sizeof(times));
    } else {
        times[0] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
        times[1] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
    }
    int ret = validate_times(times);
    if (ret < 0)
        return ret;

    if (!*pathname && (flags & AT_EMPTY_PATH))
        return set_fd_times(dirfd, times);

    return do_utimensat(dirfd, pathname, times, flags);
}

long libos_syscall_utime(const char* pathname, const struct utimbuf* user_times) {
    if (!is_user_string_readable(pathname))
        return -EFAULT;
    if (user_times && !is_user_memory_readable(user_times, sizeof(*user_times)))
        return -EFAULT;
    struct __kernel_timespec times[2];
    if (!user_times) {
        times[0] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
        times[1] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
    } else {
        struct utimbuf value;
        memcpy(&value, user_times, sizeof(value));
        times[0] = (struct __kernel_timespec){.tv_sec = value.actime, .tv_nsec = 0};
        times[1] = (struct __kernel_timespec){.tv_sec = value.modtime, .tv_nsec = 0};
    }
    return do_utimensat(AT_FDCWD, pathname, times, /*flags=*/0);
}

long libos_syscall_utimes(const char* pathname, const struct __kernel_timeval user_times[2]) {
    if (!is_user_string_readable(pathname))
        return -EFAULT;
    if (user_times && !is_user_memory_readable(user_times, 2 * sizeof(*user_times)))
        return -EFAULT;
    struct __kernel_timeval value[2];
    struct __kernel_timespec times[2];
    if (!user_times) {
        times[0] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
        times[1] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
    } else {
        memcpy(value, user_times, sizeof(value));
        for (size_t i = 0; i < 2; i++) {
            if (value[i].tv_usec < 0 || value[i].tv_usec >= 1000000)
                return -EINVAL;
            times[i] = (struct __kernel_timespec){.tv_sec = value[i].tv_sec,
                                                  .tv_nsec = value[i].tv_usec * 1000};
        }
    }
    return do_utimensat(AT_FDCWD, pathname, times, /*flags=*/0);
}

/* futimesat(dirfd, NULL, times) is the old futimes(): the times of the file `dirfd` refers to. A
 * non-NULL pathname follows the utimensat rules (relative to `dirfd`, absolute paths ignore it). */
long libos_syscall_futimesat(int dirfd, const char* pathname,
                             const struct __kernel_timeval user_times[2]) {
    if (pathname && !is_user_string_readable(pathname))
        return -EFAULT;
    if (user_times && !is_user_memory_readable(user_times, 2 * sizeof(*user_times)))
        return -EFAULT;
    struct __kernel_timeval value[2];
    struct __kernel_timespec times[2];
    if (!user_times) {
        times[0] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
        times[1] = (struct __kernel_timespec){.tv_nsec = UTIME_NOW};
    } else {
        memcpy(value, user_times, sizeof(value));
        for (size_t i = 0; i < 2; i++) {
            if (value[i].tv_usec < 0 || value[i].tv_usec >= 1000000)
                return -EINVAL;
            times[i] = (struct __kernel_timespec){.tv_sec = value[i].tv_sec,
                                                  .tv_nsec = value[i].tv_usec * 1000};
        }
    }
    if (!pathname)
        return set_fd_times(dirfd, times);
    return do_utimensat(dirfd, pathname, times, /*flags=*/0);
}

long libos_syscall_symlink(const char* target, const char* linkpath) {
    return libos_syscall_symlinkat(target, AT_FDCWD, linkpath);
}

long libos_syscall_symlinkat(const char* target, int newdirfd, const char* linkpath) {
    if (!is_user_string_readable(target) || !is_user_string_readable(linkpath))
        return -EFAULT;

    struct libos_dentry* dir = NULL;
    struct libos_dentry* new = NULL;
    int ret;
    if (*linkpath != '/' && (ret = get_dirfd_dentry(newdirfd, &dir)) < 0)
        return ret;

    lock(&g_dcache_lock);
    ret = path_lookupat(dir, linkpath, LOOKUP_NO_FOLLOW | LOOKUP_CREATE, &new);
    if (ret < 0)
        goto out;
    if (new->inode) {
        ret = -EEXIST;
        goto out;
    }
    if (!new->parent) {
        ret = -EEXIST;
        goto out;
    }
    ret = check_permissions(new->parent, MAY_WRITE | MAY_EXEC);
    if (ret < 0)
        goto out;
    struct libos_d_ops* d_ops = new->mount->fs->d_ops;
    if (!d_ops || !d_ops->symlink) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    ret = d_ops->symlink(target, new);
out:
    unlock(&g_dcache_lock);
    if (dir)
        put_dentry(dir);
    if (new)
        put_dentry(new);
    return ret;
}

long libos_syscall_link(const char* oldpath, const char* newpath) {
    return libos_syscall_linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

long libos_syscall_linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
                          int flags) {
    if (!is_user_string_readable(oldpath) || !is_user_string_readable(newpath))
        return -EFAULT;
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH))
        return -EINVAL;

    struct libos_dentry* old_dir = NULL;
    struct libos_dentry* old = NULL;
    struct libos_dentry* new_dir = NULL;
    struct libos_dentry* new = NULL;
    struct libos_handle* old_hdl = NULL;
    int ret;
    if (!*oldpath && (flags & AT_EMPTY_PATH)) {
        old_hdl = get_fd_handle(olddirfd, NULL, NULL);
        if (!old_hdl)
            return -EBADF;
        old = old_hdl->dentry;
        if (!old) {
            put_handle(old_hdl);
            return -EBADF;
        }
        get_dentry(old);
    } else if (*oldpath != '/' && (ret = get_dirfd_dentry(olddirfd, &old_dir)) < 0) {
        return ret;
    }
    if (*newpath != '/' && (ret = get_dirfd_dentry(newdirfd, &new_dir)) < 0) {
        goto out;
    }

    lock(&g_dcache_lock);
    if (!old) {
        int lookup_flags = flags & AT_SYMLINK_FOLLOW ? LOOKUP_FOLLOW : LOOKUP_NO_FOLLOW;
        ret = path_lookupat(old_dir, oldpath, lookup_flags, &old);
        if (ret < 0)
            goto unlock_out;
    }
    if (!old->inode) {
        ret = -ENOENT;
        goto unlock_out;
    }
    if (old->inode->type == S_IFDIR) {
        ret = -EPERM;
        goto unlock_out;
    }
    ret = path_lookupat(new_dir, newpath, LOOKUP_NO_FOLLOW | LOOKUP_CREATE, &new);
    if (ret < 0)
        goto unlock_out;
    if (new->inode) {
        ret = -EEXIST;
        goto unlock_out;
    }
    if (!new->parent) {
        ret = -EEXIST;
        goto unlock_out;
    }
    ret = check_permissions(new->parent, MAY_WRITE | MAY_EXEC);
    if (ret < 0)
        goto unlock_out;
    if (old->mount != new->mount) {
        ret = -EXDEV;
        goto unlock_out;
    }
    struct libos_d_ops* d_ops = new->mount->fs->d_ops;
    if (!d_ops || !d_ops->link) {
        ret = -EOPNOTSUPP;
        goto unlock_out;
    }
    ret = d_ops->link(old, new);
unlock_out:
    unlock(&g_dcache_lock);
out:
    if (old_hdl)
        put_handle(old_hdl);
    if (old_dir)
        put_dentry(old_dir);
    if (old)
        put_dentry(old);
    if (new_dir)
        put_dentry(new_dir);
    if (new)
        put_dentry(new);
    return ret;
}

long libos_syscall_sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
    long ret;
    char* buf = NULL;

    /* "unused" to silence newer Clang; we keep this variable as it may be helpful once we improve
     * the `sendfile()` implementation, see one of the TODOs below */
    __attribute__((unused)) size_t read_from_in = 0;

    size_t copied_to_out = 0;

    if (offset && !is_user_memory_writable(offset, sizeof(*offset)))
        return -EFAULT;

    struct libos_handle* in_hdl = get_fd_handle(in_fd, NULL, NULL);
    if (!in_hdl)
        return -EBADF;

    struct libos_handle* out_hdl = get_fd_handle(out_fd, NULL, NULL);
    if (!out_hdl) {
        put_handle(in_hdl);
        return -EBADF;
    }

    if (!in_hdl->fs || !in_hdl->fs->fs_ops || !out_hdl->fs || !out_hdl->fs->fs_ops) {
        ret = -EINVAL;
        goto out;
    }

    if (out_hdl->flags & O_APPEND) {
        /* Linux errors out if output fd has the O_APPEND flag set; comply with this behavior */
        ret = -EINVAL;
        goto out;
    }

    /* FIXME: This sendfile() emulation is very simple and not particularly efficient: it reads from
     *        input FD in BUF_SIZE chunks and writes into output FD. Mmap-based emulation may be
     *        more efficient but adds complexity (not all handle types provide mmap callback). */

    bool buf_in_use = __atomic_exchange_n(&g_sendfile_buf_in_use, true, __ATOMIC_ACQUIRE);
    if (!buf_in_use) {
        /* no other thread was using the static buffer */
        buf = g_sendfile_buf;
    } else {
        buf = malloc(BUF_SIZE);
        if (!buf) {
            ret = -ENOMEM;
            goto out;
        }
    }

    if (!count) {
        ret = 0;
        goto out;
    }

    /*
     * If `offset` is not NULL, we use `*offset` as starting offset for reading, and update
     * `*offset` afterwards (and keep the offset in input handle unchanged).
     *
     * If `offset` is NULL, we use the offset in input handle, and update it afterwards (only if the
     * input handle is seekable).
     */
    file_off_t pos_in = 0;
    if (offset) {
        if (!in_hdl->fs->fs_ops->seek) {
            ret = -ESPIPE;
            goto out;
        }
        pos_in = *offset;
        if (pos_in < 0) {
            ret = -EINVAL;
            goto out;
        }
    } else {
        maybe_lock_pos_handle(in_hdl);
        pos_in = in_hdl->pos;
        maybe_unlock_pos_handle(in_hdl);
    }

    if (!(out_hdl->acc_mode & MAY_WRITE)) {
        /* Linux errors out if output fd isn't writable */
        ret = -EBADF;
        goto out;
    }

    while (copied_to_out < count) {
        size_t to_copy = count - copied_to_out > BUF_SIZE ? BUF_SIZE : count - copied_to_out;

        ssize_t x = in_hdl->fs->fs_ops->read(in_hdl, buf, to_copy, &pos_in);
        if (x < 0) {
            ret = x;
            goto out_update;
        }
        assert(x <= (ssize_t)to_copy);

        read_from_in += x;

        if (x == 0) {
            /* no more data in input FD, let's return however many bytes copied_to_out up until now */
            break;
        }

        maybe_lock_pos_handle(out_hdl);
        ssize_t y = out_hdl->fs->fs_ops->write(out_hdl, buf, x, &out_hdl->pos);
        maybe_unlock_pos_handle(out_hdl);
        if (y < 0) {
            ret = y;
            goto out_update;
        }
        assert(y <= x);

        copied_to_out += y;

        if (y < x) {
            /* written less bytes to output fd than read from input fd -> out of sync now; don't try
             * to be smart and simply return however many bytes we copied_to_out up until now */
            /* TODO: need to revert in_fd's file position to (read_from_in - x + y) from original
             *       offset and maybe continue this loop */
            break;
        }
    }

    ret = 0;

out_update:
    /* Update either `*offset` or the offset in input file (see the comment above `pos_in`
     * declaration). Note that we do it even if one of the read/write operations failed. */
    if (offset) {
        *offset = pos_in;
    } else if (in_hdl->seekable) {
        maybe_lock_pos_handle(in_hdl);
        in_hdl->pos = pos_in;
        maybe_unlock_pos_handle(in_hdl);
    }

out:
    if (buf == g_sendfile_buf)
        __atomic_store_n(&g_sendfile_buf_in_use, 0, __ATOMIC_RELEASE);
    else
        free(buf);
    put_handle(in_hdl);
    put_handle(out_hdl);
    return copied_to_out ? (long)copied_to_out : ret;
}

long libos_syscall_chroot(const char* filename) {
    if (!is_user_string_readable(filename))
        return -EFAULT;

    int ret = 0;
    struct libos_dentry* dent = NULL;
    lock(&g_dcache_lock);
    ret = path_lookupat(/*start=*/NULL, filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &dent);
    unlock(&g_dcache_lock);
    if (ret < 0)
        goto out;

    if (!dent) {
        ret = -ENOENT;
        goto out;
    }

    lock(&g_process.fs_lock);
    put_dentry(g_process.root);
    g_process.root = dent;
    unlock(&g_process.fs_lock);
out:
    return ret;
}
