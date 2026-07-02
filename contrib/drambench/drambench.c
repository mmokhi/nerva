// Copyright (c) 2018-2026, The Nerva Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/* drambench: dependent pointer chase over working sets from cache-sized to
 * DRAM-sized, to ground the HF14 discussion about making CNA tick at DRAM
 * latency on every machine (1 CPU = 1 vote).
 *
 *   linux/android:  cc -O2 -pthread drambench.c -o drambench
 *   windows:        gcc -O2 drambench.c -o drambench.exe   (msys2/mingw)
 *   freebsd/macos:  cc -O2 -pthread drambench.c -o drambench
 *
 * Run without arguments. Every hop is one 64-byte line from a Sattolo cycle
 * (a single random permutation loop), so hops cannot be prefetched or
 * overlapped within a thread. Three things get measured, each answering a
 * concrete HF14 design question:
 *
 *  1. single-thread latency curve, pure chase vs chase + ALU. v6 interleaves
 *     multiply-heavy ALU, so the delta between the two columns is how much a
 *     fast out-of-order core can pull ahead of a slow one on the compute part,
 *     even when both wait on the same DRAM. Small delta => latency dominates
 *     and per-core stays close; large delta => ALU re-opens the gap.
 *
 *  2. per-thread private scaling: each thread chases its OWN region, like v6's
 *     per-thread scratchpads. Total memory grows with the thread count; ns/hop
 *     per thread shows how much each core slows as they all hit DRAM.
 *
 *  3. shared-region scaling: all threads chase ONE region. Memory is fixed no
 *     matter the core count; contention shows up sooner. This is the other
 *     HF14 lever (one shared read-only dataset vs per-thread pads).
 *
 * The size sweep also brackets the L3 cliff (where a big desktop falls out of
 * cache into DRAM) and, on a small board, finds the largest region it can hold
 * at all -- the weakest device we want to keep mining sets the ceiling. */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#elif defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/cpuset.h>
#endif
#endif

/* Pin the calling thread to one core. A pointer chase migrating between cores
 * refills its working set from DRAM on every move, which swamps the cache
 * hierarchy we are trying to measure (worse on hybrid P/E chips). macOS only
 * has advisory affinity, so it stays unpinned there. */
static void bind_thread(int cpu)
{
#if defined(__linux__) || defined(__ANDROID__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
#elif defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << (cpu & 63));
#elif defined(__FreeBSD__)
    cpuset_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(set), &set);
#else
    (void)cpu;
#endif
}

#define NODE_SIZE 64
#define RUN_MS 1200
#define ALU_MULS 8      /* dependent multiplies per hop for the "+ALU" column */
#define MAX_CHAINS 16   /* independent in-flight chases per thread (MLP knob) */

typedef struct { uint32_t next; char pad[NODE_SIZE - 4]; } node_t;

static uint64_t now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * (1000000000.0 / freq.QuadPart));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}

static uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

/* Sattolo's algorithm: one cycle visiting every node exactly once. */
static void build_cycle(node_t *nodes, uint32_t n)
{
    uint64_t seed = 0x6e657276615f7631ull;
    uint32_t *perm = malloc((size_t)n * sizeof(uint32_t));
    uint32_t i;
    if (perm == NULL) { fprintf(stderr, "oom building cycle\n"); exit(1); }
    for (i = 0; i < n; i++)
        perm[i] = i;
    for (i = n - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(xorshift64(&seed) % i);
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (i = 0; i < n - 1; i++)
        nodes[perm[i]].next = perm[i + 1];
    nodes[perm[n - 1]].next = perm[0];
    free(perm);
}

static node_t *alloc_nodes(size_t bytes, const char **mode)
{
    void *p;
#ifdef _WIN32
    p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *mode = "normal";
    if (p == NULL)
        return NULL;
#else
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
#ifdef MADV_HUGEPAGE
    *mode = madvise(p, bytes, MADV_HUGEPAGE) == 0 ? "thp" : "normal";
#else
    *mode = "normal";
#endif
#endif
    return (node_t *)p;
}

static void free_nodes(node_t *p, size_t bytes)
{
#ifdef _WIN32
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, bytes);
#endif
}

/* per-thread work item; nodes may point at a shared region or a private one */
typedef struct {
    node_t  *nodes;
    uint32_t count;
    uint32_t start;
    int      cpu;
    int      alu;
    uint64_t hops;
    uint32_t sink;
} tctx_t;

static volatile int g_go, g_stop;
static int g_chains = 1;   /* independent chases in flight per thread */

static void chase_body(tctx_t *c)
{
    const int nchains = g_chains;
    const int alu = c->alu;
    uint32_t idx[MAX_CHAINS];
    uint64_t hops = 0;
    uint64_t acc = 0x9e3779b97f4a7c15ull;
    int ci;
    /* spread the chains' start points so they sit in different regions */
    for (ci = 0; ci < nchains; ci++)
        idx[ci] = (uint32_t)(((uint64_t)(c->start + (uint32_t)ci) * 2654435761u) % c->count);
    bind_thread(c->cpu);
    while (!g_go)
        ;
    while (!g_stop) {
        int k;
        for (k = 0; k < 64; k++) {
            /* nchains loads with no dependency between them: the core can keep
             * all nchains outstanding at once, so this is the memory-level
             * parallelism knob. Each chain is still internally serial. */
            for (ci = 0; ci < nchains; ci++)
                idx[ci] = c->nodes[idx[ci]].next;
            if (alu) {
                /* multiplies depending on a loaded value, like v6's IMUL/MIX;
                 * a faster ALU shows up as a lower +ALU ns/hop */
                int m;
                acc ^= idx[0];
                for (m = 0; m < ALU_MULS; m++)
                    acc = acc * 0x100000001b3ull + 0x9e3779b9u;
            }
        }
        hops += (uint64_t)64 * nchains;
    }
    c->hops = hops;
    for (ci = 0; ci < nchains; ci++)
        acc ^= idx[ci];
    c->sink = (uint32_t)acc;
}

#ifdef _WIN32
static DWORD WINAPI chase(LPVOID arg) { chase_body((tctx_t *)arg); return 0; }
#else
static void *chase(void *arg) { chase_body((tctx_t *)arg); return NULL; }
#endif

/* Run `threads` chasers for RUN_MS. private!=0 gives each thread its own
 * `mb`-MB region (v6 per-thread pads); otherwise all share one `mb`-MB region.
 * alu!=0 mixes ALU_MULS multiplies into every hop. Returns Mhops/s and, via
 * *ns_per_hop, the wall-time cost of one hop as seen by a single thread (the
 * per-core rate under whatever contention `threads` creates). Returns <0 on
 * allocation failure so the caller can note the ceiling. */
static double run_case(size_t mb, int threads, int private_regions, int alu, int chains,
                       double *ns_per_hop, const char **mode_out, size_t *total_mb)
{
    const size_t bytes = mb << 20;
    const char *mode = "?";
    node_t *shared = NULL;
    uint64_t total = 0, t0, t1;
    int t, made = 0;
    double secs, mhops;
    tctx_t ctx[256];
#ifdef _WIN32
    HANDLE th[256];
#else
    pthread_t th[256];
#endif

    memset(ctx, 0, sizeof(ctx));
    g_chains = chains;   /* read by each thread at start, before g_go */

    if (!private_regions) {
        shared = alloc_nodes(bytes, &mode);
        if (shared == NULL) return -1.0;
        build_cycle(shared, (uint32_t)(bytes / NODE_SIZE));
    }

    for (t = 0; t < threads; t++) {
        if (private_regions) {
            const char *m;
            ctx[t].nodes = alloc_nodes(bytes, &m);
            if (ctx[t].nodes == NULL) break;      /* out of memory: stop here */
            mode = m;
            build_cycle(ctx[t].nodes, (uint32_t)(bytes / NODE_SIZE));
        } else {
            ctx[t].nodes = shared;
        }
        ctx[t].count = (uint32_t)(bytes / NODE_SIZE);
        ctx[t].start = (uint32_t)(((uint64_t)t * 2654435761u) % ctx[t].count);
        ctx[t].cpu   = t;
        ctx[t].alu   = alu;
        made++;
    }
    if (made < threads) {
        /* couldn't allocate one region per thread: report the shortfall */
        for (t = 0; t < made; t++)
            if (private_regions) free_nodes(ctx[t].nodes, bytes);
        return -1.0;
    }

    g_go = 0; g_stop = 0;
    for (t = 0; t < threads; t++)
#ifdef _WIN32
        th[t] = CreateThread(NULL, 0, chase, &ctx[t], 0, NULL);
#else
        pthread_create(&th[t], NULL, chase, &ctx[t]);
#endif

    t0 = now_ns();
    g_go = 1;
#ifdef _WIN32
    Sleep(RUN_MS);
#else
    { struct timespec ts = { RUN_MS / 1000, (RUN_MS % 1000) * 1000000L }; nanosleep(&ts, NULL); }
#endif
    g_stop = 1;
    for (t = 0; t < threads; t++)
#ifdef _WIN32
        { WaitForSingleObject(th[t], INFINITE); CloseHandle(th[t]); }
#else
        pthread_join(th[t], NULL);
#endif
    t1 = now_ns();

    for (t = 0; t < threads; t++)
        total += ctx[t].hops;
    secs  = (double)(t1 - t0) / 1e9;
    mhops = total / secs / 1e6;
    /* per-thread ns/hop = wall time divided by one thread's own hops; under
     * contention every thread does fewer hops so this rises */
    if (ns_per_hop)
        *ns_per_hop = (double)(t1 - t0) / ((double)total / (double)threads);
    if (mode_out)  *mode_out  = mode;
    if (total_mb)  *total_mb  = private_regions ? mb * (size_t)threads : mb;

    if (private_regions) {
        for (t = 0; t < threads; t++)
            free_nodes(ctx[t].nodes, bytes);
    } else {
        free_nodes(shared, bytes);
    }
    return mhops;
}

static int cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

int main(int argc, char **argv)
{
    /* sizes bracket the L3 cliff (64-128 MB on current desktops) and climb
     * until a small board runs out of memory, which is exactly the ceiling
     * we care about */
    static const size_t sizes[] = { 8, 16, 32, 64, 96, 128, 192, 256, 512, 1024 };
    const int ncpu = cpu_count();
    size_t s;
    int t;
    (void)argc; (void)argv;

    printf("drambench: %d cpus, %d ms/case, 64 B hops on a random cycle, %d muls/hop in +ALU, threads pinned\n\n",
           ncpu, RUN_MS, ALU_MULS);

    printf("single-thread latency curve (per-core rate; +ALU shows how much fast ALU helps):\n");
    printf("  %6s  %12s  %12s  %6s\n", "size", "pure ns/hop", "+ALU ns/hop", "pages");
    for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        double np = 0, na = 0;
        const char *mode = "?";
        double r = run_case(sizes[s], 1, 0, 0, 1, &np, &mode, NULL);
        if (r < 0) { printf("  %5zu MB  %12s\n", sizes[s], "(no memory)"); continue; }
        run_case(sizes[s], 1, 0, 1, 1, &na, &mode, NULL);
        printf("  %5zu MB  %12.1f  %12.1f  %6s\n", sizes[s], np, na, mode);
    }

    /* powers of two up to, and including, the full core count -- the last
     * step to all cores is where saturation (if any) shows up */
    {
        int counts[24], nc = 0, x, ci;
        for (x = 1; x < ncpu && x <= 256; x *= 2)
            counts[nc++] = x;
        counts[nc++] = ncpu <= 256 ? ncpu : 256;

        printf("\nper-thread private scaling (each thread its own 64 MB, like v6 pads):\n");
        printf("  %4s  %10s  %12s  %8s  %6s\n", "thr", "Mhops/s", "ns/hop/thr", "totalMB", "pages");
        for (ci = 0; ci < nc; ci++) {
            double np = 0; const char *mode = "?"; size_t tot = 0;
            double r = run_case(64, counts[ci], 1, 0, 1, &np, &mode, &tot);
            if (r < 0) { printf("  %4d  %10s (out of memory at this thread count)\n", counts[ci], "-"); break; }
            printf("  %4d  %10.2f  %12.1f  %6zu MB  %6s\n", counts[ci], r, np, tot, mode);
        }

        printf("\nshared-region scaling (all threads chase one 512 MB region):\n");
        printf("  %4s  %10s  %12s  %6s\n", "thr", "Mhops/s", "ns/hop/thr", "pages");
        for (ci = 0; ci < nc; ci++) {
            double np = 0; const char *mode = "?";
            double r = run_case(512, counts[ci], 0, 0, 1, &np, &mode, NULL);
            if (r < 0) { printf("  %4d  %10s (no memory)\n", counts[ci], "-"); break; }
            printf("  %4d  %10.2f  %12.1f  %6s\n", counts[ci], r, np, mode);
        }

        /* MLP experiment: does giving each hash K independent in-flight
         * accesses (a) let one core go faster, and (b) make the whole machine
         * saturate its memory controller at FEWER cores -- i.e. cap per-machine
         * throughput near "one package = one vote"? */
        {
            static const int Ks[] = { 1, 2, 4, 8 };
            int ki;

            printf("\nMLP single-thread (512 MB, one core, more chains = more requests in flight):\n");
            printf("  %6s  %10s  %12s\n", "chains", "Mhops/s", "ns/access");
            for (ki = 0; ki < 4; ki++) {
                double np = 0; const char *mode = "?";
                double r = run_case(512, 1, 0, 0, Ks[ki], &np, &mode, NULL);
                if (r < 0) { printf("  %6d  %10s\n", Ks[ki], "(no memory)"); continue; }
                printf("  %6d  %10.2f  %12.1f\n", Ks[ki], r, np);
            }

            printf("\nMLP scaling at 4 chains/thread (512 MB shared) -- does it saturate sooner?\n");
            printf("  %4s  %10s  %12s  %6s\n", "thr", "Mhops/s", "ns/access", "pages");
            for (ci = 0; ci < nc; ci++) {
                double np = 0; const char *mode = "?";
                double r = run_case(512, counts[ci], 0, 0, 4, &np, &mode, NULL);
                if (r < 0) { printf("  %4d  %10s (no memory)\n", counts[ci], "-"); break; }
                printf("  %4d  %10.2f  %12.1f  %6s\n", counts[ci], r, np, mode);
            }

            /* Machine ceiling: all cores, chains swept. Where Mhops/s stops
             * growing is this machine's DRAM random-access ceiling -- the
             * per-package number a saturating PoW would pin every box to,
             * regardless of how many cores it has. Compare boxes by this. */
            printf("\nmachine ceiling (all %d threads, 512 MB shared, chains swept):\n", ncpu);
            printf("  %6s  %8s  %10s  %12s  %8s\n", "chains", "inflight", "Mhops/s", "ns/access", "GB/s");
            {
                static const int CKs[] = { 1, 2, 4, 8, 16 };
                for (ki = 0; ki < 5; ki++) {
                    double np = 0; const char *mode = "?";
                    double r = run_case(512, ncpu, 0, 0, CKs[ki], &np, &mode, NULL);
                    if (r < 0) { printf("  %6d  %8s\n", CKs[ki], "(no mem)"); continue; }
                    printf("  %6d  %8d  %10.2f  %12.1f  %8.2f\n",
                           CKs[ki], CKs[ki] * ncpu, r, np, r * NODE_SIZE / 1000.0);
                }
            }
        }
    }

    return 0;
}
