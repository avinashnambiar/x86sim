#!/bin/bash
# Boot a stock Linux kernel (Alpine linux-virt 6.6, downloaded once into
# tests/linux/cache) with a tiny initramfs whose /init echoes a typed line
# and reboots. Passes when the line comes back and the guest resets cleanly:
# on one CPU, on two CPUs, and on two and eight CPUs with the MESI cache hierarchy.
cd "$(dirname "$0")/../.."
CACHE=tests/linux/cache
OUT=tests/out
mkdir -p $CACHE $OUT
KERNEL=$CACHE/vmlinuz-virt
URL=https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/netboot/vmlinuz-virt
if [ ! -f $KERNEL ]; then
    echo "downloading $URL"
    curl -sSfL -o $KERNEL.part "$URL" && mv $KERNEL.part $KERNEL || { echo "download failed"; exit 1; }
fi
clang -target x86_64-linux-gnu -c tests/linux/init.S -o $OUT/linux-init.o || exit 1
python3 tests/linux/mkinitramfs.py $OUT/linux-init.o $OUT/linux-initrd.cpio || exit 1
# boot NAME "flags" [extra pattern]: the typed line must come back, the guest
# must reset cleanly and the extra pattern (if any) must appear in the log.
fail=0
boot() {
    local name=$1 flags=$2 want=$3 start rc secs
    start=$(date +%s)
    printf 'hello\rexit\r' | ./x86sim $flags --kernel $KERNEL --initrd $OUT/linux-initrd.cpio > $OUT/$name.got 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    if [ $rc -eq 0 ] && grep -q "you typed: hello" $OUT/$name.got && grep -q "reboot: Restarting system" $OUT/$name.got &&
       { [ -z "$want" ] || grep -q "$want" $OUT/$name.got; } && ! grep -q "unchecked MSR" $OUT/$name.got; then
        printf '  %-20s \033[32mPASS\033[0m (%ss)\n' $name $secs
    else
        printf '  %-20s \033[31mFAIL\033[0m (exit %d)\n' $name $rc
        tail -20 $OUT/$name.got | sed 's/^/      /'
        fail=1
    fi
}
boot linux-boot ""
boot linux-smp "--smp 2" "Brought up 1 node, 2 CPUs"
boot linux-smp-cache "--smp 2 --cache" "Brought up 1 node, 2 CPUs"
boot linux-smp8-cache "--smp 8 --cache" "Brought up 1 node, 8 CPUs"
exit $fail
