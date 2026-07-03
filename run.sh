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

# Bandwidth floor: the f(n)=n asymptote. A correct parser cannot beat the time
# it takes to merely stream every byte once. When the champion approaches this,
# you're memory-bound and further algorithmic wins are gone — stop chasing.
echo "== bandwidth floor (f(n)=n target) =="
floor=999999
for _ in $(seq "$RUNS"); do
  t0=$(python3 -c 'import time;print(time.monotonic())')
  cat input.txt > /dev/null
  t1=$(python3 -c 'import time;print(time.monotonic())')
  dt=$(python3 -c "print($t1-$t0)")
  awk -v a="$dt" -v b="$floor" 'BEGIN{exit !(a<b)}' && floor=$dt
done
echo "floor (cat > /dev/null) = ${floor}s  <- champion should approach this"

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
: > /tmp/pi_results.tsv          # machine-readable: name<TAB>seconds<TAB>status (feeds gen_index.py)
for f in champion/main.cpp variants/*.cpp; do
  [ -e "$f" ] || continue
  case "$f" in champion/*) name=champion ;; *) name="variants/$(basename "$f" .cpp)" ;; esac
  bin="/tmp/pi_${name//\//_}"
  if ! $CXX $FLAGS "$f" -o "$bin" 2>/tmp/build.log; then
    printf "%-24s %-10s %-8s %s\n" "$name" "-" "-" "COMPILE_FAIL"; sed 's/^/    /' /tmp/build.log
    printf "%s\t-\tCOMPILE_FAIL\n" "$name" >> /tmp/pi_results.tsv; continue
  fi
  t=$(bench "$bin"); got=$(cat /tmp/out.txt)
  if [ "$got" = "$EXPECTED" ]; then st=OK; results+=("$t $name"); else st="WRONG"; fi
  printf "%-24s %-10s %-8s %s\n" "$name" "$got" "$t" "$st"
  printf "%s\t%s\t%s\n" "$name" "$t" "$st" >> /tmp/pi_results.tsv
done

echo "== ranking (correct only, fastest first) =="
if [ ${#results[@]} -gt 0 ]; then printf '%s\n' "${results[@]}" | sort -n | nl; else echo "(none correct)"; fi
echo

echo "== edge-case suite (champion) =="
if [ -x /tmp/pi_champion ]; then
  bash tests/edge.sh /tmp/pi_champion || echo "!! CHAMPION FAILS EDGE CASES — do NOT submit or promote"
fi
echo
echo "== index.html (leaderboard view) =="
python3 gen_index.py /tmp/pi_results.tsv index.html "$EXPECTED" || echo "(gen_index.py failed — non-fatal)"
echo
echo "expected=$EXPECTED  |  promote fastest correct into champion/main.cpp, then update SCOREBOARD.md."
