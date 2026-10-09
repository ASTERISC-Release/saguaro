#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>

#define SERVER_PORT 55555
#define SERVER_ADDR "127.0.0.1"

#define SMALL_MSG 16
#define MED_MSG   512
#define BIG_MSG   4096
#define BURST_CNT 5000

static volatile int server_ready = 0;

/* ==================== Server thread ==================== */

void *server_thread(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sock >= 0);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(SERVER_PORT),
        .sin_addr.s_addr = inet_addr(SERVER_ADDR),
    };
    assert(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    /* ⏱️ timeout so we never hang */
    struct timeval tv = {
        .tv_sec = 2,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    server_ready = 1;

    char buf[8192];

    int seen_small = 0;
    int seen_med   = 0;
    int seen_big   = 0;
    int seen_order = 0;
    int burst_received = 0;
    int last_order = -1;

    while (1) {
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        fprintf(stderr, "BUF: %s\n", buf);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* timeout = assume client finished */
                break;
            }
            perror("recvfrom");
            exit(1);
        }

        /* safe debug */
        fprintf(stderr, "[server] recv n=%zd\n", n);

        /* DONE is only a hint */
        if (n == 4 && memcmp(buf, "DONE", 4) == 0)
            continue;

        /* SMALL */
        if (n == SMALL_MSG && buf[0] == 'A') {
            for (int i = 0; i < SMALL_MSG; i++)
                assert(buf[i] == 'A');
            seen_small = 1;
            continue;
        }

        /* MED */
        if (n == MED_MSG && buf[0] == 'B') {
            for (int i = 0; i < MED_MSG; i++)
                assert(buf[i] == 'B');
            seen_med = 1;
            continue;
        }

        /* BIG */
        if (n == BIG_MSG && buf[0] == 'C') {
            for (int i = 0; i < BIG_MSG; i++)
                assert(buf[i] == 'C');
            seen_big = 1;
            continue;
        }

        /* ORDERED ints (loss-tolerant) */
        if (n == sizeof(int)) {
            int v;
            memcpy(&v, buf, sizeof(v));
            assert(v > last_order);   // monotonic only
            last_order = v;
            seen_order = 1;
            continue;
        }

        /* BURST */
        if (n == 256 && buf[0] == 'D') {
            for (int i = 0; i < 256; i++)
                assert(buf[i] == 'D');
            burst_received++;
            continue;
        }

        assert(!"unexpected UDP payload");
    }

    /* Final correctness checks */
    assert(seen_small);
    assert(seen_med);
    assert(seen_big);
    assert(seen_order);
    assert(burst_received > 0);

    fprintf(stderr,
        "[server] OK small=%d med=%d big=%d order=%d burst=%d\n",
        seen_small, seen_med, seen_big, seen_order, burst_received);

    close(sock);
    return NULL;
}

/* ==================== Client thread ==================== */

void *client_thread(void *arg)
{
    while (!server_ready)
        sched_yield();

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sock >= 0);

    /* explicit client bind */
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port   = 0,
        .sin_addr.s_addr = inet_addr(SERVER_ADDR),
    };
    assert(bind(sock, (struct sockaddr *)&local, sizeof(local)) == 0);

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(SERVER_PORT),
        .sin_addr.s_addr = inet_addr(SERVER_ADDR),
    };

    /* SMALL */
    char small[SMALL_MSG];
    memset(small, 'A', sizeof(small));
    sendto(sock, small, sizeof(small), 0,
           (struct sockaddr *)&dst, sizeof(dst));

    /* MED */
    char med[MED_MSG];
    memset(med, 'B', sizeof(med));
    sendto(sock, med, sizeof(med), 0,
           (struct sockaddr *)&dst, sizeof(dst));

    /* BIG */
    char big[BIG_MSG];
    memset(big, 'C', sizeof(big));
    sendto(sock, big, sizeof(big), 0,
           (struct sockaddr *)&dst, sizeof(dst));

    /* ORDER */
    for (int i = 0; i < 1000; i++) {
        sendto(sock, &i, sizeof(i), 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }

    /* BURST */
    char burst[256];
    memset(burst, 'D', sizeof(burst));
    for (int i = 0; i < BURST_CNT; i++) {
        sendto(sock, burst, sizeof(burst), 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }

    /* DONE hint (not relied upon) */
    for (int i = 0; i < 5; i++) {
        sendto(sock, "DONE", 4, 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }

    close(sock);
    return NULL;
}

/* ==================== Main ==================== */

int main(void)
{
    pthread_t srv, cli;

    pthread_create(&srv, NULL, server_thread, NULL);
    pthread_create(&cli, NULL, client_thread, NULL);

    pthread_join(cli, NULL);
    pthread_join(srv, NULL);

    printf("UDP SOCKET TEST PASSED (HOST + CUSTOM UDP)\n");
    return 0;
}
