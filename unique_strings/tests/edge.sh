#!/usr/bin/env bash
# Edge-case correctness suite for unique_strings. Usage: bash tests/edge.sh <binary>
# Exits non-zero if any case fails. Every champion MUST pass this.
# Inputs respect the challenge contract: newline-terminated, non-empty tokens,
# length 1..16, alphabet [0-9 a-z A-Z @ # % *]. The check is the DISTINCT count.
set -uo pipefail
BIN="${1:?usage: edge.sh <binary>}"
pass=0; fail=0
check(){ got=$(printf "%b" "$2" | "$BIN"); if [ "$got" = "$3" ]; then pass=$((pass+1));
  else echo "FAIL | $1 -> got '$got' want '$3'"; fail=$((fail+1)); fi; }

check "empty input"            ''                       0
check "single token"           'a\n'                    1
check "duplicate pair"         'a\na\n'                 1
check "two distinct"           'a\nb\n'                 2
check "repeats collapse"       'x\ny\nx\nz\ny\n'        3
check "case sensitive"         'abc\nABC\n'             2
check "digits vs letters"      '007\n7\n'               2
check "special chars"          '@#%*\n@#%\n'            2
check "all 4 specials + dup"   '@\n#\n%\n*\n@\n'        4
check "max length 16"          'abcdefghijklmnop\n'     1
check "len16 distinct from 15" 'abcdefghijklmnop\nabcdefghijklmno\n' 2

echo "unique_strings edge: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
