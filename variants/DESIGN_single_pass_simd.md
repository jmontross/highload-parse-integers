# Design spec: single-pass, minimal-state SIMD parser (owner's idea)

**Owner's framing:** "go memory-level deep and only track the sum and the next number."

This is the right target. It says: **touch every input byte exactly once**, and carry
only **two registers of live state** across the stream — `sum` (the running total) and
`v` (the number currently being built). Everything else is transient.

## Why it matters — the current champion breaks this
`champion/main.cpp` (SWAR v2) calls `memchr` to find each newline, THEN re-reads those
same bytes in `parse_8`. That is **two passes over every line's bytes** (memchr scans,
parse reads again). The owner's rule — one pass, minimal state — says: eliminate the
memchr scan. Find boundaries and parse in the *same* sweep.

## Scalar form (already minimal-state, but latency-bound)
```
sum=0, v=0
for each byte b (once):
    d = b - '0'
    if d <= 9: v = v*10 + d      // extend the "next number"
    else:      sum += v; v = 0   // newline: bank it, reset
```
Two registers, one pass. But the `v=v*10+d` chain is serial → ~11 ns/line. So keep the
*state model* and lift it to SIMD width.

## SIMD form (the win) — same two-register state, one pass, 32/64 bytes at a time
Process the mmap'd buffer in AVX2 (32B) or AVX-512 (64B) blocks. The ONLY state carried
between blocks is `sum` and a small **partial** for a number that straddles the block
boundary. Per block:
1. Load 32/64 bytes. Compare to `'\n'` → `movemask`/mask = newline bitmap.
2. From the bitmap, know where each complete number ends inside the block.
3. Compute each byte's positional weight = `10^(distance to its number's end)` — derivable
   from the newline positions. Digits are `byte - '0'`.
4. `sum += Σ (digit · weight)` via `pmaddubsw` / `vpdpbusd` (multiply-add).
5. Carry the leading fragment (before the first newline) into the previous block's
   trailing `partial`; export this block's trailing fragment as the new `partial`.

Cross-block state = `{ sum, partial, partial_len }` — exactly "sum + the next number,"
just at SIMD width. Each byte is read once. This drives toward the **bandwidth floor**
(`run.sh` prints it): when time ≈ floor, we're memory-bound and done.

## Correctness + portability (per AGENT.md)
- Gate behind `#ifdef __AVX2__` / `__AVX512F__`; scalar loop above as `#else`.
- Must reproduce canonical sum `53687387166542798` AND pass `tests/edge.sh`.
- Watch the boundary math for numbers that span two blocks and for the final block
  (no trailing newline). Validate on the x86 routine box (ARM can't build the intrinsics).

**Priority:** highest. This is the direct path from ~7× above the floor down toward it.
