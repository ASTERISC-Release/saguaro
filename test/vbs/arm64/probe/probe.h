/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

// Saguaro phase-0 spike: interface between the probe host and the probe enclave.
#pragma once

#define PROBE_TEXT 32768

typedef struct PROBE_IO {
    unsigned long long args[8];   // inputs, test specific
    unsigned long long out[8];    // numeric results, test specific
    int len;                      // bytes used in text
    char text[PROBE_TEXT];        // human-readable log written by the enclave
} PROBE_IO;
