/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * The Linux ABI of the System V IPC primitives the LibOS implements directly
 * (shared memory today; see `libos/src/sys/libos_sysv_shm.c`). The layouts match
 * `<asm-generic/ipcbuf.h>` and `<asm-generic/shmbuf.h>` as used by all three
 * architectures this LibOS builds for (x86_64, aarch64, riscv64 — all 64-bit, so
 * `shmid_ds` is `shmid64_ds` and the time fields are one `long`).
 */

#ifndef _LINUX_ABI_IPC_H
#define _LINUX_ABI_IPC_H

#include "libos_types.h"

/* ipc(2) keys and semflg/shmflg bits, <linux/ipc.h> */
#define LINUX_IPC_PRIVATE   0 /* ((key_t)0): always create a new, unnamed segment */
#define LINUX_IPC_CREAT     01000
#define LINUX_IPC_EXCL      02000
#define LINUX_IPC_NOWAIT    04000

#define LINUX_IPC_RMID      0
#define LINUX_IPC_SET       1
#define LINUX_IPC_STAT      2
#define LINUX_IPC_INFO      3

/* shmget() shmflg extra bits, <linux/shm.h> */
#define LINUX_SHM_HUGETLB   04000
#define LINUX_SHM_NORESERVE 010000

/* shmat() shmflg bits, <linux/shm.h> */
#define LINUX_SHM_RDONLY    010000
#define LINUX_SHM_RND       020000 /* round a given attach address to SHMLBA (accepted, ignored) */
#define LINUX_SHM_REMAP     040000 /* replace the existing mapping at the given address */
#define LINUX_SHM_EXEC      0100000
/* SHMLBA is PAGE_SIZE on every architecture this LibOS builds for (`asm/shm.h` on x86_64,
 * asm-generic elsewhere). */
#define LINUX_SHMLBA        0x1000

/* shmctl() extra commands, <linux/shm.h> */
#define LINUX_SHM_LOCK      11
#define LINUX_SHM_UNLOCK    12
#define LINUX_SHM_STAT      13
#define LINUX_SHM_INFO      14
#define LINUX_SHM_STAT_ANY  15

/* bit `<linux/shm.h>` reports in `shmid_ds.shm_perm.mode` while SHM_LOCK is held */
#define LINUX_SHM_LOCKED    02000

/* <asm-generic/ipcbuf.h> struct ipc64_perm; glibc's `struct ipc_perm` has this
 * exact layout on all supported (64-bit) architectures. */
struct linux_ipc_perm {
    int key;
    unsigned int uid;
    unsigned int gid;
    unsigned int cuid;
    unsigned int cgid;
    unsigned short mode;
    unsigned char __pad1[4 - sizeof(unsigned short)];
    unsigned short seq;
    unsigned short __pad2;
    unsigned long __unused1;
    unsigned long __unused2;
};

/* <asm-generic/shmbuf.h> struct shmid64_ds on 64-bit architectures. */
struct linux_shmid_ds {
    struct linux_ipc_perm shm_perm;
    size_t shm_segsz;
    long shm_atime;
    long shm_dtime;
    long shm_ctime;
    int shm_cpid;
    int shm_lpid;
    unsigned long shm_nattch;
    unsigned long __unused4;
    unsigned long __unused5;
};

/* <asm-generic/shmbuf.h> struct shminfo64 (glibc's `struct shminfo`). */
struct linux_shminfo64 {
    unsigned long shmmax;
    unsigned long shmin;
    unsigned long shmmni;
    unsigned long shmseg;
    unsigned long shmall;
    unsigned long __unused1;
    unsigned long __unused2;
    unsigned long __unused3;
    unsigned long __unused4;
};

/* <linux/shm.h> struct shm_info (what IPC_INFO/SHM_INFO report on older kernels;
 * the accounting fields are not tracked by the LibOS and read as zero). */
struct linux_shm_info {
    int used_ids;
    unsigned long shm_tot;
    unsigned long shm_rss;
    unsigned long shm_swp;
    unsigned long swap_attempts;
    unsigned long swap_successes;
};

#endif /* _LINUX_ABI_IPC_H */
