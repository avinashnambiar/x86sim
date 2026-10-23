#!/bin/bash
# Check src/softfloat.c.
#  - Basic arithmetic, conversions, FPREM, FSCALE, BCD, compares and constants
#    must match the real x87 (x86-64 reference under Rosetta) bit for bit,
#    including the exception flags and condition codes.
#  - Transcendentals (FSIN..FYL2XP1) are correctly rounded by design; Rosetta's
#    (like real Intel hardware) are only accurate to ~1 ulp, so these are
#    verified against mpmath at 300 bits instead, when mpmath is available.
# usage: tests/softfloat/run.sh [cases-per-op] [op]
set -e
cd "$(dirname "$0")/../.."
out=tests/out/softfloat
mkdir -p "$out"
clang -arch x86_64 -O1 -w tests/softfloat/fxcheck.c -o "$out/fxref"
clang -O1 -w -Isrc tests/softfloat/fxcheck.c src/softfloat.c -o "$out/fxsim"
arch -x86_64 "$out/fxref" "$@" > "$out/ref.all"
"$out/fxsim" "$@" > "$out/sim.all"
trans='^(sin|cos|tan|sincos|atan|2xm1|yl2x|yl2xp1) '
grep -Ev "$trans" "$out/ref.all" > "$out/ref.txt" || true
grep -Ev "$trans" "$out/sim.all" > "$out/sim.txt" || true
total=$(wc -l < "$out/ref.txt")
bad=$(diff "$out/ref.txt" "$out/sim.txt" | grep -c '^<' || true)
echo "softfloat exact: $((total - bad))/$total cases match the x87"
rc=0
if [ "$bad" != 0 ]; then
    diff "$out/ref.txt" "$out/sim.txt" | grep '^<' | awk '{print $2}' | sort | uniq -c | sort -rn
    rc=1
fi
if python3 -c 'import mpmath' 2>/dev/null; then
    grep -E "$trans" "$out/sim.all" | python3 tests/softfloat/verify.py || rc=1
else
    echo "softfloat transcendentals: skipped (pip3 install mpmath to verify)"
fi
exit $rc
