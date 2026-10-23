#!/bin/bash
# Differential test suite.
#
# Each C test is compiled (a) natively for x86-64 macOS and run under Rosetta
# to produce the reference output, and (b) as freestanding Linux .o files in
# several configurations that run inside x86sim. Stdout and exit codes must
# match exactly.
set -u
cd "$(dirname "$0")/.."

SIM=./x86sim
OUT=tests/out
mkdir -p "$OUT"
pass=0
fail=0
failed=()

green() { printf '\033[32m%s\033[0m' "$1"; }
red() { printf '\033[31m%s\033[0m' "$1"; }

check() { # name expected_out expected_rc actual_out actual_rc
    if cmp -s "$2" "$4" && [ "$3" = "$5" ]; then
        pass=$((pass + 1))
        printf '  %-28s %s\n' "$1" "$(green PASS)"
    else
        fail=$((fail + 1))
        failed+=("$1")
        printf '  %-28s %s (exit %s, expected %s)\n' "$1" "$(red FAIL)" "$5" "$3"
        diff "$2" "$4" | head -8 | sed 's/^/      /'
    fi
}

COMMON="-ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin -w"

if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
    echo "Rosetta is required for reference outputs (softwareupdate --install-rosetta)."
    exit 1
fi

echo "== assembly programs =="
clang -target x86_64-linux-gnu -c tests/asm/hello64.s -o "$OUT/hello64.o"
clang -target i386-linux-gnu -c tests/asm/hello32.s -o "$OUT/hello32.o"
clang -target i386-linux-gnu -c examples/hello16.s -o "$OUT/hello16.o"
printf 'Hello from x86-64!\n' > "$OUT/hello64.exp"
printf 'Hello from i386!\n' > "$OUT/hello32.exp"
printf 'Hello from 16-bit real mode!\r\n01234\n' > "$OUT/hello16.exp"
$SIM "$OUT/hello64.o" > "$OUT/hello64.got"; check hello64 "$OUT/hello64.exp" 42 "$OUT/hello64.got" $?
$SIM "$OUT/hello32.o" > "$OUT/hello32.got"; check hello32 "$OUT/hello32.exp" 7 "$OUT/hello32.got" $?
$SIM -b 16 "$OUT/hello16.o" > "$OUT/hello16.got"; check hello16 "$OUT/hello16.exp" 3 "$OUT/hello16.got" $?

echo "== C programs (reference: native x86-64 under Rosetta) =="
for src in tests/c/*.c; do
    t=$(basename "$src" .c)
    extra=""
    args=""
    configs="x64-O0 x64-O2 x64-O2-pie i386-O0 i386-O2"
    case $t in
        flags) extra="-mno-red-zone"; configs="x64-O0 x64-O2" ;;
        algo) args="hello" ;;
    esac

    clang -arch x86_64 -O1 -w $extra "$src" runtime/sim_libc.c -o "$OUT/$t.native" || { echo "native build of $t failed"; exit 1; }
    arch -x86_64 "$OUT/$t.native" $args > "$OUT/$t.exp"
    exp_rc=$?

    for cfg in $configs; do
        case $cfg in
            x64-O0) flags="-target x86_64-linux-gnu -O0 -fno-pic" ;;
            x64-O2) flags="-target x86_64-linux-gnu -O2 -fno-pic" ;;
            x64-O2-pie) flags="-target x86_64-linux-gnu -O2 -fPIE" ;;
            i386-O0) flags="-target i386-linux-gnu -O0 -fno-pic" ;;
            i386-O2) flags="-target i386-linux-gnu -O2 -fno-pic" ;;
        esac
        o="$OUT/$t-$cfg"
        clang $flags $COMMON $extra -c "$src" -o "$o.o" &&
            clang $flags $COMMON -c runtime/sim_libc.c -o "$o.lib.o" || { echo "build of $t ($cfg) failed"; exit 1; }
        $SIM -m 4000000000 "$o.o" "$o.lib.o" -- $args > "$o.got" 2> "$o.err"
        rc=$?
        check "$t [$cfg]" "$OUT/$t.exp" $exp_rc "$o.got" $rc
        [ -s "$o.err" ] && sed 's/^/      /' "$o.err" | head -5
    done
done

echo
echo "passed: $pass  failed: $fail"
[ $fail -eq 0 ] || { printf '  %s\n' "${failed[@]}"; exit 1; }
