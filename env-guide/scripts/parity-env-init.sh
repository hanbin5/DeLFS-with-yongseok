#!/bin/busybox sh
# PID 1 in the disposable guest only. Never source this file on the host.
[ "$$" = 1 ] && [ -f /parity/expected-release ] || exit 2
/bin/busybox mount -t proc proc /proc || exit 1
/bin/busybox mount -t sysfs sysfs /sys || exit 1
/bin/busybox mount -t devtmpfs devtmpfs /dev || exit 1
/bin/busybox --install -s
export PATH=/usr/bin:/usr/sbin:/bin:/sbin HOME=/root JAVA_HOME=/opt/jre YCSB_HOME=/opt/ycsb DELFS_REPO=/opt/repo
set -eu
send_results() {
    for name in /sys/class/virtio-ports/*/name; do
        if [ -f "$name" ] && [ "$(cat "$name")" = parity.results ]; then
            port=$(basename "$(dirname "$name")")
            tar -czf "/dev/$port" -C /results .
            return $?
        fi
    done
    return 1
}
fail() {
    rc=$?
    set +e
    for f in /results/filebench.log /results/ycsb.log; do [ ! -f "$f" ] || tail -30 "$f"; done
    mkdir -p /results
    dmesg > /results/dmesg.log
    send_results
    echo "PARITY_ENV_FAIL rc=$rc"
    poweroff -f
}
trap fail EXIT
test "$(uname -r)" = "$(cat /parity/expected-release)"
test "$(cat /sys/devices/system/cpu/online)" = 0-15
test "$(cat /sys/block/vda/serial)" = PARITY-DATA
test -b /dev/vda && test ! -b /dev/vdb && test ! -b /dev/sda
test "$(awk '$2=="/" {print $1}' /proc/mounts)" = rootfs
grep -qw parity.private=1 /proc/cmdline
mkdir -p /dev/shm /dev/pts /mnt/test /results
mount -t tmpfs tmpfs /dev/shm
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp
hostname parity
echo 0 > /proc/sys/kernel/randomize_va_space
echo 67108864 > /proc/sys/vm/dirty_background_bytes
echo 268435456 > /proc/sys/vm/dirty_bytes
echo 3000 > /proc/sys/vm/dirty_expire_centisecs
echo 500 > /proc/sys/vm/dirty_writeback_centisecs
echo "guest uname=$(uname -a) online=$(cat /sys/devices/system/cpu/online) cmdline=$(cat /proc/cmdline)"
for f in /proc/sys/vm/dirty*; do
    [ ! -f "$f" ] || echo "$f=$(cat "$f")"
done
for name in logical_block_size optimal_io_size rotational scheduler write_cache fua \
            max_sectors_kb max_hw_sectors_kb max_segments max_segment_size \
            read_ahead_kb nr_requests wbt_lat_usec; do
    f=/sys/block/vda/queue/$name
    if [ -f "$f" ]; then
        value=$(cat "$f")
        echo "$f=$value"
    else
        echo "$f=unavailable"
    fi
done > /results/queue-defaults.txt
cat /results/queue-defaults.txt
echo "virtio_hw_queues=$(ls -d /sys/block/vda/mq/* | wc -l) serial=$(cat /sys/block/vda/serial) sectors=$(cat /sys/block/vda/size)"
sha256sum /parity/delfs.ko /usr/bin/mkfs.delfs /usr/bin/fio /usr/bin/filebench
grep MemTotal /proc/meminfo
insmod /parity/delfs.ko
mkfs.delfs -f /dev/vda
mount -t delfs /dev/vda /mnt/test
findmnt -nr -M /mnt/test -o SOURCE,FSTYPE,OPTIONS
test "$(findmnt -nr -M /mnt/test -o SOURCE,FSTYPE)" = "/dev/vda delfs"
if [ -f /parity/payload.tar ]; then tar -xf /parity/payload.tar -C /; fi
if [ -f /parity/job ]; then
    # Caller-owned job; keep raw outputs in /results, NOT on the data disk.
    bash /parity/job
else
    fio --version
    PARITY_SEED=7 filebench -c cvar-gamma > /results/filebench.log 2>&1
    grep 'Custom variable type: cvar-gamma' /results/filebench.log
    python3 -c 'import json, pathlib, subprocess, hashlib; print("Guest Python OK")'
    "$JAVA_HOME/bin/java" -version
    printf 'insert probe field0=parity\nread probe\nquit\n' |
        "$YCSB_HOME/bin/ycsb.sh" shell rocksdb -p rocksdb.dir=/mnt/test/rocks-probe > /results/ycsb.log 2>&1
    grep 'Result: OK' /results/ycsb.log
    grep 'Return code: OK' /results/ycsb.log
    grep -x 'field0=parity' /results/ycsb.log
fi
sync
umount /mnt/test
rmmod delfs
dmesg > /results/dmesg.log
# Match the actual rmmod print and a non-zero counter, not just strings in a .ko.
if [ "$(cat /parity/mode)" = on ]; then
    if [ -f /parity/job ]; then
        grep -E 'f3fs_file_write_iter is called [0-9,]+ times' /results/dmesg.log
    else
        grep -E 'f3fs_file_write_iter is called [1-9][0-9,]* times' /results/dmesg.log
    fi
else
    ! grep -q 'f3fs_file_write_iter is called' /results/dmesg.log || exit 1
fi
if grep -Ei 'BUG:|Oops:|WARNING: CPU:|blocked for more than|I/O error|Kernel panic' /results/dmesg.log; then exit 1; fi
send_results
echo PARITY_ENV_PASS
trap - EXIT
poweroff -f
