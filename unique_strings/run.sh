#!/usr/bin/env bash
# Test + benchmark harness for the HighLoad.fun unique_strings challenge.
# Correctness gate: every program's output MUST equal the exact distinct-token
# count computed independently (Python set). Speed metric: interleaved best-of-N
# wall-clock of `./prog < input.txt`, reported as MEDIAN with a jitter band. The
# fastest CORRECT program is the champion — but a variant is only a *significant*
# winner (promotable) if it beats the champion's median by more than the noise
# band (see "promotion gate"). tmp files are namespaced /tmp/us_* so this never
# collides with a concurrent parse_integers run on the same box.
set -uo pipefail
cd "$(dirname "$0")"

CANON=5706971                    # distinct tokens for seeded gen @ N=10,000,000, M=8,000,000
N=${N:-10000000}
RUNS=${RUNS:-5}                  # confirm wins with >=5 runs
CXX=${CXX:-c++}
FLAGS=${FLAGS:--O3 -march=native} # same spirit as the judge

echo "== build generator & input =="
$CXX $FLAGS gen.cpp -o /tmp/us_gen || { echo "gen build failed"; exit 1; }
# Self-heal: regenerate when input is missing OR its line count != N.
if [ ! -f input.txt ] || [ "$(wc -l < input.txt)" -ne "$N" ]; then
  echo "generating $N lines..."; /tmp/us_gen "$N" > input.txt
fi

echo "== reference count (exact) =="
EXP_FILE=".expected_$N"
EXPECTED=""
[ -f "$EXP_FILE" ] && EXPECTED=$(cat "$EXP_FILE")
# Cache is keyed on N only; if input.txt was regenerated it can be poisoned. At
# the canonical size we KNOW the answer, so drop a wrong cache instead of trusting it.
if [ "$N" = "10000000" ] && [ "$EXPECTED" != "$CANON" ]; then EXPECTED=""; fi
if [ -z "$EXPECTED" ]; then
  EXPECTED=$(python3 -c "print(len(set(open('input.txt','rb').read().split())))")
  echo "$EXPECTED" > "$EXP_FILE"
fi
if [ "$N" = "10000000" ] && [ "$EXPECTED" != "$CANON" ]; then
  echo "FATAL: reference ($EXPECTED) != canonical ($CANON) — bad gen.cpp, not stale input."; exit 1; fi
echo "expected = $EXPECTED distinct tokens"

# ---------------------------------------------------------------------------
# Timing primitive: bash builtin `time` (sub-ms, no subprocess spawned per sample).
# TIMEFORMAT='%R' gives real seconds straight from the shell.
# ---------------------------------------------------------------------------
timeit() { # $@ = argv (binary); echoes real seconds
  local t
  t=$( { TIMEFORMAT='%R'; time "$@" > /tmp/us_out.txt < input.txt; } 2>&1 )
  printf '%s' "$t"
}
# stats: reads newline-separated samples on stdin -> "min median max"
stats() {
  sort -n | awk '{a[NR]=$1} END{
    n=NR; if(n==0){print "0 0 0"; exit}
    if(n%2==1) med=a[(n+1)/2]; else med=(a[n/2]+a[n/2+1])/2.0
    printf "%.4f %.4f %.4f\n", a[1], med, a[n]}'
}
# fcmp A OP B : exit 0 iff (A OP B) numerically
fcmp() { awk -v a="$1" -v b="$3" "BEGIN{exit !(a $2 b)}"; }

# Bandwidth floor: the f(n)=n asymptote — the time to merely stream every byte
# once. A correct counter cannot beat it. When the champion approaches this
# (within ~2x), the game is I/O-bound and further algorithmic wins are gone.
echo "== bandwidth floor (f(n)=n target) =="
floor_samples=$(for _ in $(seq "$RUNS"); do timeit cat; echo; done)
read -r FLOOR_MIN FLOOR _ < <(printf '%s\n' "$floor_samples" | stats)
echo "floor (cat > /dev/null) = ${FLOOR_MIN}s min / ${FLOOR}s median  <- champion should approach this"

echo "== build variants =="
progs=(); bins=(); state=()
for f in champion/main.cpp variants/*.cpp; do
  [ -e "$f" ] || continue
  case "$f" in champion/*) name=champion ;; *) name="variants/$(basename "$f" .cpp)" ;; esac
  bin="/tmp/us_${name//\//_}"
  if ! $CXX $FLAGS "$f" -o "$bin" 2>/tmp/us_build.log; then
    echo "  $name: COMPILE_FAIL"; sed 's/^/    /' /tmp/us_build.log
    progs+=("$name"); bins+=("$bin"); state+=("COMPILE_FAIL"); continue
  fi
  progs+=("$name"); bins+=("$bin"); state+=("BUILT")
done

# Correctness gate (one run each) BEFORE timing — never rank a WRONG program.
for i in "${!progs[@]}"; do
  [ "${state[$i]}" = "BUILT" ] || continue
  "${bins[$i]}" < input.txt > /tmp/us_out.txt 2>/dev/null
  if [ "$(cat /tmp/us_out.txt)" = "$EXPECTED" ]; then state[$i]=OK; else state[$i]=WRONG; fi
done

# Interleaved timing: round-robin so thermal/frequency drift averages across
# programs instead of biasing whoever ran first.
echo "== timing (interleaved, RUNS=$RUNS) =="
for i in "${!progs[@]}"; do : > "/tmp/us_s_$i"; done
for _ in $(seq "$RUNS"); do
  for i in "${!progs[@]}"; do
    [ "${state[$i]}" = "OK" ] || continue
    { timeit "${bins[$i]}"; echo; } >> "/tmp/us_s_$i"
  done
done

# Per-program stats + machine-readable tsv for gen_index.py (median seconds).
# Indexed arrays (parallel to progs[]) — no `declare -A`, so this runs on the
# owner's stock bash 3.2 as well as the cloud's bash 4+.
echo "== test + benchmark =="
printf "%-24s %-12s %-10s %-9s %s\n" NAME RESULT "MED(s)" "JITTER" STATUS
: > /tmp/us_results.tsv
med=(); jit=()
for i in "${!progs[@]}"; do
  name="${progs[$i]}"; st="${state[$i]}"
  if [ "$st" = "OK" ]; then
    read -r mn md mx < <(stats < "/tmp/us_s_$i")
    j=$(awk -v a="$mx" -v b="$mn" 'BEGIN{printf "%.4f", a-b}')
    med[$i]=$md; jit[$i]=$j
    printf "%-24s %-12s %-10s %-9s %s\n" "$name" "$EXPECTED" "$md" "±$j" OK
    printf "%s\t%s\t%s\n" "$name" "$md" OK >> /tmp/us_results.tsv
  else
    med[$i]=""; jit[$i]=""
    printf "%-24s %-12s %-10s %-9s %s\n" "$name" "-" "-" "-" "$st"
    printf "%s\t-\t%s\n" "$name" "$st" >> /tmp/us_results.tsv
  fi
done

echo "== ranking (correct only, by median, fastest first) =="
ranked=$(for i in "${!progs[@]}"; do [ -n "${med[$i]}" ] && printf '%s %s\n' "${med[$i]}" "${progs[$i]}"; done | sort -n)
if [ -n "$ranked" ]; then printf '%s\n' "$ranked" | nl; else echo "(none correct)"; fi
echo

champ_idx=-1
for i in "${!progs[@]}"; do [ "${progs[$i]}" = champion ] && champ_idx=$i; done

# ---------------------------------------------------------------------------
# Promotion gate — a variant is only a real, promotable win if its median beats
# the champion's median by MORE than the noise band (the larger of the two
# jitters). The chosen candidate must ALSO pass the edge suite on ITS OWN binary
# before promotion. Writes /tmp/us_verdict: PROMOTE <name> | HOLD | BLOCKED <name> | STOP-FLOOR
# ---------------------------------------------------------------------------
echo "== promotion gate =="
verdict="HOLD"; verdict_name=""
champ_med=""
[ "$champ_idx" -ge 0 ] && champ_med="${med[$champ_idx]}"
if [ -z "$champ_med" ]; then
  echo "champion is not OK — cannot gate; fix champion first."
  verdict="BLOCKED"; verdict_name=champion
else
  if fcmp "$champ_med" "<" "$(awk -v f="$FLOOR" 'BEGIN{print 2*f}')"; then
    echo "champion median ${champ_med}s is within 2× of floor ${FLOOR}s — at the wall; stop chasing."
    verdict="STOP-FLOOR"
  fi
  best_i=-1; best_med=""
  for i in "${!progs[@]}"; do
    [ "$i" = "$champ_idx" ] && continue
    [ -n "${med[$i]}" ] || continue
    if [ -z "$best_med" ] || fcmp "${med[$i]}" "<" "$best_med"; then
      best_med="${med[$i]}"; best_i=$i; fi
  done
  if [ "$best_i" -ge 0 ]; then
    best_v="${progs[$best_i]}"
    band=$(awk -v a="${jit[$champ_idx]}" -v b="${jit[$best_i]:-0}" 'BEGIN{print (a>b)?a:b}')
    delta=$(awk -v c="$champ_med" -v v="$best_med" 'BEGIN{printf "%.4f", c-v}')
    printf "best variant: %s  median=%ss  Δ vs champion=%ss  noise band=±%ss\n" \
           "$best_v" "$best_med" "$delta" "$band"
    if fcmp "$delta" ">" "$band"; then
      echo "  Δ exceeds noise band → candidate is a SIGNIFICANT win; checking edge cases on its binary..."
      cbin="/tmp/us_${best_v//\//_}"
      if bash tests/edge.sh "$cbin" >/tmp/us_edge_cand.log 2>&1; then
        tail -n1 /tmp/us_edge_cand.log
        echo "  PROMOTE $best_v → copy to champion/main.cpp, then update SCOREBOARD.md."
        verdict="PROMOTE"; verdict_name="$best_v"
      else
        echo "  candidate FAILS edge cases — do NOT promote:"; sed 's/^/     /' /tmp/us_edge_cand.log
        verdict="BLOCKED"; verdict_name="$best_v"
      fi
    else
      echo "  Δ is within noise → HOLD champion (not a real win). Re-run or make a bigger change."
    fi
  else
    echo "no correct variants to compare."
  fi
fi
printf '%s %s\n' "$verdict" "$verdict_name" > /tmp/us_verdict
echo "verdict: $verdict $verdict_name  (written to /tmp/us_verdict)"
echo

echo "== edge-case suite (champion) =="
if [ -x /tmp/us_champion ]; then
  bash tests/edge.sh /tmp/us_champion || echo "!! CHAMPION FAILS EDGE CASES — do NOT submit or promote"
fi
echo
echo "== index.html (leaderboard view) =="
python3 gen_index.py /tmp/us_results.tsv index.html "$EXPECTED" || echo "(gen_index.py failed — non-fatal)"
echo
echo "expected=$EXPECTED  |  verdict=$verdict $verdict_name"
echo "loop: PROMOTE→copy candidate to champion/main.cpp & re-run; HOLD→make a bigger change; STOP-FLOOR→I/O-bound, stop."
