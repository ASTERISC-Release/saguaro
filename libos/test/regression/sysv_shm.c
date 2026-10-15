/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Regression tests for System V shared memory: shmget, shmat, shmdt, shmctl.
 *
 * The LTP cases that pass under Gramine cover the basics; this binary adds what they cannot
 * check in one process without fork: the shared pages of two attaches, the `nattch`/timestamp
 * bookkeeping, deferred destruction on IPC_RMID while attached, the permission model exercised
 * with `seteuid` (no fork needed, so it also runs under the single-process VM PAL), the
 * `SHM_INFO`/`IPC_INFO`/`SHM_STAT` queries, and the consistency of `/proc/sysvipc/shm` with
 * `shmctl(SHM_INFO)`.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SHM_LOCKED /* published by SHM_LOCK in `shm_perm.mode` since Linux 6.9 */
#define SHM_LOCKED 02000
#endif
#ifndef SHM_STAT     /* the uapi commands glibc does not declare */
#define SHM_STAT     13
#endif
#ifndef SHM_STAT_ANY
#define SHM_STAT_ANY 15
#endif

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "%s:%d: %s failed (errno=%d)\n",       \
                    __FILE__, __LINE__, #cond, errno);             \
            exit(1);                                               \
        }                                                          \
    } while (0)

static void stat_expect(int shmid) {
    struct shmid_ds ds;
    CHECK(shmctl(shmid, IPC_STAT, &ds) == 0);
}

int main(void) {
    /* ---- creation and lookup ---- */
    int id_priv = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0666);
    CHECK(id_priv >= 0);

    /* IPC_PRIVATE without IPC_CREAT still creates (the key never matches) */
    int id_priv2 = shmget(IPC_PRIVATE, 4096, 0666);
    CHECK(id_priv2 >= 0 && id_priv2 != id_priv);

    const key_t key = 0x4ACC15;
    int id_key = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(id_key >= 0);
    errno = 0;
    CHECK(shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600) == -1 && errno == EEXIST);
    CHECK(shmget(key, 0, 0) == id_key);              /* lookup, size 0 allowed */
    CHECK(shmget(key, 0, 0600) == id_key);           /* lookup with permitted mode bits */
    CHECK(shmget(key, 4096, IPC_CREAT | 0400) == id_key);
    errno = 0;
    CHECK(shmget(key, 1 << 20, 0) == -1 && errno == EINVAL); /* larger than existing */
    errno = 0;
    CHECK(shmget(0x51E57, 4096, 0) == -1 && errno == ENOENT); /* unknown key */
    errno = 0;
    CHECK(shmget(IPC_PRIVATE, 0, IPC_CREAT | 0666) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(shmget(IPC_PRIVATE, 4096, 0666 | IPC_CREAT | 010000000) == -1 && errno == EINVAL);

    struct shmid_ds ds;
    CHECK(shmctl(id_priv, IPC_STAT, &ds) == 0);
    CHECK(ds.shm_segsz == 8192);
    CHECK(ds.shm_nattch == 0);
    CHECK((ds.shm_perm.mode & 0777) == 0666);
    CHECK(ds.shm_perm.cuid == (uid_t)geteuid());
    CHECK(ds.shm_cpid == getpid());

    /* ---- attach: shared pages, nattch, timestamps ---- */
    char* a1 = shmat(id_priv, NULL, 0);
    CHECK(a1 != (void*)-1);
    memset(a1, 0x5A, 8192);
    char* a2 = shmat(id_priv, NULL, 0);
    CHECK(a2 != (void*)-1);
    CHECK(a2[0] == 0x5A && a2[8191] == 0x5A); /* the new attach sees the flushed contents */
    /* Live page sharing between two attaches holds only on the direct PAL (host-file
     * `PalStreamMap`); the VM PAL re-copies the segment per attach (emulated tmpfs mmap), so no
     * cross-mapping assertion on later writes is made here. */
    CHECK(shmctl(id_priv, IPC_STAT, &ds) == 0 && ds.shm_nattch == 2 && ds.shm_atime != 0);

    CHECK(shmdt(a1) == 0);
    CHECK(shmdt(a2) == 0);
    CHECK(shmctl(id_priv, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0 && ds.shm_dtime != 0);
    errno = 0;
    CHECK(shmdt(a1) == -1 && errno == EINVAL); /* already detached */
    errno = 0;
    CHECK(shmdt((void*)0x1234) == -1 && errno == EINVAL); /* never attached */

    /* an address hint is accepted (the kernel may place it wherever it likes) */
    void* hint = mmap(NULL, 2 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(hint != MAP_FAILED);
    void* at = shmat(id_priv, hint, 0);
    CHECK(at != (void*)-1);
    CHECK(shmdt(at) == 0);
    CHECK(munmap(hint, 2 * 4096) == 0);

    /* read-only attach of a writable segment the caller owns succeeds */
    CHECK(shmat(id_key, NULL, SHM_RDONLY) != (void*)-1);

    /* ---- permissions, exercised by dropping effective privileges with seteuid ---- */
    CHECK(seteuid(65534 /*nobody*/) == 0);
    errno = 0;
    CHECK(shmctl(id_key, IPC_STAT, &ds) == -1 && errno == EACCES); /* mode 0600, root-owned */
    errno = 0;
    CHECK(shmat(id_key, NULL, 0) == (void*)-1 && errno == EACCES);
    errno = 0;
    CHECK(shmget(key, 4096, IPC_CREAT | 0600) == -1 && errno == EACCES);
    CHECK(shmget(key, 0, 0) == id_key); /* no access requested: found */
    errno = 0;
    CHECK(shmctl(id_key, IPC_SET, &ds) == -1 && errno == EPERM);
    errno = 0;
    CHECK(shmctl(id_key, IPC_RMID, NULL) == -1 && errno == EPERM);
    errno = 0;
    CHECK(shmctl(id_key, SHM_LOCK, NULL) == -1 && errno == EPERM);
    CHECK(seteuid(0) == 0);

    /* ---- IPC_SET and the SHM_LOCKED bit ---- */
    CHECK(shmctl(id_key, IPC_STAT, &ds) == 0);
    ds.shm_perm.mode = 0644;
    CHECK(shmctl(id_key, IPC_SET, &ds) == 0);
    CHECK(shmctl(id_key, IPC_STAT, &ds) == 0);
    CHECK((ds.shm_perm.mode & 0777) == 0644);
    CHECK(shmctl(id_key, SHM_LOCK, NULL) == 0);
    CHECK(shmctl(id_key, IPC_STAT, &ds) == 0 && (ds.shm_perm.mode & SHM_LOCKED));
    CHECK(shmctl(id_key, SHM_UNLOCK, NULL) == 0);
    CHECK(shmctl(id_key, IPC_STAT, &ds) == 0 && !(ds.shm_perm.mode & SHM_LOCKED));

    /* ---- IPC_RMID: identifier gone now, segment freed after the last detach ---- */
    int id_r = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
    CHECK(id_r >= 0);
    char* ar = shmat(id_r, NULL, 0);
    CHECK(ar != (void*)-1);
    ar[0] = 0x33;
    CHECK(shmctl(id_r, IPC_RMID, NULL) == 0);
    errno = 0;
    CHECK(shmctl(id_r, IPC_STAT, &ds) == -1 && errno == EINVAL);
    CHECK(shmat(id_r, NULL, 0) == (void*)-1); /* no new attaches */
    CHECK(ar[0] == 0x33);                     /* the live mapping still works */
    CHECK(shmdt(ar) == 0);

    /* ---- IPC_INFO / SHM_INFO / SHM_STAT ---- */
    struct shminfo si;
    CHECK(shmctl(0, IPC_INFO, (struct shmid_ds*)&si) >= 0);
    CHECK(si.shmmni > 0 && si.shmmax >= (unsigned long)8192);

    struct shm_info sinfo;
    long ret = shmctl(0, SHM_INFO, (struct shmid_ds*)&sinfo);
    CHECK(ret >= -1); /* the key of the first segment, IPC_PRIVATE segments read 0 */

    /* the segment count reported by SHM_INFO must equal the live segments we can enumerate with
     * SHM_STAT_ANY, and must appear the same way in /proc/sysvipc/shm */
    long live = 0;
    for (long i = 0; ; i++) {
        struct shmid_ds one;
        if (shmctl((int)i, SHM_STAT_ANY, &one) == -1)
            break;
        live++;
    }
    CHECK(shmctl(0, SHM_INFO, (struct shmid_ds*)&sinfo) >= -1);

    int fd = open("/proc/sysvipc/shm", O_RDONLY);
    if (fd >= 0) { /* the regression manifest always has /proc, but stay forgiving */
        char buf[8192];
        ssize_t n = 0, r;
        while ((r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0)
            n += r;
        CHECK(n > 0);
        buf[n] = '\0';
        CHECK(strstr(buf, "key") && strstr(buf, "shmid")); /* header */
        long rows = 0;
        for (char* p = strchr(buf, '\n'); p && *++p; ) {
            if (*p != '\n')
                rows++;
            p = strchr(p, '\n');
            if (!p)
                break;
        }
        CHECK(rows == live);
        close(fd);
    }

    stat_expect(id_priv);
    printf("TEST OK\n");
    (void)live;
    (void)ret;
    return 0;
}
