/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/*
 * Entry of the vbs PAL proper: an AArch64 Linux ELF packed into the signed VBS enclave image and
 * entered by the Windows enclave runtime (W) once W has prepared it (relocated it in place with the
 * packed-object loader, pal/src/host/vbs/enclave/pal_vbs_packed.c). The single argument is the
 * table of W services (struct pal_vbs_services). This file does what the Linux PAL's
 * pal_linux_main() does, with W's services instead of Linux system calls: TCB, memory windows,
 * manifest (a packed file), topology, then pal_main().
 */

#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_internal.h"
#include "pal_rtld.h"
#include "pal_vbs.h"
#include "toml.h"
#include "toml_utils.h"

const struct pal_vbs_services* g_vbs;

const size_t g_page_size = PRESET_PAGESIZE;

/* the first thread's TCB and handle (one enclave thread for now) */
typedef struct pal_vbs_tcb {
    PAL_TCB common;
    PAL_HANDLE handle;
} PAL_VBS_TCB;

static PAL_VBS_TCB g_first_tcb;

noreturn void vbs_exit(int64_t code) {
    pal_vbs_wcall(g_vbs->exit, (uint64_t)code, 0, 0);
    __builtin_unreachable();
}

static void early_write(const char* s) {
    vbs_console_write(s, strlen(s));
}

static void call_init_array(void) {
    extern void (*__init_array_start)(int, char**, char**) __attribute__((visibility("hidden")));
    extern void (*__init_array_end)(int, char**, char**) __attribute__((visibility("hidden")));
    for (void (**f)(int, char**, char**) = &__init_array_start; f < &__init_array_end; f++)
        (*f)(0, NULL, NULL);
}

static int get_topology_info(struct pal_topo_info* topo_info) {
    /* No host /sys inside the enclave: a synthetic single-node topology with one CPU per enclave
     * thread, as the vm PAL does (pal/src/host/vm-common/arch/aarch64/pal_common_misc.c). */
#define VBS_MAX_CPUS 64
    static struct pal_cache_info caches[1];
    static struct pal_cpu_thread_info threads[VBS_MAX_CPUS];
    static struct pal_cpu_core_info cores[VBS_MAX_CPUS];
    static struct pal_socket_info sockets[1] = { { .unused = 0 } };
    static struct pal_numa_node_info numa_nodes[1] = {
        { .is_online = true, .nr_hugepages = {0, 0} },
    };
    static size_t distances[1] = { 10 };

    size_t n = g_vbs->thread_count ? g_vbs->thread_count : 1;
    if (n > VBS_MAX_CPUS)
        n = VBS_MAX_CPUS;
    caches[0].type  = CACHE_TYPE_UNIFIED;
    caches[0].level = 2;
    caches[0].size  = 1024 * 1024;
    caches[0].coherency_line_size     = 64;
    caches[0].number_of_sets          = 1024;
    caches[0].physical_line_partition = 1;
    for (size_t i = 0; i < n; i++) {
        threads[i].is_online = true;
        threads[i].core_id = i;
        for (size_t c = 0; c < MAX_CACHES; c++)
            threads[i].ids_of_caches[c] = 0;
        cores[i].socket_id = 0;
        cores[i].node_id = 0;
    }
    topo_info->caches = caches;
    topo_info->threads = threads;
    topo_info->cores = cores;
    topo_info->sockets = sockets;
    topo_info->numa_nodes = numa_nodes;
    topo_info->numa_distance_matrix = distances;
    topo_info->caches_cnt = 1;
    topo_info->threads_cnt = n;
    topo_info->cores_cnt = n;
    topo_info->sockets_cnt = 1;
    topo_info->numa_nodes_cnt = 1;
    return 0;
}

/* reads the packed manifest (the packed file named "manifest") into a new C string */
static char* read_manifest(void) {
    for (uint64_t i = 0; i < g_vbs->nobjs; i++) {
        const struct pal_vbs_packed_obj* o = &g_vbs->objs[i];
        if (strcmp(o->name, "manifest"))
            continue;
        char* s = malloc(o->file_size + 1);
        if (!s)
            return NULL;
        memcpy(s, vbs_file_data(o), o->file_size);
        s[o->file_size] = '\0';
        return s;
    }
    return NULL;
}

__attribute_no_stack_protector
noreturn void pal_vbs_start(const struct pal_vbs_services* svc);
__attribute_no_stack_protector
noreturn void pal_vbs_start(const struct pal_vbs_services* svc) {
    int ret;

    g_vbs = svc;
    if (svc->version != PAL_VBS_SERVICES_VERSION) {
        early_write("[vbs-pal] W services version mismatch\n");
        vbs_exit(PAL_ERROR_DENIED);
    }

    /* A TCB with a default canary before anything that may use the stack protector. */
    g_first_tcb.common.self = &g_first_tcb.common;
    pal_tcb_arch_set_stack_canary(&g_first_tcb.common, STACK_PROTECTOR_CANARY_DEFAULT);
    pal_set_tcb(&g_first_tcb.common);
    g_pal_tcb_singleton = &g_first_tcb.common;

    /* W relocated this object already (pal_vbs_packed.c); only record its symbols. */
    ret = setup_pal_binary(/*apply_relocations=*/false);
    if (ret < 0) {
        early_write("[vbs-pal] setup_pal_binary failed\n");
        vbs_exit(PAL_ERROR_DENIED);
    }

    call_init_array();

    g_pal_public_state.alloc_align = g_page_size;
    ret = vbs_init_memory();
    if (ret < 0)
        INIT_FAIL("vbs_init_memory failed: %s", pal_strerror(ret));

    uint64_t canary;
    if (vbs_random(&canary, sizeof(canary)) == 0)
        pal_tcb_arch_set_stack_canary(&g_first_tcb.common, canary);

    set_pal_binary_name("libpal");
    init_slab_mgr();

    ret = get_topology_info(&g_pal_public_state.topo_info);
    if (ret < 0)
        INIT_FAIL("get_topology_info() failed: %s", pal_strerror(ret));

    PAL_HANDLE first_thread = calloc(1, HANDLE_SIZE(thread));
    if (!first_thread)
        INIT_FAIL("Out of memory");
    init_handle_hdr(first_thread, PAL_TYPE_THREAD);
    first_thread->thread.tid = 1;
    g_first_tcb.handle = first_thread;

    char* manifest = read_manifest();
    if (!manifest)
        INIT_FAIL("no packed manifest (a packed file named \"manifest\") in the image");
    g_pal_common_state.raw_manifest_data = manifest;

    char errbuf[256];
    g_pal_public_state.manifest_root = toml_parse(manifest, errbuf, sizeof(errbuf));
    if (!g_pal_public_state.manifest_root)
        INIT_FAIL_MANIFEST(errbuf);

    g_pal_public_state.vbs_packed_lookup = vbs_packed_lookup;

    /* No host command line: pal_main() takes loader.argv from the manifest, or the entrypoint. */
    static const char* argv[] = { NULL };
    static const char* envp[] = { NULL };
    pal_main(/*instance_id=*/0, /*parent_process=*/NULL, first_thread, argv, envp,
             /*post_callback=*/NULL);
}
