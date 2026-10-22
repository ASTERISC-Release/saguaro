//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
//
// Saguaro phase-0 spike (step h): host for the ELF-in-image enclave. Usage: host.exe [1|2]

#include <windows.h>
#include <enclaveapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "probe.h"

static PROBE_IO g_io;

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    ENCLAVE_CREATE_INFO_VBS ci = { ENCLAVE_VBS_FLAG_DEBUG, { 0 } };
    DWORD err = 0;
    void* e = CreateEnclave(GetCurrentProcess(), nullptr, 0x10000000, 0, ENCLAVE_TYPE_VBS, &ci, sizeof(ci), &err);
    if (!e) { printf("CreateEnclave failed %lu\n", GetLastError()); return 3; }
    if (!LoadEnclaveImageW(e, L"vbsenclave.dll")) { printf("LoadEnclaveImageW failed %lu\n", GetLastError()); return 4; }
    ENCLAVE_INIT_INFO_VBS ii = { sizeof(ii), 1 };
    if (!InitializeEnclave(GetCurrentProcess(), e, &ii, ii.Length, &err)) { printf("InitializeEnclave failed %lu\n", GetLastError()); return 5; }
    g_io.args[0] = argc > 1 ? atoi(argv[1]) : 1;
    void* out;
    BOOL ok = CallEnclave((PENCLAVE_ROUTINE)GetProcAddress((HMODULE)e, "E_Elf"), &g_io, TRUE, &out);
    printf("CallEnclave ok=%d\n", ok);
    fwrite(g_io.text, 1, g_io.len, stdout);
    TerminateEnclave(e, TRUE);
    DeleteEnclave(e);
    return 0;
}
