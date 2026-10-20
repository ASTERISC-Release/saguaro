/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* A small Vulkan program for Windows, for the Proton feasibility study
 * (dev docs: proton-vm/): loads vulkan-1.dll (Wine's winevulkan), creates an instance
 * with VK_KHR_win32_surface, a swap chain on a Win32 window, clears each image to a cycling colour
 * and presents in the immediate mode when available (otherwise FIFO), printing the frame rate every
 * second. Exits after -s seconds (default 10). Usage: vkwin.exe [-s SEC] [-w W -h H] [-g GPU].
 * Uses the Vulkan headers of the Linux loader package (platform-independent) and no import
 * library: every entry point is fetched with vkGetInstanceProcAddr. Built with
 * x86_64-w64-mingw32-gcc (test/proton-vm/build-tests.sh). */

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

#define LOAD(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s failed: %d\n", #x, r_); return 1; } } while (0)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char** argv) {
    int seconds = 10, width = 1280, height = 720, gpu = 0;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "-s")) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w")) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g")) gpu = atoi(argv[++i]);
    }
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) {
        printf("vulkan-1.dll not found: %lu\n", GetLastError());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    LOAD(vkEnumerateInstanceVersion);
    uint32_t ver = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
    const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "proton-vm vkwin";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);
    LOAD(vkGetDeviceProcAddr);
    LOAD(vkCreateWin32SurfaceKHR);
    LOAD(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD(vkGetPhysicalDeviceSurfacePresentModesKHR);

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    if (n == 0) {
        printf("no Vulkan device\n");
        return 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        printf("vkwin: gpu %u: %s (vendor 0x%04x, api %u.%u.%u, driver 0x%x)%s\n", i, p.deviceName,
               p.vendorID, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion), p.driverVersion, (int)i == gpu ? " *" : "");
    }
    if ((uint32_t)gpu >= n) gpu = 0;
    VkPhysicalDevice pd = pds[gpu];
    printf("vkwin: loader version %u.%u, %ux%u\n", VK_VERSION_MAJOR(ver), VK_VERSION_MINOR(ver), width, height);
    fflush(stdout);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "proton-vm-vkwin";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "proton-vm vkwin", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL, NULL, wc.hInstance, NULL);
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = wc.hInstance;
    sci.hwnd = hwnd;
    VkSurfaceKHR surf;
    CHECK(vkCreateWin32SurfaceKHR(inst, &sci, NULL, &surf));

    uint32_t qn = 0, qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties qp[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
    for (qf = 0; qf < qn; qf++) {
        VkBool32 sup = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, qf, surf, &sup);
        if ((qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) break;
    }
    if (qf == qn) {
        printf("no graphics+present queue\n");
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    DLOAD(vkGetDeviceQueue);
    DLOAD(vkCreateSwapchainKHR);
    DLOAD(vkGetSwapchainImagesKHR);
    DLOAD(vkAcquireNextImageKHR);
    DLOAD(vkQueuePresentKHR);
    DLOAD(vkCreateCommandPool);
    DLOAD(vkAllocateCommandBuffers);
    DLOAD(vkBeginCommandBuffer);
    DLOAD(vkEndCommandBuffer);
    DLOAD(vkCmdPipelineBarrier);
    DLOAD(vkCmdClearColorImage);
    DLOAD(vkQueueSubmit);
    DLOAD(vkCreateSemaphore);
    DLOAD(vkCreateFence);
    DLOAD(vkWaitForFences);
    DLOAD(vkResetFences);
    DLOAD(vkDeviceWaitIdle);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    uint32_t fn = 1;
    VkSurfaceFormatKHR fmt;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fn, &fmt);
    uint32_t pn = 8;
    VkPresentModeKHR pms[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &pn, pms);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < pn; i++)
        if (pms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = pms[i];
    for (uint32_t i = 0; i < pn && mode == VK_PRESENT_MODE_FIFO_KHR; i++)
        if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pms[i];
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xffffffff) { ext.width = width; ext.height = height; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sc = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surf;
    sc.minImageCount = count;
    sc.imageFormat = fmt.format;
    sc.imageColorSpace = fmt.colorSpace;
    sc.imageExtent = ext;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = mode;
    sc.clipped = VK_TRUE;
    VkSwapchainKHR swap;
    CHECK(vkCreateSwapchainKHR(dev, &sc, NULL, &swap));
    uint32_t in = 0;
    vkGetSwapchainImagesKHR(dev, swap, &in, NULL);
    VkImage imgs[16];
    if (in > 16) in = 16;
    vkGetSwapchainImagesKHR(dev, swap, &in, imgs);
    printf("vkwin: swapchain %u images, format %d, present mode %d (%s), extent %ux%u\n", in,
           fmt.format, mode, mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                             : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo",
           ext.width, ext.height);
    fflush(stdout);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qf;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = in;
    VkCommandBuffer cbs[16];
    CHECK(vkAllocateCommandBuffers(dev, &cai, cbs));
    VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphore acq[16], rel[16];
    VkFence fences[16];
    for (uint32_t i = 0; i < in; i++) {
        vkCreateSemaphore(dev, &semi, NULL, &acq[i]);
        vkCreateSemaphore(dev, &semi, NULL, &rel[i]);
        vkCreateFence(dev, &fci, NULL, &fences[i]);
    }

    LARGE_INTEGER freq, start, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    last = start;
    unsigned frames = 0, total = 0, frame = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        uint32_t fi = frame % in;
        vkWaitForFences(dev, 1, &fences[fi], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fences[fi]);
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, acq[fi], VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            printf("vkAcquireNextImageKHR: %d\n", r);
            break;
        }
        VkCommandBuffer cb = cbs[fi];
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[idx];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        float t = (float)(total % 600) / 600.0f;
        VkClearColorValue col = {{0.2f + 0.6f * t, 0.3f, 0.8f - 0.6f * t, 1.0f}};
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1,
                             &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acq[fi];
        si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rel[fi];
        vkQueueSubmit(q, 1, &si, fences[fi]);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rel[fi];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(q, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            printf("vkQueuePresentKHR: %d\n", r);
            break;
        }
        frames++;
        total++;
        frame++;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - last.QuadPart >= freq.QuadPart) {
            printf("vkwin: %u frames in %.2f s = %.1f FPS\n", frames,
                   (double)(now.QuadPart - last.QuadPart) / freq.QuadPart,
                   frames * (double)freq.QuadPart / (now.QuadPart - last.QuadPart));
            fflush(stdout);
            frames = 0;
            last = now;
        }
        if (now.QuadPart - start.QuadPart >= (LONGLONG)seconds * freq.QuadPart) break;
    }
out:
    vkDeviceWaitIdle(dev);
    printf("vkwin: %u frames total, %.1f FPS average\n", total,
           total * (double)freq.QuadPart / (double)(now.QuadPart - start.QuadPart));
    DestroyWindow(hwnd);
    return 0;
}
