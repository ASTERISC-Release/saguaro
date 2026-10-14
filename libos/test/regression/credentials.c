/* SPDX-License-Identifier: LGPL-3.0-or-later */

/*
 * Regression tests for the credential syscalls under Gramine's single-user model:
 * getresuid/getresgid, setresuid/setresgid, setreuid/setregid, setfsuid/setfsgid,
 * capget/capset, and the effect of the filesystem IDs on path permission checks.
 *
 * The test must run as root (it drops privileges and cannot get them back at the end).
 * Capability masks are software bookkeeping in the LibOS: the test checks their Linux ABI
 * validation rules, not any host privilege effect.
 *
 * Usage: credentials <base-dir>   (a subdirectory "creds" is created and removed)
 */

#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define UID_A 1000u
#define UID_B 1001u

static char base[512];
static char dir[600];
static char froot[640];
static char fworld[640];

static void check(const char* what, long got, long want) {
    if (got != want)
        errx(1, "%s: got %ld (errno=%d), want %ld", what, got, errno, want);
}

int main(int argc, const char** argv) {
    if (argc != 2)
        errx(1, "usage: credentials <base-dir>");
    snprintf(base, sizeof(base), "%s", argv[1]);

    if (getuid() != 0 || geteuid() != 0)
        errx(1, "this test must run as root");

    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;

    /* 1. the initial state is consistent */
    if (getresuid(&ruid, &euid, &suid))
        err(1, "getresuid");
    if (ruid != 0 || euid != 0 || suid != 0)
        errx(1, "initial ids are %u/%u/%u, want 0/0/0", ruid, euid, suid);
    if (getresgid(&rgid, &egid, &sgid))
        err(1, "getresgid");
    if (rgid != 0 || egid != 0 || sgid != 0)
        errx(1, "initial gids are %u/%u/%u, want 0/0/0", rgid, egid, sgid);

    /* 2. setresuid sets all three, -1 leaves one alone; keeping the saved uid at 0 allows the
     *    return to root, and losing it (Linux rule) does not: both directions are checked */
    check("setresuid", syscall(SYS_setresuid, UID_A, UID_B, 0), 0);
    getresuid(&ruid, &euid, &suid);
    check("ruid", ruid, UID_A);
    check("euid", euid, UID_B);
    check("suid", suid, 0);
    check("getuid", getuid(), UID_A);
    check("geteuid", geteuid(), UID_B);
    check("setresuid(-1,-1,-1)", syscall(SYS_setresuid, -1, -1, -1), 0);
    getresuid(&ruid, &euid, &suid);
    check("ruid after -1", ruid, UID_A);

    /* return to root through the saved uid */
    check("setresuid(0,0,0) via suid", syscall(SYS_setresuid, 0, 0, 0), 0);
    getresuid(&ruid, &euid, &suid);
    check("ruid back", ruid, 0);
    check("euid back", euid, 0);

    /* 3. setreuid: ruid changed, euid kept: the saved uid follows the euid (Linux rule) */
    check("setreuid", syscall(SYS_setreuid, UID_A, -1), 0);
    getresuid(&ruid, &euid, &suid);
    check("setreuid ruid", ruid, UID_A);
    check("setreuid euid", euid, 0);
    check("setreuid suid", suid, 0);
    check("setreuid(0,0)", syscall(SYS_setreuid, 0, 0), 0);

    /* 4. setfsuid/setfsgid return the old value; an unprivileged caller can only pick its own */
    check("setfsuid(1234) old", syscall(SYS_setfsuid, 1234), 0);
    check("setfsuid(-1) current", syscall(SYS_setfsuid, -1), 1234);
    check("setfsuid back", syscall(SYS_setfsuid, 0), 1234);
    check("setfsgid(4321) old", syscall(SYS_setfsgid, 4321), 0);
    check("setfsgid(-1) current", syscall(SYS_setfsgid, -1), 4321);
    check("setfsgid back", syscall(SYS_setfsgid, 0), 4321);

    /* 5. capget reports the software sets; capset can reduce but not expand them */
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0,
    };
    struct __user_cap_data_struct data[2];
    check("capget", syscall(SYS_capget, &hdr, data), 0);
    if (data[0].effective == 0 || data[0].permitted == 0)
        errx(1, "root should start with capabilities");
    uint32_t full0 = data[0].permitted, full1 = data[1].permitted;

    /* reduce to the lowest permitted capability only (the initial set is software bookkeeping,
     * so pick bits from what capget actually reports rather than assuming a concrete set) */
    uint32_t low = data[0].permitted & (uint32_t)(-(int32_t)data[0].permitted);
    uint32_t rest = data[0].permitted & ~low;
    uint32_t other = rest & (uint32_t)(-(int32_t)rest);
    struct __user_cap_data_struct reduced[2] = {
        {.effective = low, .permitted = low, .inheritable = 0},
        {0, 0, 0},
    };
    check("capset reduce", syscall(SYS_capset, &hdr, reduced), 0);
    check("capget after reduce", syscall(SYS_capget, &hdr, data), 0);
    check("reduced effective", data[0].effective, low);
    check("reduced permitted", data[0].permitted, low);

    /* expanding beyond the current permitted set is EPERM */
    struct __user_cap_data_struct back[2] = {
        {.effective = full0, .permitted = full0, .inheritable = 0},
        {full1, full1, 0},
    };
    long r = syscall(SYS_capset, &hdr, back);
    if (r == 0 || errno != EPERM)
        errx(1, "capset expansion: want EPERM, got %ld/%d", r, errno);

    /* effect mask must stay a subset of the permitted mask */
    if (!other)
        errx(1, "initial capability set has fewer than two bits");
    struct __user_cap_data_struct bad[2] = {
        {.effective = other, .permitted = low, .inheritable = 0},
        {0, 0, 0},
    };
    r = syscall(SYS_capset, &hdr, bad);
    if (r == 0 || errno != EPERM)
        errx(1, "capset effective>permitted: want EPERM, got %ld/%d", r, errno);

    /* 6. prepare files for the permission checks, then drop privileges for good. The group ids
     * must be dropped before the user ids: after the uid drop the process is unprivileged and
     * cannot change its gids any more (same rule as Linux). */
    snprintf(dir, sizeof(dir), "%s/creds", base);
    snprintf(froot, sizeof(froot), "%s/root_only", dir);
    snprintf(fworld, sizeof(fworld), "%s/world", dir);
    rmdir(dir); /* leftovers */
    if (mkdir(dir, 0755))
        err(1, "mkdir %s", dir);
    int fd = open(froot, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        err(1, "create %s", froot);
    close(fd);
    fd = open(fworld, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        err(1, "create %s", fworld);
    close(fd);
    /* hand the directory to the unprivileged user so that it can clean the files up */
    if (chown(dir, UID_A, UID_A))
        err(1, "chown %s", dir);
    if (chmod(dir, 0700))
        err(1, "chmod %s", dir);

    check("setresgid drop", syscall(SYS_setresgid, UID_A, UID_A, UID_A), 0);
    check("setresuid drop", syscall(SYS_setresuid, UID_A, UID_A, UID_A), 0);
    check("fsuid follows euid", syscall(SYS_setfsuid, -1), UID_A);
    check("fsgid follows egid", syscall(SYS_setfsgid, -1), UID_A);

    /* 7. a dropped process cannot get root back */
    r = syscall(SYS_setresuid, 0, 0, 0);
    if (r == 0 || errno != EPERM)
        errx(1, "setresuid(0) after drop: want EPERM, got %ld/%d", r, errno);
    r = syscall(SYS_setuid, 0);
    if (r == 0 || errno != EPERM)
        errx(1, "setuid(0) after drop: want EPERM, got %ld/%d", r, errno);
    r = syscall(SYS_setreuid, 0, 0);
    if (r == 0 || errno != EPERM)
        errx(1, "setreuid(0) after drop: want EPERM, got %ld/%d", r, errno);
    /* an arbitrary other uid is rejected too; one from the own set is accepted */
    r = syscall(SYS_setresuid, UID_B, UID_B, UID_B);
    if (r == 0 || errno != EPERM)
        errx(1, "setresuid(other) after drop: want EPERM, got %ld/%d", r, errno);
    check("setresuid(-1,own,-1)", syscall(SYS_setresuid, -1, UID_A, -1), 0);
    /* setfsuid of a foreign id is refused silently and returns the current fsuid */
    check("setfsuid(foreign)", syscall(SYS_setfsuid, 1234), UID_A);

    /* 8. the permission check uses the filesystem IDs: 0600 root-owned file is denied,
     *    the world-readable one is readable but not writable */
    r = open(froot, O_RDONLY);
    if (r != -1 || errno != EACCES)
        errx(1, "open(root_only): want EACCES, got %ld/%d", r, errno);
    r = open(fworld, O_RDONLY);
    if (r < 0)
        err(1, "open(world)");
    close((int)r);
    r = open(fworld, O_WRONLY);
    if (r != -1 || errno != EACCES)
        errx(1, "open(world,write): want EACCES, got %ld/%d", r, errno);

    /* as the owner of the directory we may unlink the root-owned files */
    if (unlink(froot))
        err(1, "unlink root_only");
    if (unlink(fworld))
        err(1, "unlink world");
    if (rmdir(dir))
        err(1, "rmdir creds");

    puts("TEST OK");
    return 0;
}
