/* Copyright (C) 2023 Gramine contributors
 * SPDX-License-Identifier: BSD-3-Clause */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PTRS_ARRAY_SIZE 512
const size_t PAGE_SIZE = 4096;

static void* COMMS_REGION         = (void*)0xf00000ULL;
static void* DATA_REGION          = (void*)0x100008000ULL;
const size_t DATA_SIZE            = 1024 * 1024 * 512;      // 512MB

void* hostAllocations[PTRS_ARRAY_SIZE] = {0};

int main(void) {
    /*
     * DATA and COMMS setup code
     */
    // Map data region
    void* data = mmap(DATA_REGION, DATA_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (data == MAP_FAILED) {
        perror("mmap DATA_REGION");
        return 1;
    }

    // Map comms region in
    void* comms = mmap(COMMS_REGION, PAGE_SIZE * 2, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (comms == MAP_FAILED) {
        perror("mmap COMMS_REGION");
        return 1;
    }
    // Signal ready to host thread
    *(volatile uint64_t*)comms = 0x1234567812345678ULL;
    /*
     * ---------------------------------------------------------
     * ---------------------------------------------------------
     */

    // We're just using this as a flag capture this inside libos_mmap.c
    size_t special_allocation = 0x123456789ULL;

    // Each of these allocations gets captured in gramine-tdx/libos/src/sys/libos_mmap.c
    for (int i = 0; i < PTRS_ARRAY_SIZE; i++) {
        // Note that MAP_ANONYMOUS gets xored off inside qemu/util/mmap-alloc.c
        hostAllocations[i] =
            mmap(NULL, special_allocation, PROT_WRITE | PROT_READ, MAP_ANONYMOUS, 0, 0);
        printf("%s\n", (char*)hostAllocations[i]);
    }
}
