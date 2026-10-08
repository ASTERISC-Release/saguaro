#include <asm/fcntl.h>
#include <linux/fcntl.h> /* AT_FDCWD */

#include "api.h"
#include "linux_utils.h"
#include "syscall.h"

/* In theory we could get symlink length using `lstat`, but that does not work on `/proc/self/exe`
 * (because it's not really a symlink). */
#define BUF_SIZE 1023u

char* get_main_exec_path(void) {
    char* buf = malloc(BUF_SIZE + 1);
    if (!buf) {
        return NULL;
    }

    /* `readlink` has no aarch64 syscall number (aarch64's generic ABI only has the *at() forms);
     * `readlinkat(AT_FDCWD, ...)` is the direct, universally-portable equivalent and works
     * identically on x86_64. */
    ssize_t len = DO_SYSCALL(readlinkat, AT_FDCWD, "/proc/self/exe", buf, BUF_SIZE);
    if (len < 0) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';

    return buf;
}
