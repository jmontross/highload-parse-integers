# highload-parse-integers

Race to the fastest correct solution for the [HighLoad.fun **parse_integers**](https://highload.fun/challenges/compute/parse_integers)
challenge: sum 50,000,000 newline-separated integers, scored on wall-time (30s / 512MB / 1 core).

An autonomous agent runs every 2 hours to invent, **test**, and benchmark new variants,
promoting the fastest correct one to `champion/`. See **[AGENT.md](AGENT.md)** for the loop
and **[SCOREBOARD.md](SCOREBOARD.md)** for standings.

---

## 📌 PICK UP HERE (paused 2026-07-03)

**Current best on the judge: rank 119 / 900** — AVX2 block parse, 307 ms, score 35,819,
`g++10.5.0 -O3 -march=native`. Your rank can only go *up*: highload.fun keeps your **best**
submission, so a slower one never displaces it.

### The one open decision
`variants/avx512_blockparse.cpp` (AVX-512, tiered AVX512BW→AVX2→scalar) is written and its
algorithm is validated, but **it hasn't been raced against AVX2 on x86 yet**. AVX-512 is *not*
a guaranteed win here (the per-number parse is still scalar; AVX-512 can also down-clock), so
**don't submit it blind** — measure first.

### To resume (get x86 numbers, then submit only a confirmed winner)
1. **Get real x86 timings** (this ARM Mac cannot benchmark AVX — it only *builds* the SIMD
   paths as the scalar fallback). Either:
   - Re-enable the cloud routine: `RemoteTrigger update {enabled:true}` on
     `trig_01DJPAkz6WoXJLdczKgeoUUd`, or its [routine page](https://claude.ai/code/routines/trig_01DJPAkz6WoXJLdczKgeoUUd); **or**
   - `bash run.sh` on any x86 Linux box.
2. **Read the verdict.** `run.sh` prints the significance gate (best-of-N + margin, edge-checked)
   and a **compiler sweep**; `index.html` has a "this run — all variants" table with each
   variant's x86 time. That's the v5 / AVX2 / AVX-512 head-to-head.
3. **Submit the confirmed fastest** at highload.fun → **Solve** → paste `champion/main.cpp`
   (or the winning variant) → pick the compiler the sweep named (try g++13.3/14.2 or
   clang++18 — neighbors at this tier use those) → **Submit**. Needs your GitHub login.

### What's paused / gotchas
- Cloud routine `trig_01DJPAkz6WoXJLdczKgeoUUd` is **disabled** (`enabled:false`). It's the
  only routine. No local timers armed.
- The 18:30 run I triggered may have been in-flight at pause and could push one stray
  `run: …` commit — review or ignore it.
- **This repo/main is shared by concurrent agents** (some on a *different* `unique_strings`
  challenge). Expect unrelated commits on `main`; stage specific files, never `git add -A`.

### What we did this session (2026-07-03)
- **Champion 0.565 s → 0.264 s on ARM** via a gated improve-loop: v3 memchr-free SWAR
  newline-find (−24%), v4 zero-reload hot path (−2.6%), v5 branchless high-digits (−28%,
  the branch mispredict was the dominant cost).
- **Hardened `run.sh`**: significance gate that promotes only on a **best-of-N win beyond the
  noise margin** *and* passing the edge suite on the candidate binary (was flipping on ~0.4%
  noise before); interleaved median+jitter timing via bash builtins (no more per-sample
  `python3` spawns); bash-3.2 compatible; machine-readable verdict; a **compiler sweep**.
- **Wrote the SIMD variants** `avx2_blockparse.cpp` and `avx512_blockparse.cpp` — one
  `vpcmpeqb`+movemask finds all newlines in a block so per-number parses pipeline. Validated
  the block algorithm on ARM via `-DBLOCK_SCALAR_SIM` (identical loop, scalar mask → 50M
  sum + 9/9 edge), so only the intrinsics were unproven locally.
- **Submitted to the judge** (browser): baseline #460 (749 ms) → v5 scalar #167 (392 ms) →
  **AVX2 #119 (307 ms)** — the AVX2 run *proved the SIMD path on real x86* (~22% over v5).
- **Unblocked the x86 cloud**: the real blocker was that the **Claude GitHub _App_ was never
  installed** (only OAuth-authorized) — installing it with repo access fixed the
  `github_repo_access_denied` clone failures. Then paused the routine at your request.

---

## Layout
- `champion/main.cpp` — current best, ready to paste into the judge.
- `variants/` — one experiment per file.
- `gen.cpp` — reproducible input generator (fixed seed).
- `bench_parsers.cpp` — head-to-head parse-strategy timing (asserts all agree).
- `run.sh` — build + **correctness gate** (canonical sum `53687387166542798`) + benchmark + rank.

## Run it
```bash
bash run.sh            # tests + benchmarks champion and all variants
N=10000000 bash run.sh # smaller input for a quicker loop
```

## Correctness invariant
Every program must output **`53687387166542798`** for the seeded 50M input, else it is
rejected. Only correct programs are ranked; only a correct + faster program becomes champion.

Notes & narrative live in the owner's Obsidian vault (`HighLoad.fun/parse_integers`).
