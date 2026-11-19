/* A stand-in for testing tools/base-probe's memory stretch: a process that reads RAM in known ways.
 *   base-memwalk [SECONDS=20]
 * Thread "chase":  random pointer chase through 1 GiB of its own memory, in one area (huge pages possible)
 * Thread "stream": sums 1 GiB of its own memory front to back, over and over
 * Thread "small":  random reads in 2000 separate areas of 256 KiB (no huge page can hold them)
 * Thread "file":   random reads in a 512 MiB file mapped from /tmp-like storage given by TMPDIR (or the
 *                  current directory); the file is removed again
 * Build: gcc -O2 -pthread -o base-memwalk base-memwalk.c
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static void *chase(void *arg)
{
    size_t n = (1ull << 30) / sizeof(uint64_t), i;
    uint64_t *a = mmap(0, n * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), s = 88172645463325252ull, x = 0;
    prctl(PR_SET_NAME, "chase");
    for (i = 0; i < n; i++) a[i] = rnd(&s) % n;
    while (!stop) for (i = 0; i < 1000000; i++) x = a[x];
    return (void *)(uintptr_t)x;
}

static void *stream(void *arg)
{
    size_t n = (1ull << 30) / sizeof(uint64_t), i;
    uint64_t *a = mmap(0, n * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), x = 0;
    prctl(PR_SET_NAME, "stream");
    memset(a, 1, n * 8);
    while (!stop) for (i = 0; i < n; i++) x += a[i];
    return (void *)(uintptr_t)x;
}

static void *small(void *arg)
{
    enum { N = 2000, SZ = 256 << 10 };
    static unsigned char *area[N];
    uint64_t s = 1234567, x = 0;
    int i;
    prctl(PR_SET_NAME, "small");
    for (i = 0; i < N; i++) {
        /* a guard page between them keeps the kernel from joining them into one area */
        area[i] = (unsigned char *)mmap(0, SZ + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(area[i] + SZ, 4096, PROT_NONE);
        memset(area[i], i, SZ);
    }
    while (!stop) for (i = 0; i < 1000000; i++) { uint64_t r = rnd(&s); x += area[r % N][(r >> 20) % SZ]; }
    return (void *)(uintptr_t)x;
}

static void *file(void *arg)
{
    size_t sz = 512u << 20, i;
    char path[512];
    const char *dir = getenv("TMPDIR");
    int fd;
    unsigned char *m;
    uint64_t s = 987654321, x = 0;
    prctl(PR_SET_NAME, "file");
    snprintf(path, sizeof(path), "%s/base-memwalk-file.bin", dir ? dir : ".");
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, sz)) { perror(path); return 0; }
    m = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (i = 0; i < sz; i += 4096) m[i] = i >> 12;
    while (!stop) for (i = 0; i < 1000000; i++) { uint64_t r = rnd(&s); x += m[r % sz]; }
    unlink(path);
    return (void *)(uintptr_t)x;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20, i;
    pthread_t t[4];
    void *(*fn[4])(void *) = { chase, stream, small, file };
    for (i = 0; i < 4; i++) pthread_create(&t[i], 0, fn[i], 0);
    printf("pid %d, %d s\n", getpid(), secs); fflush(stdout);
    sleep(secs);
    stop = 1;
    for (i = 0; i < 4; i++) pthread_join(t[i], 0);
    return 0;
}
