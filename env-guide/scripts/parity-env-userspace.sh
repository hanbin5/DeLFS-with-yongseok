#!/bin/bash
# Construct a common, kernel-independent initramfs from trusted local x86_64
# tools. No installation, host system tree copy, chroot, mount, or network.
set -euo pipefail
umask 077
[[ $# == 1 && $(id -u) != 0 ]] || { echo "usage: bash $0 NEW-DIR" >&2; exit 2; }
repo=$(git rev-parse --show-toplevel)
out=$(realpath -m -- "$1")
mkdir -- "$out"
root=$out/root
mkdir -p -- "$out/tool-tmp" "$root"/{bin,sbin,etc,usr/bin,usr/sbin,dev,sys,proc,tmp,run,root,mnt/test,opt,parity}
export TMPDIR="$out/tool-tmp"
fb=${FILEBENCH_PREFIX:-$HOME/bench/filebench}
fbguest=${FILEBENCH_GUEST_PREFIX:-$fb}
ycsb=${YCSB_DIST:-$HOME/bench/ycsb-rocksdb}
java=$(dirname "$(dirname "$(realpath "$(command -v java)")")")
for f in "$fb/bin/filebench" "$ycsb/bin/ycsb.sh" "$java/bin/jlink"; do test -x "$f"; done
test -f "$ycsb/lib/rocksdbjni-6.2.2.jar"
git -C "$repo" archive 058914c8d226d5bd12b7765d46f388e58749a5af:delfs-mkfs |
    (mkdir "$out/formatter" && tar -x -C "$out/formatter")
(cd "$out/formatter" && sh autogen.sh && ./configure --disable-shared && nice -n 10 make -j2)

cp -L "$(command -v busybox)" "$root/bin/busybox"
cp -L /bin/bash "$root/bin/bash"
ln -s bash "$root/bin/sh"
cp -a "$ycsb" "$root/opt/parity-ycsb"
ln -s /opt/parity-ycsb "$root/opt/ycsb"
# Filebench 1.5-alpha3 dlopens cvars at its compile-time absolute path.
[[ "$fbguest" = /* ]]
mkdir -p -- "$root$(dirname "$fbguest")"
cp -a -- "$fb" "$root$fbguest"
ln -s "$fbguest/bin/filebench" "$root/usr/bin/filebench"
"$java/bin/jlink" -J-XX:ActiveProcessorCount=2 -J-Xmx512m -J-XX:-UsePerfData \
    -J-Djava.io.tmpdir="$TMPDIR" --module-path "$java/jmods" --strip-debug \
    --no-header-files --no-man-pages --add-modules java.base,java.logging,java.management,java.naming,java.sql,jdk.management,jdk.unsupported \
    --output "$root/opt/jre"
ln -s /opt/jre/bin/java "$root/usr/bin/java"
py=$(python3 -c 'import sys;print("python%d.%d"%sys.version_info[:2])')
cp -L "$(command -v "$py")" "$root/usr/bin/$py"
ln -s "$py" "$root/usr/bin/python3"
mkdir "$root/usr/lib"
cp -a "/usr/lib/$py" "$root/usr/lib/"
for cmd in fio findmnt lsblk blockdev stat du df sync sha256sum awk sed grep find sort env taskset numfmt tar; do
    cp -L -- "$(command -v "$cmd")" "$root/usr/bin/$cmd"
done
cp -L "$out/formatter/mkfs/mkfs.delfs" "$root/usr/bin/mkfs.delfs"
mkdir -p "$root/opt/repo"
git -C "$repo" archive 058914c8d226d5bd12b7765d46f388e58749a5af \
    script/fio script/filebench script/RocksDB/workloads | tar -x -C "$root/opt/repo"
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$root/etc/passwd"
printf 'root:x:0:\n' > "$root/etc/group"
printf '127.0.0.1 localhost parity\n' > "$root/etc/hosts"
printf 'passwd: files\ngroup: files\nhosts: files\n' > "$root/etc/nsswitch.conf"
cp /etc/os-release "$root/etc/os-release"
# Resolve the ELF interpreter and DSO closure of native entry points, cvars,
# Python extensions, JNI/JVM (trusted inputs only; ldd is not a sandbox).
python3 - "$root" <<'PY'
import os, pathlib, re, shutil, subprocess, sys
root = pathlib.Path(sys.argv[1])
env = dict(os.environ, LC_ALL="C", LD_LIBRARY_PATH=":".join(
    str(root / p) for p in ("opt/jre/lib", "opt/jre/lib/server", "opt/jre/lib/jli")))
for binary in list(root.rglob("*")):
    if not binary.is_file() or binary.is_symlink():
        continue
    with binary.open("rb") as f:
        if f.read(4) != b"\x7fELF":
            continue
    result = subprocess.run(["ldd", binary], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if "not found" in result.stdout:
        sys.exit(f"ldd {binary}: {result.stdout}")
    for name in re.findall(r"(?:=> |^\s*)(/\S+) \(", result.stdout, re.M):
        if name.startswith(str(root) + "/"):
            continue
        dest = root / name.lstrip("/")
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            shutil.copy(name, dest)
PY
# rocksdbjni is extracted by Java at run time, not an ELF in the root listing above.
for lib in libstdc++.so.6 libgcc_s.so.1 libnss_files.so.2; do
    f=$(ldconfig -p | awk -v x="$lib" '$1==x && /x86-64/ && !f {f=$NF} END {print f}')
    [[ $f = /* ]]
    mkdir -p "$root$(dirname "$f")"; cp -L "$f" "$root$f"
done
(cd "$root" && LC_ALL=C find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) > "$out/files.sha256"
(cd "$root" && LC_ALL=C find . -print0 | LC_ALL=C sort -z | cpio --null -o -H newc --owner=0:0 --quiet | gzip -1n) > "$out/common.cpio.gz"
sha256sum "$out/common.cpio.gz" "$out/files.sha256"
