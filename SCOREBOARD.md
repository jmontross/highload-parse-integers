# Scoreboard — parse_integers

Canonical sum (seeded `gen`, 50,000,000 lines): **`53687387166542798`**
Champion metric: best-of-N wall-clock of `./prog < input.txt` on this Linux box.

> The **judge** (highload.fun) is the real score — submit the champion and record its
> reported time here. Local times are for relative ranking between runs.

Bandwidth floor (`cat input.txt > /dev/null`, page-cached) ≈ **0.084s** on the ARM
Mac — the f(n)=n asymptote. `run.sh` prints it every run. Champion is memory-bound
(done) when it approaches this.

## Champion
- **v5** — `branchless high digits` — removes the one hostile branch (`h==2`,
  a 10-vs-9-digit coin-flip mispredicting ~47%) by computing the high 0–2 digits
  branchlessly from h∈{0,1,2}. Portable. **best ~0.264s @50M ARM (~28% faster
  than v4)** — branch mispredict was the dominant cost. Promoted via min-of-N
  gate (best AND median lower; 9/9 edge). Now ~2.9× off the bandwidth floor.
  **Judge (submitted 2026-07-03): rank 167/900, 392 ms, score 46,107, g++10.5.0
  `-O3 -march=native`** (up from an early baseline at rank 460 / 749 ms). ~5.7×
  off the rank-18 bar (69 ms) — the AVX2/AVX-512 block parse is what closes it.
  AVX2 block algorithm now VALIDATED on ARM via `-DBLOCK_SCALAR_SIM`
  (`variants/avx2_blockparse.cpp`): same block loop, scalar mask → 50M sum ✓ +
  9/9 edge; only the `vpmovmskb` intrinsic is unexercised locally.
- **v4** — `SWAR nlfind + zero-reload hot path` — on the common 9–10 digit path,
  reconstructs the low-8 parse chunk by shifting the two words already loaded for
  the newline scan (w0,w1) instead of a third `memcpy` — one fewer load per
  number. Portable. **best ~0.368s @50M ARM (~2.6% faster than v3)**, promoted
  via the min-of-N gate (best AND median both lower; 9/9 edge). Judge: _not yet_.
- **v3** — `mmap + SWAR newline-find + SWAR number parse` — drops the per-number
  `memchr` call; derives each newline offset with a SWAR compare over a 16-byte
  window (branch-light, inline). Portable (no intrinsics). **~0.40s @50M ARM
  (~24% faster than v2, 0.533→0.405s)**, promoted through the significance gate
  (Δ ≫ noise band; 9/9 edge cases). Still ~5–6× above the bandwidth floor → AVX2
  is still the next big lever. Judge time: _not yet submitted_.
- **v2** — `mmap + SWAR number parse` — parses a whole number in a short shift/add
  tree instead of a serial multiply per digit. Portable (no intrinsics), ~**0.565s**
  @50M ARM (~11% faster than v1). Still ~7× above the bandwidth floor → AVX2 next.
  Judge time: _not yet submitted_.
- v1 — `mmap + branch-light scalar` — latency-bound on serial `v=v*10+d` (~0.63s @50M ARM). Superseded by v2.

## Log
| Date | Variant | Local time | Correct? | Kept? | Notes |
|---|---|---|---|---|---|
| 2026-07-02 | v0 original (fread + per-byte lambda + asserts) | — | ✓ | ✗ | replaced; per-byte call + asserts in hot loop |
| 2026-07-02 | v1 mmap + branch-light scalar | ~0.63s @50M | ✓ | ✗ | superseded by v2 |
| 2026-07-02 | branchless (cmov) | ~0.57s | ✓ | ✗ | no gain — latency-bound, not branch-bound |
| 2026-07-02 | memchr boundaries | ~0.62s | ✓ | ✗ | marginal; inner parse still serial |
| 2026-07-03 | v2 SWAR block parse (`swar_blockparse.cpp`) | ~0.565s @50M | ✓ (+9 edge) | ✓ champion | breaks the per-digit chain; portable, no intrinsics |
| 2026-07-03 | v3 SWAR newline-find (`swar_nlfind.cpp`) | ~0.40s @50M | ✓ (+9 edge) | ✓ champion | memchr-free boundary find; PROMOTE via significance gate (Δ=0.14s ≫ ±0.01 band) |
| 2026-07-03 | swar_prefetch (reuse load + prefetch) | ~0.396s @50M | ✓ | ✗ dead | prefetch evicts more than it saves; mmap already streams |
| 2026-07-03 | v4 swar_noreload (zero-reload hot path) | best ~0.368s @50M | ✓ (+9 edge) | ✓ champion | reconstruct low-8 chunk from w0/w1; ~2.6% win via min-of-N gate |
| 2026-07-03 | v5 swar_branchless (branchless high digits) | best ~0.264s @50M | ✓ (+9 edge) | ✓ champion | kills the h==2 coin-flip mispredict — 28% win, biggest since v3 |
| 2026-07-03 | swar_unroll2 (2 numbers/iter, dual accum) | best ~0.342s @50M | ✓ | ✗ dead | slower than v5 — serial boundary scans leave little to overlap; tail machinery + helper defeated -O3's own scheduling |

## Tried & dead (don't repeat without a new angle)
- Pure scalar micro-tweaks (branch vs branchless vs memchr) — all ~equal; latency-bound.
- `__builtin_prefetch` ahead of the scan (`swar_prefetch`) — prefetch evicts more
  than it saves; mmap MAP_POPULATE/MADV_SEQUENTIAL already streams optimally.
- 2 numbers/iter with dual accumulators (`swar_unroll2`) — slower; the per-number
  boundary scans are serial (number 2 starts where number 1 ends) so there's no
  independent work to fill the OOO window, and the manual unroll defeated -O3.
  A real latency-hiding win needs a block newline **bitmask** (find all newlines
  in a 32-byte window in one shot, then parse with boundaries already known) —
  which is essentially the AVX2/AVX-512 path. Portable single-thread looks
  latency-capped at ~0.26s on ARM (~2.9× the bandwidth floor).

## Next hypotheses (highest expected payoff first)
1. **AVX2 32-byte block parse** — `variants/avx2_blockparse.cpp`. One
   `vpcmpeqb`+`vpmovmskb` per 32 bytes yields all newline positions at once
   (~3 numbers/load), breaking the serial per-number scan chain; each number
   parsed with the v5 SWAR routine. Gated `#ifdef __AVX2__` with v5 as `#else`.
   **SUBMITTED & VALIDATED ON THE JUDGE (2026-07-03 18:00): Success, rank 119/900,
   307 ms (306,675,659 ns), score 35,819 — up from v5's rank 167 / 392 ms / 46,107
   (~22% faster).** Intrinsics + block algorithm proven on real x86.
2. **AVX-512 64-byte block parse** — WRITTEN as `variants/avx512_blockparse.cpp`.
   Tiered: `__AVX512BW__`→64B native `_mm512_cmpeq_epi8_mask`, else `__AVX2__`→32B
   `vpmovmskb`, else v5 — one file auto-picks the widest SIMD the judge CPU has
   (and sidesteps AVX-512 downclock on CPUs without AVX512BW). Block algorithm
   VALIDATED on ARM via `-DBLOCK_SCALAR_SIM` (BLK=64): sum ✓, 9/9 edge. Only the
   intrinsics themselves are unexercised locally — the judge is their x86 test.
2. AVX-512 `vpdpbusd` digit×weight reduction (one multiply-add per block).
3. SWAR without per-number `memchr`: derive newline offsets from a SWAR/`movemask`
   compare so boundary-finding is vectorized too.
4. mmap hugepages (`MADV_HUGEPAGE`) + `__builtin_prefetch` on top of the winner.
5. Rust / Zig port of the winner.
