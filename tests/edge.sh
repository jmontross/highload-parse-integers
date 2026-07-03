#!/usr/bin/env bash
# Edge-case correctness suite. Usage: bash tests/edge.sh /path/to/binary
# Exits non-zero if any case fails. Every champion MUST pass this.
set -uo pipefail
BIN="${1:?usage: edge.sh <binary>}"
pass=0; fail=0
check(){ got=$(printf "%b" "$2" | "$BIN"); if [ "$got" = "$3" ]; then pass=$((pass+1));
  else echo "FAIL | $1 -> got '$got' want '$3'"; fail=$((fail+1)); fi; }

check "single"                 '5\n'                 5
check "zero"                   '0\n'                 0
check "max uint32"             '2147483647\n'        2147483647
check "three lines"            '1\n2\n3\n'           6
check "no trailing newline"    '7'                   7
check "leading zeros"          '007\n'               7
check "two multidigit"         '10\n20\n'            30
check "empty input"            ''                    0
check "overflow-safe (10x max)" '2147483647\n2147483647\n2147483647\n2147483647\n2147483647\n2147483647\n2147483647\n2147483647\n2147483647\n2147483647\n' 21474836470

echo "edge: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
