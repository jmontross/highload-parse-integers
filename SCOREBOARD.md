# Scoreboard — parse_integers

Canonical sum (seeded `gen`, 50,000,000 lines): **`53687387166542798`**
Champion metric: best-of-N wall-clock of `./prog < input.txt` on this Linux box.

> The **judge** (highload.fun) is the real score — submit the champion and record its
> reported time here. Local times are for relative ranking between runs.

## Champion
- **v1** — `mmap + branch-light scalar` — local baseline ~11.4 ns/line (~0.57s @50M on ARM Mac; will differ on x86). Judge time: _not yet submitted_.

## Log
| Date | Variant | Local time | Correct? | Kept? | Notes |
|---|---|---|---|---|---|
| 2026-07-02 | v0 original (fread + per-byte lambda + asserts) | — | ✓ | ✗ | replaced; per-byte call + asserts in hot loop |
| 2026-07-02 | v1 mmap + branch-light scalar | ~0.57s @50M | ✓ | ✓ champion | current baseline |
| 2026-07-02 | branchless (cmov) | ~0.57s | ✓ | ✗ | no gain — latency-bound, not branch-bound |
| 2026-07-02 | memchr boundaries | ~0.55s | ✓ | ~ | marginal; inner parse still serial |

## Tried & dead (don't repeat without a new angle)
- Pure scalar micro-tweaks (branch vs branchless vs memchr) — all ~equal; latency-bound.

## Next hypotheses (highest expected payoff first)
1. AVX2 32-byte block parse with newline `movemask`.
2. AVX-512 `vpdpbusd` digit×weight reduction.
3. mmap hugepages + prefetch on top of the winning parser.
4. Rust / Zig port of the winner.
