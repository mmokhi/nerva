# HF14 / CNA v7 design notes

Status: implementation phase, on this branch. Everything here is measured and
reproducible with the bench in this directory. Last updated 2026-07-06. Not an
issue yet on purpose, the team is busy shipping HF13.

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
  The one specific sub-question: v6's 8 MB per-nonce pad incidentally capped
  GPU nonce-concurrency (VRAM / 8 MB); the 256 KB pad lifts that cap ~30x.
  Reasoned prediction: no material change, because occupancy hides memory
  latency, not warp divergence, and the per-nonce divergent program (identical
  to v6, not JIT-able since it changes every nonce) was and stays the GPU's
  binding constraint; ~3000 resident nonces already hid latency under v6.
  On ASIC the pad shrink cuts the other way: 8 MB was feasible as on-die
  SRAM, 240 MB of commodity DRAM is not, so ASIC resistance is equal or
  better. Settled only by the port test below.
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

## Implementation spec

The open items above, spelled out.

Crypto layer (all in C, compiled twice as _hw/_sw like v13):

- cn_slow_hash_v14(context, data, len, hash, seed, dataset, dataset_qwords):
  v13's framework verbatim (Keccak absorb, AES pad fill, salt XOR cycling
  every CN_SALT_MEMORY, the 32 random-value pokes, regs from state.k, one
  per-nonce program, iteration loop, register fold, AES fold, extra hash),
  with only these deltas: the pad is CN_SCRATCHPAD_MEMORY_V14 = 256 KB, the
  iteration calls cn_vm_execute_v7 with the dataset, and a chain_state
  (seeded from the Keccak state, e.g. state.init+64) threads through all
  passes and folds into a register at the end so the walk is load-bearing.
- cn_vm_execute_v7: per pass, 8 segments of [chain ^= regs[seg & 7], then
  128 serial hops in one tight loop, then 64 program steps]. Each hop:
  index = high word of chain * dataset_qwords (mul128, no divide, no pow2
  length requirement), load 8 bytes, chain = value + hop_index (kills short
  value cycles), record into vals[]. SP_READ consumes vals round-robin
  (+imm); SP_WRITE keeps v6 semantics against the 256 KB pad; ALU/CBRANCH
  unchanged; pc persists across segments.
- pad storage: reuse the first 256 KB of the existing 8 MB cna_scratchpad.
  The v13 buffer must stay allocated for historical validation anyway, so a
  separate allocation only adds RAM.
- compile-time tripwires, both consensus-critical: CN_V7_HOPS >=
  CN_PROGRAM_SIZE (a segment supplies 128 values and consumes at most 64;
  break the ratio and SP_READ reads uninitialised stack, forking the chain)
  and sizeof(block_cache_data) == 56 (the chase reads the struct's raw
  bytes; reorder or pad it and the PoW silently changes; little-endian
  assumed, same as v6).
- self-test: extend cn_slow_hash_self_test with a v14 case over a small
  deterministic synthetic dataset so HW and SW paths are compared at start.

DB layer:

- expose the block cache as a read-only flat view {data, qwords} plus a
  guard holding a shared lock on m_block_cache_lock for the whole hash, so
  build_block_cache (unique lock) cannot reallocate the vector under the
  chase. Take build_block_cache(stable_height) first, then the shared lock,
  then clamp the view to min(cache size, stable_height) entries so every
  node chases the identical immutable prefix regardless of cache fill.
- lock-hold tradeoff, accepted: a block-add waits behind in-flight hashers
  for up to one hash duration. Fine at 60 s blocks, revisit if hash time
  grows.
- sizing policy (still to decide): sliding cap, newest N blocks totalling
  256-512 MB, above every consumer cache, inside a 2 GB SBC forever.

Consensus plumbing:

- get_block_longhash_v14 mirrors v13: stable_height = height - 256, salt
  from get_cna_v6_data, seed = blob_hash XOR salt[0..32) (pool resistance
  unchanged), random_values from get_cna_v2_data BUT bounded to the 256 KB
  pad. HAZARD found during a prior implementation pass: context->
  cached_height caches random_values per height and the bound differs
  between v13 (8 MB) and v14 (256 KB); around the fork a context can hash
  both versions at the same height (competing chains), and a stale
  v13-bounded cache served to v14 forks the chain. Key the cache by
  (height, version) or do not cache on the v14 path at all.
- dispatch in get_block_longhash: case 13 -> v13, default >= 14 -> v14.
- activation: hard_forks entries for testnet/stagenet; mainnet height only
  at launch. Difficulty retargets through the fork on its own, but choose
  the iteration count so absolute hashrate lands near v13's (the benched
  shape at 2048 passes is ~4x v13's per-hash cost; ~512 passes is close to
  parity) to soften the difficulty cliff.
- miner UX: the slow-pages warning is v13-pad-specific; under v14 the pad is
  256 KB and the binding is the dataset, so regate that message post-fork.

## Validation gates, in order

- post-implementation: run the real cn_slow_hash_v14 on both reference boxes
  and confirm the cross-box ratios carry over from the bench (also settles
  the xorshift-for-AES approximation), plus the HW/SW self-test on both.
- testnet: fork at the testnet height, mine and validate across the fleet.
- pre-mainnet: GPU port test, v6 vs v7 throughput on the same card, to close
  the occupancy question above. Only then set the mainnet height.

## Reproducing

    cc -O2 -pthread -I. hf14bench.c hc128.c -o hf14bench   # plus cna-vm.h, hc128.h/.c
    ./hf14bench            # all cases
    ./hf14bench <threads>  # cap threads

drambench.c in contrib/drambench measures the raw latency/bandwidth/MLP
numbers cited above. Raw run outputs from the campaign live on the jetson in
~/benches and as hf14bench-jetson*.txt in the repo root (untracked).
