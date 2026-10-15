/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * x_input: X keyboard and pointer events through the X connection. Opens a small window, prints
 * every key press (keysym name) and button press it receives, and exits after `q` or a timeout.
 * Events can come from a real keyboard or be sent to the window, for example with
 * `xdotool key --window <id> a b q` (XSendEvent: only this window receives them).
 * Usage: x_input [seconds]. Prints "TEST OK" when it got at least one key press.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    setvbuf(stdout, NULL, _IONBF, 0);
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        printf("cannot open display\n");
        return 1;
    }
    Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 200, 100, 0, 0, 0);
    XStoreName(dpy, w, "x_input");
    XSelectInput(dpy, w, KeyPressMask | ButtonPressMask | ExposureMask);
    XMapWindow(dpy, w);
    XFlush(dpy);
    printf("window 0x%lx\n", (unsigned long)w);

    int keys = 0;
    time_t end = time(NULL) + seconds;
    while (time(NULL) < end) {
        if (!XPending(dpy)) {
            struct pollfd p = {.fd = ConnectionNumber(dpy), .events = POLLIN};
            poll(&p, 1, 200);
            continue;
        }
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress) {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            const char* name = XKeysymToString(sym);
            printf("key %s%s\n", name ? name : "?", ev.xkey.send_event ? " (sent)" : "");
            keys++;
            if (sym == XK_q)
                break;
        } else if (ev.type == ButtonPress) {
            printf("button %u%s\n", ev.xbutton.button, ev.xbutton.send_event ? " (sent)" : "");
        }
    }
    XCloseDisplay(dpy);
    if (keys)
        printf("TEST OK\n");
    return keys ? 0 : 1;
}
