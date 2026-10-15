/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vk-session-shim: a phase-0 test aid for Vulkan under gramine-vm (Documentation/devel/vulkan/).
 *
 * The graphics preload library (libgraphics.so) starts the guest's graphics session -- the comm-page
 * handshake with the host listener, after which the LibOS forwards opens of /dev/dri and /sys to the
 * host -- at the first GLX or EGL call. A Vulkan program makes none, so without help it never sees
 * the host GPU. This shim interposes vkCreateInstance, starts the session through the preload
 * library's graphics_ensure_session() and then calls the real vkCreateInstance. Preload it after
 * libgraphics.so: LD_PRELOAD = "./libgraphics.so:/vk-session-shim.so".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info,
                                                const VkAllocationCallbacks* alloc,
                                                VkInstance* instance) {
    static PFN_vkCreateInstance real;
    bool (*ensure)(void) = (bool (*)(void))dlsym(RTLD_DEFAULT, "graphics_ensure_session");
    if (ensure)
        fprintf(stderr, "[vk-session-shim] graphics session: %s\n", ensure() ? "ready" : "FAILED");
    else
        fprintf(stderr, "[vk-session-shim] graphics_ensure_session not found (libgraphics.so not preloaded?)\n");
    if (!real)
        real = (PFN_vkCreateInstance)dlsym(RTLD_NEXT, "vkCreateInstance");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;
    return real(info, alloc, instance);
}
