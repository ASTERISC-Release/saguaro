/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_table.h"
#include "libos_thread.h"

long libos_syscall_getresuid(uid_t* ruid, uid_t* euid, uid_t* suid) {
    if ((ruid && !is_user_memory_writable(ruid, sizeof(*ruid))) ||
        (euid && !is_user_memory_writable(euid, sizeof(*euid))) ||
        (suid && !is_user_memory_writable(suid, sizeof(*suid))))
        return -EFAULT;
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    if (ruid) *ruid = t->uid;
    if (euid) *euid = t->euid;
    if (suid) *suid = t->suid;
    unlock(&t->lock);
    return 0;
}

long libos_syscall_getresgid(gid_t* rgid, gid_t* egid, gid_t* sgid) {
    if ((rgid && !is_user_memory_writable(rgid, sizeof(*rgid))) ||
        (egid && !is_user_memory_writable(egid, sizeof(*egid))) ||
        (sgid && !is_user_memory_writable(sgid, sizeof(*sgid))))
        return -EFAULT;
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    if (rgid) *rgid = t->gid;
    if (egid) *egid = t->egid;
    if (sgid) *sgid = t->sgid;
    unlock(&t->lock);
    return 0;
}

static long setresid(bool group, uint32_t rid, uint32_t eid, uint32_t sid) {
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    uint32_t old_r = group ? t->gid : t->uid;
    uint32_t old_e = group ? t->egid : t->euid;
    uint32_t old_s = group ? t->sgid : t->suid;
    bool privileged = t->euid == 0;
    if ((!privileged && rid != UINT32_MAX && rid != old_r && rid != old_e && rid != old_s) ||
        (!privileged && eid != UINT32_MAX && eid != old_r && eid != old_e && eid != old_s) ||
        (!privileged && sid != UINT32_MAX && sid != old_r && sid != old_e && sid != old_s)) {
        unlock(&t->lock);
        return -EPERM;
    }
    if (rid != UINT32_MAX) { if (group) t->gid = rid; else t->uid = rid; }
    if (eid != UINT32_MAX) {
        if (group) {
            t->egid = eid;
            t->fsgid = eid;
        } else {
            t->euid = eid;
            t->fsuid = eid;
        }
    }
    if (sid != UINT32_MAX) { if (group) t->sgid = sid; else t->suid = sid; }
    unlock(&t->lock);
    return 0;
}

long libos_syscall_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    return setresid(false, ruid, euid, suid);
}
long libos_syscall_setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
    return setresid(true, rgid, egid, sgid);
}
long libos_syscall_setreuid(uid_t ruid, uid_t euid) {
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    uint32_t old_uid = t->uid;
    bool privileged = t->euid == 0;
    if ((!privileged && ruid != UINT32_MAX && ruid != t->uid && ruid != t->euid) ||
        (!privileged && euid != UINT32_MAX && euid != t->uid && euid != t->euid && euid != t->suid)) {
        unlock(&t->lock);
        return -EPERM;
    }
    if (ruid != UINT32_MAX) t->uid = ruid;
    if (euid != UINT32_MAX) { t->euid = euid; t->fsuid = euid; }
    if (ruid != UINT32_MAX || (euid != UINT32_MAX && euid != old_uid)) t->suid = t->euid;
    unlock(&t->lock);
    return 0;
}
long libos_syscall_setregid(gid_t rgid, gid_t egid) {
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    uint32_t old_gid = t->gid;
    bool privileged = t->euid == 0;
    if ((!privileged && rgid != UINT32_MAX && rgid != t->gid && rgid != t->egid) ||
        (!privileged && egid != UINT32_MAX && egid != t->gid && egid != t->egid && egid != t->sgid)) {
        unlock(&t->lock);
        return -EPERM;
    }
    if (rgid != UINT32_MAX) t->gid = rgid;
    if (egid != UINT32_MAX) { t->egid = egid; t->fsgid = egid; }
    if (rgid != UINT32_MAX || (egid != UINT32_MAX && egid != old_gid)) t->sgid = t->egid;
    unlock(&t->lock);
    return 0;
}

long libos_syscall_setfsuid(uid_t uid) {
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    uid_t old = t->fsuid;
    if (uid == (uid_t)-1 || t->euid == 0 || uid == t->uid || uid == t->euid || uid == t->suid)
        if (uid != (uid_t)-1) t->fsuid = uid;
    unlock(&t->lock);
    return old;
}
long libos_syscall_setfsgid(gid_t gid) {
    struct libos_thread* t = get_cur_thread();
    lock(&t->lock);
    gid_t old = t->fsgid;
    if (gid == (gid_t)-1 || t->euid == 0 || gid == t->gid || gid == t->egid || gid == t->sgid)
        if (gid != (gid_t)-1) t->fsgid = gid;
    unlock(&t->lock);
    return old;
}
