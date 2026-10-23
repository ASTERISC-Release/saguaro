/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/*
 * vk-session-shim: a phase-0 test aid for Vulkan under saguaro-vm (dev docs: vulkan/).
 *
 * The graphics preload library (libgraphics.so) starts the guest's graphics session -- the comm-page
 * handshake with the host listener, after which the LibOS forwards opens of /dev/dri and /sys to the
 * host -- at the first GLX or EGL call. A Vulkan program makes none, so without help it never sees
 * the host GPU. This shim interposes vkCreateInstance, starts the session through the preload
 * library's graphics_ensure_session() and then calls the real vkCreateInstance. Preload it before
 * libgraphics.so: LD_PRELOAD = "/vk-session-shim.so:./libgraphics.so" (see shim_init()).
 *
 * It also replaces memfd_create() (VKSHIM_MEMFD=host, the default; =tmp; =0 for the LibOS's own):
 * the LibOS emulates memfd_create with a file under /dev/shm that it unlinks at once, but with the
 * graphics session up every open under /dev goes to the host, and the unlink then trips a LibOS
 * reference-count BUG (libos_dcache.c, seen in phase 0). "host" opens the file under /dev/shm the
 * same way (so the host listener opens, sizes and maps it: writable shared mappings work) and does
 * not unlink it (the files stay in the host's /dev/shm until removed by hand). "tmp" creates and
 * unlinks the file under /tmp (a host directory in the manifest); writable shared mappings of it
 * are refused by the VM PAL, which is not enough for anv.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static void ensure_session(void) {
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
        return;
    bool (*ensure)(void) = (bool (*)(void))dlsym(RTLD_DEFAULT, "graphics_ensure_session");
    if (ensure)
        fprintf(stderr, "[vk-session-shim] graphics session: %s\n", ensure() ? "ready" : "FAILED");
    else
        fprintf(stderr, "[vk-session-shim] graphics_ensure_session not found (libgraphics.so not preloaded?)\n");
}

/* Programs that load the Vulkan loader with dlopen (vkcube and vulkaninfo use volk-style loading)
 * bypass the interposed entry points below, so the session is also started from this constructor
 * (unless VKSHIM_EAGER=0). It must run after libgraphics.so's constructor, which glibc does when
 * the shim comes first in LD_PRELOAD (constructors run in reverse load order). */
__attribute__((constructor)) static void shim_init(void) {
    const char* eager = getenv("VKSHIM_EAGER");
    if (!eager || strcmp(eager, "0"))
        ensure_session();
}

/* Not dlsym(RTLD_NEXT): inside the saguaro-vm guest it returned the shim's own function again
 * (endless recursion, seen in phase 0); ask the loader library by name instead. */
static void* loader_sym(const char* name, void* self) {
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    void* f = loader ? dlsym(loader, name) : NULL;
    return f == self ? NULL : f;
}

/* The instance-level entry points a program may call before vkCreateInstance (vkcube enumerates
 * the instance extensions first). */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer,
                                                                      uint32_t* count,
                                                                      VkExtensionProperties* props) {
    static PFN_vkEnumerateInstanceExtensionProperties real;
    ensure_session();
    if (!real)
        real = (PFN_vkEnumerateInstanceExtensionProperties)loader_sym(
            "vkEnumerateInstanceExtensionProperties", (void*)vkEnumerateInstanceExtensionProperties);
    return real ? real(layer, count, props) : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count,
                                                                  VkLayerProperties* props) {
    static PFN_vkEnumerateInstanceLayerProperties real;
    ensure_session();
    if (!real)
        real = (PFN_vkEnumerateInstanceLayerProperties)loader_sym(
            "vkEnumerateInstanceLayerProperties", (void*)vkEnumerateInstanceLayerProperties);
    return real ? real(count, props) : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info,
                                                const VkAllocationCallbacks* alloc,
                                                VkInstance* instance) {
    static PFN_vkCreateInstance real;
    ensure_session();
    if (!real)
        real = (PFN_vkCreateInstance)loader_sym("vkCreateInstance", (void*)vkCreateInstance);
    return real ? real(info, alloc, instance) : VK_ERROR_INITIALIZATION_FAILED;
}

int memfd_create(const char* name, unsigned int flags) {
    static unsigned long counter;
    const char* env = getenv("VKSHIM_MEMFD");
    if (env && !strcmp(env, "0")) {
        int (*real)(const char*, unsigned int) =
            (int (*)(const char*, unsigned int))dlsym(RTLD_NEXT, "memfd_create");
        if (!real || real == memfd_create) {
            errno = ENOSYS;
            return -1;
        }
        return real(name, flags);
    }
    bool host = !env || strcmp(env, "tmp");
    char path[64];
    for (int attempt = 0; attempt < 32; attempt++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        snprintf(path, sizeof(path), "%s/.vkshim-memfd-%d-%lx-%lu", host ? "/dev/shm" : "/tmp",
                 getpid(), (unsigned long)ts.tv_nsec ^ (unsigned long)ts.tv_sec,
                 __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
        int fd = open(path, O_RDWR | O_CREAT | O_EXCL | ((flags & MFD_CLOEXEC) ? O_CLOEXEC : 0), 0600);
        if (fd >= 0) {
            if (!host)
                unlink(path);
            return fd;
        }
        if (errno != EEXIST)
            return -1;
    }
    return -1;
}
