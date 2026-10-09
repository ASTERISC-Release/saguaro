// libos/src/sys/libos_getresid.c
#include "libos_internal.h"
#include "libos_table.h"

// not implemented: return success
long libos_syscall_getresuid(uid_t* ruid, uid_t* euid, uid_t* suid) {
    (void)ruid;
    (void)euid;
    (void)suid;
    return 0;
}

// not implemented: return success
long libos_syscall_getresgid(gid_t* rgid, gid_t* egid, gid_t* sgid) {
    (void)rgid;
    (void)egid;
    (void)sgid;
    return 0;
}
