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
// Gramine phase-0 spike (step 0): the VBS enclave of Microsoft's "VbsEnclave" sample, reduced to
// the two test functions. Derived from the sample in Windows-classic-samples (MIT).

#include <winenclave.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

const IMAGE_ENCLAVE_CONFIG __enclave_config = {
    sizeof(IMAGE_ENCLAVE_CONFIG),
    IMAGE_ENCLAVE_MINIMUM_CONFIG_SIZE,
    IMAGE_ENCLAVE_POLICY_DEBUGGABLE,    // DO NOT SHIP DEBUGGABLE ENCLAVES TO PRODUCTION
    0,
    0,
    0,
    { 0xFE, 0xFE },    // family id
    { 0x01, 0x01 },    // image id
    0,                 // version
    0,                 // SVN
    0x10000000,        // size
    16,                // number of threads
    IMAGE_ENCLAVE_FLAG_PRIMARY_IMAGE
};

ULONG InitialCookie;

BOOL DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD dwReason, _In_ LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(lpvReserved);
    if (dwReason == DLL_PROCESS_ATTACH) {
        InitialCookie = 0xDADAF00D;
    }
    return TRUE;
}

int CALLBACK EnclaveTestThread(_In_ void* Context)
{
    UNREFERENCED_PARAMETER(Context);
    WCHAR String[32];
    swprintf_s(String, ARRAYSIZE(String), L"%s\n", L"EnclaveTestThread started");
    OutputDebugStringW(String);
    return (int)(0xffff);
}

void* CALLBACK CallEnclaveTest(_In_ void* Context)
{
    WCHAR String[32];
    swprintf_s(String, ARRAYSIZE(String), L"%s\n", L"CallEnclaveTest started");
    OutputDebugStringW(String);
    return (void*)((ULONG_PTR)(Context) ^ InitialCookie);
}
