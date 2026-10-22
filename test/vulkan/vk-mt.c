/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vk-mt: Vulkan from several threads (dev docs: vulkan/, blocker 7). Under saguaro-vm
 * every device call goes through the comm page; if a thread blocked in a long wait holds it, the
 * other threads' calls stall. Steps (one "STEP" line each, exit status = failed steps):
 *
 *  timeout-300ms:    a wait on a value nobody signals returns VK_TIMEOUT after about 300 ms.
 *  wait-host-signal: thread A waits (10 s timeout) on a timeline value nobody has signalled yet;
 *                    after 200 ms the main thread signals it from the host (vkSignalSemaphore).
 *                    Pass if A wakes within 1 s of the signal.
 *  wait-gpu-signal:  the same, but the main thread submits an empty batch that signals the value.
 * *  parallel-submit:  two threads each do N submit+wait rounds on one queue
 *                    with a mutex (Vulkan requires external synchronisation of a queue); reports
 *                    the time against one thread doing 2N rounds.
 *
 * Environment: VKH_DEVICE (index or name substring), VKH_ITER (rounds, default 500).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static int g_failures;
#define STEP(name, ok, ...)                                         \
    do {                                                            \
        char _label[64];                                            \
        snprintf(_label, sizeof(_label), "%s:", name);              \
        printf("STEP %-22s %s ", _label, (ok) ? "PASS" : "FAIL");   \
        printf(__VA_ARGS__);                                        \
        printf("\n");                                               \
        fflush(stdout);                                             \
        if (!(ok))                                                  \
            g_failures++;                                           \
    } while (0)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;

struct waiter {
    VkSemaphore sem;
    uint64_t value;
    VkResult result;
    double started_at, woke_at;
};

static void* wait_thread(void* arg) {
    struct waiter* w = arg;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &w->sem, .pValues = &w->value};
    w->started_at = now_s();
    w->result = vkWaitSemaphores(g_dev, &wi, 10000000000ull);
    w->woke_at = now_s();
    return NULL;
}

static VkSemaphore make_timeline(void) {
    VkSemaphoreTypeCreateInfo t = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &t};
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(g_dev, &ci, NULL, &s);
    return s;
}

static void wait_test(const char* name, bool gpu) {
    struct waiter w = {.sem = make_timeline(), .value = 1};
    pthread_t th;
    pthread_create(&th, NULL, wait_thread, &w);
    usleep(200000);
    double signalled = now_s();
    VkResult r;
    if (gpu) {
        uint64_t one = 1;
        VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                            .signalSemaphoreValueCount = 1,
                                            .pSignalSemaphoreValues = &one};
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts,
                           .signalSemaphoreCount = 1, .pSignalSemaphores = &w.sem};
        pthread_mutex_lock(&g_queue_lock);
        r = vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
        pthread_mutex_unlock(&g_queue_lock);
    } else {
        VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                    .semaphore = w.sem, .value = 1};
        r = vkSignalSemaphore(g_dev, &si);
    }
    double call_done = now_s();
    pthread_join(th, NULL);
    double latency = w.woke_at - signalled;
    STEP(name, r == VK_SUCCESS && w.result == VK_SUCCESS && latency < 1.0,
         "signal call %d took %.1f ms; waiter result %d after %.1f ms, woke %.1f ms after the signal",
         r, (call_done - signalled) * 1e3, w.result, (w.woke_at - w.started_at) * 1e3,
         latency * 1e3);
    vkDestroySemaphore(g_dev, w.sem, NULL);
}

struct submitter {
    int rounds;
    VkCommandBuffer cb;
    VkFence fence;
    int failed;
};

static void* submit_thread(void* arg) {
    struct submitter* s = arg;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                       .pCommandBuffers = &s->cb};
    for (int i = 0; i < s->rounds; i++) {
        pthread_mutex_lock(&g_queue_lock);
        VkResult r = vkResetFences(g_dev, 1, &s->fence);
        if (r == VK_SUCCESS)
            r = vkQueueSubmit(g_queue, 1, &si, s->fence);
        pthread_mutex_unlock(&g_queue_lock);
        if (r == VK_SUCCESS)
            r = vkWaitForFences(g_dev, 1, &s->fence, VK_TRUE, 5000000000ull);
        if (r != VK_SUCCESS) {
            s->failed = r;
            return NULL;
        }
    }
    return NULL;
}

int main(void) {
    const char* sel = getenv("VKH_DEVICE");
    int iters = getenv("VKH_ITER") ? atoi(getenv("VKH_ITER")) : 500;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "vk-mt", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        STEP("instance", false, "vkCreateInstance failed");
        return 1;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice devs[16];
    if (n > 16)
        n = 16;
    vkEnumeratePhysicalDevices(inst, &n, devs);
    int pick = -1;
    for (uint32_t i = 0; i < n && pick < 0; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (!sel || (sel[strspn(sel, "0123456789")] == '\0' ? (uint32_t)atoi(sel) == i
                                                            : strcasestr(p.deviceName, sel) != NULL)) {
            pick = (int)i;
            printf("  using: %s\n", p.deviceName);
        }
    }
    if (pick < 0) {
        STEP("device", false, "no device matches");
        return 1;
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[pick], &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16)
        nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[pick], &nq, qf);
    for (g_qfam = 0; g_qfam < nq && !(qf[g_qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_qfam++)
        ;
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = g_qfam, .queueCount = 1,
                                   .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    if (vkCreateDevice(devs[pick], &dci, NULL, &g_dev) != VK_SUCCESS) {
        STEP("device", false, "vkCreateDevice failed");
        return 1;
    }
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);

    /* a single-threaded 300 ms timeout: checks that deadlines are honoured at all */
    {
        struct waiter w = {.sem = make_timeline(), .value = 1};
        VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                  .semaphoreCount = 1, .pSemaphores = &w.sem, .pValues = &w.value};
        double t0 = now_s();
        VkResult r = vkWaitSemaphores(g_dev, &wi, 300000000ull);
        double dt = now_s() - t0;
        STEP("timeout-300ms", r == VK_TIMEOUT && dt > 0.25 && dt < 1.0, "result %d after %.1f ms",
             r, dt * 1e3);
        vkDestroySemaphore(g_dev, w.sem, NULL);
    }

    wait_test("wait-host-signal", false);
    wait_test("wait-gpu-signal", true);

    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = g_qfam};
    VkCommandPool pool;
    vkCreateCommandPool(g_dev, &cpci, NULL, &pool);
    struct submitter s[2] = {{0}};
    for (int i = 0; i < 2; i++) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        vkAllocateCommandBuffers(g_dev, &ai, &s[i].cb);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(s[i].cb, &bi);
        vkEndCommandBuffer(s[i].cb);
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(g_dev, &fci, NULL, &s[i].fence);
    }
    s[0].rounds = 2 * iters;
    double t0 = now_s();
    submit_thread(&s[0]);
    double one = now_s() - t0;
    s[0].rounds = s[1].rounds = iters;
    pthread_t th[2];
    t0 = now_s();
    for (int i = 0; i < 2; i++)
        pthread_create(&th[i], NULL, submit_thread, &s[i]);
    for (int i = 0; i < 2; i++)
        pthread_join(th[i], NULL);
    double two = now_s() - t0;
    STEP("parallel-submit", !s[0].failed && !s[1].failed,
         "%d rounds: one thread %.1f ms, two threads %.1f ms (%.0f%%)", 2 * iters, one * 1e3,
         two * 1e3, 100.0 * two / one);

    vkDeviceWaitIdle(g_dev);
    printf("RESULT %s: %d failed steps\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures;
}
