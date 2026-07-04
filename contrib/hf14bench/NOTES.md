# HF14 / CNA v7 design notes

Status: parked until HF13 is published and HF14 work opens. Everything here is
measured and reproducible with the bench in this directory. Last updated
2026-07-04. Not an issue yet on purpose, the team is busy shipping HF13.

Naming: HF numbers name the fork and its hash entry point (HF13 activates
`cn_slow_hash_v13`), CNA numbers name the algorithm variant (HF13 runs CNA v6).
The candidate below is CNA v7, to be activated by HF14 as `cn_slow_hash_v14`.

## Goal

One box, one vote, as close as physics allows. Close the hashrate gap between
a strong machine and a small board without barring SBCs from mining or
validating, without becoming RandomX, and without giving up GPU/ASIC/pool
resistance. Farms of N separate boxes get N votes, that is PoW and accepted.

## Reference pair and method

- strong box: Core Ultra 7 155U laptop, 14 threads, LPDDR5, 12 MB L3
- small box: Jetson Nano 2GB, 4x Cortex-A57 1.43 GHz, LPDDR4
- threads pinned, miner stopped and verified by nervad CPU% before every run,
  sustained (warm) numbers, every result confirmed by a repeat run
- cross-arch determinism enforced by a selftest digest, identical on
  x86_64/aarch64, gcc 7 and 14, -O0/-O2/-O3
- the bench approximates the AES and HC128 streams with xorshift (same memory
  traffic per hash, cheaper rounds), so absolute H/s are not calibrated
  against the real miner. Only the cross-box ratios are load-bearing, and
  those come from memory behavior the approximation does not touch.

## Results

Box totals in H/s, gap is laptop/jetson. v6 baseline is the real v13 shape
(per-nonce program, per-nonce 8 MB pad refill, 512 ops x 2048 iterations).

| design                        | laptop | jetson | per-core gap | box gap |
|-------------------------------|--------|--------|--------------|---------|
| v6 (HF13 today)               | 180.9  | 43.0   | 4.72x        | 4.21x   |
| v7-A: 8 parallel lanes        | 246.4  | 51.3   | 2.25x        | 4.81x   |
| v7-B: serial dataset chase    | 40.6   | 12.0   | 1.05x        | 3.38x   |

v7-B's box gap equals the hardware thread ratio (14/4), the floor for a
parallel-nonce PoW. Absolute H/s differ per design because memory work per
hash differs, difficulty absorbs that, only cross-box ratios matter.

## What was learned on the way (all measured)

- Serial DRAM latency is the one hardware quantity where the boxes are equal:
  143.5 vs 147 ns, and under full load the laptop is slightly worse (152 ns).
  Everything else (clock, cache, reorder window, memory bandwidth) favors the
  strong box: bandwidth ceiling 36 vs 6.76 GB/s (5.3x), which is why the
  saturation design v7-A lost, ceilings differ more than thread counts (3.5x).
- v13's 8 MB per-nonce pad refill is a fast-core amplifier: clock-bound serial
  compute, about 75% of hash time, and it keeps the memory system idle.
- The 8 MB pad fits a desktop L3: single-thread v13 mining is cache-resident
  there (10x the DRAM rate). Any big-cache box plays a different game than an
  SBC. A dataset larger than every cache kills this meta.
- A small in-order core cannot overlap loads woven through the interpreter
  (A57 effective MLP 1.1 vs 6 in a bare loop) and prefetch hints do not help.
  Big out-of-order cores overlap them fine. Parallel-lane designs therefore
  reward reorder-buffer size, the wrong thing.
- The VM interpreter itself is a fast-core amplifier (~7 us/pass on A57 vs
  ~0.5 us on a desktop core, indirect-branch dispatch), so the hash must be
  mostly chase, not mostly interpreter.

## Adopted design: CNA v7 candidate B

One strictly serial chain (K=1, v6's own chain idea) walking a large shared
read-only dataset, interleaved with the v6 VM program at segment granularity:

- per pass: 8 segments, each = 128 serial hops over the dataset in one tight
  loop, then 64 program steps. 1024 hops/pass, 2048 passes, program unchanged
  from v6 (per-nonce random generation, HC128, IMUL/MIX weighting).
- each chase segment is gated on a register the previous program segment just
  mutated. Nothing can advance the walk without executing the per-nonce
  divergent program: that barrier is the GPU/ASIC defense, same class as v6.
  Measured cost of the barrier on CPUs: zero.
- dataset: stands in for the block cache (~240 MB at current height), read
  uniformly. Exceeds every consumer cache, inherits pool resistance (the
  chain is the dataset).
- pad: 256 KB, write-hardness only, refilled per nonce. Fits everyone's
  cache, so it costs everyone alike. The 8 MB refill amplifier is gone.

## Security properties, stated precisely

- one box one vote: per-core 1.05x on the reference pair, box gap = thread
  count. Caveats: SMT threads count as votes (accepted, candidate A killed
  SMT but lost overall), and per-core parity is DRAM-latency-class parity, a
  tuned desktop with ~65 ns DDR5 gets up to ~2x per core. Both bounded by
  physics, no cache/clock/ILP tricks remain.
- anti-GPU/ASIC: the segment barrier restores v6's defense class. The exact
  GPU margin is unproven without a GPU port, same evidentiary status as v6.
- anti-pool: chain-data-coupled, not full-node-coupled. A determined pool
  could distribute the dataset plus per-block deltas. This is exactly v6's
  existing status, no regression, but do not overclaim it.
- giant-L3 server parts (1.1 GB Genoa-X class) could re-cache a 240 MB
  dataset. Mitigation is the dataset sizing policy below.

## Open items for the real implementation

- dataset: back it with the real block cache, uniform reads, and a sizing
  policy (sliding cap, e.g. newest N blocks totalling 256-512 MB) so it stays
  above consumer caches but inside a 2 GB SBC forever.
- absolute hash cost: ~0.3 s/hash as benched. Tune V7B_HOPS and/or
  CN_VM_ITERATIONS down for verification/DoS budget, ratios are preserved.
- consensus plumbing: cn_slow_hash_v14 entry, activation height, template
  version, difficulty retarget through the change.
- implementation niceties found on the way: computed-goto dispatch for the VM
  (helps small cores most, non-consensus), hc128.h needs static inline and
  stddef.h (breaks -O0 builds of C users).

## Reproducing

    cc -O2 -pthread -I. hf14bench.c hc128.c -o hf14bench   # plus cna-vm.h, hc128.h/.c
    ./hf14bench            # all cases
    ./hf14bench <threads>  # cap threads

drambench.c in contrib/drambench measures the raw latency/bandwidth/MLP
numbers cited above. Raw run outputs from the campaign live on the jetson in
~/benches and as hf14bench-jetson*.txt in the repo root (untracked).
