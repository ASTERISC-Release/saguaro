/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
extern char** environ;
int main(void) {
    int r = system("true");
    printf("system(true) = %d (exit %d)\n", r, WIFEXITED(r) ? WEXITSTATUS(r) : -1);
    pid_t pid;
    char* argv[] = {"/bin/sh", "-c", "echo from posix_spawn; exit 3", NULL};
    r = posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ);
    int st = 0;
    if (r == 0) waitpid(pid, &st, 0);
    printf("posix_spawn = %d, child exit %d\n", r, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    r = system("pulseaudio --check");
    printf("system(pulseaudio --check) = %d (exit %d)\n", r, WIFEXITED(r) ? WEXITSTATUS(r) : -1);
    return 0;
}
