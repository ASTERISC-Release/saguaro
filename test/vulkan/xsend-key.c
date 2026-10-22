/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * xsend-key: sends a key press and release to ONE window, the parent of the window named NAME
 * (the saguaro-vm listener's presentation child is reparented into the application's window), with
 * XSendEvent; nothing else on the desktop receives it (dev docs: vulkan/, phase 2: input
 * check for vkcube, which quits on Escape).
 *
 * Usage: xsend-key NAME KEYSYM_NAME   (for example: xsend-key "XCB Demo Window" Escape)
 *        xsend-key NAME resize WxH     (asks the window manager to resize that window instead)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>

static Window find(Display* d, Window w, const char* name, int depth) {
    char* n = NULL;
    if (XFetchName(d, w, &n) && n) {
        int hit = strstr(n, name) != NULL;
        XFree(n);
        if (hit)
            return w;
    }
    if (depth > 4)
        return 0;
    Window root, parent, *kids = NULL;
    unsigned int nk = 0;
    if (!XQueryTree(d, w, &root, &parent, &kids, &nk))
        return 0;
    Window found = 0;
    for (unsigned int i = 0; i < nk && !found; i++)
        found = find(d, kids[i], name, depth + 1);
    if (kids)
        XFree(kids);
    return found;
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s NAME KEYSYM | NAME resize WxH\n", argv[0]);
        return 2;
    }
    Display* d = XOpenDisplay(NULL);
    if (!d)
        return 2;
    Window child = find(d, DefaultRootWindow(d), argv[1], 0);
    if (!child) {
        fprintf(stderr, "no window named *%s*\n", argv[1]);
        return 1;
    }
    Window root, parent, *kids = NULL;
    unsigned int nk = 0;
    XQueryTree(d, child, &root, &parent, &kids, &nk);
    if (kids)
        XFree(kids);
    if (parent == root) {
        /* not reparented into the application's window yet: send nothing */
        fprintf(stderr, "window *%s* is still a top-level window\n", argv[1]);
        return 1;
    }
    unsigned int w, h;
    if (!strcmp(argv[2], "resize") && argc == 4 && sscanf(argv[3], "%ux%u", &w, &h) == 2) {
        XResizeWindow(d, parent, w, h);
        XFlush(d);
        printf("asked to resize window 0x%lx to %ux%u\n", parent, w, h);
        XCloseDisplay(d);
        return 0;
    }
    KeyCode kc = XKeysymToKeycode(d, XStringToKeysym(argv[2]));
    XKeyEvent e = {.type = KeyPress, .display = d, .window = parent, .root = root,
                   .subwindow = None, .time = CurrentTime, .same_screen = True, .keycode = kc};
    XSendEvent(d, parent, True, KeyPressMask, (XEvent*)&e);
    e.type = KeyRelease;
    XSendEvent(d, parent, True, KeyReleaseMask, (XEvent*)&e);
    XFlush(d);
    printf("sent %s (keycode %u) to window 0x%lx\n", argv[2], kc, parent);
    XCloseDisplay(d);
    return 0;
}
