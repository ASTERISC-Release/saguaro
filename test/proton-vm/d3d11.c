/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/* A small Direct3D 11 program for the Proton feasibility study (dev docs: proton-vm/):
 * opens a window, creates a D3D11 device and swap chain (DXVK under Proton translates it to
 * Vulkan), draws a triangle whose colour cycles (the shaders are compiled at run time with
 * d3dcompiler_47, which Wine provides), presents without vsync, and prints the frame rate every
 * second. Exits after -s seconds (default 10) or when the window closes. Usage: d3d11.exe [-s SEC]
 * [-w W -h H]. Built with x86_64-w64-mingw32-gcc (test/proton-vm/build-tests.sh). */

#define COBJMACROS
#define INITGUID
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char shader_src[] =
    "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "cbuffer C : register(b0) { float4 tint; };\n"
    "VSOut vs(uint id : SV_VertexID) {\n"
    "  float2 p[3] = { float2(-0.8, -0.8), float2(0.0, 0.8), float2(0.8, -0.8) };\n"
    "  float4 c[3] = { float4(1,0,0,1), float4(0,1,0,1), float4(0,0,1,1) };\n"
    "  VSOut o; o.pos = float4(p[id], 0, 1); o.col = c[id] * tint; return o; }\n"
    "float4 ps(VSOut i) : SV_TARGET { return i.col; }\n";

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char** argv) {
    int seconds = 10, width = 1280, height = 720;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "-s")) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w")) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) height = atoi(argv[++i]);
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "proton-vm-d3d11";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "proton-vm d3d11", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL, NULL, wc.hInstance,
                              NULL);
    if (!hwnd) {
        printf("CreateWindow failed: %lu\n", GetLastError());
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    IDXGISwapChain* sc = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        printf("D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    IDXGIDevice* dxgidev = NULL;
    IDXGIAdapter* adapter = NULL;
    DXGI_ADAPTER_DESC ad = {0};
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void**)&dxgidev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgidev, &adapter)))
        IDXGIAdapter_GetDesc(adapter, &ad);
    printf("d3d11: feature level 0x%x, adapter '%ls' vendor 0x%04x device 0x%04x, %ux%u\n", fl,
           ad.Description, ad.VendorId, ad.DeviceId, width, height);
    fflush(stdout);

    ID3D11Texture2D* bb = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)bb, NULL, &rtv);

    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* ps = NULL;
    ID3D11Buffer* cb = NULL;
    int triangle = 0;
    if (SUCCEEDED(D3DCompile(shader_src, sizeof(shader_src) - 1, "t", NULL, NULL, "vs", "vs_4_0", 0,
                             0, &vsb, &err)) &&
        SUCCEEDED(D3DCompile(shader_src, sizeof(shader_src) - 1, "t", NULL, NULL, "ps", "ps_4_0", 0,
                             0, &psb, &err))) {
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                        ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                       ID3D10Blob_GetBufferSize(psb), NULL, &ps);
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
        triangle = vs && ps && cb;
    } else if (err) {
        printf("D3DCompile failed: %s\n", (char*)ID3D10Blob_GetBufferPointer(err));
    }
    printf("d3d11: %s\n", triangle ? "triangle" : "clear only (no shader compiler)");

    D3D11_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    LARGE_INTEGER freq, start, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    last = start;
    unsigned frames = 0, total = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        float t = (float)(total % 600) / 600.0f;
        float clear[4] = {0.1f, 0.1f, 0.3f + 0.2f * t, 1.0f};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        if (triangle) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)cb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float tint[4] = {0.5f + 0.5f * t, 1.0f - 0.5f * t, 1.0f, 1.0f};
                memcpy(m.pData, tint, sizeof(tint));
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)cb, 0);
            }
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
            ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        IDXGISwapChain_Present(sc, 0, 0);
        frames++;
        total++;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - last.QuadPart >= freq.QuadPart) {
            printf("d3d11: %u frames in %.2f s = %.1f FPS\n", frames,
                   (double)(now.QuadPart - last.QuadPart) / freq.QuadPart,
                   frames * (double)freq.QuadPart / (now.QuadPart - last.QuadPart));
            fflush(stdout);
            frames = 0;
            last = now;
        }
        if (now.QuadPart - start.QuadPart >= (LONGLONG)seconds * freq.QuadPart) break;
    }
out:
    printf("d3d11: %u frames total, %.1f FPS average\n", total,
           total * (double)freq.QuadPart / (double)(now.QuadPart - start.QuadPart));
    DestroyWindow(hwnd);
    return 0;
}
