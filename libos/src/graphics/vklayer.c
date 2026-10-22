/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */
/*
 * VK_LAYER_GRAMINE_present: Vulkan presentation under saguaro-vm (dev docs: vulkan/,
 * "Phase 2 design").
 *
 * An implicit Vulkan layer that implements the X11 window-system extensions (VK_KHR_surface,
 * VK_KHR_xcb_surface, VK_KHR_xlib_surface, VK_KHR_swapchain) itself, because the driver's own
 * X11 presentation needs DRI3 with fd passing over the X connection, which the guest's X connection
 * over vsock cannot carry. The application renders into ordinary images; at present the layer
 * blits the image, flipped, into one of three linear images whose memory is exported as a dma-buf,
 * and hands that buffer to the host listener with the requests of the EGL path (egl.c, glx.c):
 * UPDATE_WINDOW_SIZE, X11_SETUP and X11_PRESENT. The listener flips every buffer when it copies it
 * into the window (OpenGL renders bottom-up), hence the flipped blit.
 *
 * Active only under the VM with EGL_VULKAN_PRESENT=1 (the layer manifest's enable_environment).
 * Limits of this first version: a frame reaches the X server one present later (the blit of the
 * current frame is not waited for on the CPU), FIFO is
 * paced with a timer, not the display (see read_pacing_env()), the layer submits to the device's first graphics queue
 * and serialises every submission to the device's queues (its own and the application's) with one
 * lock, single-plane 8-bit RGBA/BGRA formats only.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#include "common.h"
#include "graphics_comm_abi.h"

#define LAYER_NAME "VK_LAYER_GRAMINE_present"
#define MAX_OBJECTS 64
#define MAX_IMAGES 8
#define GEOMETRY_CHECK_INTERVAL 16

#define LOG(...) fprintf(stderr, "[vklayer] " __VA_ARGS__)

/* ------------------------------------------------------------------------------------------------
 * Dispatch tables, keyed by the loader's dispatch pointer (the first word of a dispatchable handle)
 */
static inline void* dispatch_key(const void* handle) {
    return *(void**)handle;
}

struct instance_data {
    void* key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
};

struct device_data {
    void* key;
    VkDevice device;
    VkPhysicalDevice physical;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData set_loader_data;
    bool export_ok;
    uint32_t queue_family;
    VkQueue queue;
    pthread_mutex_t lock;
    VkPhysicalDeviceMemoryProperties memprops;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueueSubmit2 QueueSubmit2;
    PFN_vkQueueSubmit2KHR QueueSubmit2KHR;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkQueueBindSparse QueueBindSparse;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
};

static struct instance_data* g_instances[MAX_OBJECTS];
static struct device_data* g_devices[MAX_OBJECTS];
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

static void* map_get(void** map, void* key) {
    void* r = NULL;
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (map[i] && *(void**)map[i] == key) {
            r = map[i];
            break;
        }
    pthread_mutex_unlock(&g_map_lock);
    return r;
}

static bool map_put(void** map, void* obj) {
    bool ok = false;
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (!map[i]) {
            map[i] = obj;
            ok = true;
            break;
        }
    pthread_mutex_unlock(&g_map_lock);
    return ok;
}

static void map_del(void** map, void* obj) {
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (map[i] == obj)
            map[i] = NULL;
    pthread_mutex_unlock(&g_map_lock);
}

static struct instance_data* get_instance(const void* handle) {
    return map_get((void**)g_instances, dispatch_key(handle));
}

static struct device_data* get_device(const void* handle) {
    return map_get((void**)g_devices, dispatch_key(handle));
}

/* ------------------------------------------------------------------------------------------------
 * Surfaces: the X connection and window of the application
 */
struct layer_surface {
    xcb_connection_t* conn;
    xcb_window_t window;
};

static bool window_geometry(struct layer_surface* s, uint32_t* w, uint32_t* h) {
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(s->conn,
                                                          xcb_get_geometry(s->conn, s->window),
                                                          NULL);
    if (!g)
        return false;
    *w = g->width;
    *h = g->height;
    free(g);
    return true;
}

static struct layer_surface* to_surface(VkSurfaceKHR s) {
    return (struct layer_surface*)(uintptr_t)s;
}

/* ------------------------------------------------------------------------------------------------
 * Swapchains
 */
struct swapchain {
    struct device_data* dev;
    struct layer_surface* surface;
    VkExtent2D extent;
    VkFormat format;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];
    VkDeviceMemory image_memory[MAX_IMAGES];
    bool held[MAX_IMAGES]; /* acquired by the application and not yet presented */
    uint32_t next_image;
    /* export ring */
    VkImage export_images[NUM_BUFFERS];
    VkDeviceMemory export_memory[NUM_BUFFERS];
    check* bufs;     /* read and written by the host listener: physical memory when available */
    uint64_t bufs_pa; /* what the listener gets (equal to bufs with identity-mapped memory) */
    check bufs_local[NUM_BUFFERS];
    uint32_t next_export;
    VkCommandPool pool;
    VkCommandBuffer cmds[NUM_BUFFERS]; /* one per export image */
    VkFence fences[NUM_BUFFERS];       /* signalled when that export image's blit is done */
    int pending;                       /* export image blitted, not yet sent to X (-1: none) */
    uint32_t acquires;
    bool out_of_date;
    VkPresentModeKHR mode;
    double last_x_present;   /* when the last buffer went to the listener (s) */
    uint64_t presents;
    uint64_t skipped;        /* frames not sent to X because of the pacing below */
    double rate_log_start;   /* EGL_VULKAN_RATE_LOG: the window's start and presents then */
    uint64_t rate_log_presents;
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* How often a frame goes to the X server. The listener copies each buffer into the window with
 * XRender and does not wait for the display, so without pacing an application in immediate mode
 * floods the X server with thousands of copies per second (the host compositor then stalls). FIFO
 * and FIFO-relaxed are paced to EGL_VULKAN_REFRESH Hz (default 60); immediate and mailbox render
 * every frame but send at most EGL_VULKAN_MAX_FPS (default 144) of them per second (each copy
 * costs the X server time; with the Intel GPU rendering and the AMD GPU driving the display it is a
 * cross-GPU copy of about 3 ms per 1920x1080 frame). */
static double g_fifo_interval = 1.0 / 60;
static double g_max_interval = 1.0 / 144;
/* EGL_VULKAN_RATE_LOG=<seconds>: a line with the presents of that window and their rate, on
 * stderr, every so many seconds (a frame-rate reading where the window's own HUD is not visible,
 * e.g. the Xorg dummy server; 0 or unset: off) */
static double g_rate_log_interval = 0;

static void read_pacing_env(void) {
    const char* r = getenv("EGL_VULKAN_REFRESH");
    if (r && atof(r) > 0)
        g_fifo_interval = 1.0 / atof(r);
    const char* m = getenv("EGL_VULKAN_MAX_FPS");
    if (m && atof(m) > 0)
        g_max_interval = 1.0 / atof(m);
    const char* l = getenv("EGL_VULKAN_RATE_LOG");
    if (l && atof(l) > 0)
        g_rate_log_interval = atof(l);
}

static void rate_log(struct swapchain* sc) {
    if (g_rate_log_interval <= 0)
        return;
    double now = now_s();
    if (!sc->rate_log_start) {
        sc->rate_log_start = now;
        sc->rate_log_presents = sc->presents;
        return;
    }
    double dt = now - sc->rate_log_start;
    if (dt < g_rate_log_interval)
        return;
    uint64_t n = sc->presents - sc->rate_log_presents;
    LOG("rate: %lu presents in %.1f s = %.1f per second (%lu since start)\n", (unsigned long)n,
        dt, n / dt, (unsigned long)sc->presents);
    sc->rate_log_start = now;
    sc->rate_log_presents = sc->presents;
}

/* ------------------------------------------------------------------------------------------------
 * The listener requests of the EGL path
 */
static void send_window_size(uint32_t w, uint32_t h, xcb_window_t window) {
    long off = acquire_libos_lock();
    comm_page_t* c = comm_page(off);
    c->p1 = w;
    c->p2 = h;
    c->p3 = window;
    __sync_synchronize();
    c->req_bit = UPDATE_WINDOW_SIZE;
    comm_sync_notify(c);
    relinquish_libos_lock(off);
}

static void send_setup(uint64_t bufs_pa, int i, uint64_t size, uint64_t stride) {
    long off = acquire_libos_lock();
    comm_page_t* c = comm_page(off);
    c->p1 = bufs_pa;
    c->p2 = (uint64_t)i;
    c->p3 = size;
    c->p4 = stride;
    __sync_synchronize();
    c->req_bit = X11_SETUP;
    comm_sync_notify(c);
    relinquish_libos_lock(off);
}

static void send_present(uint64_t bufs_pa, int i) {
    long off = acquire_libos_lock();
    comm_page_t* c = comm_page(off);
    c->p1 = bufs_pa;
    c->p2 = (uint64_t)i;
    __sync_synchronize();
    c->req_bit = X11_PRESENT;
    comm_sync_notify(c);
    relinquish_libos_lock(off);
}

/* ------------------------------------------------------------------------------------------------
 * Instance
 */
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo* info,
                                                          const VkAllocationCallbacks* alloc,
                                                          VkInstance* instance) {
    VkLayerInstanceCreateInfo* chain = (VkLayerInstanceCreateInfo*)info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo*)chain->pNext;
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(info, alloc, instance);
    if (r != VK_SUCCESS)
        return r;
    struct instance_data* d = calloc(1, sizeof(*d));
    if (!d)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->key = dispatch_key(*instance);
    d->instance = *instance;
    d->gipa = gipa;
#define GET(name) d->name = (PFN_vk##name)gipa(*instance, "vk" #name)
    GET(DestroyInstance);
    GET(GetPhysicalDeviceQueueFamilyProperties);
    GET(GetPhysicalDeviceMemoryProperties);
    GET(EnumerateDeviceExtensionProperties);
    GET(GetPhysicalDeviceFormatProperties);
#undef GET
    map_put((void**)g_instances, d);
    read_pacing_env();
    if (in_gramine_vm)
        graphics_ensure_session();
    LOG("active (presentation through the host listener)\n");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance,
                                                       const VkAllocationCallbacks* alloc) {
    struct instance_data* d = get_instance(instance);
    if (!d)
        return;
    PFN_vkDestroyInstance destroy = d->DestroyInstance;
    map_del((void**)g_instances, d);
    free(d);
    destroy(instance, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateXcbSurfaceKHR(
        VkInstance instance, const VkXcbSurfaceCreateInfoKHR* info,
        const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface) {
    (void)instance;
    (void)alloc;
    struct layer_surface* s = calloc(1, sizeof(*s));
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->conn = info->connection;
    s->window = info->window;
    *surface = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateXlibSurfaceKHR(
        VkInstance instance, const VkXlibSurfaceCreateInfoKHR* info,
        const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface) {
    (void)instance;
    (void)alloc;
    static xcb_connection_t* (*get_xcb)(Display*);
    if (!get_xcb) {
        void* h = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_GLOBAL);
        get_xcb = h ? (xcb_connection_t* (*)(Display*))dlsym(h, "XGetXCBConnection") : NULL;
        if (!get_xcb)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct layer_surface* s = calloc(1, sizeof(*s));
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->conn = get_xcb(info->dpy);
    s->window = (xcb_window_t)info->window;
    *surface = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                         const VkAllocationCallbacks* alloc) {
    (void)instance;
    (void)alloc;
    free(to_surface(surface));
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfaceSupportKHR(
        VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR surface, VkBool32* supported) {
    (void)surface;
    struct instance_data* d = get_instance(pd);
    VkQueueFamilyProperties props[16];
    uint32_t n = 16;
    if (!d) {
        *supported = VK_FALSE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    d->GetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    *supported = family < n && (props[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    return VK_SUCCESS;
}

static void fill_caps(struct layer_surface* s, VkSurfaceCapabilitiesKHR* caps) {
    uint32_t w = 0, h = 0;
    bool known = window_geometry(s, &w, &h);
    memset(caps, 0, sizeof(*caps));
    caps->minImageCount = 2;
    caps->maxImageCount = MAX_IMAGES;
    if (known) {
        /* as Mesa's X11 WSI: the window's size is the only extent. Proton's full-screen hack
         * (win32u's vkCreateSwapchainKHR) sizes its host swapchain from minImageExtent, so a
         * minimum of 1x1 gave The Witcher 1 a 1x1 swapchain on a display larger than its mode
         * (proton-vm/FINDINGS.md 2026-10-18) */
        caps->currentExtent = (VkExtent2D){w, h};
        caps->minImageExtent = caps->currentExtent;
        caps->maxImageExtent = caps->currentExtent;
    } else {
        caps->currentExtent = (VkExtent2D){1920, 1080};
        caps->minImageExtent = (VkExtent2D){1, 1};
        caps->maxImageExtent = (VkExtent2D){16384, 16384};
    }
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                                    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
    (void)pd;
    fill_caps(to_surface(surface), caps);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfaceCapabilities2KHR(
        VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR* info,
        VkSurfaceCapabilities2KHR* caps) {
    (void)pd;
    fill_caps(to_surface(info->surface), &caps->surfaceCapabilities);
    return VK_SUCCESS;
}

static const VkSurfaceFormatKHR g_formats[] = {
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
#define N_FORMATS (sizeof(g_formats) / sizeof(g_formats[0]))

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfaceFormatsKHR(
        VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t* count, VkSurfaceFormatKHR* formats) {
    (void)pd;
    (void)surface;
    if (!formats) {
        *count = N_FORMATS;
        return VK_SUCCESS;
    }
    uint32_t n = *count < N_FORMATS ? *count : N_FORMATS;
    memcpy(formats, g_formats, n * sizeof(*formats));
    *count = n;
    return n < N_FORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfaceFormats2KHR(
        VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR* info, uint32_t* count,
        VkSurfaceFormat2KHR* formats) {
    (void)pd;
    (void)info;
    if (!formats) {
        *count = N_FORMATS;
        return VK_SUCCESS;
    }
    uint32_t n = *count < N_FORMATS ? *count : N_FORMATS;
    for (uint32_t i = 0; i < n; i++)
        formats[i].surfaceFormat = g_formats[i];
    *count = n;
    return n < N_FORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static const VkPresentModeKHR g_modes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                                           VK_PRESENT_MODE_MAILBOX_KHR,
                                           VK_PRESENT_MODE_FIFO_RELAXED_KHR};
#define N_MODES (sizeof(g_modes) / sizeof(g_modes[0]))

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceSurfacePresentModesKHR(
        VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t* count, VkPresentModeKHR* modes) {
    (void)pd;
    (void)surface;
    if (!modes) {
        *count = N_MODES;
        return VK_SUCCESS;
    }
    uint32_t n = *count < N_MODES ? *count : N_MODES;
    memcpy(modes, g_modes, n * sizeof(*modes));
    *count = n;
    return n < N_MODES ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL layer_GetPhysicalDeviceXcbPresentationSupportKHR(
        VkPhysicalDevice pd, uint32_t family, xcb_connection_t* conn, xcb_visualid_t visual) {
    (void)conn;
    (void)visual;
    VkBool32 ok = VK_FALSE;
    layer_GetPhysicalDeviceSurfaceSupportKHR(pd, family, VK_NULL_HANDLE, &ok);
    return ok;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL layer_GetPhysicalDeviceXlibPresentationSupportKHR(
        VkPhysicalDevice pd, uint32_t family, Display* dpy, VisualID visual) {
    (void)dpy;
    (void)visual;
    VkBool32 ok = VK_FALSE;
    layer_GetPhysicalDeviceSurfaceSupportKHR(pd, family, VK_NULL_HANDLE, &ok);
    return ok;
}

/* Device extensions that extend swapchains or presentation, which the layer's own swapchains do
 * not implement: hidden from the application, or it would pass a layer swapchain to the driver
 * (vkWaitForPresentKHR, vkReleaseSwapchainImagesEXT, vkSetHdrMetadataEXT) or chain present
 * structures the layer ignores (present fences of swapchain_maintenance1). */
static const char* const g_hidden_device_extensions[] = {
    "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1", "VK_KHR_present_id",
    "VK_KHR_present_wait", "VK_KHR_present_id2", "VK_KHR_present_wait2", "VK_EXT_hdr_metadata",
    "VK_EXT_full_screen_exclusive", "VK_GOOGLE_display_timing", "VK_EXT_present_timing",
};

static bool hidden_extension(const char* name) {
    for (size_t i = 0; i < sizeof(g_hidden_device_extensions) / sizeof(g_hidden_device_extensions[0]);
            i++)
        if (!strcmp(name, g_hidden_device_extensions[i]))
            return true;
    return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_EnumerateDeviceExtensionProperties(
        VkPhysicalDevice pd, const char* layer, uint32_t* count, VkExtensionProperties* props) {
    struct instance_data* d = get_instance(pd);
    if (!d)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (layer && !strcmp(layer, LAYER_NAME)) {
        *count = 0;
        return VK_SUCCESS;
    }
    uint32_t n = 0;
    VkResult r = d->EnumerateDeviceExtensionProperties(pd, layer, &n, NULL);
    if (r != VK_SUCCESS)
        return r;
    VkExtensionProperties* all = calloc(n ? n : 1, sizeof(*all));
    if (!all)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = d->EnumerateDeviceExtensionProperties(pd, layer, &n, all);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!hidden_extension(all[i].extensionName))
            all[kept++] = all[i];
    if (!props) {
        *count = kept;
        free(all);
        return VK_SUCCESS;
    }
    uint32_t c = *count < kept ? *count : kept;
    memcpy(props, all, c * sizeof(*props));
    *count = c;
    free(all);
    return c < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ------------------------------------------------------------------------------------------------
 * Device
 */
static bool has_extension(struct instance_data* inst, VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    inst->EnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties* e = calloc(n ? n : 1, sizeof(*e));
    if (!e)
        return false;
    inst->EnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    bool found = false;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            found = true;
    free(e);
    return found;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice pd,
                                                        const VkDeviceCreateInfo* info,
                                                        const VkAllocationCallbacks* alloc,
                                                        VkDevice* device) {
    struct instance_data* inst = get_instance(pd);
    VkLayerDeviceCreateInfo* chain = (VkLayerDeviceCreateInfo*)info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo*)chain->pNext;
    if (!chain || !inst)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(inst->instance, "vkCreateDevice");
    PFN_vkSetDeviceLoaderData set_loader_data = NULL;
    for (VkLayerDeviceCreateInfo* cb = (VkLayerDeviceCreateInfo*)info->pNext; cb;
            cb = (VkLayerDeviceCreateInfo*)cb->pNext)
        if (cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                cb->function == VK_LOADER_DATA_CALLBACK)
            set_loader_data = cb->u.pfnSetDeviceLoaderData;

    /* the export images need dma-buf export */
    static const char* const wanted[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                         VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    const char* exts[128];
    uint32_t n = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount && n < 120; i++)
        if (!hidden_extension(info->ppEnabledExtensionNames[i]))
            exts[n++] = info->ppEnabledExtensionNames[i];
    bool export_ok = true;
    for (size_t w = 0; w < 2; w++) {
        bool present = false;
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(exts[i], wanted[w]))
                present = true;
        if (!present) {
            if (has_extension(inst, pd, wanted[w]))
                exts[n++] = wanted[w];
            else
                export_ok = false;
        }
    }
    VkDeviceCreateInfo ci = *info;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts;
    VkResult r = create(pd, &ci, alloc, device);
    if (r != VK_SUCCESS)
        return r;

    struct device_data* d = calloc(1, sizeof(*d));
    if (!d)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->key = dispatch_key(*device);
    d->device = *device;
    d->physical = pd;
    d->gdpa = gdpa;
    d->set_loader_data = set_loader_data;
    d->export_ok = export_ok;
    pthread_mutex_init(&d->lock, NULL);
#define GET(name) d->name = (PFN_vk##name)gdpa(*device, "vk" #name)
    GET(DestroyDevice);
    GET(GetDeviceQueue);
    GET(CreateImage);
    GET(DestroyImage);
    GET(GetImageMemoryRequirements);
    GET(GetImageSubresourceLayout);
    GET(AllocateMemory);
    GET(FreeMemory);
    GET(BindImageMemory);
    GET(GetMemoryFdKHR);
    GET(CreateCommandPool);
    GET(DestroyCommandPool);
    GET(AllocateCommandBuffers);
    GET(BeginCommandBuffer);
    GET(EndCommandBuffer);
    GET(CmdPipelineBarrier);
    GET(CmdBlitImage);
    GET(QueueSubmit);
    GET(QueueSubmit2);
    GET(QueueSubmit2KHR);
    GET(QueueWaitIdle);
    GET(QueueBindSparse);
    GET(CreateFence);
    GET(DestroyFence);
    GET(WaitForFences);
    GET(ResetFences);
    GET(DeviceWaitIdle);
#undef GET
    inst->GetPhysicalDeviceMemoryProperties(pd, &d->memprops);
    /* the first graphics queue family the application created a queue in */
    d->queue_family = UINT32_MAX;
    VkQueueFamilyProperties props[16];
    uint32_t nf = 16;
    inst->GetPhysicalDeviceQueueFamilyProperties(pd, &nf, props);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
        uint32_t f = info->pQueueCreateInfos[i].queueFamilyIndex;
        if (f < nf && (props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            d->queue_family = f;
            break;
        }
    }
    if (d->queue_family != UINT32_MAX) {
        d->GetDeviceQueue(*device, d->queue_family, 0, &d->queue);
        if (set_loader_data)
            set_loader_data(*device, d->queue);
    }
    if (!export_ok)
        LOG("the driver lacks dma-buf export; presentation is not available\n");
    map_put((void**)g_devices, d);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device,
                                                     const VkAllocationCallbacks* alloc) {
    struct device_data* d = get_device(device);
    if (!d)
        return;
    PFN_vkDestroyDevice destroy = d->DestroyDevice;
    map_del((void**)g_devices, d);
    free(d);
    destroy(device, alloc);
}

static int find_memtype(struct device_data* d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d->memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static VkResult make_image(struct device_data* d, VkFormat format, VkExtent2D extent,
                           VkImageUsageFlags usage, bool exportable, VkImageCreateFlags flags,
                           const void* next, VkImage* image, VkDeviceMemory* memory) {
    VkExternalMemoryImageCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .pNext = exportable ? (const void*)&ext : next,
                            .flags = flags,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {extent.width, extent.height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = exportable ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = d->CreateImage(d->device, &ci, NULL, image);
    if (r != VK_SUCCESS)
        return r;
    VkMemoryRequirements req;
    d->GetImageMemoryRequirements(d->device, *image, &req);
    int mt = -1;
    if (exportable && getenv("EGL_VULKAN_DUMP")) /* the dump maps the export images */
        mt = find_memtype(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt < 0)
        mt = find_memtype(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0)
        mt = find_memtype(d, req.memoryTypeBits, 0);
    VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                         .handleTypes =
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &export,
        .image = *image};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = exportable ? &dedicated : NULL,
                                .allocationSize = req.size,
                                .memoryTypeIndex = (uint32_t)mt};
    r = d->AllocateMemory(d->device, &mai, NULL, memory);
    if (r != VK_SUCCESS) {
        d->DestroyImage(d->device, *image, NULL);
        return r;
    }
    return d->BindImageMemory(d->device, *image, *memory, 0);
}

static void destroy_swapchain_images(struct swapchain* sc) {
    struct device_data* d = sc->dev;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->images[i])
            d->DestroyImage(d->device, sc->images[i], NULL);
        if (sc->image_memory[i])
            d->FreeMemory(d->device, sc->image_memory[i], NULL);
    }
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (sc->export_images[i])
            d->DestroyImage(d->device, sc->export_images[i], NULL);
        if (sc->export_memory[i])
            d->FreeMemory(d->device, sc->export_memory[i], NULL);
    }
    for (int i = 0; i < NUM_BUFFERS; i++)
        if (sc->fences[i])
            d->DestroyFence(d->device, sc->fences[i], NULL);
    if (sc->pool)
        d->DestroyCommandPool(d->device, sc->pool, NULL);
}

static VkFormat export_format(VkFormat app) {
    switch (app) {
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        default:
            return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateSwapchainKHR(VkDevice device,
                                                              const VkSwapchainCreateInfoKHR* info,
                                                              const VkAllocationCallbacks* alloc,
                                                              VkSwapchainKHR* out) {
    (void)alloc;
    struct device_data* d = get_device(device);
    if (!d || !d->export_ok || d->queue_family == UINT32_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct swapchain* sc = calloc(1, sizeof(*sc));
    if (sc) {
        sc->bufs = graphics_physmem(sizeof(check) * NUM_BUFFERS, &sc->bufs_pa);
        if (!sc->bufs) {
            sc->bufs = sc->bufs_local;
            sc->bufs_pa = (uint64_t)(uintptr_t)sc->bufs_local;
        }
    }
    if (!sc)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->dev = d;
    sc->surface = to_surface(info->surface);
    sc->extent = info->imageExtent;
    sc->format = info->imageFormat;
    sc->mode = info->presentMode;
    sc->image_count = info->minImageCount < 2 ? 2 : info->minImageCount;
    if (sc->image_count > MAX_IMAGES)
        sc->image_count = MAX_IMAGES;

    /* VK_KHR_swapchain_mutable_format (DXVK uses it for sRGB views): mutable images, with the
     * application's format list */
    VkImageCreateFlags image_flags = 0;
    VkImageFormatListCreateInfo format_list = {.sType =
                                                   VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    const void* image_next = NULL;
    if (info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        image_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        for (const VkBaseInStructure* p = info->pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
                const VkImageFormatListCreateInfo* l = (const VkImageFormatListCreateInfo*)p;
                format_list.viewFormatCount = l->viewFormatCount;
                format_list.pViewFormats = l->pViewFormats;
                image_next = &format_list;
            }
    }
    VkResult r;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        r = make_image(d, sc->format, sc->extent,
                       info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, image_flags,
                       image_next, &sc->images[i], &sc->image_memory[i]);
        if (r != VK_SUCCESS)
            goto fail;
    }

    send_window_size(sc->extent.width, sc->extent.height, sc->surface->window);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        r = make_image(d, export_format(sc->format), sc->extent, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       true, 0, NULL, &sc->export_images[i], &sc->export_memory[i]);
        if (r != VK_SUCCESS)
            goto fail;
        VkMemoryGetFdInfoKHR gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                    .memory = sc->export_memory[i],
                                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1;
        r = d->GetMemoryFdKHR(d->device, &gfi, &fd);
        if (r != VK_SUCCESS)
            goto fail;
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        d->GetImageSubresourceLayout(d->device, sc->export_images[i], &sub, &layout);
        /* The guest fd is a forwarded host fd (host fd + GRAPHICS_FS_OFFSET, the shared ABI
         * constant: the listener gets the raw host fd number). The listener's XCB hands the fd to
         * the X server and closes it, so give it a host duplicate and keep our own fd. The guest
         * handle of the duplicate goes stale then and is left alone (the LibOS drops it when the
         * host reuses the number). */
        int dup_fd = dup(fd);
        close(fd);
        if (dup_fd < (int)GRAPHICS_FS_OFFSET) {
            LOG("unexpected dma-buf fd %d (not a forwarded host fd)\n", dup_fd);
            r = VK_ERROR_INITIALIZATION_FAILED;
            goto fail;
        }
        memset(&sc->bufs[i], 0, sizeof(sc->bufs[i]));
        sc->bufs[i].bo_fd = dup_fd - GRAPHICS_FS_OFFSET;
        send_setup(sc->bufs_pa, i, layout.size + layout.offset, layout.rowPitch);
    }

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = d->queue_family};
    r = d->CreateCommandPool(d->device, &pci, NULL, &sc->pool);
    if (r != VK_SUCCESS)
        goto fail;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = sc->pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = NUM_BUFFERS};
    r = d->AllocateCommandBuffers(d->device, &cai, sc->cmds);
    if (r != VK_SUCCESS)
        goto fail;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                             .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < NUM_BUFFERS; i++) {
        /* a command buffer the layer allocates below the loader needs the loader's dispatch data */
        if (d->set_loader_data)
            d->set_loader_data(device, sc->cmds[i]);
        else
            *(void**)sc->cmds[i] = *(void**)device;
        r = d->CreateFence(d->device, &fci, NULL, &sc->fences[i]);
        if (r != VK_SUCCESS)
            goto fail;
    }
    sc->pending = -1;

    if (info->oldSwapchain)
        ((struct swapchain*)(uintptr_t)info->oldSwapchain)->out_of_date = true;
    LOG("swapchain %ux%u, %u images, format %d\n", sc->extent.width, sc->extent.height,
        sc->image_count, sc->format);
    *out = (VkSwapchainKHR)(uintptr_t)sc;
    return VK_SUCCESS;

fail:
    LOG("swapchain creation failed: %d\n", r);
    destroy_swapchain_images(sc);
    free(sc);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroySwapchainKHR(VkDevice device,
                                                           VkSwapchainKHR swapchain,
                                                           const VkAllocationCallbacks* alloc) {
    (void)alloc;
    struct swapchain* sc = (struct swapchain*)(uintptr_t)swapchain;
    if (!sc)
        return;
    struct device_data* d = get_device(device);
    if (d)
        d->DeviceWaitIdle(d->device);
    LOG("swapchain destroyed after %lu presents, %lu not sent to X (pacing)\n",
        (unsigned long)sc->presents, (unsigned long)sc->skipped);
    destroy_swapchain_images(sc);
    free(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetSwapchainImagesKHR(VkDevice device,
                                                                 VkSwapchainKHR swapchain,
                                                                 uint32_t* count, VkImage* images) {
    (void)device;
    struct swapchain* sc = (struct swapchain*)(uintptr_t)swapchain;
    if (!images) {
        *count = sc->image_count;
        return VK_SUCCESS;
    }
    uint32_t n = *count < sc->image_count ? *count : sc->image_count;
    memcpy(images, sc->images, n * sizeof(*images));
    *count = n;
    return n < sc->image_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult signal_acquire(struct device_data* d, VkSemaphore sem, VkFence fence) {
    if (!sem && !fence)
        return VK_SUCCESS;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .signalSemaphoreCount = sem ? 1 : 0,
                       .pSignalSemaphores = &sem};
    pthread_mutex_lock(&d->lock);
    VkResult r = d->QueueSubmit(d->queue, 1, &si, fence);
    pthread_mutex_unlock(&d->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_AcquireNextImageKHR(VkDevice device,
                                                               VkSwapchainKHR swapchain,
                                                               uint64_t timeout, VkSemaphore sem,
                                                               VkFence fence, uint32_t* index) {
    (void)timeout;
    struct device_data* d = get_device(device);
    struct swapchain* sc = (struct swapchain*)(uintptr_t)swapchain;
    if (sc->out_of_date)
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (++sc->acquires % GEOMETRY_CHECK_INTERVAL == 0) {
        uint32_t w, h;
        if (window_geometry(sc->surface, &w, &h) &&
                (w != sc->extent.width || h != sc->extent.height)) {
            sc->out_of_date = true;
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
    }
    uint32_t found = UINT32_MAX;
    for (uint32_t k = 0; k < sc->image_count; k++) {
        uint32_t i = (sc->next_image + k) % sc->image_count;
        if (!sc->held[i]) {
            found = i;
            break;
        }
    }
    if (found == UINT32_MAX)
        return timeout ? VK_TIMEOUT : VK_NOT_READY;
    sc->held[found] = true;
    sc->next_image = (found + 1) % sc->image_count;
    VkResult r = signal_acquire(d, sem, fence);
    if (r != VK_SUCCESS)
        return r;
    *index = found;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_AcquireNextImage2KHR(VkDevice device,
                                                                const VkAcquireNextImageInfoKHR* ai,
                                                                uint32_t* index) {
    return layer_AcquireNextImageKHR(device, ai->swapchain, ai->timeout, ai->semaphore, ai->fence,
                                     index);
}

static void image_barrier(struct device_data* d, VkCommandBuffer cmd, VkImage image,
                          VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = src_access,
                              .dstAccessMask = dst_access,
                              .oldLayout = from,
                              .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    d->CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* EGL_VULKAN_DUMP=<frame>:<path>: after that many presents, write the export image (as the
 * listener receives it, bottom-up) to a PPM file, when its memory can be mapped (a debugging aid
 * for the layer; on an integrated GPU device-local memory is host-visible). */
static void maybe_dump(struct swapchain* sc, uint32_t e) {
    static long frame = -1;
    static char path[256];
    if (frame == -1) {
        const char* v = getenv("EGL_VULKAN_DUMP");
        frame = -2;
        if (v && strchr(v, ':')) {
            frame = atol(v);
            snprintf(path, sizeof(path), "%s", strchr(v, ':') + 1);
        }
    }
    if (frame < 0 || (long)sc->presents != frame)
        return;
    struct device_data* d = sc->dev;
    PFN_vkMapMemory map = (PFN_vkMapMemory)d->gdpa(d->device, "vkMapMemory");
    PFN_vkUnmapMemory unmap = (PFN_vkUnmapMemory)d->gdpa(d->device, "vkUnmapMemory");
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    d->GetImageSubresourceLayout(d->device, sc->export_images[e], &sub, &layout);
    uint8_t* p = NULL;
    if (map(d->device, sc->export_memory[e], 0, VK_WHOLE_SIZE, 0, (void**)&p) != VK_SUCCESS) {
        LOG("dump: the export memory cannot be mapped\n");
        return;
    }
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", sc->extent.width, sc->extent.height);
        for (uint32_t y = 0; y < sc->extent.height; y++) {
            uint8_t* row = p + layout.offset + y * layout.rowPitch;
            for (uint32_t x = 0; x < sc->extent.width; x++) {
                fputc(row[4 * x + 2], f);
                fputc(row[4 * x + 1], f);
                fputc(row[4 * x + 0], f);
            }
        }
        fclose(f);
        LOG("dump: frame %ld written to %s\n", frame, path);
    }
    unmap(d->device, sc->export_memory[e]);
}

/* Sends the export image whose blit was submitted at the previous present to the listener, once the
 * blit is done (one frame of latency, so the CPU does not wait for the current blit). */
static void deliver_pending(struct swapchain* sc) {
    struct device_data* d = sc->dev;
    if (sc->pending < 0)
        return;
    int e = sc->pending;
    sc->pending = -1;
    if (d->WaitForFences(d->device, 1, &sc->fences[e], VK_TRUE, 5000000000ull) != VK_SUCCESS)
        return;
    maybe_dump(sc, (uint32_t)e);
    bool fifo = sc->mode == VK_PRESENT_MODE_FIFO_KHR || sc->mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    double now = now_s();
    double interval = fifo ? g_fifo_interval : g_max_interval;
    if (fifo) {
        /* FIFO: the application waits for the next refresh, as with vsync */
        double next = sc->last_x_present + interval;
        /* not nanosleep(): the VM PAL serves timed waits from a 100 ms tick, which made FIFO run
         * at 10 frames per second */
        while (now < next) {
            sched_yield();
            now = now_s();
        }
    } else if (now - sc->last_x_present < interval) {
        sc->skipped++;
        return;
    }
    sc->last_x_present = now;
    send_present(sc->bufs_pa, e);
}

static VkResult present_one(struct swapchain* sc, uint32_t index, const VkSemaphore* waits,
                            uint32_t wait_count) {
    struct device_data* d = sc->dev;
    uint32_t e = sc->next_export;
    sc->next_export = (e + 1) % NUM_BUFFERS;
    /* the previous blit into this export image (NUM_BUFFERS presents ago) */
    VkResult r = d->WaitForFences(d->device, 1, &sc->fences[e], VK_TRUE, 5000000000ull);
    if (r != VK_SUCCESS)
        return r;
    VkCommandBuffer cmd = sc->cmds[e];
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    d->BeginCommandBuffer(cmd, &bi);
    image_barrier(d, cmd, sc->images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(d, cmd, sc->export_images[e], VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    int32_t w = (int32_t)sc->extent.width, h = (int32_t)sc->extent.height;
    /* flipped: the listener flips every buffer back (OpenGL renders bottom-up) */
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, h, 0}, {w, 0, 1}}};
    d->CmdBlitImage(cmd, sc->images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    sc->export_images[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                    VK_FILTER_NEAREST);
    image_barrier(d, cmd, sc->export_images[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    image_barrier(d, cmd, sc->images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    d->EndCommandBuffer(cmd);

    VkPipelineStageFlags stages[16];
    for (uint32_t i = 0; i < wait_count && i < 16; i++)
        stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .waitSemaphoreCount = wait_count < 16 ? wait_count : 16,
                       .pWaitSemaphores = waits,
                       .pWaitDstStageMask = stages,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cmd};
    d->ResetFences(d->device, 1, &sc->fences[e]);
    pthread_mutex_lock(&d->lock);
    r = d->QueueSubmit(d->queue, 1, &si, sc->fences[e]);
    pthread_mutex_unlock(&d->lock);
    if (r != VK_SUCCESS)
        return r;
    /* The application may acquire this image again at once: its next use waits on the acquire
     * semaphore, whose signal comes later in submission order than this blit. */
    sc->held[index] = false;
    sc->presents++;
    rate_log(sc);
    deliver_pending(sc);
    sc->pending = (int)e;
    return sc->out_of_date ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueuePresentKHR(VkQueue queue,
                                                           const VkPresentInfoKHR* info) {
    (void)queue;
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        struct swapchain* sc = (struct swapchain*)(uintptr_t)info->pSwapchains[i];
        /* the application's semaphores are waited for by the first swapchain's blit */
        VkResult r = present_one(sc, info->pImageIndices[i], i == 0 ? info->pWaitSemaphores : NULL,
                                 i == 0 ? info->waitSemaphoreCount : 0);
        if (info->pResults)
            info->pResults[i] = r;
        if (r != VK_SUCCESS && result == VK_SUCCESS)
            result = r;
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_GetDeviceGroupSurfacePresentModesKHR(
        VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR* modes) {
    (void)device;
    (void)surface;
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

/* The application's own submissions to the queue the layer also submits to take the layer's lock
 * too: Vulkan requires queue access to be externally synchronised, and the layer's submissions (at
 * acquire and present) are invisible to the application's own locking. */
static VKAPI_ATTR VkResult VKAPI_CALL layer_QueueSubmit(VkQueue queue, uint32_t n,
                                                       const VkSubmitInfo* submits, VkFence fence) {
    struct device_data* d = get_device(queue);
    if (!d)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(&d->lock);
    VkResult r = d->QueueSubmit(queue, n, submits, fence);
    pthread_mutex_unlock(&d->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueueSubmit2(VkQueue queue, uint32_t n,
                                                        const VkSubmitInfo2* submits,
                                                        VkFence fence) {
    struct device_data* d = get_device(queue);
    if (!d)
        return VK_ERROR_DEVICE_LOST;
    PFN_vkQueueSubmit2 next = d->QueueSubmit2 ? d->QueueSubmit2 : d->QueueSubmit2KHR;
    pthread_mutex_lock(&d->lock);
    VkResult r = next(queue, n, submits, fence);
    pthread_mutex_unlock(&d->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueueWaitIdle(VkQueue queue) {
    struct device_data* d = get_device(queue);
    if (!d)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(&d->lock);
    VkResult r = d->QueueWaitIdle(queue);
    pthread_mutex_unlock(&d->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueueBindSparse(VkQueue queue, uint32_t n,
                                                           const VkBindSparseInfo* info,
                                                           VkFence fence) {
    struct device_data* d = get_device(queue);
    if (!d)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(&d->lock);
    VkResult r = d->QueueBindSparse(queue, n, info, fence);
    pthread_mutex_unlock(&d->lock);
    return r;
}

/* ------------------------------------------------------------------------------------------------
 * Proc-address lookup and the loader negotiation
 */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device,
                                                                       const char* name);

#define INTERCEPT(fn) \
    if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)layer_##fn

static PFN_vkVoidFunction device_intercept(const char* name) {
    INTERCEPT(DestroyDevice);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(GetSwapchainImagesKHR);
    INTERCEPT(AcquireNextImageKHR);
    INTERCEPT(AcquireNextImage2KHR);
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(GetDeviceGroupSurfacePresentModesKHR);
    INTERCEPT(QueueSubmit);
    INTERCEPT(QueueSubmit2);
    INTERCEPT(QueueWaitIdle);
    INTERCEPT(QueueBindSparse);
    if (!strcmp(name, "vkQueueSubmit2KHR"))
        return (PFN_vkVoidFunction)layer_QueueSubmit2;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance,
                                                                         const char* name) {
    INTERCEPT(CreateInstance);
    INTERCEPT(DestroyInstance);
    INTERCEPT(CreateDevice);
    INTERCEPT(EnumerateDeviceExtensionProperties);
    INTERCEPT(CreateXcbSurfaceKHR);
    INTERCEPT(CreateXlibSurfaceKHR);
    INTERCEPT(DestroySurfaceKHR);
    INTERCEPT(GetPhysicalDeviceSurfaceSupportKHR);
    INTERCEPT(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    INTERCEPT(GetPhysicalDeviceSurfaceCapabilities2KHR);
    INTERCEPT(GetPhysicalDeviceSurfaceFormatsKHR);
    INTERCEPT(GetPhysicalDeviceSurfaceFormats2KHR);
    INTERCEPT(GetPhysicalDeviceSurfacePresentModesKHR);
    INTERCEPT(GetPhysicalDeviceXcbPresentationSupportKHR);
    INTERCEPT(GetPhysicalDeviceXlibPresentationSupportKHR);
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)layer_GetInstanceProcAddr;
    PFN_vkVoidFunction f = device_intercept(name);
    if (f)
        return f;
    if (!instance)
        return NULL;
    struct instance_data* d = get_instance(instance);
    return d ? d->gipa(instance, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device,
                                                                       const char* name) {
    PFN_vkVoidFunction f = device_intercept(name);
    if (f)
        return f;
    struct device_data* d = get_device(device);
    return d ? d->gdpa(device, name) : NULL;
}
#undef INTERCEPT

/* The entry point named in the layer manifest (VkLayer_gramine_present.json). Deliberately not
 * vkNegotiateLoaderLayerInterfaceVersion: this library is preloaded into the application, so its
 * exported symbols must not shadow standard Vulkan names. */
__attribute__((visibility("default"))) VkResult gramine_vk_negotiate(
        VkNegotiateLayerInterface* iface) {
    if (!iface || iface->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!in_gramine_vm) {
        /* outside the VM the driver's own presentation works: stay out of the way */
        LOG("not under saguaro-vm; the layer passes everything through\n");
    }
    if (iface->loaderLayerInterfaceVersion > 2)
        iface->loaderLayerInterfaceVersion = 2;
    iface->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
    iface->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
    iface->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
