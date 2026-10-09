/* Copyright (C) 2023 Gramine contributors
 * SPDX-License-Identifier: BSD-3-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void) {
    printf("Hello, world\n");
    volatile void* data = mmap(NULL, 512*1024*1024, 
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed, returning...\n");
        perror("mmap DATA_REGION");
        return 1;
    }
    return 0;
}
