/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * The host<->enclave ABI boundary of the Windows VBS-enclave PAL (phase 1 skeleton).
 *
 * This header is shared by the enclave side (compiled for aarch64-pc-windows-msvc) and the host
 * side (a Windows ARM64 executable). It defines the two call tables that cross the boundary:
 *
 *   ECALLs  host -> enclave:  the host calls an exported enclave routine with CallEnclave().
 *                             Phase 1 has a single bootstrap ECALL, E_PalMain (see the .def).
 *   OCALLs  enclave -> host:  the enclave calls the host's single `ocall` dispatcher (a host
 *                             function pointer passed in through the shared struct) with
 *                             CallEnclave() from inside the enclave, demultiplexed by an OCALL
 *                             number. This mirrors the SGX PAL's OCALL table.
 *
 * Boundary conventions (decided in Documentation/devel/vbs-enclave/PLAN.md, D4; measured in
 * phase 0):
 *   - EVERY value that crosses the boundary is a fixed-width type (uint64_t / int64_t / pointers).
 *     Never `long`: Windows ARM64 is LLP64 (long = 32 bits), Linux AArch64 is LP64 (long = 64).
 *   - x18 is the enclave TEB and must be preserved across any call into Linux System V/AAPCS64
 *     code and back (phase 2 glibc/LibOS). The enclave boundary thunk saves/restores it.
 *   - The host cannot read enclave memory (phase 0), so anything the host must see is copied OUT
 *     of the enclave into the host-owned shared struct with EnclaveCopyOutOfEnclave() first.
 */

#ifndef PAL_VBS_ABI_H
#define PAL_VBS_ABI_H

#include <stdint.h>

/* OCALL numbers (enclave -> host). Keep in sync on both sides. */
enum pal_vbs_ocall {
    PAL_VBS_OCALL_CONSOLE_WRITE = 0, /* a0 = byte count already placed in the calling thread's
                                        console buffer, a1 = that buffer (host memory) */
    PAL_VBS_OCALL_GET_TIME_NS   = 1, /* returns Unix time in nanoseconds (host wall clock) */
    PAL_VBS_OCALL_EXIT          = 2, /* a0 = exit code; records it, returns (host tears the enclave down) */
    PAL_VBS_OCALL_THREAD_CREATE = 3, /* a0 = token: start a host thread that enters E_ThreadStart */
    PAL_VBS_OCALL_EXIT_PROCESS  = 4, /* a0 = exit code: end the whole process now (exit from a
                                        thread other than the first one) */
    PAL_VBS_OCALL_HOST          = 5, /* a request in the calling thread's I/O buffer
                                        (struct pal_vbs_hostreq): files and sockets */
    PAL_VBS_OCALL_MAX
};

#define PAL_VBS_CONBUF_SIZE 4096u /* host-owned bounce buffer for console OCALLs */
#define PAL_VBS_IOBUF_SIZE  (64u * 1024u) /* host-owned buffer of host requests, per thread */
#define PAL_VBS_ARGS_SIZE   4096u          /* host command-line arguments for the application */

/*
 * Host requests (PAL_VBS_OCALL_HOST): host files and host sockets, served by the untrusted host
 * with Win32 file and Winsock calls. The vbs PAL builds a request (header + input bytes), W copies
 * it into the calling thread's I/O buffer in host memory, the host replaces the header (result,
 * output length) and puts its output bytes after it, and W copies them back. Values follow Linux:
 * open flags, socket domains (AF_INET 2, AF_INET6 10), struct sockaddr layouts, and negative Linux
 * errno values as results. Paths are absolute Linux-style paths that the host maps below its host
 * root directory. Nothing the host returns is trusted by the PAL beyond its declared lengths.
 */
enum pal_vbs_host_op {
    PAL_VBS_HOST_OPEN = 1,      /* in: path; a: flags (O_*), mode -> id */
    PAL_VBS_HOST_PREAD,         /* a: id, offset, len -> bytes read (out) */
    PAL_VBS_HOST_PWRITE,        /* in: bytes; a: id, offset -> bytes written */
    PAL_VBS_HOST_FSTAT,         /* a: id -> out: struct pal_vbs_host_stat */
    PAL_VBS_HOST_STAT,          /* in: path -> out: struct pal_vbs_host_stat */
    PAL_VBS_HOST_CLOSE,         /* a: id (file or socket) */
    PAL_VBS_HOST_FTRUNCATE,     /* a: id, length */
    PAL_VBS_HOST_UNLINK,        /* in: path; a: is_dir */
    PAL_VBS_HOST_RENAME,        /* in: old path '\0' new path */
    PAL_VBS_HOST_MKDIR,         /* in: path; a: mode */
    PAL_VBS_HOST_LISTDIR,       /* in: path; a: first index -> out: names '\0' (dirs end in '/'),
                                   result: number of names */
    PAL_VBS_HOST_SOCKET = 32,   /* a: domain, type (SOCK_STREAM 1 / SOCK_DGRAM 2), nonblocking */
    PAL_VBS_HOST_BIND,          /* in: sockaddr; a: id -> out: bound sockaddr */
    PAL_VBS_HOST_LISTEN,        /* a: id, backlog */
    PAL_VBS_HOST_ACCEPT,        /* a: id, nonblocking -> id; out: peer sockaddr */
    PAL_VBS_HOST_CONNECT,       /* in: sockaddr; a: id -> 0 or -EINPROGRESS; out: local sockaddr */
    PAL_VBS_HOST_SEND,          /* in: [sockaddr_len u32][sockaddr][bytes]; a: id, dontwait */
    PAL_VBS_HOST_RECV,          /* a: id, len, dontwait -> out: [sockaddr 128][bytes]; result:
                                   bytes received */
    PAL_VBS_HOST_SHUTDOWN,      /* a: id, how */
    PAL_VBS_HOST_SETNONBLOCK,   /* a: id, on */
    PAL_VBS_HOST_SETSOCKOPT,    /* a: id, level, name, value (Linux constants) */
    PAL_VBS_HOST_GETSOCKNAME,   /* a: id -> out: sockaddr */
    PAL_VBS_HOST_FIONREAD,      /* a: id -> bytes pending */
    PAL_VBS_HOST_POLL,          /* in: struct pal_vbs_host_pollfd[n]; a: n, timeout_ms ->
                                   out: the same array with revents; result: ready count */
};

#define PAL_VBS_HOST_SOCKADDR_MAX 128u

struct pal_vbs_hostreq {
    uint64_t op;         /* enum pal_vbs_host_op */
    int64_t  a[6];       /* arguments */
    int64_t  result;     /* written by the host */
    uint64_t in_len;     /* bytes after the header, from the enclave */
    uint64_t out_len;    /* bytes after the header, from the host */
    /* followed by the input or output bytes */
};

#define PAL_VBS_HOST_DATA_MAX (PAL_VBS_IOBUF_SIZE - sizeof(struct pal_vbs_hostreq))

struct pal_vbs_host_stat {
    uint64_t size;
    uint64_t is_dir;
    uint64_t mode;
};

struct pal_vbs_host_pollfd {
    int64_t id;
    uint32_t events;     /* Linux POLLIN 1, POLLOUT 4 */
    uint32_t revents;    /* POLLIN, POLLOUT, POLLERR 8, POLLHUP 16, POLLNVAL 32 */
};

/*
 * The host OCALL dispatcher. The host passes its address into the enclave in `pal_vbs_shared.ocall`.
 * The enclave invokes it with CallEnclave(ocall, &shared->ocall_args, TRUE, &ret). All fields are
 * fixed-width. The dispatcher runs on the host thread that made the originating ECALL.
 */
typedef struct pal_vbs_ocall_args {
    uint64_t ocall_no; /* one of enum pal_vbs_ocall */
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t retval;   /* filled by the host */
} pal_vbs_ocall_args_t;

/*
 * The bootstrap struct. It lives in HOST memory; the host passes its address as the single
 * parameter of the E_PalMain ECALL. The enclave copies it in with EnclaveCopyIntoEnclave(), uses
 * `ocall` for all host services, and copies console bytes out into `conbuf` before each console
 * OCALL. `result`/`log_len` are written back on exit.
 */
typedef struct pal_vbs_shared {
    uint64_t abi_version;          /* PAL_VBS_ABI_VERSION; host and enclave must agree */
    uint64_t thread_count;         /* N passed to InitializeEnclave (ENCLAVE_INIT_INFO_VBS) */
    uint64_t enclave_base;         /* host's view of the enclave base (for logging) */
    uint64_t enclave_size;         /* image-configured size */

    /* OCALL path */
    void*    ocall;                /* host dispatcher, type pal_vbs_ocall_fn */
    pal_vbs_ocall_args_t ocall_args;
    uint8_t  conbuf[PAL_VBS_CONBUF_SIZE];
    uint8_t  iobuf[PAL_VBS_IOBUF_SIZE];

    /* what E_PalMain should do (PAL_VBS_MODE_*) and the arguments of the application (boot) */
    uint64_t mode;
    uint64_t args_len;             /* bytes of args: NUL-terminated strings, one after another */
    char     args[PAL_VBS_ARGS_SIZE]; /* host command-line arguments for the application (used
                                     * only with loader.insecure__use_cmdline_argv, untrusted) */

    /* results written back by the enclave before returning from E_PalMain */
    int64_t  result;               /* 0 = all self-tests passed, else first failing step */
    uint64_t selftests_run;
    uint64_t selftests_passed;
} pal_vbs_shared_t;

typedef uint64_t (*pal_vbs_ocall_fn)(pal_vbs_ocall_args_t* args);

/*
 * Per-thread block of a thread the host started for the enclave (PAL_VBS_OCALL_THREAD_CREATE). It
 * lives in HOST memory and is the parameter of the E_ThreadStart ECALL; the thread's OCALLs and
 * console bytes go through it as the first thread's go through pal_vbs_shared.
 */
typedef struct pal_vbs_thread_block {
    uint64_t token;      /* from the enclave's THREAD_CREATE request; checked by the enclave */
    void*    ocall;      /* host dispatcher, as pal_vbs_shared.ocall */
    pal_vbs_ocall_args_t ocall_args;
    uint8_t  conbuf[PAL_VBS_CONBUF_SIZE];
    uint8_t  iobuf[PAL_VBS_IOBUF_SIZE];
} pal_vbs_thread_block_t;

#define PAL_VBS_ABI_VERSION 5u

#define PAL_VBS_MODE_SELFTEST 0u /* run the PAL-level self-test (phases 1 and 2) */
#define PAL_VBS_MODE_BOOT     1u /* boot the packed vbs PAL, the LibOS and the application */
#define PAL_VBS_MODE_TPIDR    2u /* measurement: does TPIDR_EL0 survive ~30 s of spinning? */
#define PAL_VBS_MODE_TEBSLOT  3u /* measurement: does NT_TIB.ArbitraryUserPointer survive? */

/*
 * The table of objects packed into the image at build time (generated by scripts/vbs/pack.py into
 * pal_vbs_packed_table.h). It is read by the Windows enclave runtime (LLP64) and by the vbs PAL
 * proper, a Linux ELF (LP64), so it uses fixed-width fields and pointers only.
 */
enum pal_vbs_packed_kind {
    PAL_VBS_KIND_FILE = 0, /* data only: the original file bytes (for example the manifest) */
    PAL_VBS_KIND_PAL  = 1, /* an ELF object loaded by the PAL side (libpal, libsysdb) */
    PAL_VBS_KIND_USER = 2, /* an ELF object mapped by the LibOS (the application, the vDSO) */
};

struct pal_vbs_packed_obj {
    const char* name;       /* short name ("libpal", "libsysdb", "app", ...) */
    const char* path;       /* absolute path in the enclave's file namespace, or "" */
    uint64_t kind;          /* enum pal_vbs_packed_kind */
    uint64_t file_offset;   /* original bytes of the file: offset in the packed files data */
    uint64_t file_size;
    uint64_t image_offset;  /* load image: offset in the packed images section (.gxpack) */
    uint64_t image_size;    /* 0 for data-only files; includes the svc stubs and slot page */
    uint64_t map_size;      /* end of the last PT_LOAD, page-rounded: the extent a loader maps */
    uint64_t entry_vaddr;   /* entry symbol chosen at pack time (or e_entry), 0 if none */
    uint64_t dynamic_vaddr;
    uint64_t slot_offset;   /* offset in the image of the 8-byte syscall-entry slot, 0 if none */
    uint64_t nsvc;          /* number of `svc #0` rewritten into stubs at pack time */
};

/*
 * Services of the Windows enclave runtime (W) handed to the vbs PAL proper (P, a packed Linux ELF)
 * as the single argument of its entry `pal_vbs_start`. The function pointers are Windows ARM64
 * code: P calls them only through its x18-restoring thunk (pal_vbs_wcall.S), because Linux code
 * may have clobbered x18 (the enclave TEB). All values are fixed-width (PLAN.md D4).
 */
#define PAL_VBS_SERVICES_VERSION 4u

struct pal_vbs_services {
    uint64_t version;
    uint64_t enclave_base, enclave_size; /* the whole enclave address range */
    uint64_t image_base, image_size;     /* the signed enclave image */
    uint64_t pack_base, pack_size;       /* packed load images (.gxpack, executable) */
    uint64_t files_base, files_size;     /* packed file bytes (.gxfile, read-only data) */
    uint64_t pool_base, pool_size;       /* read-write memory committed by W for the PAL */
    const struct pal_vbs_packed_obj* objs;
    uint64_t nobjs;
    uint64_t teb;                        /* x18 of the entering enclave thread */
    uint64_t thread_count;
    uint64_t self_base;                  /* load base of P itself (already relocated by W) */

    void* console_write; /* void    (const char* buf, uint64_t len) */
    void* time_ns;       /* uint64_t (void): Unix time in ns (host OCALL) */
    void* random;        /* int64_t (void* buf, uint64_t len): 0 on success */
    void* protect;       /* int64_t (uint64_t addr, uint64_t size, uint64_t pal_prot): 0 or
                          * -(Windows error); refuses any execute permission */
    void* exit;          /* noreturn void (int64_t code): ends the process with this exit code */

    /* threads (version 2) */
    void* thread_create; /* int64_t (void* entry, void* arg): a new enclave thread runs
                          * entry(arg, ctx) in the packed code; 0 or negative */
    void* thread_exit;   /* noreturn void (void* unused, int32_t* clear_tid): ends the calling
                          * thread (W finds it by its TEB; the first thread is parked); after it
                          * left its stack, *clear_tid = 0 and its waiters are woken */
    void* wait;          /* int64_t (uint32_t* addr, uint32_t expected, uint64_t timeout_ns):
                          * WaitOnAddress; 0 woken or changed, -1 timed out; UINT64_MAX = forever */
    void* wake;          /* void (uint32_t* addr, uint64_t all) */

    /* host requests (version 3) */
    const char* args;    /* host command-line arguments (copied into the enclave), untrusted */
    uint64_t args_len;
    void* host_call;     /* int64_t (struct pal_vbs_hostreq* req, void* out, uint64_t out_cap):
                          * req (header + in_len input bytes) goes to the host; the result is
                          * returned, req->out_len set, min(out_len, out_cap) bytes copied to out */
};

/*
 * A synchronous exception in packed code, handed by W's exception handler (the language-specific
 * handler named by the packed code's static unwind data) to the vbs PAL's `pal_vbs_exception`. The
 * PAL may change the registers (to deliver a signal, or to restore a saved context); W then
 * resumes the thread with them. Fixed-width fields only (D4).
 */
enum pal_vbs_fault_kind {
    PAL_VBS_FAULT_MEMORY     = 1, /* access violation, misalignment, in-page error */
    PAL_VBS_FAULT_ILLEGAL    = 2, /* illegal/privileged instruction, breakpoint (brk) */
    PAL_VBS_FAULT_ARITHMETIC = 3, /* floating-point/integer arithmetic exception */
};

struct pal_vbs_fault {
    uint64_t kind;        /* enum pal_vbs_fault_kind */
    uint64_t code;        /* the Windows exception code */
    uint64_t addr;        /* faulting data address (memory faults), else the PC */
    uint64_t access;      /* memory faults: 0 read, 1 write, 8 execute */
    uint64_t regs[31];    /* x0..x30 */
    uint64_t sp, pc, pstate;
    uint32_t fpcr, fpsr;
    uint8_t  vregs[32][16];
};

/* PAL protection bits as passed to `protect` (the values of pal_prot_flags_t) */
#define PAL_VBS_PROT_READ  0x1u
#define PAL_VBS_PROT_WRITE 0x2u
#define PAL_VBS_PROT_EXEC  0x4u

#endif /* PAL_VBS_ABI_H */
