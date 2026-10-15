/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vk-headless: a window-less Vulkan test that walks the device path step by step, so that a run
 * under gramine-direct or gramine-vm shows how far it gets. Every step prints one line
 * "STEP <name>: PASS|FAIL|SKIP <detail>"; the exit status is the number of failed steps.
 *
 * Steps: instance, device selection, logical device (Vulkan 1.3 features: timeline semaphores,
 * synchronization2, dynamic rendering), host-visible memory allocation and map, compute dispatch
 * with a checked result, fence wait, timeline semaphore (GPU signal, host wait; host signal),
 * submit round-trip latency, off-screen render with read-back and pixel count, export of device
 * memory as a dma-buf fd, export of a semaphore as an opaque (syncobj) fd and as a sync_file fd.
 *
 * Environment: VKH_DEVICE=<index or name substring> selects the physical device (default 0, the
 * loader's order); VKH_ITER=<n> sets the number of submits in the latency step (default 1000);
 * VKH_SIZE=<n> the number of uints in the compute buffer (default 1M); VKH_NO_TIMEOUT_WAIT=1 skips
 * the timeline wait that has to time out; VKH_MEMFLAGS=<VkMemoryPropertyFlags> the property flags the
 * compute buffer's memory type must have (default 0x6, host-visible and coherent: the first such
 * type; 0xe adds host-cached, 0x7 asks for device-local, which on a discrete GPU is VRAM through
 * the PCI BAR).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "compute_spv.h"
#include "frag_spv.h"
#include "vert_spv.h"

static int g_failures;

#define STEP(name, ok, ...)                                    \
    do {                                                       \
        printf("STEP %-22s %s ", name ":", (ok) ? "PASS" : "FAIL"); \
        printf(__VA_ARGS__);                                   \
        printf("\n");                                          \
        fflush(stdout);                                        \
        if (!(ok))                                             \
            g_failures++;                                      \
    } while (0)

#define SKIP(name, ...)                                        \
    do {                                                       \
        printf("STEP %-22s SKIP ", name ":");                  \
        printf(__VA_ARGS__);                                   \
        printf("\n");                                          \
        fflush(stdout);                                        \
    } while (0)

/* like CHECK, but continues at `label` instead of stopping the program */
#define CHECK_SOFT(name, call, label)                                      \
    do {                                                                   \
        VkResult _r = (call);                                              \
        if (_r != VK_SUCCESS) {                                            \
            STEP(name, false, "%s returned %d", #call, _r);                \
            goto label;                                                    \
        }                                                                  \
    } while (0)

#define CHECK(name, call)                                                  \
    do {                                                                   \
        VkResult _r = (call);                                              \
        if (_r != VK_SUCCESS) {                                            \
            STEP(name, false, "%s returned %d", #call, _r);                \
            goto out;                                                      \
        }                                                                  \
    } while (0)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static VkInstance g_inst;
static VkPhysicalDevice g_pdev;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VkPhysicalDeviceMemoryProperties g_memprops;

static int find_memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static VkResult make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                            VkBuffer* buf, VkDeviceMemory* mem) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult r = vkCreateBuffer(g_dev, &bci, NULL, buf);
    if (r != VK_SUCCESS)
        return r;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, *buf, &req);
    int mt = find_memtype(req.memoryTypeBits, props);
    if (mt < 0)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt};
    r = vkAllocateMemory(g_dev, &mai, NULL, mem);
    if (r != VK_SUCCESS)
        return r;
    return vkBindBufferMemory(g_dev, *buf, *mem, 0);
}

static VkShaderModule make_shader(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_dev, &ci, NULL, &m);
    return m;
}

static bool has_dev_ext(const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_pdev, NULL, &n, NULL);
    VkExtensionProperties* e = calloc(n, sizeof(*e));
    vkEnumerateDeviceExtensionProperties(g_pdev, NULL, &n, e);
    bool found = false;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            found = true;
    free(e);
    return found;
}

int main(void) {
    const char* sel = getenv("VKH_DEVICE");
    int iters = getenv("VKH_ITER") ? atoi(getenv("VKH_ITER")) : 1000;
    uint32_t n = getenv("VKH_SIZE") ? (uint32_t)strtoul(getenv("VKH_SIZE"), NULL, 0) : 1u << 20;

    /* instance */
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "vk-headless", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app};
    CHECK("instance", vkCreateInstance(&ici, NULL, &g_inst));
    STEP("instance", true, "%s", "");

    /* physical devices */
    uint32_t ndev = 0;
    CHECK("enumerate", vkEnumeratePhysicalDevices(g_inst, &ndev, NULL));
    VkPhysicalDevice* devs = calloc(ndev ? ndev : 1, sizeof(*devs));
    CHECK("enumerate", vkEnumeratePhysicalDevices(g_inst, &ndev, devs));
    int pick = -1;
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("  device %u: %s (API %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (pick < 0) {
            if (!sel && i == 0)
                pick = 0;
            else if (sel && sel[strspn(sel, "0123456789")] == '\0' && *sel)
                pick = (uint32_t)atoi(sel) == i ? (int)i : -1;
            else if (sel && strcasestr(p.deviceName, sel))
                pick = (int)i;
        }
    }
    STEP("enumerate", pick >= 0, "%u devices, picked %d", ndev, pick);
    if (pick < 0)
        goto out;
    g_pdev = devs[pick];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_pdev, &props);
    vkGetPhysicalDeviceMemoryProperties(g_pdev, &g_memprops);
    printf("  using: %s\n", props.deviceName);
    for (uint32_t i = 0; i < g_memprops.memoryHeapCount; i++)
        printf("  heap %u: %" PRIu64 " MiB flags 0x%x\n", i, g_memprops.memoryHeaps[i].size >> 20,
               g_memprops.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        printf("  memtype %u: heap %u flags 0x%x\n", i, g_memprops.memoryTypes[i].heapIndex,
               g_memprops.memoryTypes[i].propertyFlags);

    /* logical device */
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, NULL);
    VkQueueFamilyProperties* qf = calloc(nq, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, qf);
    g_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            g_qfam = i;
            break;
        }
    if (g_qfam == UINT32_MAX) {
        STEP("device", false, "no graphics+compute queue family");
        goto out;
    }
    const char* exts[4];
    uint32_t next = 0;
    bool ext_mem = has_dev_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                   has_dev_ext(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    bool ext_sem = has_dev_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    if (ext_mem) {
        exts[next++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        exts[next++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    if (ext_sem)
        exts[next++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &f13, .timelineSemaphore = VK_TRUE};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = g_qfam, .queueCount = 1,
                                   .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
    CHECK("device", vkCreateDevice(g_pdev, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    STEP("device", true, "queue family %u", g_qfam);

    /* memory */
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize bytes = (VkDeviceSize)n * 4;
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (getenv("VKH_MEMFLAGS"))
        want = (VkMemoryPropertyFlags)strtoul(getenv("VKH_MEMFLAGS"), NULL, 0);
    CHECK("memory", make_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, want, &buf, &mem));
    uint32_t* data;
    CHECK("memory", vkMapMemory(g_dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&data));
    for (uint32_t i = 0; i < n; i++)
        data[i] = i;
    STEP("memory", true, "%" PRIu64 " KiB, memory flags 0x%x, mapped at %p", bytes >> 10, want,
         (void*)data);

    /* compute pipeline */
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = 1, .pBindings = &b};
    VkDescriptorSetLayout dsl;
    CHECK("compute", vkCreateDescriptorSetLayout(g_dev, &dslci, NULL, &dsl));
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 4};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout pl;
    CHECK("compute", vkCreatePipelineLayout(g_dev, &plci, NULL, &pl));
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = make_shader(compute_spv, sizeof(compute_spv)), .pName = "main"},
        .layout = pl};
    VkPipeline cpipe;
    CHECK("compute", vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpci, NULL, &cpipe));
    VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp;
    CHECK("compute", vkCreateDescriptorPool(g_dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dp, .descriptorSetCount = 1,
                                        .pSetLayouts = &dsl};
    VkDescriptorSet ds;
    CHECK("compute", vkAllocateDescriptorSets(g_dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = {.buffer = buf, .range = VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpoolci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = g_qfam};
    VkCommandPool cpool;
    CHECK("compute", vkCreateCommandPool(g_dev, &cpoolci, NULL, &cpool));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    VkCommandBuffer cb;
    CHECK("compute", vkAllocateCommandBuffers(g_dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
    vkCmdDispatch(cb, (n + 63) / 64, 1, 1);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK("compute", vkCreateFence(g_dev, &fci, NULL, &fence));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                       .pCommandBuffers = &cb};
    double t0 = now_s();
    CHECK("compute", vkQueueSubmit(g_queue, 1, &si, fence));
    CHECK("fence-wait", vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 5000000000ull));
    double t1 = now_s();
    STEP("fence-wait", true, "%.3f ms", (t1 - t0) * 1e3);
    uint32_t bad = 0, stale = 0, from_zero = 0, first_bad = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++)
        if (data[i] != i * 3u + 1u) {
            bad++;
            if (first_bad == UINT32_MAX)
                first_bad = i;
            if (data[i] == i)
                stale++; /* the CPU still sees its own value: the GPU's store is not visible */
            else if (data[i] == 1u)
                from_zero++; /* the GPU read 0: the CPU's store was not visible to it */
        }
    if (bad)
        STEP("compute", false, "%u of %u values wrong (first at %u; %u still the CPU's value, %u "
             "computed from 0, %u other)", bad, n, first_bad, stale, from_zero,
             bad - stale - from_zero);
    else
        STEP("compute", true, "0 of %u values wrong", n);

    /* timeline semaphore: GPU signals value 1 after an empty submit, host waits; then host signals 2 */
    VkSemaphoreTypeCreateInfo stci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stci};
    VkSemaphore tl;
    CHECK("timeline", vkCreateSemaphore(g_dev, &sci, NULL, &tl));
    uint64_t one = 1;
    VkTimelineSemaphoreSubmitInfo tssi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                          .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &one};
    VkSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tssi,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &tl};
    CHECK("timeline", vkQueueSubmit(g_queue, 1, &tsi, VK_NULL_HANDLE));
    VkSemaphoreWaitInfo swi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                               .pSemaphores = &tl, .pValues = &one};
    CHECK("timeline", vkWaitSemaphores(g_dev, &swi, 5000000000ull));
    VkSemaphoreSignalInfo ssi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = tl,
                                 .value = 2};
    CHECK("timeline", vkSignalSemaphore(g_dev, &ssi));
    uint64_t val = 0;
    CHECK("timeline", vkGetSemaphoreCounterValue(g_dev, tl, &val));
    /* a wait that must time out: value 3 is never signalled */
    uint64_t three = 3;
    swi.pValues = &three;
    VkResult tr = VK_TIMEOUT;
    if (getenv("VKH_NO_TIMEOUT_WAIT"))
        printf("  (VKH_NO_TIMEOUT_WAIT: the wait that must time out is skipped)\n");
    else
        tr = vkWaitSemaphores(g_dev, &swi, 10000000ull);
    STEP("timeline", val == 2 && tr == VK_TIMEOUT, "counter %" PRIu64 ", 10 ms wait on 3 -> %d (expect %d)",
         val, tr, VK_TIMEOUT);

    /* submit round trip: an empty command buffer with a fence, wait, reset; iters times */
    VkCommandBuffer ecb;
    CHECK("submit-latency", vkAllocateCommandBuffers(g_dev, &cbai, &ecb));
    vkBeginCommandBuffer(ecb, &cbbi);
    vkEndCommandBuffer(ecb);
    VkSubmitInfo esi = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                        .pCommandBuffers = &ecb};
    t0 = now_s();
    for (int i = 0; i < iters; i++) {
        CHECK("submit-latency", vkResetFences(g_dev, 1, &fence));
        CHECK("submit-latency", vkQueueSubmit(g_queue, 1, &esi, fence));
        CHECK("submit-latency", vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 5000000000ull));
    }
    t1 = now_s();
    STEP("submit-latency", true, "%d submit+wait of an empty command buffer: %.1f us each", iters,
         (t1 - t0) * 1e6 / (iters ? iters : 1));

    /* off-screen render: clear blue, draw the lower-left half red, copy to a buffer, count pixels */
    {
        const uint32_t W = 256, H = 256;
        VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VkImage img;
        CHECK("render", vkCreateImage(g_dev, &imci, NULL, &img));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_dev, img, &req);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize = req.size,
                                    .memoryTypeIndex = (uint32_t)find_memtype(
                                        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory imem;
        CHECK("render", vkAllocateMemory(g_dev, &mai, NULL, &imem));
        CHECK("render", vkBindImageMemory(g_dev, img, imem, 0));
        VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = VK_FORMAT_R8G8B8A8_UNORM,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView iv;
        CHECK("render", vkCreateImageView(g_dev, &ivci, NULL, &iv));
        VkBuffer rb;
        VkDeviceMemory rmem;
        CHECK("render", make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    &rb, &rmem));
        VkPipelineLayoutCreateInfo gplci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPipelineLayout gpl;
        CHECK("render", vkCreatePipelineLayout(g_dev, &gplci, NULL, &gpl));
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = make_shader(vert_spv, sizeof(vert_spv)),
             .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = make_shader(frag_spv, sizeof(frag_spv)),
             .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
        VkRect2D sc = {{0, 0}, {W, H}};
        VkPipelineViewportStateCreateInfo vps = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
            .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
        VkPipelineColorBlendStateCreateInfo cbs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
            .pAttachments = &cba};
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo prci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                              .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf};
        VkGraphicsPipelineCreateInfo gpci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prci, .stageCount = 2,
            .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pColorBlendState = &cbs, .layout = gpl};
        VkPipeline gp;
        CHECK("render", vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gpci, NULL, &gp));

        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &cbbi);
        VkImageMemoryBarrier2 toatt = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .image = img,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toatt};
        vkCmdPipelineBarrier2(cb, &dep);
        VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                         .imageView = iv,
                                         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .clearValue = {.color = {.float32 = {0, 0, 1, 1}}}};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc,
                              .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        VkImageMemoryBarrier2 tosrc = toatt;
        tosrc.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        tosrc.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        tosrc.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        tosrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        tosrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        tosrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dep.pImageMemoryBarriers = &tosrc;
        vkCmdPipelineBarrier2(cb, &dep);
        VkBufferImageCopy bic = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .imageExtent = {W, H, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bic);
        VkMemoryBarrier2 hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo hdep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                 .pMemoryBarriers = &hb};
        vkCmdPipelineBarrier2(cb, &hdep);
        vkEndCommandBuffer(cb);
        CHECK("render", vkResetFences(g_dev, 1, &fence));
        CHECK("render", vkQueueSubmit(g_queue, 1, &si, fence));
        CHECK("render", vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 5000000000ull));
        uint32_t* px;
        CHECK("render", vkMapMemory(g_dev, rmem, 0, VK_WHOLE_SIZE, 0, (void**)&px));
        uint32_t red = 0, blue = 0, other = 0;
        for (uint32_t i = 0; i < W * H; i++) {
            if (px[i] == 0xff0000ffu)
                red++;
            else if (px[i] == 0xffff0000u)
                blue++;
            else
                other++;
        }
        /* half of 65536 pixels, give or take the diagonal */
        bool ok = red > 32000 && red < 33600 && other == 0 && red + blue == W * H;
        STEP("render", ok, "red %u blue %u other %u", red, blue, other);
    }

    /* map churn: allocate, map, touch, unmap and free host-visible memory many times; in total far
     * more than the VM's GEM arenas hold (2 GiB by default), so it only passes if unmapped ranges
     * are reused */
    {
        int churn = getenv("VKH_CHURN") ? atoi(getenv("VKH_CHURN")) : 64;
        VkDeviceSize csize = 64ull << 20;
        int done = 0;
        VkResult cr = VK_SUCCESS;
        t0 = now_s();
        for (; done < churn; done++) {
            VkBuffer cbuf;
            VkDeviceMemory cmem;
            cr = make_buffer(csize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &cbuf, &cmem);
            if (cr != VK_SUCCESS)
                break;
            uint8_t* p8;
            cr = vkMapMemory(g_dev, cmem, 0, VK_WHOLE_SIZE, 0, (void**)&p8);
            if (cr == VK_SUCCESS) {
                p8[0] = 1;
                p8[csize - 1] = 2;
                vkUnmapMemory(g_dev, cmem);
            }
            vkDestroyBuffer(g_dev, cbuf, NULL);
            vkFreeMemory(g_dev, cmem, NULL);
            if (cr != VK_SUCCESS)
                break;
        }
        t1 = now_s();
        STEP("map-churn", done == churn, "%d of %d rounds of 64 MiB (%d MiB in total), %.1f ms; last result %d",
             done, churn, done * 64, (t1 - t0) * 1e3, cr);
    }

    /* one mapping larger than the VM's arenas: natively it works; in the VM it has to fail cleanly
     * (VK_ERROR_MEMORY_MAP_FAILED or an allocation error), not abort */
    {
        VkDeviceSize bsize = (getenv("VKH_BIGMAP_MIB") ? strtoull(getenv("VKH_BIGMAP_MIB"), NULL, 0)
                                                      : 3072ull) << 20;
        VkBuffer bbuf;
        VkDeviceMemory bmem;
        VkResult br = make_buffer(bsize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &bbuf, &bmem);
        VkResult mr = VK_NOT_READY;
        if (br == VK_SUCCESS) {
            uint8_t* p8;
            mr = vkMapMemory(g_dev, bmem, 0, VK_WHOLE_SIZE, 0, (void**)&p8);
            if (mr == VK_SUCCESS) {
                p8[0] = 1;
                p8[bsize - 1] = 2;
                vkUnmapMemory(g_dev, bmem);
            }
            vkDestroyBuffer(g_dev, bbuf, NULL);
            vkFreeMemory(g_dev, bmem, NULL);
        }
        STEP("big-map", true, "%" PRIu64 " MiB: allocate %d, map %d (%s)", bsize >> 20, br, mr,
             mr == VK_SUCCESS ? "mapped" : "refused cleanly");
    }

    /* export device memory as a dma-buf fd */
    if (ext_mem && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && access("/dev/udmabuf", R_OK)) {
        SKIP("export-dmabuf", "CPU device without /dev/udmabuf (lavapipe exports through it)");
    } else if (ext_mem) {
        PFN_vkGetMemoryFdKHR getMemoryFd =
            (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_dev, "vkGetMemoryFdKHR");
        VkExportMemoryAllocateInfo emai = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &emai,
                                    .allocationSize = 1 << 20,
                                    .memoryTypeIndex = (uint32_t)find_memtype(
                                        ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory xmem;
        CHECK_SOFT("export-dmabuf", vkAllocateMemory(g_dev, &mai, NULL, &xmem), sem_export);
        VkMemoryGetFdInfoKHR mgfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = xmem,
                                     .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1;
        CHECK_SOFT("export-dmabuf", getMemoryFd(g_dev, &mgfi, &fd), sem_export);
        struct stat st;
        int sr = fstat(fd, &st);
        off_t sz = lseek(fd, 0, SEEK_END);
        STEP("export-dmabuf", sr == 0 && sz >= (1 << 20), "fd %d, fstat %d (errno %d), size %lld", fd,
             sr, sr ? errno : 0, (long long)sz);
        close(fd);
        vkFreeMemory(g_dev, xmem, NULL);
    } else {
        SKIP("export-dmabuf", "no VK_KHR_external_memory_fd/VK_EXT_external_memory_dma_buf");
    }

sem_export:
    /* export semaphores: opaque fd (a DRM syncobj fd) and sync_file */
    if (ext_sem) {
        PFN_vkGetSemaphoreFdKHR getSemFd =
            (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(g_dev, "vkGetSemaphoreFdKHR");
        VkExportSemaphoreCreateInfo esci = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkSemaphoreCreateInfo xsci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci};
        VkSemaphore xs;
        CHECK_SOFT("export-syncobj", vkCreateSemaphore(g_dev, &xsci, NULL, &xs), done);
        VkSemaphoreGetFdInfoKHR sgfi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                        .semaphore = xs,
                                        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        int fd = -1;
        CHECK_SOFT("export-syncobj", getSemFd(g_dev, &sgfi, &fd), done);
        struct stat st;
        int sr = fstat(fd, &st);
        STEP("export-syncobj", sr == 0, "fd %d, fstat %d (errno %d)", fd, sr, sr ? errno : 0);
        close(fd);
        vkDestroySemaphore(g_dev, xs, NULL);

        esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        CHECK_SOFT("export-syncfile", vkCreateSemaphore(g_dev, &xsci, NULL, &xs), done);
        VkSubmitInfo ssi2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1,
                             .pSignalSemaphores = &xs};
        CHECK_SOFT("export-syncfile", vkResetFences(g_dev, 1, &fence), done);
        CHECK_SOFT("export-syncfile", vkQueueSubmit(g_queue, 1, &ssi2, fence), done);
        sgfi.semaphore = xs;
        sgfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        fd = -1;
        CHECK_SOFT("export-syncfile", getSemFd(g_dev, &sgfi, &fd), done);
        CHECK_SOFT("export-syncfile", vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 5000000000ull), done);
        sr = fd >= 0 ? fstat(fd, &st) : 0;
        /* fd -1 is valid: the payload was already signalled */
        STEP("export-syncfile", sr == 0, "fd %d, fstat %d (errno %d)", fd, sr, sr ? errno : 0);
        if (fd >= 0)
            close(fd);
        vkDestroySemaphore(g_dev, xs, NULL);
    } else {
        SKIP("export-syncobj", "no VK_KHR_external_semaphore_fd");
    }

    done:
    vkDeviceWaitIdle(g_dev);
    printf("RESULT %s: %d failed steps\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures;

out:
    printf("RESULT FAIL: stopped early, %d failed steps\n", g_failures);
    return g_failures ? g_failures : 1;
}
