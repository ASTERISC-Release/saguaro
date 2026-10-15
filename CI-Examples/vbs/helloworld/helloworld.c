/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 Gramine contributors */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    printf("Hello, world from a VBS enclave (pid %d, %d argument%s)\n", getpid(), argc - 1,
           argc == 2 ? "" : "s");
    return 0;
}
