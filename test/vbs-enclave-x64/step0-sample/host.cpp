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
// Gramine phase-0 spike (step 0): the host of Microsoft's "VbsEnclave" sample without the WIL
// dependency; every API result is printed with its Win32 error code.

#include <windows.h>
#include <enclaveapi.h>
#include <stdio.h>

static void fail(const char* what) {
    DWORD e = GetLastError();
    printf("FAIL %s: GetLastError=%lu (0x%08lx)\n", what, e, e);
}

int main(int argc, char** argv)
{
    const wchar_t* image = L"vbsenclave.dll";
    (void)argc; (void)argv;
    printf("host: PROCESSOR_ARCHITECTURE=%s\n", getenv("PROCESSOR_ARCHITECTURE"));
    printf("IsEnclaveTypeSupported(ENCLAVE_TYPE_VBS)=%d\n", IsEnclaveTypeSupported(ENCLAVE_TYPE_VBS));
    printf("IsEnclaveTypeSupported(ENCLAVE_TYPE_VBS_BASIC)=%d\n", IsEnclaveTypeSupported(ENCLAVE_TYPE_VBS_BASIC));
    printf("IsEnclaveTypeSupported(ENCLAVE_TYPE_SGX)=%d\n", IsEnclaveTypeSupported(ENCLAVE_TYPE_SGX));
    if (!IsEnclaveTypeSupported(ENCLAVE_TYPE_VBS)) { fail("IsEnclaveTypeSupported(VBS)"); return 2; }

    ENCLAVE_CREATE_INFO_VBS CreateInfo = { ENCLAVE_VBS_FLAG_DEBUG, { 0x10, 0x20, 0x30, 0x40, 0x41, 0x31, 0x21, 0x11 } };
    DWORD EnclaveError = 0;
    PVOID Enclave = CreateEnclave(GetCurrentProcess(), nullptr, 0x10000000, 0, ENCLAVE_TYPE_VBS,
                                  &CreateInfo, sizeof(CreateInfo), &EnclaveError);
    if (!Enclave) { fail("CreateEnclave"); printf("  EnclaveError=0x%08lx\n", EnclaveError); return 3; }
    printf("CreateEnclave ok: base=%p\n", Enclave);

    DWORD previousMode = GetThreadErrorMode();
    SetThreadErrorMode(previousMode | SEM_FAILCRITICALERRORS, nullptr);
    BOOL ok = LoadEnclaveImageW(Enclave, image);
    SetThreadErrorMode(previousMode, nullptr);
    if (!ok) { fail("LoadEnclaveImageW"); DeleteEnclave(Enclave); return 4; }
    printf("LoadEnclaveImageW ok\n");

    ENCLAVE_INIT_INFO_VBS InitInfo = {};
    InitInfo.Length = sizeof(InitInfo);
    InitInfo.ThreadCount = 1;
    if (!InitializeEnclave(GetCurrentProcess(), Enclave, &InitInfo, InitInfo.Length, &EnclaveError)) {
        fail("InitializeEnclave"); printf("  EnclaveError=0x%08lx\n", EnclaveError); DeleteEnclave(Enclave); return 5;
    }
    printf("InitializeEnclave ok (ThreadCount=%lu)\n", InitInfo.ThreadCount);

    PENCLAVE_ROUTINE Routine = (PENCLAVE_ROUTINE)GetProcAddress((HMODULE)Enclave, "CallEnclaveTest");
    if (!Routine) { fail("GetProcAddress(CallEnclaveTest)"); return 6; }
    ULONG_PTR Input = 0x1234;
    void* Output = nullptr;
    if (!CallEnclave(Routine, (void*)Input, TRUE, &Output)) { fail("CallEnclave(CallEnclaveTest)"); return 7; }
    printf("CallEnclaveTest(0x1234) -> %p (expected 0x%lx): %s\n", Output, (ULONG)(0x1234 ^ 0xDADAF00D),
           (((ULONG_PTR)Output ^ Input) == 0xDADAF00D) ? "OK" : "WRONG");

    Routine = (PENCLAVE_ROUTINE)GetProcAddress((HMODULE)Enclave, "EnclaveTestThread");
    if (!Routine) { fail("GetProcAddress(EnclaveTestThread)"); return 8; }
    if (!CallEnclave(Routine, nullptr, TRUE, &Output)) { fail("CallEnclave(EnclaveTestThread)"); return 9; }
    printf("EnclaveTestThread() -> %p (expected 0xffff)\n", Output);

    if (!TerminateEnclave(Enclave, TRUE)) fail("TerminateEnclave");
    if (!DeleteEnclave(Enclave)) fail("DeleteEnclave");
    printf("step0: SUCCESS\n");
    return 0;
}
