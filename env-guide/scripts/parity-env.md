# Non-root DeLFS parity guest

**Environment verified, not a performance result.** The existing 7.2.8 build and fio-only initramfs did not constitute a pair; no matching 6.6.8 boot image was found in the build/boot areas. Fresh builds used the exact `7790de5aa` and `058914c8d` kernel and module trees, a common config seed and GCC 11.5. All four boot/format/mount/use/unload checks passed under KVM, as uid 1000. No shared host devices, host filesystem mounts, networking, TCG, or timed workload.

## Use the prepared pair

From the repo root, `p=$PWD/.parity-env-0f273f8b`. Both builds at `$p/{6,7}` contain `obj/arch/x86/boot/bzImage`, `obj/.config`, `on/delfs.ko`, `off/delfs.ko`, compilation commands and `build.manifest`. `$p/guest/common.cpio.gz` is the **same bytes** for both; `$p/guest/files.sha256` inventories its regular files. The guest has fio 3.16, instrumented Filebench 1.5-alpha3 + cvars, seeded YCSB 0.18/RocksDB JNI 6.2.2, JRE 11, Python 3.8, bash, util-linux, and the pinned workload profiles in `/opt/repo`.

| Build | bzImage SHA-256 prefix | ON module | OFF module |
|---|---|---|---|
| 6.6.8 | `1e1e84c22dbd` | `e0805618dd5d` | `0760b6afd12f` |
| 7.2.8 | `b480f9bb03e4` | `f526b4ee9b65` | `e497d4051aee` |

Common image `a54a3d046b951e967d5b3a6e8d2a6575e8419c3656bbbdd2d2b8d6b9db5c9bd1`; formatter `3ef952e63e1368394b17847721c332e7af67b78f65b7b12f019630ce363ac61a`. Full proof per combination is in `$p/preflight-{6,7}-{on,off}/{vm.json,serial.log,results.tar.gz}`. `vm.json` records input hashes, QMP-confirmed acceleration, block cache/flush behavior and CPU thread IDs. Serial includes actual online CPUs, queues, proc/sys limits, mount options and smoke assertions. Result tar contains Filebench's gamma plugin confirmation, successful YCSB shell insert/read roundtrip, and kernel dmesg after rmmod.

**6.6.8 instrumentation trap:** the module-local `calc/calclock.h` defines `CONFIG_CALCLOCK`, but `super.c`, `file.c`, etc. actually include `<linux/calclock.h>` whose definition is commented out. The unmodified default module linked `__ktprint` but `super.o` did **not call it** (`6/as-shipped/super.undefined`). Thus this binary is not an official ON baseline. For ON, compile just the external original module with `CFLAGS_MODULE=-DCONFIG_CALCLOCK=` (empty definition matches the local header). The 7.2.8 module uses `CALCLOCK=1`. In both ON builds `super.o` references `__ktprint` and unload logged nonzero `f3fs_file_write_iter` calls; neither OFF build did. OFF 6 uses a private module copy without the local define. No changes to either checked-out kernel or module.

Example **non-timed** smoke (requires non-root KVM access):

```sh
p=$PWD/.parity-env-0f273f8b
python3 env-guide/scripts/parity-env-vm.py "$p/6" "$p/guest/common.cpio.gz" on "$p/new-smoke"
```

Direct `/dev/kvm` access is denied for the ordinary group list on the validation host. Where separately authorized, the coordinator used the scoped prefix `sudo -A -- /usr/bin/setpriv --reuid=ubuntu --regid=kvm --init-groups --no-new-privs /usr/bin/python3` **in place of** `python3` above. The VM launcher rejects uid 0 and requires KVM API 12 and QMP `query-kvm.enabled` before unpausing; it never tries TCG.

* Fixed machine `pc-i440fx-4.2`, `-cpu host`, 16 vCPUs pinned 1:1 to host CPUs 0–15, I/O thread pinned to 16, emulator on 17, 4096 MiB RAM, no host tuning. Dedicated 16-queue virtio `PARITY-DATA` `/dev/vda`, fresh private raw file per boot, native AIO, direct I/O, discard unmap, flush enabled. RAM root. Default smoke disk 8 GiB; choose the **same** `--disk-gib` after workload sizing and leave ≥8 GiB host headroom beyond its logical capacity. Never pass an existing disk. A successful run removes only its own raw data and boot initrd unless `--keep-images`; failures retain these for diagnosis.
* Inside the guest, mkfs and mount run with 16 CPUs. Dirty background/hard limits are 64/256 MiB, writeback/expiration 500/3000 centiseconds, ASLR off for Filebench. Common barriers/flush, discard, adaptive mode and fsync settings are visible in both mounts; original also displays `no_heap`, port `lookup_mode=perf`. Record rather than suppress these release-specific default displays. Builds have SMP, PREEMPT_DYNAMIC (`preempt=full`), HZ=1000, matched F2FS suboptions, CRC32 built in. Full configurations are retained, not presumed identical across releases.
* For an authorized workload, pass `--job private.sh --payload trusted.tar`; the tar contains paths **relative to guest `/`**, e.g. `opt/repo/env-guide/scripts/parity-run.py`. PID 1 starts that shell **after** verifying `/mnt/test` is the dedicated `delfs` mount, not after doing the smoke writes. Use `/usr/bin/fio`, `/opt/parity-filebench/bin/filebench`, `/opt/parity-ycsb/bin/ycsb.sh`. Put raw results in `/results/`, outside the workload filesystem; they leave by a write-only virtio serial channel. Set `--timeout` for the full job including preparation. A smoke only proves the environment: before any official timing, coordinate exclusive use of the host, freeze the job/inputs/limits, reject host interference and failed captures.

## Rebuild without host installation

Start as a non-root user from repo root with a **new private directory** and local toolchain, ≥15 GiB free before each kernel build. Supply the same full `seed.config` to both builds. The saved seed is SHA-256 `b0329151e90b4e3abfaafd2584a165bf4479b99d59b0faa1ab92395607391dc1`. To reproduce it from scratch with the validated toolchain:

```sh
umask 077
repo=$PWD; s=$repo/env-guide/scripts
w=$repo/.parity-private-new
mkdir "$w" && mkdir "$w/seed-src" "$w/seed-obj" "$w/scratch"
export TMPDIR=$w/scratch
git archive 058914c8d:DeLFS/linux-7.2.8 | tar -x -C "$w/seed-src"
make -s -C "$w/seed-src" O="$w/seed-obj" x86_64_defconfig
(cd "$w/seed-obj" && cp .config default.config &&
 "$w/seed-src/scripts/kconfig/merge_config.sh" -m default.config "$s/parity-env-seed.fragment")
make -s -C "$w/seed-src" O="$w/seed-obj" olddefconfig
sha256sum "$w/seed-obj/.config"
JOBS=4 bash "$s/parity-env-build.sh" "$w/6" 6 "$w/seed-obj/.config"
JOBS=4 bash "$s/parity-env-build.sh" "$w/7" 7 "$w/seed-obj/.config"
```

`parity-env-userspace.sh` consumes **trusted, already built** local Filebench prefix and YCSB release distribution (not a source checkout), fio, OpenJDK 11 including `jlink`, Python 3, static BusyBox, ldd, cpio, and autotools/compiler for the pinned formatter. No download/host package install. Use the parity instrumenter's builds, not a stock binary, for qualification. On the validation host both inputs were retained privately:

```sh
FILEBENCH_PREFIX="$p/inputs/parity-filebench" FILEBENCH_GUEST_PREFIX=/opt/parity-filebench \
YCSB_DIST="$p/inputs/parity-ycsb" bash "$s/parity-env-userspace.sh" "$w/guest"
```

On another host supply corresponding paths to the instrumented inputs. Filebench's guest prefix must match its compiled-in cvar path. Freeze the resulting single image across all runs (timestamps/toolchain affect hashes when rebuilding). Use `parity-env-vm.py --help` for the boot interface.

## Reproduce the write-path candidate

`parity-env-build.sh` intentionally builds the **unmodified** modules. For the
two-change candidate in the [pilot table](../README.md#6-현재-결과), keep the
matched kernels, userspace and original module. Build the changed 7.2.8
module separately; do not overwrite the controls. From the repository root,
with `w` set to the absolute directory containing the prepared `6/`, `7/` and
`guest/`, run in Bash:

```bash
set -eu
umask 077
c="$w/write-path-candidate"
mkdir "$c" "$c/7"
ln -s "$w/guest" "$c/guest"
ln -s "$w/6" "$c/6"
ln -s "$w/7/obj" "$c/7/obj"
for mode in on off; do
    m="$c/7/$mode"; mkdir "$m"
    git archive 058914c8d:DeLFS/module_DeLFS-7.2.8 | tar -x -C "$m"
    cp DeLFS/module_DeLFS-7.2.8/{data.c,segment.h} "$m/"
    switch=0; if test "$mode" = on; then switch=1; fi
    make -s -C "$w/7/src" O="$w/7/obj" M="$m" CALCLOCK="$switch" -j4 modules
    sha256sum "$m/delfs.ko"
done
python3 -B env-guide/scripts/parity-series.py "$c" "$c/pilot-on" \
    --mode on --scope pilot --pairs 6 --case fio:rand-write:1 --case fio:rand-write:16
```

Use a new output directory and `--mode off` for the separate OFF pilot.
The measured candidate module SHA-256 prefixes were `4312e13c060b` (ON) and
`2ce2b1a5f9f3` (OFF); record the actual hashes for every rebuild. For
[recovery verification](parity-recovery.md) pass `"$c/7/on/delfs.ko"`, not
`"$w/7/on/delfs.ko"`.
