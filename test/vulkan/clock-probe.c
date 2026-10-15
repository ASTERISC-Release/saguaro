/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Prints CLOCK_MONOTONIC, CLOCK_BOOTTIME and CLOCK_REALTIME (seconds), to compare a guest's clocks
 * with the host's (Documentation/devel/vulkan/, blocker 4: DRM waits take absolute
 * CLOCK_MONOTONIC deadlines). */
#include <stdio.h>
#include <time.h>
static double t(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
int main(void) {
    printf("CLOCKS monotonic %.6f boottime %.6f realtime %.6f\n", t(CLOCK_MONOTONIC),
           t(CLOCK_BOOTTIME), t(CLOCK_REALTIME));
    return 0;
}
