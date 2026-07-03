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
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/* hf14bench: HF14 prototype bench. Runs the real CNA v6 VM program in two
 * memory bindings and compares hashrate and memory pressure:
 *
 *   v6 mode:  SP_READ/SP_WRITE against a private 8 MB pad through one serial
 *             chain, exactly cn_vm_execute from cna-vm.c. One read in flight
 *             per thread; a box's rate scales with its core count.
 *
 *   v7b mode: each pass opens with one long, strictly serial chase over a
 *             shared read-only dataset (stands in for the ~240 MB block
 *             cache); the program consumes the chased values and SP_WRITE
 *             keeps a small private pad. Serial DRAM latency is the one
 *             hardware quantity where a small board equals a desktop, so
 *             the hash is made almost entirely of it.
 *
 * Same program and iteration count; v7b does more memory work per hash, so
 * absolute H/s differ by design (difficulty rescales that). The number that
 * matters is the cross-box ratio within one mode.
 *
 * Build (needs cna-vm.h, hc128.h, hc128.c next to this file, or -I to them):
 *   cc -O2 -pthread -I. hf14bench.c hc128.c -o hf14bench
 * Run:
 *   ./hf14bench            all cases
 *   ./hf14bench <threads>  cap the thread count */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cna-vm.h"
#include "hc128.h"

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

/* stands in for the block cache; power of 2 so addressing is one mask
 * (the real thing sizes to chain height, ~240 MB at 4.3M blocks) */
#define DATASET_SIZE   ((size_t)256 << 20)
/* matches CN_SCRATCHPAD_MEMORY_V13 (hash-ops.h) */
#define PAD_SIZE       ((size_t)8 << 20)
/* CNA v7 candidate B: one strictly serial chain (K=1, like v6's own chain)
 * walking the shared dataset. Serial DRAM latency is the one quantity where
 * a Jetson core equals a desktop core (143.5 vs 152.2 ns under full load,
 * the desktop is even slightly worse), so the hash is made mostly of it:
 * a long tight chase per pass drowns the clock-bound parts (interpreter,
 * fill) that candidate A left exposed. Candidate A (8 parallel lanes,
 * saturate the memory ceiling) measured 4.81x box gap vs v6's 4.21x on the
 * reference pair and was rejected; ceilings differ 5.3x between the boxes
 * while thread count differs only 3.5x, so on this pair the bandwidth world
 * can never win. Numbers in the branch history. */
#define V7B_HOPS       1024
/* the chase is cut into segments gated on a register, so nothing can walk
 * the dataset without executing the per-nonce random program in between:
 * that execute-between-chases barrier is what keeps GPUs (divergent
 * programs) and ASICs (CPU-shaped circuit required) out, same defense
 * class as v6. Within a segment the chase stays one tight serial loop. */
#define V7B_SEGMENTS   8
#define V7B_SEG_HOPS   (V7B_HOPS / V7B_SEGMENTS)
/* small pad: write-hardness only, and the v13-style 8 MB per-nonce refill
 * was a clock-bound fast-core amplifier (~75% of hash time). 256 KB per
 * thread also fits caches on both boxes, so writes cost everyone alike. */
#define V7B_PAD_SIZE   ((size_t)256 << 10)
/* matches CN_SALT_MEMORY (hash-ops.h) */
#define SALT_SIZE      ((size_t)256 << 10)
#define RUN_SECONDS    6

/* ------------------------------------------------------------------ */
/* copied verbatim from src/crypto/cna-vm.c @ 440dcf0 so this stays a
 * 4-file standalone build (cna-vm.c drags in hash-ops.h and friends); the
 * v14 variant below is the only thing that differs from the original    */
/* ------------------------------------------------------------------ */

static void bench_generate_program(cn_vm_program_t *prog, const uint8_t seed[32])
{
    HC128_State rng;
    HC128_Init(&rng, (unsigned char *)seed, (unsigned char *)(seed + 16));
    HC128_NextKeys(&rng);

    size_t key_idx = 0;

    static const uint8_t alu_ops[7] = {
        CN_OP_IADD_RS, CN_OP_ISUB, CN_OP_IMUL, CN_OP_IXOR,
        CN_OP_IROR,    CN_OP_CBRANCH, CN_OP_MIX
    };
    const uint32_t mem_pct = 51U + HC128_U32(&rng, &key_idx, 13U);

    for (int i = 0; i < CN_PROGRAM_SIZE; i++)
    {
        cn_vm_instruction_t *ins = &prog->instructions[i];

        if (HC128_U32(&rng, &key_idx, 100U) < mem_pct)
            ins->op = (HC128_U32(&rng, &key_idx, 100U) < 55U) ? CN_OP_SP_READ : CN_OP_SP_WRITE;
        else
            ins->op = alu_ops[HC128_U32(&rng, &key_idx, 7U)];
        ins->dst   = (uint8_t)HC128_U32(&rng, &key_idx, CN_REG_COUNT);
        ins->src   = (uint8_t)HC128_U32(&rng, &key_idx, CN_REG_COUNT);
        ins->shift = (uint8_t)HC128_U32(&rng, &key_idx, 256);
        uint32_t lo = HC128_U32(&rng, &key_idx, 0x10000);
        uint32_t hi = HC128_U32(&rng, &key_idx, 0x10000);
        ins->imm = (hi << 16) | lo;
    }
}

static inline uint64_t ror64(uint64_t x, uint32_t r)
{
    r &= 63;
    if (r == 0)
        return x;
    return (x >> r) | (x << (64 - r));
}

static inline uint64_t mix64(uint64_t val, uint32_t key_material)
{
    uint64_t k = (uint64_t)key_material * UINT64_C(0x9e3779b97f4a7c15);
    val ^= k;
    val ^= val >> 30;
    val *= UINT64_C(0xbf58476d1ce4e5b9);
    val ^= val >> 27;
    val *= UINT64_C(0x94d049bb133111eb);
    val ^= val >> 31;
    return val;
}

/* v6 baseline: cn_vm_execute verbatim (private pad, one serial chain) */
static void bench_execute_v6(cn_vm_program_t *prog, uint8_t *scratchpad, uint64_t regs[CN_REG_COUNT])
{
    const size_t sp_mask = (size_t)(PAD_SIZE - 1) & ~(size_t)7;
    const int    pc_mask = CN_PROGRAM_SIZE - 1;

    int pc = 0;
    uint64_t chain = 0;

    for (int step = 0; step < CN_PROGRAM_SIZE; step++)
    {
        const cn_vm_instruction_t *ins = &prog->instructions[pc & pc_mask];
        int next_pc = pc + 1;

        const uint8_t dst = ins->dst;
        const uint8_t src = ins->src;

        switch ((cn_vm_opcode_t)ins->op)
        {
        case CN_OP_IADD_RS:
            regs[dst] += regs[src] << (ins->shift & 3);
            break;
        case CN_OP_ISUB:
            regs[dst] -= regs[src];
            break;
        case CN_OP_IMUL:
            regs[dst] *= regs[src];
            break;
        case CN_OP_IXOR:
            regs[dst] ^= regs[src];
            break;
        case CN_OP_IROR:
            regs[dst] = ror64(regs[dst], (uint32_t)(regs[src] & 63));
            break;
        case CN_OP_CBRANCH:
            if (regs[dst] & ((uint64_t)ins->imm | 1))
            {
                int target = (pc + (int)((int8_t)ins->shift) + CN_PROGRAM_SIZE) & pc_mask;
                next_pc = target;
            }
            break;
        case CN_OP_SP_READ:
        {
            size_t addr = ((size_t)((uint64_t)regs[src] + (uint64_t)ins->imm + chain)) & sp_mask;
            memcpy(&regs[dst], &scratchpad[addr], sizeof(uint64_t));
            chain = regs[dst];
            break;
        }
        case CN_OP_SP_WRITE:
        {
            size_t addr = ((size_t)((uint64_t)regs[dst] + (uint64_t)ins->imm + chain)) & sp_mask;
            uint64_t tmp;
            memcpy(&tmp, &scratchpad[addr], sizeof(uint64_t));
            tmp ^= regs[src];
            memcpy(&scratchpad[addr], &tmp, sizeof(uint64_t));
            chain += tmp;
            break;
        }
        case CN_OP_MIX:
            regs[dst] = mix64(regs[dst] ^ regs[src], ins->imm);
            break;
        default:
            break;
        }

        pc = next_pc;
    }
}

/* ------------------------------------------------------------------ */
/* CNA v7 candidate B: same program, but each pass starts with one long,
 * strictly serial chase over the shared dataset -- every hop's address
 * needs the previous hop's value, one load in flight, on any hardware.
 * SP_READ consumes the recorded values, SP_WRITE keeps the small pad.
 * The interpreter couples into the walk once per pass, so the chase stays
 * sequential across passes and cannot be precomputed. The +h keeps a pass
 * from ever closing a short value cycle inside the random dataset. */
/* ------------------------------------------------------------------ */

static void bench_execute_v7b(cn_vm_program_t *prog, const uint8_t *dataset,
                              uint8_t *scratchpad, uint64_t regs[CN_REG_COUNT],
                              uint64_t *chain_state)
{
    const size_t ds_mask = (DATASET_SIZE - 1) & ~(size_t)7;
    const size_t sp_mask = (size_t)(V7B_PAD_SIZE - 1) & ~(size_t)7;
    const int    pc_mask = CN_PROGRAM_SIZE - 1;

    int pc = 0;
    unsigned consume = 0;
    uint64_t wchain = 0;
    uint64_t vals[V7B_HOPS];
    uint64_t c = *chain_state;

    for (int seg = 0; seg < V7B_SEGMENTS; seg++)
    {
    /* chase segment: gated on a register the previous program segment just
     * mutated, so the walk cannot be advanced without executing the
     * program. Consumption stays behind the chase by construction: a
     * program segment reads at most CN_PROGRAM_SIZE/V7B_SEGMENTS values
     * while each chase segment supplies V7B_SEG_HOPS of them. */
    {
        int h;
        c ^= regs[seg & (CN_REG_COUNT - 1)];
        for (h = 0; h < V7B_SEG_HOPS; h++)
        {
            size_t addr = ((size_t)c) & ds_mask;
            uint64_t v;
            memcpy(&v, dataset + addr, sizeof(uint64_t));
            c = v + (uint64_t)h;
            vals[seg * V7B_SEG_HOPS + h] = v;
        }
    }

    for (int step = 0; step < CN_PROGRAM_SIZE / V7B_SEGMENTS; step++)
    {
        const cn_vm_instruction_t *ins = &prog->instructions[pc & pc_mask];
        int next_pc = pc + 1;

        const uint8_t dst = ins->dst;
        const uint8_t src = ins->src;

        switch ((cn_vm_opcode_t)ins->op)
        {
        case CN_OP_IADD_RS:
            regs[dst] += regs[src] << (ins->shift & 3);
            break;
        case CN_OP_ISUB:
            regs[dst] -= regs[src];
            break;
        case CN_OP_IMUL:
            regs[dst] *= regs[src];
            break;
        case CN_OP_IXOR:
            regs[dst] ^= regs[src];
            break;
        case CN_OP_IROR:
            regs[dst] = ror64(regs[dst], (uint32_t)(regs[src] & 63));
            break;
        case CN_OP_CBRANCH:
            if (regs[dst] & ((uint64_t)ins->imm | 1))
            {
                int target = (pc + (int)((int8_t)ins->shift) + CN_PROGRAM_SIZE) & pc_mask;
                next_pc = target;
            }
            break;
        case CN_OP_SP_READ:
            /* consume the chased values in order; imm keeps the
             * consumption per-instruction distinct */
            regs[dst] ^= vals[consume & (V7B_HOPS - 1)] + (uint64_t)ins->imm;
            consume++;
            break;
        case CN_OP_SP_WRITE:
        {
            size_t addr = ((size_t)((uint64_t)regs[dst] + (uint64_t)ins->imm + wchain)) & sp_mask;
            uint64_t tmp;
            memcpy(&tmp, &scratchpad[addr], sizeof(uint64_t));
            tmp ^= regs[src];
            memcpy(&scratchpad[addr], &tmp, sizeof(uint64_t));
            wchain += tmp;
            break;
        }
        case CN_OP_MIX:
            regs[dst] = mix64(regs[dst] ^ regs[src], ins->imm);
            break;
        default:
            break;
        }

        pc = next_pc;
    }
    }

    *chain_state = c;
    /* fold write-chain back so pad writes stay load-bearing too */
    regs[0] ^= wchain;
}

/* ------------------------------------------------------------------ */
/* harness                                                              */
/* ------------------------------------------------------------------ */

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

static void *big_alloc(size_t bytes, const char **mode)
{
#ifdef _WIN32
    *mode = "normal";
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
#ifdef MADV_HUGEPAGE
    *mode = madvise(p, bytes, MADV_HUGEPAGE) == 0 ? "thp" : "normal";
#else
    *mode = "normal";
#endif
    return p;
#endif
}

static const uint8_t *g_dataset;
static volatile int g_go, g_stop;

typedef struct {
    int      cpu;
    int      v14;
    uint8_t *pad;
    uint8_t *salt;
    uint64_t hashes;
    uint64_t digest;
} wctx_t;

/* One full hash shaped like cn_slow_hash_v13: fresh program from the nonce
 * (v13 seeds it with blob_hash ^ chain_salt, both per-nonce), 256 KB salt
 * rebuilt per nonce (get_cna_v6_data), full pad fill pass, salt XOR pass,
 * CN_VM_ITERATIONS program passes with persistent registers, and a full
 * fold pass of the pad into the digest (the finalization AES read). The AES
 * and HC128 streams are approximated with xorshift: same memory traffic per
 * hash, cheaper rounds. Benching only the VM loop overstates cache
 * residency and hides the per-hash bandwidth tax. */
static uint64_t one_hash(uint8_t *pad, uint8_t *salt, int v14, uint64_t nonce)
{
    cn_vm_program_t prog;
    uint8_t seed[32];
    uint64_t regs[CN_REG_COUNT];
    uint64_t chain_state;
    /* fill/salt/fold cover the pad the mode actually addresses */
    const size_t psize = v14 ? V7B_PAD_SIZE : PAD_SIZE;
    uint64_t s = nonce ^ UINT64_C(0x6e657276615f7634);
    uint64_t d = 0;
    size_t off;
    int i;

    for (i = 0; i < 32; i++)
        seed[i] = (uint8_t)(xorshift64(&s) >> 56);
    bench_generate_program(&prog, seed);

    {
        uint64_t f = xorshift64(&s);
        for (off = 0; off < SALT_SIZE; off += 8)
        {
            uint64_t v = xorshift64(&f);
            memcpy(salt + off, &v, 8);
        }
        for (off = 0; off < psize; off += 8)
        {
            uint64_t v = xorshift64(&f);
            memcpy(pad + off, &v, 8);
        }
        for (off = 0; off < psize; off += 8)
        {
            uint64_t v, w;
            memcpy(&v, pad + off, 8);
            memcpy(&w, salt + (off & (SALT_SIZE - 1)), 8);
            v ^= w;
            memcpy(pad + off, &v, 8);
        }
    }

    for (i = 0; i < CN_REG_COUNT; i++)
        regs[i] = xorshift64(&s);
    chain_state = xorshift64(&s);

    for (i = 0; i < CN_VM_ITERATIONS; i++)
    {
        if (v14)
            bench_execute_v7b(&prog, g_dataset, pad, regs, &chain_state);
        else
            bench_execute_v6(&prog, pad, regs);
    }

    /* one read per line pulls the same DRAM traffic as the AES fold */
    for (off = 0; off < psize; off += 64)
    {
        uint64_t v;
        memcpy(&v, pad + off, 8);
        d = mix64(d ^ v, (uint32_t)off);
    }
    for (i = 0; i < CN_REG_COUNT; i++)
        d = mix64(d ^ regs[i], (uint32_t)i);
    if (v14)
        d = mix64(d ^ chain_state, 8);
    return d;
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID arg)
#else
static void *worker(void *arg)
#endif
{
    wctx_t *c = (wctx_t *)arg;
    uint64_t nonce = (uint64_t)c->cpu << 32;

    bind_thread(c->cpu);

    while (!g_go)
        ;
    while (!g_stop)
    {
        c->digest ^= one_hash(c->pad, c->salt, c->v14, nonce++);
        c->hashes++;
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* v6 reads are program-mix dependent (mem_pct in [51,63], 55% of those are
 * reads; midpoint estimate); v7b chase hops are exact by construction */
#define READS_PER_HASH_V6  ((double)CN_PROGRAM_SIZE * CN_VM_ITERATIONS * 0.57 * 0.55)
#define READS_PER_HASH_V7B ((double)V7B_HOPS * CN_VM_ITERATIONS)

static void run_mode(const char *name, int v14, int threads, uint8_t **pads, uint8_t **salts)
{
    wctx_t ctx[256];
    uint64_t t0, t1, hashes = 0;
    int t;
#ifdef _WIN32
    HANDLE th[256];
#else
    pthread_t th[256];
#endif

    memset(ctx, 0, sizeof(ctx));
    g_go = 0; g_stop = 0;

    for (t = 0; t < threads; t++)
    {
        ctx[t].cpu  = t;
        ctx[t].v14  = v14;
        ctx[t].pad  = pads[t];
        ctx[t].salt = salts[t];
#ifdef _WIN32
        th[t] = CreateThread(NULL, 0, worker, &ctx[t], 0, NULL);
#else
        pthread_create(&th[t], NULL, worker, &ctx[t]);
#endif
    }

    t0 = now_ns();
    g_go = 1;
#ifdef _WIN32
    Sleep(RUN_SECONDS * 1000);
#else
    { struct timespec ts = { RUN_SECONDS, 0 }; nanosleep(&ts, NULL); }
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
        hashes += ctx[t].hashes;
    {
        const double secs  = (double)(t1 - t0) / 1e9;
        const double hs    = hashes / secs;
        const double reads = hs * (v14 ? READS_PER_HASH_V7B : READS_PER_HASH_V6);
        /* every 8 B read touches one 64 B line at a random address */
        printf("  %-10s  %3d  %9.2f  %8.1fM  %6.2f\n",
               name, threads, hs, reads / 1e6, reads * 64.0 / 1e9);
    }
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
    const int ncpu = cpu_count();
    int threads = (argc > 1) ? atoi(argv[1]) : ncpu;
    const char *ds_mode = "?", *pad_mode = "?";
    uint8_t *pads[256];
    uint8_t *salts[256];
    uint64_t s = UINT64_C(0xda7a5e7);
    size_t i;
    int t;

    if (threads < 1 || threads > 256)
        threads = ncpu;

    printf("hf14bench: dataset %zu MB, pad %zu MB (v6) / %zu KB (v7b), %d ops x %d iters/hash, %d hops/pass, threads pinned\n",
           DATASET_SIZE >> 20, PAD_SIZE >> 20, V7B_PAD_SIZE >> 10, CN_PROGRAM_SIZE, CN_VM_ITERATIONS, V7B_HOPS);

    uint8_t *ds = (uint8_t *)big_alloc(DATASET_SIZE, &ds_mode);
    if (ds == NULL) { fprintf(stderr, "dataset allocation failed\n"); return 1; }
    /* pseudo-random fill stands in for block cache content */
    for (i = 0; i < DATASET_SIZE; i += 8)
    {
        uint64_t v = xorshift64(&s);
        memcpy(ds + i, &v, 8);
    }
    g_dataset = ds;

    for (t = 0; t < threads; t++)
    {
        /* content does not matter here: one_hash refills the pad and the
         * salt every hash, like the real per-nonce AES fill does */
        pads[t]  = (uint8_t *)big_alloc(PAD_SIZE, &pad_mode);
        salts[t] = (uint8_t *)big_alloc(SALT_SIZE, &pad_mode);
        if (pads[t] == NULL || salts[t] == NULL) { fprintf(stderr, "pad allocation failed\n"); return 1; }
    }
    printf("dataset pages=%s, pad pages=%s\n\n", ds_mode, pad_mode);

    /* determinism spot check, must print the same on every machine. The two
     * hashes both mutate pad[0], so they are sequenced as statements; as
     * printf arguments their evaluation order would be unspecified and the
     * digests would differ between compilers. */
    {
        uint64_t d6, d14;
        d6  = one_hash(pads[0], salts[0], 0, 42);
        d14 = one_hash(pads[0], salts[0], 1, 42);
        printf("selftest: v6=%016llx v7b=%016llx\n\n",
               (unsigned long long)d6, (unsigned long long)d14);
    }

    printf("  %-10s  %3s  %9s  %9s  %6s\n", "mode", "thr", "H/s", "reads/s", "GB/s");
    run_mode("v6", 0, 1, pads, salts);
    if (threads > 1)
        run_mode("v6", 0, threads, pads, salts);
    run_mode("v7b serial", 1, 1, pads, salts);
    if (threads > 1)
        run_mode("v7b serial", 1, threads, pads, salts);

    return 0;
}
