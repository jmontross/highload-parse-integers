# Scoreboard — unique_strings

Canonical distinct count (seeded `gen`, N=10,000,000, M=8,000,000): **`5706971`**
Champion metric: median-of-N wall-clock of `./prog < input.txt` on this box.

> The **judge** (highload.fun) is the real score — submit the champion and record its
> reported time here. Local times are for relative ranking between runs.

## Champion
- **v1 (seed)** — owner's baseline: FNV-1a hash → `std::unordered_set<uint64_t>`,
  print set size. Correct on the generated input (matches exact count, no collision).
  Counts distinct *hashes* — a 64-bit collision would undercount (see AGENT.md).
  Judge time: _not yet submitted_.

## Log
| Date | Variant | Local median | Correct? | Kept? | Notes |
|---|---|---|---|---|---|
| 2026-07-03 | v1 seed (FNV-1a + std::unordered_set) | _first run pending_ | ✓ (matches 5706971, +11 edge) | ✓ champion | owner's baseline; asserts active under -O3 |

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
