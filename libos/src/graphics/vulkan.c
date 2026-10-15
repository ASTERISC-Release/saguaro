/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/*
 * Vulkan programs under gramine-vm (Documentation/devel/vulkan/).
 *
 * The graphics session (the comm-page handshake with the host listener, after which the LibOS
 * forwards the opens of /dev/dri and /sys to the host) starts at the first GLX or EGL call. A
 * Vulkan program makes none, so this file interposes the Vulkan loader's instance-level entry
 * points that every program calls first: they start the session and then call the loader's own
 * definition (the next one in the lookup order). Outside the VM (the "process" mode used natively
 * and under gramine-direct) they only forward.
 *
 * Programs that load the Vulkan loader with dlopen() and look up its entry points themselves
 * (volk-style loaders, as in vkcube and vulkaninfo) do not come through here; for them,
 * EGL_EAGER_SESSION=1 in the environment starts the session from the library constructor
 * (main.c).
 *
 * The Vulkan headers are not needed to build this: the types below have the ABI of the real ones
 * on x86_64 Linux (VkResult is a 32-bit enum, handles and structures are passed by pointer).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

#include "common.h"

typedef int32_t vk_result_t;
#define VK_ERROR_INITIALIZATION_FAILED_ (-3)

typedef vk_result_t (*create_instance_fn)(const void* info, const void* alloc, void* instance);
typedef vk_result_t (*enum_ext_fn)(const char* layer, uint32_t* count, void* props);
typedef vk_result_t (*enum_layer_fn)(uint32_t* count, void* props);
typedef vk_result_t (*enum_version_fn)(uint32_t* version);

extern void resolve_dlsym(void);

static void vulkan_session(void) {
    if (in_gramine_vm && !graphics_ensure_session())
        fprintf(stderr, "[vulkan] the VM graphics session did not start; the GPU is not reachable\n");
}

/* The loader's definition: the next one after this library (the real dlsym, called from here). */
static void* next_symbol(const char* name) {
    resolve_dlsym();
    return real_dlsym(RTLD_NEXT, name);
}

vk_result_t vkCreateInstance(const void* info, const void* alloc, void* instance) {
    static create_instance_fn next;
    vulkan_session();
    if (!next)
        next = (create_instance_fn)next_symbol("vkCreateInstance");
    return next ? next(info, alloc, instance) : VK_ERROR_INITIALIZATION_FAILED_;
}

vk_result_t vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
                                                   void* props) {
    static enum_ext_fn next;
    vulkan_session();
    if (!next)
        next = (enum_ext_fn)next_symbol("vkEnumerateInstanceExtensionProperties");
    return next ? next(layer, count, props) : VK_ERROR_INITIALIZATION_FAILED_;
}

vk_result_t vkEnumerateInstanceLayerProperties(uint32_t* count, void* props) {
    static enum_layer_fn next;
    vulkan_session();
    if (!next)
        next = (enum_layer_fn)next_symbol("vkEnumerateInstanceLayerProperties");
    return next ? next(count, props) : VK_ERROR_INITIALIZATION_FAILED_;
}

vk_result_t vkEnumerateInstanceVersion(uint32_t* version) {
    static enum_version_fn next;
    vulkan_session();
    if (!next)
        next = (enum_version_fn)next_symbol("vkEnumerateInstanceVersion");
    return next ? next(version) : VK_ERROR_INITIALIZATION_FAILED_;
}
