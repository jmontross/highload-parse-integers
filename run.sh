#!/usr/bin/env bash
# Test + benchmark harness for the HighLoad.fun parse_integers challenge.
# Correctness gate: every program's output MUST equal an independently computed
# reference sum (exact Python big-int). Speed metric: interleaved best-of-N
# wall-clock of `./prog < input.txt`, reported as MEDIAN with a run-to-run
# jitter band. The fastest CORRECT program is the champion — but a variant is
# only a *significant* winner (promotable) if it beats the champion's median by
# more than the noise band (see "promotion gate" below). This significance gate
# is what makes an autonomous improve-loop safe: without it, a ~0.4% noise gap
# would flip the ranking every run and churn false promotions into git forever.
set -uo pipefail
cd "$(dirname "$0")"

CANON=53687387166542798          # seeded gen @ N=50,000,000 (defense-in-depth check)
N=${N:-50000000}
RUNS=${RUNS:-5}                  # AGENT.md rule 3: confirm wins with >=5 runs
CXX=${CXX:-c++}
FLAGS=${FLAGS:--O3 -march=native} # same spirit as the judge

echo "== build generator & input =="
$CXX $FLAGS gen.cpp -o /tmp/gen || { echo "gen build failed"; exit 1; }
# Self-heal: regenerate when input is missing OR its line count != N (a stale
# input.txt left over from a run with a different N is the classic silent trap).
if [ ! -f input.txt ] || [ "$(wc -l < input.txt)" -ne "$N" ]; then
  echo "generating $N lines..."; /tmp/gen "$N" > input.txt
fi

echo "== reference sum (exact) =="
EXP_FILE=".expected_$N"
EXPECTED=""
[ -f "$EXP_FILE" ] && EXPECTED=$(cat "$EXP_FILE")
# The cache is keyed on N only; if input.txt was ever regenerated it can be
# poisoned. At the canonical size we KNOW the answer, so drop a wrong cache
# instead of trusting it (this is exactly the FATAL you can hit locally).
if [ "$N" = "50000000" ] && [ "$EXPECTED" != "$CANON" ]; then EXPECTED=""; fi
if [ -z "$EXPECTED" ]; then
  EXPECTED=$(python3 -c "print(sum(int(x) for x in open('input.txt','rb').read().split()))")
  echo "$EXPECTED" > "$EXP_FILE"
fi
if [ "$N" = "50000000" ] && [ "$EXPECTED" != "$CANON" ]; then
  echo "FATAL: reference ($EXPECTED) != canonical ($CANON) — bad gen.cpp, not stale input."; exit 1; fi
echo "expected = $EXPECTED"

# ---------------------------------------------------------------------------
# Timing primitive: bash builtin `time` (sub-ms, no subprocess). The previous
# harness wrapped every sample in `t0=$(python3 -c monotonic)` — spawning an
# interpreter on each side of a ~590ms measurement injected tens of ms of
# *variable* startup/teardown jitter into every sample. That noise (not any
# real code difference) is what made the champion/variant ranking flip between
# runs. `TIMEFORMAT='%R'` gives real seconds straight from the shell.
# ---------------------------------------------------------------------------
timeit() { # $@ = argv (binary); echoes real seconds
  local t
  t=$( { TIMEFORMAT='%R'; time "$@" > /tmp/out.txt < input.txt; } 2>&1 )
  printf '%s' "$t"
}
# stats: reads newline-separated samples on stdin -> "min median max"
stats() {
  sort -n | awk '{a[NR]=$1} END{
    n=NR; if(n==0){print "0 0 0"; exit}
    if(n%2==1) med=a[(n+1)/2]; else med=(a[n/2]+a[n/2+1])/2.0
    printf "%.4f %.4f %.4f\n", a[1], med, a[n]}'
}
# fcmp A OP B : exit 0 iff (A OP B) numerically, OP is a comparator like '<'
fcmp() { awk -v a="$1" -v b="$3" "BEGIN{exit !(a $2 b)}"; }

# Bandwidth floor: the f(n)=n asymptote. A correct parser cannot beat the time
# it takes to merely stream every byte once. When the champion approaches this
# (AGENT.md: within ~2×), you're memory-bound and further algorithmic wins are
# gone — the improve-loop should STOP here rather than chase noise.
echo "== bandwidth floor (f(n)=n target) =="
floor_samples=$(for _ in $(seq "$RUNS"); do timeit cat; echo; done)
read -r FLOOR_MIN FLOOR _ < <(printf '%s\n' "$floor_samples" | stats)
echo "floor (cat > /dev/null) = ${FLOOR_MIN}s min / ${FLOOR}s median  <- champion should approach this"

echo "== build variants =="
progs=(); bins=(); state=()
for f in champion/main.cpp variants/*.cpp; do
  [ -e "$f" ] || continue
  case "$f" in champion/*) name=champion ;; *) name="variants/$(basename "$f" .cpp)" ;; esac
  bin="/tmp/pi_${name//\//_}"
  if ! $CXX $FLAGS "$f" -o "$bin" 2>/tmp/build.log; then
    echo "  $name: COMPILE_FAIL"; sed 's/^/    /' /tmp/build.log
    progs+=("$name"); bins+=("$bin"); state+=("COMPILE_FAIL"); continue
  fi
  progs+=("$name"); bins+=("$bin"); state+=("BUILT")
done
# Rust variants (if rustc available)
if command -v rustc >/dev/null 2>&1; then
  RUST_FLAGS="-C opt-level=3 -C target-cpu=native"
  for f in variants/*.rs; do
    [ -e "$f" ] || continue
    name="variants/$(basename "$f" .rs)"
    bin="/tmp/pi_${name//\//_}"
    if ! rustc $RUST_FLAGS "$f" -o "$bin" 2>/tmp/build.log; then
      echo "  $name: COMPILE_FAIL"; sed 's/^/    /' /tmp/build.log
      progs+=("$name"); bins+=("$bin"); state+=("COMPILE_FAIL"); continue
    fi
    progs+=("$name"); bins+=("$bin"); state+=("BUILT")
  done
fi

# Correctness gate (one run each) BEFORE timing — never rank a WRONG program.
for i in "${!progs[@]}"; do
  [ "${state[$i]}" = "BUILT" ] || continue
  "${bins[$i]}" < input.txt > /tmp/out.txt 2>/dev/null
  if [ "$(cat /tmp/out.txt)" = "$EXPECTED" ]; then state[$i]=OK; else state[$i]=WRONG; fi
done

# Interleaved timing: round-robin (champ, var, champ, var, ...) so thermal /
# frequency drift averages across programs instead of biasing whoever ran
# first. Samples accumulate per program; we report median + jitter band.
echo "== timing (interleaved, RUNS=$RUNS) =="
for i in "${!progs[@]}"; do : > "/tmp/pi_s_$i"; done
for _ in $(seq "$RUNS"); do
  for i in "${!progs[@]}"; do
    [ "${state[$i]}" = "OK" ] || continue
    { timeit "${bins[$i]}"; echo; } >> "/tmp/pi_s_$i"
  done
done

# Per-program stats + machine-readable tsv for gen_index.py (median seconds).
# Indexed arrays only (no `declare -A`) so this runs on the owner's stock macOS
# bash 3.2 as well as the x86 Linux routine box. med[$i]/jit[$i] parallel progs.
echo "== test + benchmark =="
printf "%-24s %-18s %-9s %-9s %-9s %s\n" NAME RESULT "BEST(s)" "MED(s)" "JITTER" STATUS
: > /tmp/pi_results.tsv
med=(); jit=(); bmin=(); champ_idx=-1
for i in "${!progs[@]}"; do
  name="${progs[$i]}"; st="${state[$i]}"
  [ "$name" = champion ] && champ_idx=$i
  if [ "$st" = "OK" ]; then
    read -r mn md mx < <(stats < "/tmp/pi_s_$i")
    j=$(awk -v a="$mx" -v b="$mn" 'BEGIN{printf "%.4f", a-b}')
    med[$i]=$md; jit[$i]=$j; bmin[$i]=$mn
    printf "%-24s %-18s %-9s %-9s %-9s %s\n" "$name" "$EXPECTED" "$mn" "$md" "±$j" OK
    # tsv carries BEST (min) — deterministic-compute true-cost estimator; the
    # gate and gen_index.py rank on it (noise only adds time, so min is cleanest).
    printf "%s\t%s\t%s\n" "$name" "$mn" OK >> /tmp/pi_results.tsv
  else
    med[$i]=""; jit[$i]=""; bmin[$i]=""
    printf "%-24s %-18s %-9s %-9s %-9s %s\n" "$name" "-" "-" "-" "-" "$st"
    printf "%s\t-\t%s\n" "$name" "$st" >> /tmp/pi_results.tsv
  fi
done

echo "== ranking (correct only, by BEST time, fastest first) =="
ranked=$(for i in "${!progs[@]}"; do
  [ -n "${bmin[$i]}" ] && printf '%s %s\n' "${bmin[$i]}" "${progs[$i]}"; done | sort -n)
if [ -n "$ranked" ]; then printf '%s\n' "$ranked" | nl; else echo "(none correct)"; fi
echo

# ---------------------------------------------------------------------------
# Promotion gate — the safety rail for the improve-loop.
#
# METRIC: best-of-N (min), not median. Wall-time noise on this workload is
# ONE-SIDED — the OS can only ever make a run slower, never faster than the true
# compute cost — so the minimum is the cleanest, most reproducible estimator and
# the max−min *range* is a terrible noise measure (one slow outlier inflates it
# and blocks real small wins forever — which is exactly what we hit).
#
# PROMOTE a variant only when BOTH hold:
#   (a) its BEST beats the champion's BEST by more than PROMOTE_MARGIN (default
#       1.5% of champion best) — a margin above timer granularity, and
#   (b) its MEDIAN is also below the champion's median — so a single lucky fast
#       sample can't win; the typical run must improve too.
# The candidate must ALSO pass the edge suite on ITS OWN binary before promotion
# (passing the 50M sum doesn't prove empty-input / no-trailing-newline / overflow).
# Writes /tmp/pi_verdict for the loop:  PROMOTE <name> | HOLD | BLOCKED <name> | STOP-FLOOR
# ---------------------------------------------------------------------------
PROMOTE_MARGIN=${PROMOTE_MARGIN:-0.015}   # relative: candidate must be >=1.5% faster
echo "== promotion gate =="
verdict="HOLD"; verdict_name=""
champ_min=""; champ_med=""
if [ "$champ_idx" -ge 0 ]; then champ_min="${bmin[$champ_idx]}"; champ_med="${med[$champ_idx]}"; fi
if [ -z "$champ_min" ]; then
  echo "champion is not OK — cannot gate; fix champion first."
  verdict="BLOCKED"; verdict_name=champion
else
  # Stop condition: champion best within ~2× of the bandwidth floor (best) —
  # that's the f(n)=n asymptote; below it there's nothing left to win.
  if fcmp "$champ_min" "<" "$(awk -v f="$FLOOR_MIN" 'BEGIN{print 2*f}')"; then
    echo "champion best ${champ_min}s is within 2× of floor ${FLOOR_MIN}s — at the wall; stop chasing."
    verdict="STOP-FLOOR"
  fi
  # Best correct variant (exclude champion) by BEST time.
  best_v=""; best_min=""; best_i=-1
  for i in "${!progs[@]}"; do
    [ "$i" = "$champ_idx" ] && continue
    [ -n "${bmin[$i]}" ] || continue
    if [ -z "$best_min" ] || fcmp "${bmin[$i]}" "<" "$best_min"; then
      best_min="${bmin[$i]}"; best_v="${progs[$i]}"; best_i=$i; fi
  done
  if [ -n "$best_v" ]; then
    need=$(awk -v c="$champ_min" -v m="$PROMOTE_MARGIN" 'BEGIN{printf "%.4f", c*(1-m)}')
    delta=$(awk -v c="$champ_min" -v v="$best_min" 'BEGIN{printf "%.4f", c-v}')
    printf "best variant: %s  best=%ss (need <=%ss)  median=%ss vs champ %ss  Δbest=%ss\n" \
           "$best_v" "$best_min" "$need" "${med[$best_i]}" "$champ_med" "$delta"
    if fcmp "$best_min" "<" "$need" && fcmp "${med[$best_i]}" "<" "$champ_med"; then
      echo "  beats champion best by ≥${PROMOTE_MARGIN} AND median also lower → SIGNIFICANT; checking edge cases..."
      cbin="/tmp/pi_${best_v//\//_}"
      if bash tests/edge.sh "$cbin" >/tmp/pi_edge_cand.log 2>&1; then
        tail -n1 /tmp/pi_edge_cand.log
        echo "  PROMOTE $best_v → copy to champion/main.cpp, then update SCOREBOARD.md."
        verdict="PROMOTE"; verdict_name="$best_v"
      else
        echo "  candidate FAILS edge cases — do NOT promote:"; sed 's/^/     /' /tmp/pi_edge_cand.log
        verdict="BLOCKED"; verdict_name="$best_v"
      fi
    else
      echo "  not both conditions met → HOLD champion (within noise, or only a lucky single sample)."
    fi
  else
    echo "no correct variants to compare."
  fi
fi
printf '%s %s\n' "$verdict" "$verdict_name" > /tmp/pi_verdict
echo "verdict: $verdict $verdict_name  (written to /tmp/pi_verdict)"
echo

echo "== edge-case suite (champion) =="
if [ -x /tmp/pi_champion ]; then
  bash tests/edge.sh /tmp/pi_champion || echo "!! CHAMPION FAILS EDGE CASES — do NOT submit or promote"
fi
echo

# ---------------------------------------------------------------------------
# Compiler sweep (champion) — the judge lets you PICK the compiler per submit,
# and the leaderboard's top ranks span g++ 10.5/13.3/14.2 AND clang++ 18, so
# there's no a-priori best one: MEASURE it. Builds the champion under each
# available compiler × flag set, times best-of-N, prints the fastest so you know
# exactly how to submit. Set SWEEP=0 to skip. Locally (ARM Mac) only Apple clang
# exists so it's a near no-op; on the x86 routine box it does the real work.
# ---------------------------------------------------------------------------
if [ "${SWEEP:-1}" = "1" ] && [ "$champ_idx" -ge 0 ] && [ -n "${med[$champ_idx]}" ]; then
  echo "== compiler sweep (champion) =="
  SWEEP_CXX=${SWEEP_CXX:-"g++ g++-13 g++-14 g++-10 clang++ clang++-18 clang++-17"}
  SWEEP_FLAGS=${SWEEP_FLAGS:-"-O3 -march=native|-Ofast -march=native -funroll-loops"}
  sweep_best=999999; sweep_combo=""
  seen_ver=""
  for cxx in $SWEEP_CXX; do
    command -v "$cxx" >/dev/null 2>&1 || continue
    ver=$($cxx --version 2>/dev/null | head -1)
    case "$seen_ver" in *"[$ver]"*) continue;; esac   # skip dup toolchains (e.g. g++→clang alias)
    seen_ver="$seen_ver[$ver]"
    IFS='|' read -ra FSETS <<< "$SWEEP_FLAGS"
    for fs in "${FSETS[@]}"; do
      if $cxx $fs champion/main.cpp -o /tmp/pi_sweep 2>/dev/null; then
        /tmp/pi_sweep < input.txt > /tmp/out.txt 2>/dev/null
        if [ "$(cat /tmp/out.txt)" = "$EXPECTED" ]; then
          : > /tmp/pi_sweep_s
          for _ in $(seq "$RUNS"); do { timeit /tmp/pi_sweep; echo; } >> /tmp/pi_sweep_s; done
          read -r smin _ _ < <(stats < /tmp/pi_sweep_s)
          printf "  %-10s %-34s best=%ss\n" "$cxx" "$fs" "$smin"
          if fcmp "$smin" "<" "$sweep_best"; then sweep_best=$smin; sweep_combo="$cxx $fs"; fi
        else
          printf "  %-10s %-34s WRONG (skip)\n" "$cxx" "$fs"
        fi
      fi
    done
  done
  if [ -n "$sweep_combo" ]; then
    echo "  → submit under: $sweep_combo   (champion best ${sweep_best}s here)"
  else
    echo "  (no working compiler found for the sweep)"
  fi
  echo
fi
echo "== index.html (leaderboard view) =="
python3 gen_index.py /tmp/pi_results.tsv index.html "$EXPECTED" || echo "(gen_index.py failed — non-fatal)"
echo
echo "expected=$EXPECTED  |  verdict=$verdict $verdict_name"
echo "loop: PROMOTE→copy candidate to champion/main.cpp & re-run; HOLD→make a bigger change; STOP-FLOOR→memory-bound, stop."
