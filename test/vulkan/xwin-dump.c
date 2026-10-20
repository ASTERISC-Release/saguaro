/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * xwin-dump: writes the contents of one X window, found by a substring of its WM_NAME, to a PPM
 * file (dev docs: vulkan/, phase 2: checks what the presentation layer put on screen,
 * reading only that window from the X server, not the desktop).
 *
 * Usage: xwin-dump NAME_SUBSTRING OUT.ppm   (exit 1 if no such window is found)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static xcb_window_t find(xcb_connection_t* c, xcb_window_t w, const char* name, int depth) {
    xcb_get_property_reply_t* p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY, 0, 256), NULL);
    if (p) {
        int len = xcb_get_property_value_length(p);
        char buf[257] = {0};
        memcpy(buf, xcb_get_property_value(p), len < 256 ? len : 256);
        free(p);
        if (len > 0 && strstr(buf, name))
            return w;
    }
    if (depth > 4)
        return XCB_NONE;
    xcb_query_tree_reply_t* t = xcb_query_tree_reply(c, xcb_query_tree(c, w), NULL);
    if (!t)
        return XCB_NONE;
    xcb_window_t* kids = xcb_query_tree_children(t);
    xcb_window_t found = XCB_NONE;
    for (int i = 0; i < xcb_query_tree_children_length(t) && !found; i++)
        found = find(c, kids[i], name, depth + 1);
    free(t);
    return found;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s NAME_SUBSTRING OUT.ppm\n", argv[0]);
        return 2;
    }
    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c))
        return 2;
    xcb_screen_t* s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t w = find(c, s->root, argv[1], 0);
    if (!w) {
        fprintf(stderr, "no window named *%s*\n", argv[1]);
        return 1;
    }
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
    if (!g)
        return 1;
    xcb_get_image_reply_t* img = xcb_get_image_reply(
        c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, w, 0, 0, g->width, g->height, ~0u), NULL);
    if (!img) {
        fprintf(stderr, "GetImage failed\n");
        return 1;
    }
    uint8_t* px = xcb_get_image_data(img);
    FILE* f = fopen(argv[2], "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", g->width, g->height);
    for (int i = 0; i < g->width * g->height; i++) {
        fputc(px[4 * i + 2], f);
        fputc(px[4 * i + 1], f);
        fputc(px[4 * i + 0], f);
    }
    fclose(f);
    printf("window 0x%x %dx%d written to %s\n", w, g->width, g->height, argv[2]);
    return 0;
}
