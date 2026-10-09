// libos/src/sys/libos_prctl.c
#include "libos_internal.h"
#include "libos_table.h"

// not implemented: return success
long libos_syscall_prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4,
                         unsigned long arg5) {
    __UNUSED(option);
    __UNUSED(arg2);
    __UNUSED(arg3);
    __UNUSED(arg4);
    __UNUSED(arg5);
    return 0;
}
