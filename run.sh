#!/usr/bin/env bash
# Test + benchmark harness for the HighLoad.fun parse_integers challenge.
# Correctness gate: every program's output MUST equal an independently computed
# reference sum (exact Python big-int). Speed metric: best-of-N wall-clock of
# `./prog < input.txt`. The fastest CORRECT program is the champion.
set -uo pipefail
cd "$(dirname "$0")"

CANON=53687387166542798          # seeded gen @ N=50,000,000 (defense-in-depth check)
N=${N:-50000000}
RUNS=${RUNS:-3}
CXX=${CXX:-c++}
FLAGS=${FLAGS:--O3 -march=native} # same spirit as the judge

echo "== build generator & input =="
$CXX $FLAGS gen.cpp -o /tmp/gen || { echo "gen build failed"; exit 1; }
if [ ! -f input.txt ]; then echo "generating $N lines..."; /tmp/gen "$N" > input.txt; fi

echo "== reference sum (exact) =="
EXP_FILE=".expected_$N"
if [ -f "$EXP_FILE" ]; then EXPECTED=$(cat "$EXP_FILE")
else EXPECTED=$(python3 -c "print(sum(int(x) for x in open('input.txt','rb').read().split()))"); echo "$EXPECTED" > "$EXP_FILE"; fi
if [ "$N" = "50000000" ] && [ "$EXPECTED" != "$CANON" ]; then
  echo "FATAL: reference ($EXPECTED) != canonical ($CANON) — bad input?"; exit 1; fi
echo "expected = $EXPECTED"

bench() { # $1 = binary -> echoes best seconds
  local best=999999 t0 t1 dt
  for _ in $(seq "$RUNS"); do
    t0=$(python3 -c 'import time;print(time.monotonic())')
    "$1" < input.txt > /tmp/out.txt
    t1=$(python3 -c 'import time;print(time.monotonic())')
    dt=$(python3 -c "print($t1-$t0)")
    awk -v a="$dt" -v b="$best" 'BEGIN{exit !(a<b)}' && best=$dt
  done
  echo "$best"
}

echo "== test + benchmark =="
printf "%-24s %-10s %-8s %s\n" NAME RESULT "TIME(s)" STATUS
results=()
for f in champion/main.cpp variants/*.cpp; do
  [ -e "$f" ] || continue
  case "$f" in champion/*) name=champion ;; *) name="variants/$(basename "$f" .cpp)" ;; esac
  bin="/tmp/pi_${name//\//_}"
  if ! $CXX $FLAGS "$f" -o "$bin" 2>/tmp/build.log; then
    printf "%-24s %-10s %-8s %s\n" "$name" "-" "-" "COMPILE_FAIL"; sed 's/^/    /' /tmp/build.log; continue
  fi
  t=$(bench "$bin"); got=$(cat /tmp/out.txt)
  if [ "$got" = "$EXPECTED" ]; then st=OK; results+=("$t $name"); else st="WRONG"; fi
  printf "%-24s %-10s %-8s %s\n" "$name" "$got" "$t" "$st"
done

echo "== ranking (correct only, fastest first) =="
if [ ${#results[@]} -gt 0 ]; then printf '%s\n' "${results[@]}" | sort -n | nl; else echo "(none correct)"; fi
echo
echo "expected=$EXPECTED  |  promote fastest correct into champion/main.cpp, then update SCOREBOARD.md."
