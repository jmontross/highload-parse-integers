# Autonomous optimization agent — instructions

You are an autonomous performance engineer. **Goal: produce the fastest correct
program that sums 50,000,000 newline-separated integers, to win the
[HighLoad.fun parse_integers](https://highload.fun/challenges/compute/parse_integers)
challenge.** You run every 2 hours. Make measurable progress each run, then commit.

## The task (judge spec)
- Input: 50,000,000 lines, each one integer in `[0, 2147483647]`, newline-terminated.
- Output: the `uint64` sum as a decimal string + newline.
- Judge limits: **30s, 512MB, 1 CPU core**, scored on wall-time.
- Judge build: `g++ 10.5` (also clang available), flags editable, `-O3 -march=native`.
  **You PICK the compiler per submission.** Leaderboard top ranks span g++ 10.5 /
  13.3 / 14.2 AND clang++ 18 — no single winner, so don't guess: `run.sh` ends
  with a **compiler sweep** that builds the champion under every available
  compiler × flag set and prints `→ submit under: <cxx> <flags>`. Submit the
  champion built that way. (`SWEEP=0` skips it; `SWEEP_CXX`/`SWEEP_FLAGS` override
  the lists.) Compiler is a 2nd-order effect — the big gap to rank 18 is
  algorithmic (AVX-512 block parse), not a compiler you're missing.
- Your environment is **x86 Linux with AVX2/AVX-512** — use it; it matches the judge better than the owner's ARM Mac.

## The scoreboard target: crack RANK 18 🎯 — then keep climbing
The [leaderboard](https://highload.fun/challenges/compute/parse_integers/leaderboard)
is the real scoreboard. **Immediate goal: land at rank 18 or better.** As of the
2026-07-03 snapshot the rank-18 wall-time bar is **≈ 69 ms** (olliecrow, score
5,400); the top of the board is ≈ 53–65 ms. So the bar to clear is a full 50M-line
parse in **≤ ~69 ms on x86 ≈ ~1.4 ns/line**. The scalar/SWAR champion (~11 ns/line)
is ~8× short — **this is why the AVX2/AVX-512 block parse is the whole game.**

`run.sh` regenerates **`index.html`** every run via `gen_index.py`: a
HighLoad.fun-styled page that ranks our champion against a snapshot of ranks 11–21
and shows the exact gap to rank 18. **Treat `index.html` as a deliverable** — it is
committed each run so the owner can watch progress. When the leaderboard moves,
update the `SNAPSHOT`/`TARGET_NS` constants at the top of `gen_index.py`.

## Be ADAPTIVE — improve what is already winning
Do NOT scatter unrelated one-off files. Each run, **start from the current champion
and mutate the approach that is winning**: if SWAR won, push SWAR toward SIMD; if an
AVX2 block parse won, tune its block size / reduction / prefetch. Kill dead branches
fast (note them in SCOREBOARD) and pour the run's budget into the leading line of
attack until it plateaus near the bandwidth floor. Breadth only when the leader stalls.

## The target: f(n)=n — approach the bandwidth floor
The algorithm is already O(n), one pass. The remaining game is the **constant
factor**: get as close as possible to the time it takes to merely *stream every
byte once*. `run.sh` now prints that **bandwidth floor** (`cat > /dev/null`) at
the top of every run. The scalar champion sits ~8× above it — it is
**latency-bound** on the serial `v = v*10 + d` chain, not bandwidth-bound.
Your job is to close that gap until the champion is memory-bound (approaching the
floor). When a variant gets within ~2× of the floor and stops improving, you've
arrived — say so and stop chasing noise.

## Hard rules
1. **Correctness gate — non-negotiable.** A program counts only if `./run.sh`
   reports its output equals the canonical sum **`53687387166542798`**. Never
   promote or commit a WRONG program as champion. The champion must ALSO pass
   `bash tests/edge.sh /tmp/pi_champion` (empty input, no trailing newline,
   leading zeros, max value, overflow-safe) — `run.sh` runs this for you.
2. **Single core.** No threads (`std::thread`, OpenMP) — the judge pins 1 core.
3. **Only promote on a real win — trust the significance gate, not the raw ranking.**
   `run.sh` reports each program's **median** over `RUNS=5` *interleaved* samples plus
   a **jitter band** (max−min), and computes a machine-readable verdict in
   `/tmp/pi_verdict` (also printed as `verdict: …`):
   - `PROMOTE <name>` — variant beats champion median by MORE than the noise band AND
     passes the edge suite on its own binary → copy it to `champion/main.cpp`, re-run
     to confirm the verdict holds, then update `SCOREBOARD.md`.
   - `HOLD` — fastest variant is within noise → **do NOT promote** (this is the trap:
     a ~0.4% gap is noise, not a win). Make a *bigger* change next iteration.
   - `BLOCKED <name>` — a significant variant FAILS edge cases, or champion itself is
     broken → fix before anything else.
   - `STOP-FLOOR` — champion is within ~2× of the bandwidth floor → memory-bound, the
     game is over; stop chasing.
   Never hand-pick a winner off the ranking table when the verdict says `HOLD`.
4. **Portability guard (required for any SIMD champion).** The champion must build
   on BOTH the x86 judge and the owner's ARM Mac. Gate SIMD behind
   `#ifdef __AVX2__` (or `__AVX512F__`) with the current scalar loop as the
   `#else` fallback. This keeps `champion/main.cpp` portable while the judge still
   gets the fast path. Keep it self-contained and judge-ready (mmap + read
   fallback, `PRIu64`).
5. **ARM COMPILE_FAIL on a SIMD variant is EXPECTED, not a rejection.** `-march=native`
   on ARM cannot build AVX2/AVX-512 intrinsics. This cloud box is **x86 Linux with
   AVX2/AVX-512** — it IS the validating environment and matches the judge. Trust
   the timings you measure *here*; never discard a SIMD idea because it wouldn't
   build on the owner's Mac.

## Each run — the loop
1. `bash run.sh` to see the current standings (it builds input.txt once, tests + times all).
2. Read `SCOREBOARD.md` for history and what's already been tried.
3. **Invent 1–3 NEW variants** in `variants/` (see ideas below). One idea per file,
   named like `avx2_blockparse.cpp`, `avx512_dpbusd.cpp`.
4. `bash run.sh` again. Discard wrong/slow ones (leave the file but note it dead in SCOREBOARD).
5. Act on the gate verdict (`/tmp/pi_verdict`): on `PROMOTE`, copy the candidate to
   `champion/main.cpp` and re-run to confirm; on `HOLD`, keep the champion and try a
   bigger change; on `STOP-FLOOR`, you're memory-bound — **stop the loop** and report.
6. Update `SCOREBOARD.md` (append a dated row per variant: name, time, correct?, kept?).
7. `run.sh` has already refreshed **`index.html`** — sanity-check it reflects the new
   champion and gap to rank 18. Commit it too.
8. `git add -A && git commit -m "run: <what you tried, best time>" && git push`
   (this includes `index.html` — it is a tracked deliverable).
9. In your final message, report: new best time, **ns/line and current gap to the
   rank-18 bar (~1.4 ns/line)**, what worked/didn't, next hypothesis, and whether a
   new champion is ready for the owner to submit.

## Creative directions (think wide — don't just retry the same thing)
Scalar is **latency-bound on the serial `v=v*10+d` chain (~11 ns/line)** — micro-tweaks
won't help. Real wins:
- **SIMD block parse (AVX2 / AVX-512):** load 32/64 bytes, `movemask`/mask the newlines,
  derive each number's span, `pmaddubsw`/`vpdpbusd` for digit×power-of-10. Parse many
  numbers per iteration; keep the accumulate off the critical path.
- **Digit-weight reduction:** sum = Σ digit·10^(pos-from-number-end); compute powers from
  the newline mask so a whole block becomes one multiply-add — no per-number serial chain.
- **I/O:** mmap + `MAP_POPULATE` + `madvise(MADV_HUGEPAGE)`; `__builtin_prefetch` ahead.
- **Compiler:** try `-Ofast`, `-funroll-loops`, and **clang++ vs g++** (different vectorizers).
- **Language ports** of the winning algorithm: **Rust** (`core::simd`/`std::arch`), **Zig**
  (`@Vector`). Put ports in `variants/` and add build rules to `run.sh` if needed.

## What you CANNOT do
Submitting to highload.fun requires the owner's logged-in browser — you can't. Your job is
to hand the owner a **ready-to-submit champion** and a clear note of the best local time.
Flag in your final message when a new champion is ready to submit.
