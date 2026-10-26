#!/bin/bash
# Boot a stock Linux kernel (Alpine linux-virt 6.6, downloaded once into
# tests/linux/cache) with a tiny initramfs whose /init echoes a typed line
# and reboots. Passes when the line comes back and the guest resets cleanly:
# on one CPU, on two CPUs, and on two and eight CPUs with the MESI cache hierarchy.
# Every boot must switch to x2APIC mode through VT-d interrupt remapping.
# Then KVM: Alpine's linux-virt package (a matching kernel and its modules)
# boots an initramfs whose /init (kvm.S) loads kvm-intel and runs a guest.
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
       { [ -z "$want" ] || grep -q "$want" $OUT/$name.got; } && ! grep -q "unchecked MSR" $OUT/$name.got &&
       grep -q "Enabled IRQ remapping in x2apic mode" $OUT/$name.got; then
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

# KVM on the simulated VMX
APK=$CACHE/linux-virt-6.6.142-r0.apk
APKURL=https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/linux-virt-6.6.142-r0.apk
KVMDIR=$CACHE/kvm-6.6.142
if [ ! -f $KVMDIR/vmlinuz-virt ]; then
    [ -f $APK ] || { echo "downloading $APKURL"; curl -sSfL -o $APK.part "$APKURL" && mv $APK.part $APK; } || { echo "download failed"; exit 1; }
    python3 - $APK $KVMDIR <<'PY' || exit 1
import gzip, io, os, sys, tarfile
apk, out = sys.argv[1], sys.argv[2]
want = {"boot/vmlinuz-virt": "vmlinuz-virt", "irqbypass.ko.gz": "irqbypass.ko",
        "kvm.ko.gz": "kvm.ko", "kvm-intel.ko.gz": "kvm-intel.ko"}
os.makedirs(out, exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(gzip.decompress(open(apk, "rb").read())), mode="r:", ignore_zeros=True) as t:
    for m in t:
        key = m.name if m.name in want else os.path.basename(m.name)
        if key in want and m.isfile():
            data = t.extractfile(m).read()
            open(os.path.join(out, want[key]), "wb").write(gzip.decompress(data) if key.endswith(".gz") else data)
missing = [v for v in want.values() if not os.path.exists(os.path.join(out, v))]
sys.exit("missing from the package: " + " ".join(missing) if missing else 0)
PY
fi
clang -target x86_64-linux-gnu -c tests/linux/kvm.S -o $OUT/kvm-init.o || exit 1
python3 tests/linux/mkinitramfs.py $OUT/kvm-init.o $OUT/kvm-initrd.cpio \
    $KVMDIR/irqbypass.ko $KVMDIR/kvm.ko $KVMDIR/kvm-intel.ko || exit 1
kvm() {
    local name=$1 flags=$2 start rc secs
    start=$(date +%s)
    ./x86sim $flags --kernel $KVMDIR/vmlinuz-virt --initrd $OUT/kvm-initrd.cpio < /dev/null > $OUT/$name.got 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    if [ $rc -eq 0 ] && grep -q "5 = 2 + 3, hello from a KVM guest on x86sim" $OUT/$name.got &&
       grep -q "enable_apicv=Y flexpriority=Y vpid=Y pml=Y preemption_timer=Y ple_gap=128 enable_shadow_vmcs=Y" $OUT/$name.got &&
       grep -q "KVM_EXIT_MMIO 0000000000020000 000000000000005a 1" $OUT/$name.got &&
       grep -q "guest: local APIC timer interrupt" $OUT/$name.got && grep -q "guest: self IPI" $OUT/$name.got &&
       grep -q "guest done, guest memory\[0x1800\] = 0000000000004b56" $OUT/$name.got &&
       grep -q "dirty pages 0000000000000082" $OUT/$name.got && grep -q "dirty pages 000000000000000a" $OUT/$name.got &&
       ! grep -qE "vmwrite failed|vmread failed|vmptrld failed|kvm.*[Ee]rror|WARNING: CPU|BUG:|Oops" $OUT/$name.got; then
        printf '  %-20s \033[32mPASS\033[0m (%ss)\n' $name $secs
    else
        printf '  %-20s \033[31mFAIL\033[0m (exit %d)\n' $name $rc
        tail -20 $OUT/$name.got | sed 's/^/      /'
        fail=1
    fi
}
kvm linux-kvm ""
kvm linux-kvm-smp "--smp 2 --cache"
exit $fail
