#define _GNU_SOURCE
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include "input_ring.h"
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/xfixes.h>

#include <X11/xshmfence.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>

#include <sys/ioctl.h>
#include <X11/xshmfence.h>

#ifndef QEMU_LISTENER
#include "common.h"
#else 
#include "include/qemu/sg.h"
#endif 

#ifdef BENCHMARKING
static uint64_t display_latency_sum = 0;
static uint64_t display_latency_count = 0;
#endif

#ifdef QEMU_LISTENER
static xcb_render_pictformat_t xrgb8888_format;
static xcb_render_picture_t window_picture;
static xcb_render_picture_t buffer_pictures[NUM_BUFFERS];
static xcb_pixmap_t picture_pixmaps[NUM_BUFFERS];
static int picture_heights[NUM_BUFFERS];
static xcb_window_t application_window;
static xcb_window_t requested_application_window;
static xcb_atom_t net_active_window_atom;
static bool application_activation_pending;
static bool activation_monitor_initialized;
static bool application_was_active;
static bool input_thread_started;
static bool active_window_changed;
static xcb_window_t requested_input_window;
static uint32_t requested_input_mask;
static xcb_window_t applied_input_window;
static uint32_t applied_input_mask;
static sg_input_ring_t *shared_input_ring;
static sg_input_latency_t *shared_input_latency;
static bool input_latency_measurement;
static uint32_t input_latency_click_index;
static uint64_t guest_tsc_offset;
static uint32_t guest_tsc_frequency_khz;
static bool guest_tsc_translation_available;

static uint64_t read_tsc(void) {
    uint32_t low;
    uint32_t high;

    __asm__ volatile("lfence; rdtscp; lfence"
                     : "=a"(low), "=d"(high) :: "rcx", "memory");
    return ((uint64_t)high << 32) | low;
}

static uint64_t monotonic_raw_ns(void) {
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp) != 0)
        return 0;
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
           (uint64_t)timestamp.tv_nsec;
}

static void measure_input_action(uint32_t target_sequence,
                                 uint64_t receive_ns,
                                 uint64_t receive_tsc,
                                 uint64_t publish_tsc) {
    if (!input_latency_measurement)
        return;

    /* This mode is only enabled by the click-latency experiment. The test app
     * records delivery and action timestamps in the translated guest TSC
     * domain, then advances processed_sequence after applying its action. */
    while (__atomic_load_n(&shared_input_latency->processed_sequence,
                           __ATOMIC_ACQUIRE) != target_sequence)
        __asm__ volatile("pause");

    uint32_t delivered_sequence =
        __atomic_load_n(&shared_input_latency->delivered_sequence,
                        __ATOMIC_ACQUIRE);
    uint64_t delivery_tsc =
        __atomic_load_n(&shared_input_latency->delivery_tsc, __ATOMIC_RELAXED);
    uint64_t action_tsc =
        __atomic_load_n(&shared_input_latency->action_tsc, __ATOMIC_RELAXED);
    fprintf(stdout,
            "gramine_input_timing index=%u receive_ns=%" PRIu64
            " receive_tsc=%" PRIu64 " publish_tsc=%" PRIu64
            " delivery_tsc=%" PRIu64 " action_tsc=%" PRIu64
            " delivered_sequence=%u tsc_khz=%u\n",
            input_latency_click_index++, receive_ns, receive_tsc, publish_tsc,
            delivery_tsc, action_tsc, delivered_sequence,
            guest_tsc_frequency_khz);
    fflush(stdout);
}

static bool is_redirected_input_event(uint8_t type) {
    return type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE ||
           type == XCB_BUTTON_PRESS || type == XCB_BUTTON_RELEASE ||
           type == XCB_MOTION_NOTIFY;
}

static void update_shared_motion(const sg_input_event_t *motion, bool is_event) {
    uint32_t seq = __atomic_load_n(&shared_input_ring->motion_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&shared_input_ring->motion_seq, seq + 1, __ATOMIC_RELEASE);
    shared_input_ring->motion = *motion;
    __atomic_store_n(&shared_input_ring->motion_seq, seq + 2, __ATOMIC_RELEASE);
    if (is_event)
        __atomic_add_fetch(&shared_input_ring->motion_serial, 1, __ATOMIC_RELEASE);
}

static void publish_shared_input_event(const xcb_generic_event_t *generic,
                                       uint64_t receive_ns,
                                       uint64_t receive_tsc) {
    if (!shared_input_ring ||
            __atomic_load_n(&shared_input_ring->enabled, __ATOMIC_ACQUIRE) == 0)
        return;

    uint8_t type = generic->response_type & 0x7f;
    if (!is_redirected_input_event(type))
        return;

    const xcb_key_press_event_t *event = (const xcb_key_press_event_t *)generic;
    sg_input_event_t compact = {
        .type = type,
        .detail = event->detail,
        .time = event->time,
        .window = event->event,
        .root = event->root,
        .child = event->child,
        .root_x = event->root_x,
        .root_y = event->root_y,
        .event_x = event->event_x,
        .event_y = event->event_y,
        .state = event->state,
        .same_screen = event->same_screen,
    };

    if (type == XCB_MOTION_NOTIFY) {
        update_shared_motion(&compact, true);
        return;
    }

    uint32_t head = __atomic_load_n(&shared_input_ring->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&shared_input_ring->tail, __ATOMIC_RELAXED);
    if (tail - head >= SG_INPUT_RING_CAPACITY) {
        __atomic_add_fetch(&shared_input_ring->dropped, 1, __ATOMIC_RELAXED);
        return;
    }

    bool measured_click = input_latency_measurement &&
        type == XCB_BUTTON_PRESS && event->detail == XCB_BUTTON_INDEX_1;
    shared_input_ring->events[tail % SG_INPUT_RING_CAPACITY] = compact;
    uint64_t publish_tsc = measured_click && guest_tsc_translation_available
        ? read_tsc() + guest_tsc_offset : 0;
    __atomic_store_n(&shared_input_ring->tail, tail + 1, __ATOMIC_RELEASE);

    if (measured_click)
        measure_input_action(input_latency_click_index + 1, receive_ns,
                             receive_tsc, publish_tsc);
}

static void *shared_input_thread(void *unused) {
    (void)unused;

    for (;;) {
        xcb_generic_event_t *event = xcb_wait_for_event(conn);
        if (!event)
            break;

        uint8_t type = event->response_type & 0x7f;
        if (type == XCB_PROPERTY_NOTIFY) {
            xcb_property_notify_event_t *property = (xcb_property_notify_event_t *)event;
            if (property->atom == net_active_window_atom)
                __atomic_store_n(&active_window_changed, true, __ATOMIC_RELEASE);
        } else {
            uint64_t receive_ns = 0;
            uint64_t receive_tsc = 0;
            const xcb_key_press_event_t *input =
                (const xcb_key_press_event_t *)event;
            if (input_latency_measurement &&
                    type == XCB_BUTTON_PRESS &&
                    input->detail == XCB_BUTTON_INDEX_1) {
                receive_ns = monotonic_raw_ns();
                if (guest_tsc_translation_available)
                    receive_tsc = read_tsc() + guest_tsc_offset;
            }
            publish_shared_input_event(event, receive_ns, receive_tsc);
        }
        free(event);
    }
    return NULL;
}

static void start_shared_input_thread(void) {
    if (input_thread_started)
        return;

    pthread_t thread;
    if (pthread_create(&thread, NULL, shared_input_thread, NULL) != 0) {
        fprintf(stderr, "[input] could not start shared input producer\n");
        return;
    }
    pthread_detach(thread);
    input_thread_started = true;
}

static void apply_shared_input_mask(void) {
    if (!conn || requested_input_window == XCB_NONE ||
            (applied_input_window == requested_input_window &&
             applied_input_mask == requested_input_mask))
        return;

    uint32_t mask = requested_input_mask;
    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
        conn, requested_input_window, XCB_CW_EVENT_MASK, &mask);
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);
    if (error) {
        /* Window creation is delivered over a separate VSOCK connection and can race this setup.
         * Keep the request pending; attach_presentation_window() retries after the window exists. */
        free(error);
        return;
    }

    applied_input_window = requested_input_window;
    applied_input_mask = requested_input_mask;
    if (shared_input_ring && requested_input_mask != 0) {
        xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(
            conn, xcb_query_pointer(conn, requested_input_window), NULL);
        if (pointer) {
            sg_input_event_t initial = {
                .type = XCB_MOTION_NOTIFY,
                .time = XCB_CURRENT_TIME,
                .window = requested_input_window,
                .root = pointer->root,
                .child = pointer->child,
                .root_x = pointer->root_x,
                .root_y = pointer->root_y,
                .event_x = pointer->win_x,
                .event_y = pointer->win_y,
                .state = pointer->mask,
                .same_screen = pointer->same_screen,
            };
            /* Seed queryable pointer state without synthesizing a MotionNotify event. */
            update_shared_motion(&initial, false);
            free(pointer);
        }
    }
    if (shared_input_ring) {
        __atomic_store_n(&shared_input_ring->enabled, requested_input_mask != 0,
                         __ATOMIC_RELEASE);
        fprintf(stderr, "[input] shared input %s for window %#x (mask %#x)\n",
                requested_input_mask ? "enabled" : "disabled",
                requested_input_window, requested_input_mask);
    }
    xcb_flush(conn);
}

void set_shared_input_mask(xcb_window_t window, uint32_t event_mask) {
    requested_input_window = window;
    requested_input_mask = event_mask;
    applied_input_window = XCB_NONE;
    applied_input_mask = 0;
    apply_shared_input_mask();
}

static bool make_presentation_window_input_transparent(void) {
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(conn, &xcb_xfixes_id);
    if (!extension || !extension->present) {
        fprintf(stderr, "[xcb] XFixes unavailable; presentation child may intercept input\n");
        return false;
    }

    xcb_xfixes_query_version_reply_t *version =
        xcb_xfixes_query_version_reply(
            conn, xcb_xfixes_query_version(conn, 5, 0), NULL);
    if (!version) {
        fprintf(stderr, "[xcb] could not negotiate XFixes for input transparency\n");
        return false;
    }
    free(version);

    xcb_xfixes_region_t empty_region = xcb_generate_id(conn);
    xcb_void_cookie_t create =
        xcb_xfixes_create_region_checked(conn, empty_region, 0, NULL);
    xcb_generic_error_t *error = xcb_request_check(conn, create);
    if (error) {
        fprintf(stderr, "[xcb] could not create empty input region (error %u)\n",
                error->error_code);
        free(error);
        return false;
    }

    xcb_void_cookie_t set_shape = xcb_xfixes_set_window_shape_region_checked(
        conn, win, XCB_SHAPE_SK_INPUT, 0, 0, empty_region);
    error = xcb_request_check(conn, set_shape);
    xcb_xfixes_destroy_region(conn, empty_region);
    if (error) {
        fprintf(stderr, "[xcb] could not make presentation child input-transparent (error %u)\n",
                error->error_code);
        free(error);
        return false;
    }
    return true;
}

static void attach_presentation_window(void) {
    if (requested_application_window == XCB_NONE ||
            requested_application_window == application_window)
        return;

    /* The guest X connection is relayed independently from the listener's
     * host connection. An UPDATE_WINDOW_SIZE message can therefore arrive
     * before Xwayland has processed creation of the SDL window. Do not commit
     * the parent until it exists, and retry from xcb_present(). */
    xcb_get_window_attributes_reply_t *attributes =
        xcb_get_window_attributes_reply(
            conn, xcb_get_window_attributes(conn, requested_application_window), NULL);
    if (!attributes)
        return;
    free(attributes);

    xcb_void_cookie_t reparent =
        xcb_reparent_window_checked(conn, win, requested_application_window, 0, 0);
    xcb_generic_error_t *error = xcb_request_check(conn, reparent);
    if (error) {
        free(error);
        return;
    }

    application_window = requested_application_window;
    application_activation_pending = true;
    application_was_active = false;
    uint32_t values[2] = {(uint32_t)win_width, (uint32_t)win_height};
    xcb_configure_window(conn, win,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    xcb_map_window(conn, win);
    apply_shared_input_mask();
    xcb_flush(conn);
}

static void activate_application_window(void) {
    if (!application_activation_pending || application_window == XCB_NONE)
        return;

    xcb_get_window_attributes_reply_t *attributes =
        xcb_get_window_attributes_reply(conn,
                                        xcb_get_window_attributes(conn, application_window),
                                        NULL);
    if (!attributes)
        return;

    bool is_viewable = attributes->map_state == XCB_MAP_STATE_VIEWABLE;
    free(attributes);
    if (!is_viewable)
        return;

    if (net_active_window_atom == XCB_NONE) {
        static const char atom_name[] = "_NET_ACTIVE_WINDOW";
        xcb_intern_atom_reply_t *atom_reply =
            xcb_intern_atom_reply(conn,
                                  xcb_intern_atom(conn, 0, sizeof(atom_name) - 1, atom_name),
                                  NULL);
        if (!atom_reply)
            return;
        net_active_window_atom = atom_reply->atom;
        free(atom_reply);
    }

    xcb_screen_t *screen =
        (xcb_screen_t *)xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    if (!activation_monitor_initialized) {
        uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(conn, screen->root, XCB_CW_EVENT_MASK,
                                     &event_mask);
        activation_monitor_initialized = true;
    }

    xcb_client_message_event_t event = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = application_window,
        .type = net_active_window_atom,
        .data.data32 = {
            1, /* normal application */
            XCB_CURRENT_TIME,
            XCB_NONE,
            0,
            0,
        },
    };
    xcb_send_event(conn, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char *)&event);

    /* Let the compositor apply focus policy through the EWMH request. Do not
     * force focus from QEMU's separate X11 client; wlroots compositors own the
     * corresponding Wayland focus state. */
    application_activation_pending = false;
}

static void monitor_application_activation(void) {
    if (!activation_monitor_initialized || net_active_window_atom == XCB_NONE ||
            application_window == XCB_NONE)
        return;

    if (!__atomic_exchange_n(&active_window_changed, false, __ATOMIC_ACQ_REL))
        return;

    xcb_screen_t *screen =
        (xcb_screen_t *)xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        conn,
        xcb_get_property(conn, 0, screen->root, net_active_window_atom,
                         XCB_ATOM_WINDOW, 0, 1),
        NULL);
    if (!reply)
        return;

    bool is_active = false;
    if (reply->format == 32 && xcb_get_property_value_length(reply) >= 4) {
        xcb_window_t active_window =
            *(xcb_window_t *)xcb_get_property_value(reply);
        is_active = active_window == application_window;
    }
    free(reply);

    if (is_active && !application_was_active) {
        /* Some wlroots/Xwayland focus paths update the server focus and
         * _NET_ACTIVE_WINDOW without delivering FocusIn to older direct-Xlib
         * clients. Notify the already-focused application; this does not move
         * X or Wayland focus and therefore cannot steal it from another app. */
        xcb_focus_in_event_t focus = {
            .response_type = XCB_FOCUS_IN,
            .detail = XCB_NOTIFY_DETAIL_NONLINEAR,
            .event = application_window,
            .mode = XCB_NOTIFY_MODE_NORMAL,
        };
        xcb_send_event(conn, 0, application_window,
                       XCB_EVENT_MASK_FOCUS_CHANGE, (const char *)&focus);
    }
    application_was_active = is_active;
}

void set_presentation_window(xcb_window_t window) {
    if (window == XCB_NONE)
        return;

    /* Keep the imported DMA-BUF surface as a child of the application's real
     * top-level window. The child is invisible to the window manager and has
     * an empty XFixes input region, so focus and input remain owned by the
     * application window. */
    requested_application_window = window;
    attach_presentation_window();
    activate_application_window();
    monitor_application_activation();
}

static bool setup_render_format(void) {
    if (xrgb8888_format != XCB_NONE)
        return true;

    xcb_render_query_pict_formats_reply_t *reply =
        xcb_render_query_pict_formats_reply(conn,
                                             xcb_render_query_pict_formats(conn), NULL);
    if (!reply)
        return false;

    for (xcb_render_pictforminfo_iterator_t it =
             xcb_render_query_pict_formats_formats_iterator(reply);
         it.rem; xcb_render_pictforminfo_next(&it)) {
        const xcb_render_pictforminfo_t *format = it.data;
        if (format->type == XCB_RENDER_PICT_TYPE_DIRECT && format->depth == 24 &&
            format->direct.red_shift == 16 && format->direct.red_mask == 0xff &&
            format->direct.green_shift == 8 && format->direct.green_mask == 0xff &&
            format->direct.blue_shift == 0 && format->direct.blue_mask == 0xff &&
            format->direct.alpha_mask == 0) {
            xrgb8888_format = format->id;
            break;
        }
    }
    free(reply);
    return xrgb8888_format != XCB_NONE;
}

static bool render_flipped_pixmap(check *buffers, int index) {
    if (!setup_render_format())
        return false;

    if (window_picture == XCB_NONE) {
        window_picture = xcb_generate_id(conn);
        xcb_render_create_picture(conn, window_picture,
                                  win, xrgb8888_format, 0, NULL);
    }
    bool new_picture = picture_pixmaps[index] != buffers[index].pixmap;
    if (new_picture) {
        if (buffer_pictures[index] != XCB_NONE)
            xcb_render_free_picture(conn, buffer_pictures[index]);
        buffer_pictures[index] = xcb_generate_id(conn);
        picture_pixmaps[index] = buffers[index].pixmap;
        xcb_render_create_picture(conn, buffer_pictures[index], buffers[index].pixmap,
                                  xrgb8888_format, 0, NULL);
    }

    /* A buffer picture can be created while the listener window still has its
     * default height. Refresh the Y-flip transform after the durable SDL
     * window dimensions arrive; the pixmap itself need not change. */
    if (new_picture || picture_heights[index] != win_height) {
        xcb_render_transform_t vertical_flip = {
            .matrix11 = 1 << 16,
            .matrix22 = -(1 << 16),
            .matrix23 = win_height << 16,
            .matrix33 = 1 << 16,
        };
        xcb_render_set_picture_transform(conn, buffer_pictures[index], vertical_flip);
        picture_heights[index] = win_height;
    }

    xcb_render_composite(conn, XCB_RENDER_PICT_OP_SRC,
                         buffer_pictures[index], XCB_NONE, window_picture,
                         0, 0, 0, 0, 0, 0, win_width, win_height);
    return true;
}
#endif

void xcb_present(check *bufs, int cur) {
    #ifdef BENCHMARKING
        struct timespec display_start, display_end;
        clock_gettime(CLOCK_MONOTONIC, &display_start);
    #endif
#ifndef QEMU_LISTENER
    /* Reduce flush frequency outside the VM so X requests can be batched. */
    static int flush_counter = 0;
#endif

#ifdef QEMU_LISTENER
    /* The guest and listener use separate X connections, so the first window
     * update can arrive before the guest's map request is visible. Retry the
     * one-shot activation on presentation until the top-level is viewable. */
    attach_presentation_window();
    activate_application_window();
    monitor_application_activation();

    /* Xwayland accepts DRI3 Present requests for this cross-process pixmap but can leave the
     * managed window's backing store unchanged. XRender gives us a deterministic GPU-side copy
     * and corrects the scanout buffer's bottom-up row order at the same time. */
    static xcb_gcontext_t blit_gc = XCB_NONE;
    if (blit_gc == XCB_NONE) {
        blit_gc = xcb_generate_id(conn);
        uint32_t gc_values[] = {0};
        xcb_create_gc(conn, blit_gc, win, XCB_GC_GRAPHICS_EXPOSURES, gc_values);
    }
    if (!render_flipped_pixmap(bufs, cur))
        xcb_copy_area(conn, bufs[cur].pixmap, win, blit_gc,
                      0, 0, 0, 0, win_width, win_height);
#else
    xshmfence_trigger(bufs[cur].shm_fence);
#ifdef IDLE_FENCE_WAIT
    if (bufs[cur].idle_fence)
        xshmfence_reset(bufs[cur].idle_fence);
    xcb_present_pixmap(conn, win, bufs[cur].pixmap, 0, XCB_NONE, XCB_NONE,
                       0, 0, XCB_NONE, bufs[cur].sync_fence,
                       bufs[cur].idle_sync_fence ? bufs[cur].idle_sync_fence : XCB_NONE,
                       XCB_PRESENT_OPTION_ASYNC, 0, 0, 0, 0, NULL);
#else
    xcb_present_pixmap(conn, win, bufs[cur].pixmap, 0, XCB_NONE, XCB_NONE,
                       0, 0, XCB_NONE, bufs[cur].sync_fence, XCB_NONE,
                       0, 0, 0, 0, 0, NULL);
#endif
#endif

    /* The VM presenter uses a separate host X connection. Flush its async
     * requests every frame so menu/idle applications do not need an unrelated
     * input event (or six future frames) before the latest image becomes
     * visible. This is not an X11 round trip. */
#ifdef QEMU_LISTENER
    xcb_flush(conn);
#else
    if (++flush_counter >= 6) {
        xcb_flush(conn);
        flush_counter = 0;
    }
#endif

    #ifdef BENCHMARKING
        xcb_flush(conn);
        clock_gettime(CLOCK_MONOTONIC, &display_end);
        uint64_t present_ns = (display_end.tv_sec - display_start.tv_sec) * 1000000000ULL +
                            (display_end.tv_nsec - display_start.tv_nsec);
        display_latency_sum += present_ns;
        display_latency_count++;
        if (display_latency_count >= STATS_INTERVAL) {
            double avg_ms = (display_latency_sum / display_latency_count) / 1000000.0;
            fprintf(stderr, "[display_latency] Avg over %lu frames: %.2f ms\n",
                    display_latency_count, avg_ms);
            display_latency_sum = 0;
            display_latency_count = 0;
        }
    #endif
}

int query_idle_fence_buffer(check* bufs, int cur) {
#ifdef IDLE_FENCE_WAIT
    /* Find a free idle_fence buffer (or wait) */
    if (bufs[cur].idle_fence) {
        if (!xshmfence_query(bufs[cur].idle_fence)) {
            int found = -1;
            for (int i = 0; i < NUM_BUFFERS; ++i) {
                if (bufs[i].idle_fence && xshmfence_query(bufs[i].idle_fence)) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                cur = found;
            } else {
                if (conn) xcb_flush(conn);
                xshmfence_await(bufs[cur].idle_fence);
            }
        }
        if (bufs[cur].shm_fence) xshmfence_reset(bufs[cur].shm_fence);
    }
#endif 
    return cur;
}

void update_window(void) {
    fprintf(stderr, "Updating window size width = %d height = %d -- %dx%d\n",
           win_width, win_height, win_width, win_height);

#ifdef QEMU_LISTENER
    if (conn && win != XCB_NONE) {
        uint32_t values[2] = {(uint32_t)win_width, (uint32_t)win_height};
        xcb_configure_window(conn, win,
                            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                            values);
        xcb_flush(conn);
    }
#endif
}

#ifndef QEMU_LISTENER
static int xlib_error_flag = 0;
static xcb_window_t guest_app_window;
static bool guest_app_window_changed;
static bool guest_app_window_creation_in_progress;

void use_native_presentation_window(GLXDrawable drawable) {
    if (!conn || in_gramine_vm || guest_app_window_creation_in_progress)
        return;

    xcb_window_t target = guest_app_window != XCB_NONE
        ? guest_app_window : (xcb_window_t)drawable;
    if (target == XCB_NONE || target == win)
        return;

    win = target;

    /* Native presentation targets the application's real X11 window. */
    xcb_present_select_input(conn, win,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY, 0);
    xcb_flush(conn);
}

void set_guest_app_window(xcb_window_t window) {
    if (window == guest_app_window)
        return;
    guest_app_window = window;
    guest_app_window_changed = true;
    use_native_presentation_window(window);
}

void set_guest_app_window_creation_in_progress(bool in_progress) {
    guest_app_window_creation_in_progress = in_progress;
}

bool consume_guest_app_window_change(void) {
    bool changed = guest_app_window_changed;
    guest_app_window_changed = false;
    return changed;
}

static int xlib_error_handler(Display *dpy, XErrorEvent *ev) {
    (void)dpy; (void)ev;
    xlib_error_flag = 1;
    return 0;
}

void update_vm_window(GLXDrawable drawable) {
    __sync_synchronize();
    long _offset = acquire_libos_lock();
    comm_page_t *c = comm_page(_offset);

    c->p1 = win_width;
    c->p2 = win_height;
    /* SDL may use a GLXWindow ID distinct from the real X11 Window. Prefer
     * SDL_GetWindowWMInfo's native window so the listener presents into the
     * focus/input-owning application window; Xlib applications can use their
     * drawable directly. */
    c->p3 = guest_app_window_creation_in_progress ? XCB_NONE :
            (guest_app_window != XCB_NONE ? guest_app_window : drawable);
    c->req_bit = UPDATE_WINDOW_SIZE;
    comm_sync_notify(c);
    relinquish_libos_lock(_offset);
}

void update_window_size_from_drawable(Display *dpy, GLXDrawable drawable) {
    XWindowAttributes attrs;
    if (!dpy || !drawable)
        return;

    /* SDL creates an internal 32x32 GLX drawable before its real X11 window
     * exists. SDL_CreateWindow's wrapper has already supplied the requested
     * dimensions, so do not replace them with this transient size. */
    if (in_gramine_vm && guest_app_window_creation_in_progress)
        return;

    /* Prefer querying GLX drawable size first (avoids touching X window
     * attributes for non-window drawables). */
    unsigned int w = 0, h = 0;
    glXQueryDrawable(dpy, drawable, GLX_WIDTH, &w);
    glXQueryDrawable(dpy, drawable, GLX_HEIGHT, &h);
    if (w > 0 && h > 0) {
        if (win_width == (int)w && win_height == (int)h)
            return;
        win_width = (int)w;
        win_height = (int)h;
        fprintf(stderr, "Using drawable size: %dx%d\n", win_width, win_height);
        if(in_gramine_vm) {
            update_vm_window(drawable);
        } else {
            update_window();
        }
        return;
    }

    /* Fallback: try XGetWindowAttributes but guard against BadWindow errors
     * which can occur when the drawable is not a valid X Window for this
     * Display connection. Use a temporary X error handler to detect failures
     * without crashing the process. */
    xlib_error_flag = 0;
    int (*old_handler)(Display *, XErrorEvent *) = XSetErrorHandler(xlib_error_handler);
    XGetWindowAttributes(dpy, (Window)drawable, &attrs);
    XSync(dpy, False);
    XSetErrorHandler(old_handler);

    if (xlib_error_flag) {
        fprintf(stderr, "[!] XGetWindowAttributes failed for drawable (likely BadWindow); skipping size update\n");
        return;
    }

    if (attrs.width <= 0 || attrs.height <= 0)
        return;

    if (win_width == attrs.width && win_height == attrs.height)
        return;

    win_width = attrs.width;
    win_height = attrs.height;
    fprintf(stderr, "Using drawable size: %dx%d\n", win_width, win_height);
    if(in_gramine_vm) {
        update_vm_window(drawable);
    } else {
        update_window();
    }
}
#endif 

void create_pixmap_from_kbuf(check *bufs, int i,
                            uint32_t size_bytes, uint32_t stride) {
    /* Generate pixmap */
    bufs[i].pixmap = xcb_generate_id(conn);
    xcb_dri3_pixmap_from_buffer(conn, bufs[i].pixmap, win, size_bytes, 
                win_width, win_height, stride, 24, 32, bufs[i].bo_fd); 
    xcb_flush(conn);

    /* Debugging */
    fprintf(stderr, "pixmap %d allocated (buffer = %d)\n", bufs[i].pixmap, i);
}

int create_xcb_fence(check *bufs, int i) {
    /* Create an xshmfence and register it as an X sync fence for this pixmap */
    bufs[i].shm_fence_fd = xshmfence_alloc_shm(); // ----- (1)
    if (bufs[i].shm_fence_fd < 0) {
        perror("xshmfence_alloc_shm");
        assert(false);
    }
    bufs[i].shm_fence = xshmfence_map_shm(bufs[i].shm_fence_fd);
    if (!bufs[i].shm_fence) {
        fprintf(stderr, "xshmfence_map_shm failed\n");
        assert(false);
    }
    xshmfence_reset(bufs[i].shm_fence); // start unsignaled
    bufs[i].sync_fence = xcb_generate_id(conn);
    xcb_dri3_fence_from_fd_checked(conn, bufs[i].pixmap, bufs[i].sync_fence, 0,
                                bufs[i].shm_fence_fd);
    xcb_flush(conn);
    fprintf(stderr, "xcb fence allocated (buffer = %d)\n", i);

    /* Create idle fence so we can wait for X to release this pixmap */
    bufs[i].idle_fence_fd = xshmfence_alloc_shm();
    if (bufs[i].idle_fence_fd < 0) {
        perror("xshmfence_alloc_shm (idle)");
        assert(false);
    }
    bufs[i].idle_fence = xshmfence_map_shm(bufs[i].idle_fence_fd);
    if (!bufs[i].idle_fence) {
        fprintf(stderr, "xshmfence_map_shm failed (idle)\n");
        assert(false);
    }
    /* Start signaled so the first render doesn't block */
    xshmfence_trigger(bufs[i].idle_fence);
    bufs[i].idle_sync_fence = xcb_generate_id(conn);
    xcb_dri3_fence_from_fd_checked(conn, bufs[i].pixmap,
                                    bufs[i].idle_sync_fence, 1,
                                    bufs[i].idle_fence_fd);
    xcb_flush(conn);

    return 0;
}

void create_and_setup_xcb_window(void) {
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "xcb_connect failed\n");
        return;
    }

    xcb_screen_t *screen = (xcb_screen_t *)xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
#ifdef QEMU_LISTENER
    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {screen->black_pixel, XCB_EVENT_MASK_EXPOSURE};
    int16_t x = (screen->width_in_pixels - win_width) / 2;
    int16_t y = (screen->height_in_pixels - win_height) / 2;
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, x, y,
                        win_width, win_height, 0,
                        XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        screen->root_visual, mask, values);

    /* Set window title */
    const char *title = "XCB Demo Window";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, strlen(title), title);
    application_window = XCB_NONE;
    requested_application_window = XCB_NONE;
    shared_input_ring = (sg_input_ring_t *)(uintptr_t)(COMM_ADDR + SG_INPUT_RING_OFFSET);
    shared_input_latency =
        (sg_input_latency_t *)(uintptr_t)(COMM_ADDR + SG_INPUT_LATENCY_OFFSET);
    memset(shared_input_ring, 0, sizeof(*shared_input_ring));
    shared_input_ring->magic = SG_INPUT_RING_MAGIC;
    const char *latency_env = getenv("SG_INPUT_LATENCY");
    input_latency_measurement = latency_env && strcmp(latency_env, "1") == 0;
    memset(shared_input_latency, 0, sizeof(*shared_input_latency));
    if (input_latency_measurement) {
        shared_input_latency->magic = SG_INPUT_LATENCY_MAGIC;
        shared_input_latency->enabled = 1;
        fprintf(stderr, "[input-latency] measuring click injection to application action\n");
        int tsc_ret = get_guest_tsc_info(&guest_tsc_offset,
                                         &guest_tsc_frequency_khz);
        if (tsc_ret == 0) {
            guest_tsc_translation_available = true;
            fprintf(stderr,
                    "[input-latency] guest TSC offset=%" PRIu64
                    " frequency=%u kHz\n",
                    guest_tsc_offset, guest_tsc_frequency_khz);
        } else {
            fprintf(stderr,
                    "[input-latency] KVM guest TSC translation unavailable: %s\n",
                    strerror(-tsc_ret));
        }
    }
    make_presentation_window_input_transparent();
    start_shared_input_thread();
#endif
#ifndef QEMU_LISTENER
    /* DRI3 only needs a same-screen drawable while buffers are imported.
     * glXMakeCurrent or SDL_CreateWindow supplies the actual presentation
     * window, so native mode never creates a second top-level window. */
    win = screen->root;
    xcb_flush(conn);
#else
    /* The listener surface remains unmapped until it can be embedded in the
     * application-owned top-level window. */
    xcb_flush(conn);
#endif
}
