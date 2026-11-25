# DeLFS recovery acceptance (functional, not timed)

Run **as a non-root user** with access to KVM, from the repository. This needs
`python3`, `cc` with static libc, GNU `cpio` and `qemu-system-x86_64`. Supply
trusted matching x86-64 kernel + DeLFS `.ko` files for each version and one
common initramfs with `/bin/busybox`, `mkfs.delfs` on PATH and its libraries.
The kernels need built-in virtio-blk, initramfs and all DeLFS dependencies
(including the original kernel's crypto `crc32` algorithm). Use the *ON*
artifacts from the parity environment for the planned comparison:

```sh
B=.parity-env-0f273f8b
python3 -B env-guide/scripts/parity-recovery.py \
  --userspace "$B/guest/common.cpio.gz" \
  --target 6.6.8 "$B/6/obj/arch/x86/boot/bzImage" "$B/6/on/delfs.ko" \
  --target 7.2.8 "$B/7/obj/arch/x86/boot/bzImage" "$B/7/on/delfs.ko" \
  --out recovery-run-01
```

The output directory **must not exist**. There is no disk, VM-name, PID, shell
hook or remote-host argument. For each target the runner creates one exclusive
sparse 8-GiB regular data file in its own private directory and an initramfs
overlay selecting a dedicated PID 1. No host shares, network, loop devices or
existing VM disks are attached. PID 1 checks the kernel release, 16 online CPUs,
exact per-run virtio serial and capacity before formatting the guest disk, and
requires an actual rw `delfs` mount. The launcher runs at most six serial guest
boots per target (default 90 s limit **per boot**). `--accel tcg` is explicit
smoke only; there is no automatic fallback. Do not run concurrently with timed
measurements.

| Check | What must be observed |
|---|---|
| `remount` | Write deterministic non-block-aligned bytes, fsync the file and directories around a cross-directory rename. Cleanly unmount and mount again; enumerate the old and new directories and check file type, exact size and every byte. |
| `fsync` | Create a **different** tree after that remount. After file and parent-directory fsync return, wait for a token-bound serial ACK; the host sends SIGKILL **only to its own QEMU child**, reaps it, and cold boots the same image. No syncfs, unmount or guest shutdown between publication and cut. Verify the acknowledged data and namespace again. |
| `checkpoint` | Establish a clean reference pack and one allocated generation file. Twice overwrite the generation file, `syncfs`, then cut externally. Read both packs **offline**: each four-block head/tail must have valid CRC and the same generation. The next version must appear in the opposite slot, leaving the prior generation intact (A→B→A); the next guest reads the previous generation's file contents. |
| `orphan` | Hold a 64-KiB allocated file open, fsync it, unlink it, fsync its parent and `syncfs`. The guest reports its live descriptor's inode with link count zero and waits without closing. After external SIGKILL the active checkpoint must list that **exact inode** in the orphan block and account for its node and data. Boot and mount again: the name must be absent, published data must survive, and after clean unmount a newer checkpoint must clear the orphan list and restore baseline valid inode, node and block counts. Absence of a name alone is not reclamation. |

`result.json` records hashes, exact boot arguments, acknowledged cuts, both
pack headers' digests/fields, per-version check statuses, and a cell-by-cell
comparison. Serial logs record formatter hash, actual mount and check failures.
`failed` means an oracle observed a mismatch; `not_measured` means the necessary
evidence was never obtained (missing target, setup error, timeout, or earlier
failure). A candidate-only failure relative to a verified original is flagged
for investigation, not automatically attributed to a particular kernel change.
A partial/single-version run exits nonzero, even when that side verifies.
Delete the result directory when no longer needed.

**Scope:** `cache=none` honors guest flushes; a cut discards guest RAM and
terminates the VM, not the host or the physical storage controller. No promises
are tested for unsynced operations, physical power loss, arbitrary tear points,
checkpoint fallback under corruption, performance, or exhaustive crash ordering.
Prior art: `env-guide/scripts/bench-lib.sh` checks device/mount identity;
`DeLFS-CrashMonkey/code/tests/checkpoint_example.cpp` exercises file/directory
sync points. This runner does not use their fixed devices or privileged launch
scripts. The offline reader follows the two DeLFS `checkpoint.c` implementations
and `delfs-mkfs/include/f2fs_fs.h` (not stock F2FS pack layout).

Host-only tests (ordinary files and owned child processes; **not** a filesystem
recovery result):

```sh
python3 -B env-guide/scripts/parity-recovery-test.py -v
```

### Bounded KVM validation

One ON run per version, each with six boots and a new image, using the same
userspace (`a54a3d046b95…`), formatter and guest payload. All four checks
verified on both. The checkpoint slots below are zero-based; counts come from
the active, checksummed on-disk pack, not from directory listings.

| Kernel / module SHA-256 prefixes | Clean → rotate1 → rotate2 slots | Held inode | Valid inodes / nodes / blocks: baseline → held → recovered |
|---|---|---:|---|
| 6.6.8 `1e1e84c22dbd` / `e0805618dd5d` | 1 → 0 → 1 | 9 | 10/10/18 → 11/11/35 → 10/10/18 |
| 7.2.8 `b480f9bb03e4` / `f526b4ee9b65` | 0 → 1 → 0 | 13 | 10/10/18 → 11/11/35 → 10/10/18 |

This is evidence for these builds and these cut points only. Rerun on any
changed candidate; it is not evidence of performance parity.
