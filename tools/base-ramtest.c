// How fast the RAM really is: bandwidth (all channels busy) and latency (one dependent load after another).
// Build and run: tools/base-ramtest.sh. Reads and writes only its own memory.
//   bandwidth: N threads on separate P-cores each sum a 1 GiB array again and again (reads), then fill it with
//              non-temporal stores (writes). Two channels of DDR4-2133 carry 34.1 GB/s at the very most,
//              DDR4-2666 42.7, DDR4-3200 51.2; a read test reaches roughly 80-90 % of that.
//   latency:   a random cycle through 1 GiB of pointers (huge pages asked for, so that page walks stay out of it):
//              nanoseconds per load that has to come from RAM.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <immintrin.h>

#define GIB (1UL << 30)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void *big(size_t n) {
    void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    madvise(p, n, MADV_HUGEPAGE);
    return p;
}
struct job { int cpu, write; double secs, gb; uint64_t *mem; pthread_barrier_t *go; uint64_t sum; };
static void *work(void *arg) {
    struct job *j = arg;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(j->cpu, &set); pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    size_t n = GIB / 8;
    for (size_t i = 0; i < n; i += 512) j->mem[i] = i;  // touch every page
    pthread_barrier_wait(j->go);
    double t0 = now(); int passes = 0; uint64_t s = 0;
    do {
        if (j->write) { __m256i v = _mm256_set1_epi64x(passes); for (size_t i = 0; i < n; i += 4) _mm256_stream_si256((__m256i *)(j->mem + i), v); _mm_sfence(); }
        else for (size_t i = 0; i < n; i += 4) s += j->mem[i] + j->mem[i + 1] + j->mem[i + 2] + j->mem[i + 3];
        passes++;
    } while (now() - t0 < j->secs);
    j->gb = passes * (double)GIB / 1e9 / (now() - t0); j->sum = s;
    return 0;
}
static double bandwidth(int threads, int write, double secs, const int *cpus) {
    pthread_t t[16]; struct job j[16]; pthread_barrier_t go; pthread_barrier_init(&go, 0, threads);
    for (int i = 0; i < threads; i++) { j[i] = (struct job){ cpus[i], write, secs, 0, big(GIB), &go, 0 }; pthread_create(&t[i], 0, work, &j[i]); }
    double total = 0;
    for (int i = 0; i < threads; i++) { pthread_join(t[i], 0); total += j[i].gb; munmap(j[i].mem, GIB); }
    return total;
}
static double latency(int cpu) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set); sched_setaffinity(0, sizeof set, &set);
    size_t n = GIB / 8; uint64_t *p = big(GIB);
    for (size_t i = 0; i < n; i++) p[i] = i;
    uint64_t x = 88172645463325252ULL;  // one cycle through all slots (Sattolo)
    for (size_t i = n - 1; i > 0; i--) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; size_t k = x % i; uint64_t t = p[i]; p[i] = p[k]; p[k] = t; }
    size_t steps = 20000000, at = 0;
    for (size_t i = 0; i < 2000000; i++) at = p[at];
    double t0 = now();
    for (size_t i = 0; i < steps; i++) at = p[at];
    double ns = (now() - t0) / steps * 1e9;
    munmap(p, GIB);
    return at == (size_t)-1 ? 0 : ns;
}
int main(int argc, char **argv) {
    int cpus[16], n = 0;
    for (int i = 1; i < argc && n < 16; i++) cpus[n++] = atoi(argv[i]);
    if (!n) { fprintf(stderr, "usage: base-ramtest CPU [CPU ...]   (one logical CPU per physical core)\n"); return 1; }
    printf("latency: %.1f ns per load from RAM (cpu %d)\n", latency(cpus[0]), cpus[0]);
    for (int t = 1; t <= n; t = t < n && t * 2 > n ? n : t * 2) {
        printf("bandwidth with %2d threads: read %5.1f GB/s", t, bandwidth(t, 0, 1.5, cpus)); fflush(stdout);
        printf(", write %5.1f GB/s\n", bandwidth(t, 1, 1.5, cpus));
        if (t == n) break;
    }
    return 0;
}
