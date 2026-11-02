#!/bin/bash
# Run the repo's filebench profiles and RocksDB/YCSB workloads scaled down for a
# 20GB nvme-loop device. Each case starts from a fresh mkfs:
#   mkfs -> load -> mount -> benchmark -> umount -> unload, then check the kernel log.
# Usage (on the test VM): sudo [FS=delfs|f2fs] ~/run-macro-small.sh [filebench|ycsb|all] [case ...]
#   e.g. sudo FS=f2fs ~/run-macro-small.sh filebench
set -u
. "$(dirname "$0")/bench-lib.sh"

REPO=$DELFS_REPO/script
FILEBENCH=$UHOME/bench/filebench/bin/filebench
YCSB=$UHOME/bench/ycsb-rocksdb

# filebench: shrink the dataset (original: 128GB-480GB) to about 5GB by file size only.
# File counts, threads and run time are kept: fileserver needs its free-entry/thread ratio,
# otherwise createfile fails with "could not obtain a file". Values are sed expressions.
declare -A FB=(
	[myfileserver]='s/mean:10737414/mean:409600/'
	[myvarmail]='s/mean:10737414/mean:204800/'
	[mywebserver]='s/mean:10737414/mean:204800/'
	[myvideoserver]='s/^set \$filesize=.*/set $filesize=128m/'
	[myoltp]='s/^set \$filesize=.*/set $filesize=40m/'
)
FB_ORDER=(myfileserver myvarmail myvideoserver mywebserver myoltp)

# YCSB: records and operations are scaled together; the files' field settings are kept as-is
# (A/B/U spell "fieldlenght", so YCSB uses the default 100B fields there; F uses 4KB).
# A/B/U: 10M records -> 2M, 1.28M ops -> 256k.  F: 2.56M records (40KB each) -> 75k, ops -> 37.5k.
declare -A YC=(
	[myworkloada]="recordcount=2000000 operationcount=256000"
	[myworkloadb]="recordcount=2000000 operationcount=256000"
	[myworkloadu]="recordcount=2000000 operationcount=256000"
	[myworkloadf]="recordcount=75000 operationcount=37500"
)
YC_ORDER=(myworkloada myworkloadb myworkloadf myworkloadu)
YC_THREADS=32
# bin/ycsb is a python2 script; Ubuntu 24.04 has no python2, so fall back to the bash launcher.
if command -v python2 >/dev/null; then YCSB_CMD=(python2 bin/ycsb); else YCSB_CMD=(bin/ycsb.sh); fi

SUITE=${1:-all}; shift || true
OUT=$UHOME/macro-small/$(date +%m%d-%H%M%S)-$SUITE-$FS-$KREL

preflight
mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
printf "%-16s %-5s %-4s %-10s %-7s %-6s %s\n" case rc ok runtime_s lockup kerr note | tee "$SUMMARY"

# filebench needs ASLR off (same as DeLFS/test.sh); restore on exit.
ASLR=$(cat /proc/sys/kernel/randomize_va_space)
trap 'echo $ASLR > /proc/sys/kernel/randomize_va_space; chown -R "$U:" $UHOME/macro-small' EXIT

setup() {   # $1 = case name
	[ -b "$DEV" ] || { echo "$1: $DEV is gone (nvme-loop controller lost?)"; return 1; }
	fs_mkfs > "$OUT/$1.mkfs" 2>&1 || { echo "$1: mkfs failed"; return 1; }
	fs_load || { echo "$1: load $MOD failed"; return 1; }
	fs_mount || { echo "$1: mount failed"; fs_unload; return 1; }
}

teardown() {   # $1 = case name; prints note
	local note=""
	df -h "$MNT" >> "$OUT/$1.log"
	rm -rf "${MNT:?}"/*
	umount "$MNT" || note="umount-failed"
	fs_unload || note="$note unload-failed"
	echo "$note"
}

report() {   # $1 case $2 rc $3 ok $4 secs $5 k0 $6 note
	local lockup kerr
	read -r lockup kerr < <(dmesg_since "$5" "$OUT/$1.dmesg")
	printf "%-16s %-5s %-4s %-10s %-7s %-6s %s\n" "$1" "$2" "$3" "$4" "$lockup" "$kerr" "$6" |
		tee -a "$SUMMARY"
}

run_filebench() {
	local cases=("$@")
	[ $# -eq 0 ] && cases=("${FB_ORDER[@]}")
	echo 0 > /proc/sys/kernel/randomize_va_space
	for c in "${cases[@]}"; do
		[ -n "${FB[$c]:-}" ] || { echo "skip unknown filebench case: $c"; continue; }
		local prof="$OUT/$c.f"
		sed -e "${FB[$c]}" "$REPO/filebench/$c.f" > "$prof"
		local k0; k0=$(dmesg | wc -l)
		setup "$c" || return 1
		local t0; t0=$(date +%s)
		(cd "$MNT" && "$FILEBENCH" -f "$prof") > "$OUT/$c.log" 2>&1
		local rc=$? t1; t1=$(date +%s)
		local ok=no
		grep -q 'IO Summary' "$OUT/$c.log" &&
			! grep -q -E 'could not obtain|Failed|FAILED|Error|ERROR' "$OUT/$c.log" && ok=yes
		local note; note=$(teardown "$c")
		report "$c" "$rc" "$ok" "$((t1 - t0))" "$k0" "$note"
		[ -n "$note" ] && return 1
	done
	return 0
}

run_ycsb() {
	local cases=("$@")
	[ $# -eq 0 ] && cases=("${YC_ORDER[@]}")
	for c in "${cases[@]}"; do
		[ -n "${YC[$c]:-}" ] || { echo "skip unknown ycsb case: $c"; continue; }
		local props=() kv
		for kv in ${YC[$c]}; do props+=(-p "$kv"); done
		local k0; k0=$(dmesg | wc -l)
		setup "$c" || return 1
		local t0; t0=$(date +%s)
		local args=(rocksdb -s -P "$REPO/RocksDB/workloads/$c" "${props[@]}"
			-p rocksdb.dir=$MNT/ycsb-rocksdb-data -threads $YC_THREADS)
		(cd "$YCSB" && "${YCSB_CMD[@]}" load "${args[@]}") > "$OUT/$c.load.log" 2>&1
		local rc1=$?
		(cd "$YCSB" && "${YCSB_CMD[@]}" run "${args[@]}") > "$OUT/$c.log" 2>&1
		local rc2=$? t1; t1=$(date +%s)
		local ok=no
		grep -q '^\[OVERALL\], RunTime' "$OUT/$c.load.log" && grep -q '^\[OVERALL\], RunTime' "$OUT/$c.log" &&
			! grep -q -E 'FAILED|Return=ERROR|Exception' "$OUT/$c.load.log" "$OUT/$c.log" && ok=yes
		du -sh "$MNT/ycsb-rocksdb-data" >> "$OUT/$c.log" 2>&1
		local note; note=$(teardown "$c")
		report "$c" "$rc1/$rc2" "$ok" "$((t1 - t0))" "$k0" "$note"
		[ -n "$note" ] && return 1
	done
	return 0
}

case "$SUITE" in
	filebench) run_filebench "$@" ;;
	ycsb)      run_ycsb "$@" ;;
	all)       run_filebench && run_ycsb ;;
	*) echo "usage: $0 [filebench|ycsb|all] [case ...]"; exit 1 ;;
esac
echo "results: $OUT"
