#!/bin/bash
# The web UI's assembly editor: starts x86sim -u with an empty PC, then
# assembles, loads and runs every example in ui/asm/ through /api/asm and
# checks the console output, the exception report, multi-CPU loads, reset
# replay, assembler diagnostics and refused load addresses (including one
# program landing on another's code or stack).
cd "$(dirname "$0")/../.."
PORT=${PORT:-8097}
U=http://127.0.0.1:$PORT
OUT=tests/out
mkdir -p $OUT
./x86sim -u -p $PORT > $OUT/asm-server.log 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
for _ in $(seq 50); do curl -s -o /dev/null $U/ && break; sleep 0.1; done

pass=0; fail=0
check() { # name, got, want (want is a substring)
    if [[ "$2" == *"$3"* ]]; then pass=$((pass+1)); printf '  %-28s \033[32mPASS\033[0m\n' "$1"
    else fail=$((fail+1)); printf '  %-28s \033[31mFAIL\033[0m\n      want: %s\n      got:  %s\n' "$1" "$3" "$2"; fi
}
field() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
newpc() { curl -s -XPOST "$U/api/newpc?smp=$1&cache=$2" > /dev/null; }
load() { curl -s -XPOST --data-binary @ui/asm/$1 "$U/api/asm?bits=$2&cpu=$3&addr=$4&syntax=${5:-att}"; }
run() { curl -s -XPOST "$U/api/continue" | field "d['status']"; }
output() { curl -s "$U/api/output" | field "d['text']"; }
slot() { printf %x $((0x200000 + $1 * 0x10000)); }

newpc 1 0; load hello16.s 16 0 10000 > /dev/null; run > /dev/null
check hello16 "$(output)" "Hello from 16-bit real mode!"
newpc 1 0; load hello32.s 32 0 200000 > /dev/null; run > /dev/null
check hello32 "$(output)" "Hello from 32-bit protected mode!"
newpc 1 0; load hello64.s 64 0 200000 > /dev/null; run > /dev/null
check hello64 "$(output)" "Hello from 64-bit mode!"
newpc 1 0; load hello64-intel.s 64 0 200000 intel > /dev/null; run > /dev/null
check hello64-intel "$(output)" "Hello from 64-bit mode, Intel syntax!"
newpc 1 0; load fault32.s 32 0 200000 > /dev/null
check fault32 "$(run)" "cpu0: unhandled #DE divide error (vector 0) at 0x200009; cpu0 halted"
newpc 4 1; for k in 0 1 2 3; do load whoami64.s 64 $k $(slot $k) > /dev/null; done; run > /dev/null
check whoami64-4cpus "$(output)" $'APIC ID 0\nHello from the CPU with APIC ID 1\nHello from the CPU with APIC ID 2\nHello from the CPU with APIC ID 3'
newpc 2 1; for k in 0 1; do load counter64.s 64 $k $(slot $k) > /dev/null; done; run > /dev/null
check counter64-2cpus "$(output)" "APIC ID 1 finished, counter = 200000"
curl -s -XPOST "$U/api/reset" > /dev/null; run > /dev/null
check reset-replays "$(output)" "APIC ID 1 finished, counter = 200000"
printf 'nop\nbogus %%eax\n' > $OUT/asm-bad.s
check diagnostics "$(curl -s -XPOST --data-binary @$OUT/asm-bad.s "$U/api/asm?bits=64&cpu=0&addr=200000" | field "d['diagnostics']")" \
    "program.s:2:1: error: invalid instruction mnemonic 'bogus'"
check check-only "$(curl -s -XPOST --data-binary @ui/asm/hello64.s "$U/api/asm?bits=64&check=1" | field "d.get('ok')")" "True"
check refuse-cpu "$(load hello64.s 64 7 200000 | field "d['diagnostics']")" "this machine has 2 CPUs"
check refuse-bios "$(load hello64.s 64 0 a0000 | field "d['diagnostics']")" "overlaps video memory and the BIOS"
check refuse-tables "$(load hello64.s 64 0 100000 | field "d['diagnostics']")" "editor's GDT, IDTs and page tables"
check refuse-align16 "$(load hello16.s 16 0 10008 | field "d['diagnostics']")" "16-byte aligned"
check memory-only "$(load hello64.s 64 -1 2000000 | field "d['asm']['cpu']")" "-1"
newpc 2 0; load hello64.s 64 0 200000 > /dev/null
check refuse-overlap "$(load hello64.s 64 1 208000 | field "d['diagnostics']")" "overlaps CPU 0's program at 0x200000-0x210000"
check refuse-overlap-data "$(load hello64.s 64 -1 20f000 | field "d['diagnostics']")" "overlaps CPU 0's program"
check own-slot-ok "$(load hello64.s 64 1 $(slot 1) | field "d['asm']['cpu']")" "1"
check reload-same-cpu "$(load hello64.s 64 0 200000 | field "d['asm']['cpu']")" "0"
echo "asm editor tests passed: $pass  failed: $fail"
[ $fail -eq 0 ]
