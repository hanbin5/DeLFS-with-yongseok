#!/bin/bash
# Bring up the nvme-loop test device (file backend) after a reboot. Safe to run twice.
# keep_alive_tmo=600: with the 5s default, a VM stall on the Hyper-V host expires
# keep-alive and the controller (and /dev/nvme0n1) disappears. kato=0 is not honored.
# Usage (on the test VM): sudo ~/nvme-loop-up.sh
set -eu

IMG=/var/lib/delfs-nvme.img
NQN=delfs
CFG=/sys/kernel/config/nvmet

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$IMG" ] || fallocate -l 20G "$IMG"
modprobe nvmet
modprobe nvme-loop

if [ ! -d "$CFG/subsystems/$NQN" ]; then
	mkdir "$CFG/subsystems/$NQN"
	echo 1 > "$CFG/subsystems/$NQN/attr_allow_any_host"
	mkdir "$CFG/subsystems/$NQN/namespaces/1"
	echo -n "$IMG" > "$CFG/subsystems/$NQN/namespaces/1/device_path"
	echo 1 > "$CFG/subsystems/$NQN/namespaces/1/enable"
fi
[ -d "$CFG/ports/1" ] || { mkdir "$CFG/ports/1"; echo loop > "$CFG/ports/1/addr_trtype"; }
[ -e "$CFG/ports/1/subsystems/$NQN" ] || ln -s "$CFG/subsystems/$NQN" "$CFG/ports/1/subsystems/$NQN"

if ! grep -qx "$NQN" /sys/class/nvme/nvme*/subsysnqn 2>/dev/null; then
	nvme connect -t loop -n "$NQN" -k 600
	sleep 1
fi

for c in /sys/class/nvme/nvme*; do
	[ "$(cat "$c/subsysnqn")" = "$NQN" ] && echo "ctrl=$(basename "$c") kato=$(cat "$c/kato")"
done
for b in /sys/block/nvme*n1; do
	case "$(basename "$b")" in nvme*c*n1) continue ;; esac   # hidden per-path node
	[ "$(cat "$b/device/subsysnqn" 2>/dev/null)" = "$NQN" ] || continue
	echo "dev=/dev/$(basename "$b") model=$(tr -d ' ' < "$b/device/model")" \
	     "lba=$(cat "$b/queue/logical_block_size") discard_max=$(cat "$b/queue/discard_max_bytes")"
done
