#include "pal.h"
#include "pal_regression.h"

#ifdef __x86_64__
#define SYMBOL_ADDR(sym)                                                    \
    ({                                                                      \
        void* _sym;                                                         \
        __asm__ volatile("movq " #sym "@GOTPCREL(%%rip), %0" : "=r"(_sym)); \
        _sym;                                                               \
    })
#elif defined(__aarch64__)
/* Same GOT-indirect pattern as libos/src/arch/aarch64/syscallas.S's g_pal_tcb_singleton lookup:
 * every one of these Pal* symbols is defined in a different shared object (libpal.so) from this
 * test binary, so a direct adrp/:lo12: pair (link-time-fixed offset, same-object only) can't
 * reach it -- unlike that lookup, this wants a *function's* address, which the GOT slot holds
 * directly, so no extra dereference is needed here (contrast with g_pal_tcb_singleton, a data
 * symbol, where the GOT slot holds the data's address and a further load gets its value). */
#define SYMBOL_ADDR(sym)                                              \
    ({                                                                \
        void* _sym;                                                  \
        __asm__ volatile(                                            \
            "adrp %0, :got:" #sym "\n"                                \
            "ldr  %0, [%0, :got_lo12:" #sym "]\n"                     \
            : "=r"(_sym));                                            \
        _sym;                                                        \
    })
#else
#error "Unsupported architecture"
#endif

#define PRINT_SYMBOL(sym) pal_printf("symbol: %s = %p\n", #sym, SYMBOL_ADDR(sym))

int main(int argc, char** argv, char** envp) {
    PRINT_SYMBOL(PalVirtualMemoryAlloc);
    PRINT_SYMBOL(PalVirtualMemoryFree);
    PRINT_SYMBOL(PalVirtualMemoryProtect);
    PRINT_SYMBOL(PalSetMemoryBookkeepingUpcalls);

    PRINT_SYMBOL(PalProcessCreate);
    PRINT_SYMBOL(PalProcessExit);

    PRINT_SYMBOL(PalStreamOpen);
    PRINT_SYMBOL(PalStreamWaitForClient);
    PRINT_SYMBOL(PalStreamRead);
    PRINT_SYMBOL(PalStreamWrite);
    PRINT_SYMBOL(PalStreamDelete);
    PRINT_SYMBOL(PalStreamMap);
    PRINT_SYMBOL(PalStreamSetLength);
    PRINT_SYMBOL(PalStreamFlush);
    PRINT_SYMBOL(PalSendHandle);
    PRINT_SYMBOL(PalReceiveHandle);
    PRINT_SYMBOL(PalStreamAttributesQuery);
    PRINT_SYMBOL(PalStreamAttributesQueryByHandle);
    PRINT_SYMBOL(PalStreamAttributesSetByHandle);
    PRINT_SYMBOL(PalStreamChangeName);
    PRINT_SYMBOL(PalStreamsWaitEvents);

    PRINT_SYMBOL(PalThreadCreate);
    PRINT_SYMBOL(PalThreadYieldExecution);
    PRINT_SYMBOL(PalThreadExit);
    PRINT_SYMBOL(PalThreadResume);

    PRINT_SYMBOL(PalSetExceptionHandler);

    PRINT_SYMBOL(PalEventCreate);
    PRINT_SYMBOL(PalEventSet);
    PRINT_SYMBOL(PalEventClear);
    PRINT_SYMBOL(PalEventWait);

    PRINT_SYMBOL(PalObjectDestroy);

    PRINT_SYMBOL(PalSystemTimeQuery);
    PRINT_SYMBOL(PalRandomBitsRead);
#if defined(__x86_64__)
    PRINT_SYMBOL(PalSegmentBaseGet);
    PRINT_SYMBOL(PalSegmentBaseSet);
#endif

    return 0;
}
