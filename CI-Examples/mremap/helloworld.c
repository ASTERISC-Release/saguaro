#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf jump_env;

static void segfault_handler(int sig) {
    siglongjmp(jump_env, 1);
}

int main() {
    size_t page_size     = getpagesize();
    size_t total_reserve = page_size * 4;
    size_t old_size      = page_size;
    size_t mid_size      = page_size * 2;
    size_t final_size    = page_size;

    printf("--- Stage 1: Grow In-Place ---\n");

    // 1. Create a large "hole" in memory first
    void* reserve = mmap(NULL, total_reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve == MAP_FAILED) {
        perror("Initial reservation failed");
        return 1;
    }
    munmap(reserve, total_reserve);

    // 2. Map only the FIRST page at that specific address
    void* addr = mmap(reserve, old_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (addr == MAP_FAILED) {
        perror("Fixed mmap failed");
        return 1;
    }
    printf("1. Map created at %p\n", addr);

    memset(addr, 0xAA, old_size);
    printf("2. Filled with 0xAA\n");

    // 3. Grow in-place
    printf("3. Calling mremap(addr=%p, old=%zu, new=%zu, flags=0)\n", addr, old_size, mid_size);
    void* mid_addr = mremap(addr, old_size, mid_size, 0);
    if (mid_addr == MAP_FAILED) {
        perror("mremap (grow) failed");
        return 1;
    }

    if (mid_addr != addr) {
        printf("FAIL: Address moved unexpectedly\n");
        return 1;
    }
    printf("4. Successfully grew to %zu bytes at %p\n", mid_size, mid_addr);

    // Verify data is still there
    if (((unsigned char*)mid_addr)[0] != 0xAA) {
        printf("FAIL: Data corrupted after grow\n");
        return 1;
    }

    printf("\n--- Stage 2: Shrink In-Place ---\n");

    // 4. Fill the new 8KB region with a distinct pattern
    memset(mid_addr, 0xBB, mid_size);
    printf("5. Filled 8KB with 0xBB\n");

    // 5. Shrink back down to 4KB
    printf("6. Calling mremap(addr=%p, old=%zu, new=%zu, flags=0)\n", mid_addr, mid_size,
           final_size);
    void* final_addr = mremap(mid_addr, mid_size, final_size, 0);
    if (final_addr == MAP_FAILED) {
        perror("mremap (shrink) failed");
        return 1;
    }

    if (final_addr != mid_addr) {
        printf("FAIL: Address moved during shrink\n");
        return 1;
    }
    printf("7. Successfully shrunk to %zu bytes at %p\n", final_size, final_addr);

    // 6. Verify data validity in the remaining 4KB
    if (((unsigned char*)final_addr)[0] != 0xBB ||
        ((unsigned char*)final_addr)[final_size - 1] != 0xBB) {
        printf("FAIL: Data corrupted after shrink\n");
        return 1;
    }
    printf("8. Data in remaining region is valid (0xBB)\n");

    // 7. Verify the shrunk region is actually unmapped (Catch SIGSEGV)
    printf("9. Verifying shrunk region is unmapped (testing address %p)...\n",
           (char*)final_addr + final_size);
    fflush(stdout);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = segfault_handler;
    sigaction(SIGSEGV, &sa, NULL);

    if (sigsetjmp(jump_env, 1) == 0) {
        // This access should trigger SIGSEGV
        unsigned char val = *((unsigned char*)final_addr + final_size);
        printf("FAIL: Shrunk tail is still accessible! Read value: 0x%02x\n", val);
        return 1;
    } else {
        printf("10. Verified: Caught SIGSEGV as expected. Shrunk tail is unmapped.\n");
    }

    // Restore default handler for safety
    sa.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &sa, NULL);

    munmap(final_addr, final_size);
    printf("\n=== ALL IN-PLACE TESTS PASSED ===\n");
    return 0;
}
