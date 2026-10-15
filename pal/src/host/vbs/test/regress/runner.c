/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 Gramine contributors */

/* Runs one packed LibOS regression program inside the enclave: /tests/<argv[1]> with the rest of
 * the host's arguments (the image packs many; the host chooses one per run). */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: runner <test> [args...]\n");
        return 2;
    }
    char path[256];
    snprintf(path, sizeof(path), "/tests/%s", argv[1]);
    execv(path, argv + 1);
    perror("execv");
    return 127;
}
