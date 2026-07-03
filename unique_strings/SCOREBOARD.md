# Scoreboard — unique_strings

Canonical distinct count (seeded `gen`, N=10,000,000, M=8,000,000): **`5706971`**
Champion metric: median-of-N wall-clock of `./prog < input.txt` on this box.

> The **judge** (highload.fun) is the real score — submit the champion and record its
> reported time here. Local times are for relative ranking between runs.

## Champion
- **v3** — `mmap + interleaved 16-byte slots + direct masked key load`. Three wins
  over v2: (1) one flat `{lo,hi}` array → 1 cache line per probe (v2 used separate
  lo[]/hi[] = 2 misses); (2) mmap zero-copy input (no fread + memmove); (3) load
  klo/khi straight from the buffer and mask to `len` (no `kb[16]` staging + 3
  memcpys per token). Same exact key → same result. **median 0.407s @10M ARM
  (~43% faster than v2, 0.712→0.407s)**, promoted via the significance gate
  (Δ 0.305s ≫ ±0.057 noise; 11/11 edge, 5706971). Judge: _pending resubmit_.
- **v2** — `open-addressing set of EXACT 128-bit keys` (linear probing, 2^24 slots).
  A token ≤16 B with no NUL zero-pads to a unique 16-byte key → **exact, no
  hash-collision risk**, and no node allocation. **~0.70s median @10M ARM
  (3.8× faster than the seed).**
  **✅ JUDGE (submitted 2026-07-03, solution 37c45): SUCCESS — score 123,238**
  (first try, correct; Joshua's RP 286.63 → 367.77). First REAL judge result —
  prior "rank/ms" lines in this repo were agent estimates, not actual submissions.
- v1 (seed) — owner's baseline: FNV-1a → `std::unordered_set<uint64_t>`. ~2.65s ARM.
  Superseded by v2 (also removes the tiny 64-bit collision risk).

## Log
| Date | Variant | Local median | Correct? | Kept? | Notes |
|---|---|---|---|---|---|
| 2026-07-03 | v1 seed (FNV-1a + std::unordered_set) | ~2.65s @10M | ✓ (matches 5706971, +11 edge) | ✗ | owner's baseline; asserts active under -O3, node-based set |
| 2026-07-03 | v2 open_addr_u128 (exact 128-bit open-addressing) | ~0.70s @10M | ✓ (5706971, +11 edge) | ✓ champion | 3.8× faster; exact (no collision); ±0.04s jitter, Δ≫noise |

## Tried & dead (don't repeat without a new angle)
_(none yet)_

## Next hypotheses (highest expected payoff first)
1. **Open-addressing hash set** (robin-hood / linear probing, power-of-2 mask) to
   replace node-based `std::unordered_set` — usually the biggest single win.
2. **Exact 128-bit keys** (token ≤16 B fits in `__int128`) in an open-addressing set —
   removes collision risk AND string storage.
3. **Faster hash** (XXH3/wyhash; single 128-bit load+mix for ≤16-byte tokens).
4. **SIMD tokenization** (AVX2/AVX-512 newline movemask) + `-DNDEBUG` to drop asserts.
5. mmap + hugepages + prefetch once the dedup structure is fast.
