#!/bin/bash
# Run the repo's fio jobs (DeLFS/script/fio) scaled down for a 20GB nvme-loop device.
# One mkfs at the start, then for each job: load -> mount -> fio -> rm files -> umount -> unload.
# Usage (on the test VM): sudo [FS=delfs|f2fs] ~/run-fio-small.sh [job ...]
#   e.g. sudo FS=f2fs ~/run-fio-small.sh
set -u
. "$(dirname "$0")/bench-lib.sh"

SRC=$DELFS_REPO/script/fio
OUT=$UHOME/fio-small/$(date +%m%d-%H%M%S)-$FS-$KREL
NUMJOBS=4

# Per-job size for NUMJOBS=4 on 20GB. 25/50/75-write and table6-* keep the
# original total/device-capacity ratio (original device ~1.82TiB).
# raw-* jobs are skipped: they write the authors' raw devices, not a file system.
declare -A SIZE=(
	[rand-write]=1G [seq-write]=1G [start]=1G
	[rand-read]=1G [seq-read]=1G
	[direct-rand-read]=1G [direct-seq-read]=1G
	[25-write]=1280M [50-write]=2560M [75-write]=3840M
	[table6-90]=500M [table6-99]=50M
)
ORDER=(rand-write seq-write start rand-read seq-read direct-rand-read direct-seq-read
       table6-99 table6-90 25-write 50-write 75-write)
[ $# -gt 0 ] && ORDER=("$@")

preflight
mkdir -p "$OUT"
trap 'chown -R "$U:" $UHOME/fio-small' EXIT
fs_mkfs > "$OUT/mkfs.log" 2>&1 || { echo "mkfs failed (see $OUT/mkfs.log)"; exit 1; }

SUMMARY="$OUT/summary.txt"
printf "%-18s %-6s %-5s %-10s %-7s %-6s %s\n" job fio_rc err runtime_s lockup kerr note | tee "$SUMMARY"

for job in "${ORDER[@]}"; do
	size=${SIZE[$job]:-}
	[ -n "$size" ] && [ -f "$SRC/$job.fio" ] || { echo "skip unknown job: $job"; continue; }
	jobfile="$OUT/$job.fio"
	sed -e "s/^numjobs=.*/numjobs=$NUMJOBS/" -e "s/^size=.*/size=$size/" "$SRC/$job.fio" > "$jobfile"

	k0=$(dmesg | wc -l)
	note=""
	fs_load || { echo "$job: load $MOD failed"; break; }
	fs_mount || { echo "$job: mount failed"; fs_unload; break; }

	t0=$(date +%s)
	(cd "$MNT" && fio "$jobfile") > "$OUT/$job.log" 2>&1
	rc=$?
	t1=$(date +%s)

	df -h "$MNT" >> "$OUT/$job.log"
	rm -f "$MNT"/file1.*
	umount "$MNT" || note="umount-failed"
	fs_unload || note="$note unload-failed"

	read -r lockup kerr < <(dmesg_since "$k0" "$OUT/$job.dmesg")
	err=$(grep -o -m1 'err= *[0-9]*' "$OUT/$job.log" | tr -d ' ' | cut -d= -f2)
	printf "%-18s %-6s %-5s %-10s %-7s %-6s %s\n" "$job" "$rc" "${err:-?}" "$((t1 - t0))" \
		"$lockup" "$kerr" "$note" | tee -a "$SUMMARY"
	[ -n "$note" ] && { echo "stop: $note"; break; }
done

echo "results: $OUT"
