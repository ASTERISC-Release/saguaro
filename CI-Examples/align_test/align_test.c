// /* Copyright (C) 2023 Gramine contributors
//  * SPDX-License-Identifier: BSD-3-Clause */
//
// // #include <stdio.h>
// //
// // const char* dataString = "Hello, world\n";
// //
// // int main(void) {
// //     printf("%p\n", dataString);
// //
// //     // Some lock here to signal qemu thread and await for their print...
// //     printf("%s", dataString);
// //     return 0;
// // }
//
// #include <assert.h>
// #include <stddef.h>
// #include <stdint.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <sys/mman.h>
// #include <unistd.h>
//
// const int PTRS_ARRAY_SIZE = 128;
// const size_t PAGE_SIZE    = 4096;
//
// static void* COMMS_REGION = (void*)0xf00000ULL;
// static void* DATA_REGION  = (void*)0x100008000ULL;
// const size_t DATA_SIZE    = 1024 * 1024 * 512;  // 512MB
//
// // **Not at 0x100000 since it was reserved/mmap failed, but close enough
// static void* VIRTUAL_ADDRESS_LOW = (void*)0xc00000ULL;  // 15MB
//
// static void* VIRTUAL_ADDRESS_HIGH = (void*)0x100000000ULL;  // 4GB
//
// static const uint64_t SET_VALUE = 0xDEADBEEFDEADBEEFULL;
//
// int main(void) {
//     /*
//      * COMMS and DATA setup code
//      */
//     // Map data region
//     void* data = mmap(DATA_REGION, DATA_SIZE, PROT_READ | PROT_WRITE,
//                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
//     if (data == MAP_FAILED) {
//         perror("mmap DATA_REGION");
//         return 1;
//     }
//
//     // Map comms region in
//     void* comms = mmap(COMMS_REGION, PAGE_SIZE * 2, PROT_READ | PROT_WRITE,
//                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
//     if (comms == MAP_FAILED) {
//         perror("mmap COMMS_REGION");
//         return 1;
//     }
//     // Signal ready to host thread
//     *(volatile uint64_t*)comms = 0x1234567812345678ULL;
//     /*
//      * ---------------------------------------------------------
//      * ---------------------------------------------------------
//      */
//
//     void* ptrs[PTRS_ARRAY_SIZE];
//     memset(ptrs, 0, PTRS_ARRAY_SIZE * sizeof(void*));
//
//     // Allocate at floor of each region
//     ptrs[0] = mmap(VIRTUAL_ADDRESS_LOW, 4096, PROT_READ | PROT_WRITE,
//                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
//     if (ptrs[0] == MAP_FAILED) {
//         perror("mmap low failed");
//         ptrs[0] = NULL;
//     }
//
//     ptrs[1] = mmap(VIRTUAL_ADDRESS_HIGH, 4096, PROT_READ | PROT_WRITE,
//                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
//     if (ptrs[1] == MAP_FAILED) {
//         perror("mmap high failed");
//         ptrs[1] = NULL;
//     }
//
//     for (int i = 0; i < 2; i++) {
//         if (!ptrs[i])
//             continue;
//
//         size_t current_size = PAGE_SIZE;
//         uint64_t* start     = (uint64_t*)ptrs[i];
//         uint64_t* end       = (uint64_t*)((uint64_t)ptrs[i] + current_size - sizeof(uint64_t));
//
//         *start = SET_VALUE;
//         *end   = SET_VALUE;
//
//         assert(*start == SET_VALUE);
//         assert(*end == SET_VALUE);
//     }
//
//     // Do some random allocations
//     for (int i = 2; i < PTRS_ARRAY_SIZE; i++) {
//         size_t current_size = i * PAGE_SIZE;
//         ptrs[i]             = malloc(current_size);
//         if (!ptrs[i]) {
//             fprintf(stderr, "[align_test] MALLOC FAILED @ ptrs[%d], when allocating [%zu] bytes",
//             i,
//                     current_size);
//             continue;
//         }
//
//         memset(ptrs[i], 0xAB, current_size);
//
//         // Set value at beggining and end of each allocation
//         uint64_t* start = (uint64_t*)ptrs[i];
//         uint64_t* end   = (uint64_t*)((uint64_t)ptrs[i] + current_size - sizeof(uint64_t));
//         *start          = SET_VALUE;
//         *end            = SET_VALUE;
//
//         assert(*start == SET_VALUE);
//         assert(*end == SET_VALUE);
//     }
//
//     const size_t ONE_MEGABYTE = 1024 * 1024;
//     for (size_t i = 0; i < 1024 * 1024 * 512; i += ONE_MEGABYTE) {
//         printf("i = %luMB - DATA REGION VALUE = (start)%c (end)%c\n", (i / (ONE_MEGABYTE)) + 1,
//                (((char*)DATA_REGION) + i)[0], (((char*)DATA_REGION) + i)[ONE_MEGABYTE - 1]);
//     }
//
//     uint64_t x = 0;
//     for (size_t i = 0; i < ONE_MEGABYTE * 512; i += ONE_MEGABYTE) {
//         for (size_t j = 0; j < ONE_MEGABYTE; j += 4) {
//             if (*(uint32_t*)DATA_REGION == 0x61616161ULL) {   // 0x6161616161616161ULL) {
//                 // printf("passed @ i = %luMB  + j = %luB\n", (i / ONE_MEGABYTE) + 1, j);
//                 // fflush(stdout);
//                 x++;
//             } else {
//                 // printf("failed @ i = %luMB  + j = %luB\n", (i / ONE_MEGABYTE) + 1, j);
//                 // fflush(stdout);
//             }
//         }
//     }
//     printf("x-value = %lu\n", x);
//
//     for(uint64_t i = 0; i < UINT64_MAX; i ++){
//         printf("panic\n");
//     }
//
//     // printf("DATA REGION VALUE = %s\n", ((char*)DATA_REGION) + 1024 * 1024 * 384);
//     // size_t total               = 512ull * 1024 * 1024;  // 512 MB
//     // const uint64_t pat         = 0x6161616161616161ULL;
//     // const size_t STEP          = 8;                    // 64-bit chunks
//     // const size_t PROG_INTERVAL = 64ull * 1024 * 1024;  // 64 MiB
//
//     // size_t off = 0, next_prog = PROG_INTERVAL;
//     // for (; off + STEP <= total; off += STEP) {
//     //     uint64_t v = *(uint64_t*)((unsigned char*)DATA_REGION + off);
//     //     if (v != pat) {
//     //         printf("NO at %zu (got 0x%016llx)\n", off, (unsigned long long)v);
//     //         fflush(stdout);
//     //         return 1;
//     //     }
//     //     if (off >= next_prog) {
//     //         printf("checked %zu / %zu bytes\n", off, total);
//     //         fflush(stdout);
//     //         sleep(4);
//     //         next_prog += PROG_INTERVAL;
//     //     }
//     // }
//     // if (off != total) {
//     //     printf("tail not multiple of 8: %zu bytes\n", total - off);
//     //     fflush(stdout);
//     //     return 1;
//     // }
//     // printf("ALL 0x61\n");
//     // fflush(stdout);
//
//     printf("\n&ptrs[0] = %p\n", ptrs);
//     fflush(stdout);
//
//     sleep(400);
//
//     if (ptrs[0])
//         munmap(ptrs[0], PAGE_SIZE);
//     if (ptrs[1])
//         munmap(ptrs[1], PAGE_SIZE);
//     for (int i = 2; i < PTRS_ARRAY_SIZE; i++) {
//         free(ptrs[i]);
//     }
//     return 0;
// }

/* Copyright (C) 2023 Gramine contributors



 * SPDX-License-Identifier: BSD-3-Clause */

// #include <stdio.h>

//

// const char* dataString = "Hello, world\n";

//

// int main(void) {

//     printf("%p\n", dataString);

//

//     // Some lock here to signal qemu thread and await for their print...

//     printf("%s", dataString);

//     return 0;

// }

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

const int PTRS_ARRAY_SIZE = 128;

const size_t PAGE_SIZE = 4096;

// **Not at 0x100000 since it was reserved/mmap failed, but close enough

static void* VIRTUAL_ADDRESS_LOW = (void*)0xf00000ULL;  // 15MB

static void* VIRTUAL_ADDRESS_HIGH = (void*)0x100000000ULL;  // 4GB

static const uint64_t SET_VALUE = 0xDEADBEEFDEADBEEFULL;

int main(void) {
    void* ptrs[PTRS_ARRAY_SIZE];

    memset(ptrs, 0, PTRS_ARRAY_SIZE * sizeof(void*));

    // Allocate at floor of each region

    ptrs[0] = mmap(VIRTUAL_ADDRESS_LOW, 4096, PROT_READ | PROT_WRITE,

                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (ptrs[0] == MAP_FAILED) {
        perror("mmap low failed");

        ptrs[0] = NULL;
    }

    ptrs[1] = mmap(VIRTUAL_ADDRESS_HIGH, 4096, PROT_READ | PROT_WRITE,

                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (ptrs[1] == MAP_FAILED) {
        perror("mmap high failed");

        ptrs[1] = NULL;
    }

    for (int i = 0; i < 2; i++) {
        if (!ptrs[i])

            continue;

        size_t current_size = PAGE_SIZE;

        uint64_t* start = (uint64_t*)ptrs[i];

        uint64_t* end = (uint64_t*)((uint64_t)ptrs[i] + current_size - sizeof(uint64_t));

        *start = SET_VALUE;

        *end = SET_VALUE;

        assert(*start == SET_VALUE);

        assert(*end == SET_VALUE);
    }

    // Do some random allocations

    for (int i = 2; i < PTRS_ARRAY_SIZE; i++) {
        size_t current_size = i * PAGE_SIZE;

        ptrs[i] = malloc(current_size);

        if (!ptrs[i]) {
            fprintf(stderr, "[align_test] MALLOC FAILED @ ptrs[%d], when allocating [%zu] bytes", i,

                    current_size);

            continue;
        }

        memset(ptrs[i], 0xAB, current_size);

        // Set value at beggining and end of each allocation

        uint64_t* start = (uint64_t*)ptrs[i];

        uint64_t* end = (uint64_t*)((uint64_t)ptrs[i] + current_size - sizeof(uint64_t));

        *start = SET_VALUE;

        *end = SET_VALUE;

        assert(*start == SET_VALUE);

        assert(*end == SET_VALUE);
    }

    printf("\n&ptrs[0] = %p\n", ptrs);

    fflush(stdout);

    sleep(400);

    if (ptrs[0])

        munmap(ptrs[0], PAGE_SIZE);

    if (ptrs[1])

        munmap(ptrs[1], PAGE_SIZE);

    for (int i = 2; i < PTRS_ARRAY_SIZE; i++) {
        free(ptrs[i]);
    }

    return 0;
}
