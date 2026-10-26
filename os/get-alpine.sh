#!/bin/bash
# Download an OS that boots on x86sim: Alpine Linux 3.20 (kernel 6.6
# linux-virt plus the Alpine minirootfs userland: busybox, musl, apk).
# Produces os/bzImage and os/initrd.cpio; the whole root filesystem lives
# in the initramfs, so changes are lost at power off. Boot it with
#   ./x86sim --kernel os/bzImage --initrd os/initrd.cpio
cd "$(dirname "$0")"
REL=https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64
ROOTFS=alpine-minirootfs-3.20.3-x86_64.tar.gz
mkdir -p cache
get() {
    [ -f cache/$1 ] && return
    echo "downloading $2"
    curl -sSfL -o cache/$1.part "$2" && mv cache/$1.part cache/$1 || { echo "download failed"; exit 1; }
}
get vmlinuz-virt $REL/netboot/vmlinuz-virt
get $ROOTFS $REL/$ROOTFS
cp cache/vmlinuz-virt bzImage
python3 - cache/$ROOTFS initrd.cpio <<'PY' || exit 1
import stat, sys, tarfile

INIT = b"""#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /dev/shm /tmp /run
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp
mount -t tmpfs tmpfs /run
hostname x86sim
echo
echo "Alpine Linux $(cat /etc/alpine-release) on x86sim ($(nproc) CPUs). Type 'exit' to power off."
setsid sh -c 'exec sh -l </dev/ttyS0 >/dev/ttyS0 2>&1'
sync
reboot -f
"""

out, ino = [], 1
def add(name, mode, data=b"", rdev=(0, 0)):
    global ino
    nm = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in (
        ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(nm), 0))
    ino += 1
    rec = hdr.encode() + nm
    rec += b"\0" * (-len(rec) % 4) + data
    out.append(rec + b"\0" * (-len(rec) % 4))

with tarfile.open(sys.argv[1]) as t:
    members = t.getmembers()
    for m in members:
        name = m.name.lstrip("./")
        if not name:
            continue
        perm = m.mode & 0o7777
        if m.isdir():
            add(name, stat.S_IFDIR | perm)
        elif m.issym():
            add(name, stat.S_IFLNK | 0o777, m.linkname.encode())
        elif m.islnk():
            add(name, stat.S_IFREG | perm, t.extractfile(t.getmember(m.linkname)).read())
        elif m.isfile():
            add(name, stat.S_IFREG | perm, t.extractfile(m).read())
add("dev/console", stat.S_IFCHR | 0o600, rdev=(5, 1))
add("init", stat.S_IFREG | 0o755, INIT)
add("TRAILER!!!", 0)
open(sys.argv[2], "wb").write(b"".join(out))
PY
echo "ready: ./x86sim --kernel os/bzImage --initrd os/initrd.cpio"
