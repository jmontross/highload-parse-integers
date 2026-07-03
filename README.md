# highload-parse-integers

Race to the fastest correct solution for the [HighLoad.fun **parse_integers**](https://highload.fun/challenges/compute/parse_integers)
challenge: sum 50,000,000 newline-separated integers, scored on wall-time (30s / 512MB / 1 core).

An autonomous agent runs every 2 hours to invent, **test**, and benchmark new variants,
promoting the fastest correct one to `champion/`. See **[AGENT.md](AGENT.md)** for the loop
and **[SCOREBOARD.md](SCOREBOARD.md)** for standings.

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
