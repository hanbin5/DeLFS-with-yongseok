#!/bin/bash
# Run with bash; the two kernel versions are built separately, never installed.
set -euo pipefail
umask 077
die() { echo "parity-env: $*" >&2; exit 1; }
[[ $(id -u) != 0 && $# == 3 ]] || die "usage (non-root): bash $0 BUILD-DIR 6|7 CONFIG-SEED"
repo=$(git rev-parse --show-toplevel)
base=$(realpath -m -- "$1")
seed=$(realpath -- "$3")
[[ -f $seed ]] || die "regular seed required"
case "$2" in
  6) rev=7790de5aad826b50f3bfdf4e502e40cb9b482091; ver=6.6.8; mod=module_DeLFS;;
  7) rev=058914c8d226d5bd12b7765d46f388e58749a5af; ver=7.2.8; mod=module_DeLFS-7.2.8;;
  *) die "unknown version";;
esac
jobs=${JOBS:-4}
[[ $jobs =~ ^[1-8]$ ]] || die "JOBS must be in 1..8"
[[ $(df -Pm -- "$(dirname "$base")" | tail -1 | awk '{print $4}') -ge 15360 ]] || die "need >=15 GiB free before each build"
export TMPDIR="$base/tool-tmp" LOCALVERSION= KBUILD_BUILD_USER=parity KBUILD_BUILD_HOST=private
export KBUILD_BUILD_TIMESTAMP="$(git -C "$repo" show -s --format=%cD "$rev")"
kmake=(nice -n 10 make --no-print-directory -C "$base/src" O="$base/obj" -j"$jobs")
mkdir -- "$base"
mkdir -- "$base/tool-tmp" "$base/obj" "$base/src"
git -C "$repo" archive "$rev:DeLFS/linux-$ver" | tar -x -C "$base/src"
cp -- "$seed" "$base/seed.config"
cp -- "$seed" "$base/obj/.config"
cfg=("$base/src/scripts/config" --file "$base/obj/.config")
"${cfg[@]}" --set-str LOCALVERSION '-delfs' -d LOCALVERSION_AUTO -e PREEMPT \
    -d PREEMPT_LAZY -d PREEMPT_NONE -d PREEMPT_VOLUNTARY -e PREEMPT_DYNAMIC \
    -e MODULES -e MODULE_UNLOAD -e SMP --set-val NR_CPUS 64 -e SYSVIPC -e AIO \
    -e BLK_DEV_INITRD -e RD_GZIP -e DEVTMPFS -e TMPFS -e VIRTIO_PCI -e VIRTIO_BLK -e VIRTIO_CONSOLE -e CRYPTO_CRC32 \
    -m F2FS_FS -e F2FS_STAT_FS -e F2FS_FS_XATTR -e F2FS_FS_POSIX_ACL -e F2FS_FS_SECURITY \
    -e F2FS_IOSTAT -d F2FS_FS_COMPRESSION -d FS_VERITY -d FS_ENCRYPTION \
    -d DEBUG_INFO -e DEBUG_INFO_NONE -d DEBUG_INFO_BTF \
    --set-str SYSTEM_TRUSTED_KEYS '' --set-str SYSTEM_REVOCATION_KEYS ''
"${kmake[@]}" olddefconfig
for opt in VIRTIO_BLK VIRTIO_PCI VIRTIO_CONSOLE CRYPTO_CRC32 RD_GZIP DEVTMPFS TMPFS MODULES SMP MODULE_UNLOAD SYSVIPC PREEMPT; do
    grep -qx "CONFIG_$opt=y" "$base/obj/.config" || die "CONFIG_$opt is not built in"
done
"${kmake[@]}" bzImage modules
"${kmake[@]}" INSTALL_MOD_PATH="$base/stage" modules_install
rel=$(cat "$base/obj/include/config/kernel.release")
[[ $rel == "$ver-delfs" ]] || die "unexpected release $rel"
for mode in on off; do
    m="$base/$mode"
    mkdir -- "$m"
    git -C "$repo" archive "$rev:DeLFS/$mod" | tar -x -C "$m"
    switch=0
    if [[ $mode == on ]]; then switch=1
    elif [[ $2 == 6 ]]; then
        [[ $(grep -c '^#define CONFIG_CALCLOCK$' "$m/calc/calclock.h") == 1 ]]
        sed -i '/^#define CONFIG_CALCLOCK$/d' "$m/calc/calclock.h"
    fi
    extra=()
    # 6.6.8 super.c/file.c actually include <linux/calclock.h>, which has
    # the define COMMENTED OUT. The module-local ON header alone is not proof!
    if [[ $2 == 6 && $mode == on ]]; then extra=(CFLAGS_MODULE=-DCONFIG_CALCLOCK=); fi
    "${kmake[@]}" M="$m" CALCLOCK="$switch" "${extra[@]}" modules
    nm -u "$m/super.o" > "$m/super.undefined"
    if [[ $mode == on ]]; then grep -qw __ktprint "$m/super.undefined"
    else ! grep -qw __ktprint "$m/super.undefined" || die "still emitting calclock prints"
    fi
    [[ $(modinfo -F vermagic "$m/delfs.ko" | cut -d' ' -f1) == "$rel" ]]
done
{
    echo "revision=$rev kernel=$ver module=$mod release=$rel jobs=$jobs"
    git -C "$repo" rev-parse "$rev:DeLFS/linux-$ver" "$rev:DeLFS/$mod"
    "${CC:-gcc}" --version | head -1
    (cd "$base" && sha256sum seed.config obj/.config obj/arch/x86/boot/bzImage \
        obj/Module.symvers {on,off}/{delfs.ko,calc/calclock.h,.super.o.cmd,super.undefined})
} > "$base/build.manifest"
