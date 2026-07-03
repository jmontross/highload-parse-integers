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
printf "%-24s %-18s %-10s %-9s %s\n" NAME RESULT "MED(s)" "JITTER" STATUS
: > /tmp/pi_results.tsv
med=(); jit=(); champ_idx=-1
for i in "${!progs[@]}"; do
  name="${progs[$i]}"; st="${state[$i]}"
  [ "$name" = champion ] && champ_idx=$i
  if [ "$st" = "OK" ]; then
    read -r mn md mx < <(stats < "/tmp/pi_s_$i")
    j=$(awk -v a="$mx" -v b="$mn" 'BEGIN{printf "%.4f", a-b}')
    med[$i]=$md; jit[$i]=$j
    printf "%-24s %-18s %-10s %-9s %s\n" "$name" "$EXPECTED" "$md" "±$j" OK
    printf "%s\t%s\t%s\n" "$name" "$md" OK >> /tmp/pi_results.tsv
  else
    med[$i]=""; jit[$i]=""
    printf "%-24s %-18s %-10s %-9s %s\n" "$name" "-" "-" "-" "$st"
    printf "%s\t-\t%s\n" "$name" "$st" >> /tmp/pi_results.tsv
  fi
done

echo "== ranking (correct only, by median, fastest first) =="
ranked=$(for i in "${!progs[@]}"; do
  [ -n "${med[$i]}" ] && printf '%s %s\n' "${med[$i]}" "${progs[$i]}"; done | sort -n)
if [ -n "$ranked" ]; then printf '%s\n' "$ranked" | nl; else echo "(none correct)"; fi
echo

# ---------------------------------------------------------------------------
# Promotion gate — the safety rail for the improve-loop. A variant is only a
# real, promotable win if its median beats the champion's median by MORE than
# the noise band (the larger of the two programs' jitter). Otherwise the "win"
# is inside the measurement noise and must NOT be promoted. The chosen
# candidate must ALSO pass the edge suite on ITS OWN binary before promotion —
# passing the 50M sum does not prove it handles empty input / no trailing
# newline / overflow. Writes /tmp/pi_verdict for the loop to act on:
#   PROMOTE <name>   HOLD   BLOCKED <name>   STOP-FLOOR
# ---------------------------------------------------------------------------
echo "== promotion gate =="
verdict="HOLD"; verdict_name=""
champ_med=""; champ_jit=""
[ "$champ_idx" -ge 0 ] && champ_med="${med[$champ_idx]}" && champ_jit="${jit[$champ_idx]}"
if [ -z "$champ_med" ]; then
  echo "champion is not OK — cannot gate; fix champion first."
  verdict="BLOCKED"; verdict_name=champion
else
  # Stop condition for the loop: champion within ~2× of the bandwidth floor
  # (use the cache-warm MIN — that's the true f(n)=n asymptote, not the median
  # which is inflated by the cold first read).
  if fcmp "$champ_med" "<" "$(awk -v f="$FLOOR_MIN" 'BEGIN{print 2*f}')"; then
    echo "champion median ${champ_med}s is within 2× of floor ${FLOOR_MIN}s — at the wall; stop chasing."
    verdict="STOP-FLOOR"
  fi
  # Best correct variant (exclude champion) by median.
  best_v=""; best_med=""; best_jit=0
  for i in "${!progs[@]}"; do
    [ "$i" = "$champ_idx" ] && continue
    [ -n "${med[$i]}" ] || continue
    if [ -z "$best_med" ] || fcmp "${med[$i]}" "<" "$best_med"; then
      best_med="${med[$i]}"; best_v="${progs[$i]}"; best_jit="${jit[$i]}"; fi
  done
  if [ -n "$best_v" ]; then
    band=$(awk -v a="$champ_jit" -v b="$best_jit" 'BEGIN{print (a>b)?a:b}')
    delta=$(awk -v c="$champ_med" -v v="$best_med" 'BEGIN{printf "%.4f", c-v}')
    printf "best variant: %s  median=%ss  Δ vs champion=%ss  noise band=±%ss\n" \
           "$best_v" "$best_med" "$delta" "$band"
    if fcmp "$delta" ">" "$band"; then
      echo "  Δ exceeds noise band → candidate is a SIGNIFICANT win; checking edge cases on its binary..."
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
      echo "  Δ is within noise → HOLD champion (not a real win). Re-run or make a bigger change."
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
echo "== index.html (leaderboard view) =="
python3 gen_index.py /tmp/pi_results.tsv index.html "$EXPECTED" || echo "(gen_index.py failed — non-fatal)"
echo
echo "expected=$EXPECTED  |  verdict=$verdict $verdict_name"
echo "loop: PROMOTE→copy candidate to champion/main.cpp & re-run; HOLD→make a bigger change; STOP-FLOOR→memory-bound, stop."
