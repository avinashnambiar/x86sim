#!/bin/bash
# Bare-metal system tests: each tests/sys/*.S boots in --pc mode, prints to
# port 0xE9 and exits through port 0xF4; output must match the .exp file.
cd "$(dirname "$0")/../.."
OUT=tests/out
mkdir -p $OUT
pass=0; fail=0
for src in tests/sys/*.S; do
    t=$(basename "$src" .S)
    clang -target i386-linux-gnu -c "$src" -o "$OUT/sys-$t.o" || { echo "  $t: assembly failed"; fail=$((fail+1)); continue; }
    args=""; [ -f "tests/sys/$t.args" ] && args=$(cat "tests/sys/$t.args")
    input=/dev/null; [ -f "tests/sys/$t.in" ] && input="tests/sys/$t.in"
    if [ -f "tests/sys/$t.disk" ]; then  # boot from a disk image built from the program
        drive=$(cat "tests/sys/$t.disk")
        python3 tests/sys/mkdisk.py "$OUT/sys-$t.o" "$OUT/sys-$t.img" $drive || { fail=$((fail+1)); continue; }
        ./x86sim -m 500000000 --$drive "$OUT/sys-$t.img" < "$input" > "$OUT/sys-$t.got" 2>&1
    else
        ./x86sim --pc -m 500000000 $args "$OUT/sys-$t.o" < "$input" > "$OUT/sys-$t.got" 2>&1
    fi
    rc=$?
    if [ $rc -eq 0 ] && cmp -s "tests/sys/$t.exp" "$OUT/sys-$t.got"; then
        pass=$((pass+1)); printf '  %-20s \033[32mPASS\033[0m\n' "$t"
    else
        fail=$((fail+1)); printf '  %-20s \033[31mFAIL\033[0m (exit %d)\n' "$t" $rc
        diff "tests/sys/$t.exp" "$OUT/sys-$t.got" | head -10 | sed 's/^/      /'
    fi
done
echo "system tests passed: $pass  failed: $fail"
[ $fail -eq 0 ]
