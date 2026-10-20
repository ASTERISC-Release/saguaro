/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* A small Direct3D 9 program for the Proton study (Documentation/devel/proton-vm/): opens a
 * window, creates a D3D9 device (DXVK under Proton), clears the back buffer to a colour that
 * changes every second (red, green, blue), presents without vsync and prints the frame rate every
 * second. -f creates a full-screen device at the window's size instead of a windowed one (as The
 * Witcher 1 does). Exits after -s seconds (default 10). Usage: d3d9.exe [-s SEC] [-w W -h H] [-f].
 * Built 64- and 32-bit with mingw-w64 (test/proton-vm/build-tests.sh). */

#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d9.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char** argv) {
    int seconds = 10, width = 1280, height = 720, fullscreen = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) fullscreen = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "-s")) seconds = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "-w")) width = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "-h")) height = atoi(argv[++i]);
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "proton-vm-d3d9";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "proton-vm d3d9", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL, NULL, wc.hInstance,
                              NULL);
    if (!hwnd) {
        printf("CreateWindow failed: %lu\n", GetLastError());
        return 1;
    }
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        printf("Direct3DCreate9 failed\n");
        return 1;
    }
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = !fullscreen;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        printf("CreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    D3DADAPTER_IDENTIFIER9 id;
    IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id);
    printf("d3d9: %d-bit, adapter '%s', %dx%d, %s\n", (int)(sizeof(void*) * 8), id.Description,
           width, height, fullscreen ? "full screen" : "windowed");
    fflush(stdout);

    static const D3DCOLOR colours[3] = {D3DCOLOR_XRGB(255, 0, 0), D3DCOLOR_XRGB(0, 255, 0),
                                        D3DCOLOR_XRGB(0, 0, 255)};
    LARGE_INTEGER freq, start, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    last = start;
    unsigned frames = 0, total = 0, second = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto out;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, colours[second % 3], 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_EndScene(dev);
        hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        if (FAILED(hr)) {
            printf("Present failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }
        frames++;
        total++;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        if (dt >= 1.0) {
            printf("d3d9: %u frames in %.2f s = %.1f FPS\n", frames, dt, frames / dt);
            fflush(stdout);
            frames = 0;
            last = now;
            second++;
        }
        if ((double)(now.QuadPart - start.QuadPart) / freq.QuadPart >= seconds)
            break;
    }
out:
    QueryPerformanceCounter(&now);
    double all = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
    printf("d3d9: %u frames total, %.1f FPS average\n", total, total / all);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
