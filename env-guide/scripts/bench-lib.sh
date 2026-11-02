# Common parts of run-fio-small.sh and run-macro-small.sh. Sourced, not executed.
# Selects the file system under test with FS=delfs (default) or FS=f2fs.

FS=${FS:-delfs}
# Paths are relative to the invoking user's home (the scripts run under sudo).
U=${SUDO_USER:-$USER}
UHOME=$(getent passwd "$U" | cut -d: -f6)
# Team DeLFS fork (https://github.com/hanbin5/DeLFS-with-yongseok), cloned to ~/DeLFS.
# Set DELFS_REPO to override. Copy this file alongside the runners on the test VM.
if [ -z "${DELFS_REPO:-}" ]; then
	for d in "$UHOME/DeLFS" "$UHOME/fs-work/DeLFS"; do
		[ -d "$d/script" ] && { DELFS_REPO=$d; break; }
	done
fi
[ -d "${DELFS_REPO:-}/script" ] || { echo "DeLFS repo not found. set DELFS_REPO=<clone path>"; exit 1; }
DEV=/dev/nvme0n1
MNT=/mnt/test
KREL=$(uname -r)

case "$FS" in
delfs)
	MKFS=$UHOME/mkfs.delfs
	KO=$UHOME/delfs.ko
	MOD=delfs
	fs_load() {
		modprobe lz4_compress; modprobe lz4hc_compress; modprobe jbd2
		insmod "$KO"
	}
	;;
f2fs)
	MKFS=$UHOME/f2fs-tools/mkfs.f2fs
	MOD=f2fs
	fs_load() { modprobe f2fs; }
	;;
*)
	echo "unknown FS=$FS (use delfs or f2fs)"; exit 1 ;;
esac

fs_unload() { rmmod "$MOD"; }
fs_mount() { mount -t "$FS" "$DEV" "$MNT"; }
fs_mkfs() { "$MKFS" -f "$DEV"; }

# Abort unless DEV is the nvme-loop device and nothing of ours is mounted or loaded.
preflight() {
	[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
	[ -b "$DEV" ] || { echo "$DEV not found. run: sudo ~/nvme-loop-up.sh"; exit 1; }
	local model
	model=$(tr -d ' ' < /sys/block/${DEV#/dev/}/device/model 2>/dev/null)
	[ "$model" = "Linux" ] || { echo "$DEV model is '$model', not nvme-loop (Linux). abort"; exit 1; }
	findmnt -rn "$MNT" >/dev/null && { echo "$MNT already mounted. abort"; exit 1; }
	if lsmod | grep -q "^$MOD "; then
		rmmod "$MOD" || { echo "$MOD is loaded and in use. abort"; exit 1; }
	fi
	mkdir -p "$MNT"
	echo "FS=$FS kernel=$KREL dev=$DEV mkfs=$MKFS"
}

# $1 = dmesg line count before the case, $2 = output file.
# Prints "<lockup> <kerr>". Soft lockups are counted apart: on the VirtualBox/Hyper-V
# host they also hit idle CPUs, so they are not attributed to the file system.
dmesg_since() {
	dmesg | tail -n +$(($1 + 1)) > "$2"
	local lockup kerr
	lockup=$(grep -c 'soft lockup' "$2")
	kerr=$(grep -E 'WARNING|Oops|BUG:|blocked for more|I/O error|failing I/O|fatal error|keep-alive' "$2" |
		grep -vc 'soft lockup')
	echo "$lockup $kerr"
}
