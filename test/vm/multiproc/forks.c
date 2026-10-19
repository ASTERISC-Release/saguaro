#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char** argv) {
    int n = atoi(argv[1]), par = atoi(argv[2]), bad = 0;
    for (int i = 0; i < n; i += par) {
        for (int j = 0; j < par; j++) { pid_t p = fork(); if (p == 0) _exit(j & 7); if (p < 0) { perror("fork"); return 1; } }
        for (int j = 0; j < par; j++) { int st; pid_t p = wait(&st); if (p < 0 || !WIFEXITED(st)) bad++; }
    }
    printf("forks=%d parallel=%d bad=%d\n", n, par, bad);
    return bad != 0;
}
