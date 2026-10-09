// tls_test.c
// Compile: gcc -O2 -std=c11 -pthread -g -o tls_test tls_test.c
#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>

/* thread-local variable */
static __thread int tls_var = 0;

void *thr(void *arg) {
    int id = (int)(intptr_t)arg;
    tls_var = id + 1000;
    fprintf(stderr, "k_tid=%d pthread_self=%lu tls_var_addr=%p tls_var=%d\n",
            (int)syscall(SYS_gettid), (unsigned long)pthread_self(),
            (void*)&tls_var, tls_var);
    /* sleep a bit so threads overlap */
    usleep(100000);
    /* print again */
    fprintf(stderr, "k_tid=%d pthread_self=%lu tls_var_addr=%p tls_var=%d (again)\n",
            (int)syscall(SYS_gettid), (unsigned long)pthread_self(),
            (void*)&tls_var, tls_var);
    return NULL;
}

int main(void) {
    const int N = 8;
    pthread_t t[N];
    for (int i = 0; i < N; ++i) {
        pthread_create(&t[i], NULL, thr, (void*)(intptr_t)i);
    }
    for (int i = 0; i < N; ++i) pthread_join(t[i], NULL);
    return 0;
}

