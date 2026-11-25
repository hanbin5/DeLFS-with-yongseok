#!/usr/bin/env python3
"""Bounded, non-timed DeLFS recovery acceptance in disposable QEMU guests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import shutil
import signal
import stat
import struct
import subprocess
import sys
import time
import zlib


HERE = Path(__file__).resolve().parent
VERSIONS = ("6.6.8", "7.2.8")
CELLS = ("remount", "fsync", "checkpoint", "orphan")
PHASES = ("publish", "restart", "rotate1", "rotate2", "orphan", "reclaim")
BLOCK = 4096
MAGIC = 0xf2f52010
MASK = (1 << 64) - 1
BAD_LOG = re.compile(rb"WARNING: CPU:|Oops:|BUG:|Kernel panic|I/O error|orphan failed")


class Mismatch(ValueError):
    def __init__(self, cell, message):
        super().__init__(message)
        self.cell = cell


def expect(ok, cell, message):
    if not ok:
        raise Mismatch(cell, message)


def u32(b, offset):
    return struct.unpack_from("<I", b, offset)[0]


def u64(b, offset):
    return struct.unpack_from("<Q", b, offset)[0]


def read_packs(disk):
    """Inspect only a stopped guest's private regular image, never a device."""
    if not disk.is_file():
        raise ValueError("not a regular image")
    with disk.open("rb") as f:
        def read(offset, size):
            f.seek(offset)
            data = f.read(size)
            if len(data) != size:
                raise Mismatch("checkpoint", "short metadata read")
            return data

        def header(block):
            # delfs_fs.h / f3fs_fs.h: four 4-KiB blocks, CRC field excluded.
            b = read(block * BLOCK, 4 * BLOCK)
            off = u32(b, 68)
            expect(12384 <= off <= len(b) - 4, "checkpoint", "bad header CRC offset")
            crc = zlib.crc32(b[:off] + b[off+4:], MAGIC ^ 0xffffffff) ^ 0xffffffff
            expect(crc == u32(b, off), "checkpoint", "bad header CRC")
            return b

        sb = read(1024, 3072)
        expect(u32(sb, 0) == MAGIC and u32(sb, 16) == 12 and u32(sb, 20) == 9,
               "checkpoint", "unexpected superblock/geometry")
        first, stride, payload = u32(sb, 76), 2 << u32(sb, 20), u32(sb, 1664)
        packs = []
        for start in (first, first + stride):
            h = header(start)
            version, flags, total, summary = u64(h, 0), u32(h, 36), u32(h, 40), u32(h, 44)
            expect(8 < total <= stride and 4 + payload <= summary <= total - 4,
                   "checkpoint", "invalid pack boundaries")
            tail = header(start + total - 4)
            expect(u64(tail, 0) == version, "checkpoint", "torn head/tail generation")
            orphans = []
            count = summary - 4 - payload
            expect(bool(flags & 2) == bool(count), "orphan", "flag/orphan extent disagree")
            for i in range(count):
                b = read((start + 4 + payload + i) * BLOCK, BLOCK)
                index, pages, entries = struct.unpack_from("<HHI", b, BLOCK - 12)
                expect(index == i + 1 and pages == count and 0 < entries <= 1020,
                       "orphan", "invalid orphan block")
                orphans.extend(struct.unpack_from("<%dI" % entries, b))
            packs.append(dict(slot=len(packs), start=start, version=version, flags=flags,
                              blocks=u64(h, 16), nodes=u32(h, 48), inodes=u32(h, 52),
                              header_sha256=hashlib.sha256(h + tail).hexdigest(),
                              orphans=orphans))
    return packs


def active(packs):
    delta = (packs[1]["version"] - packs[0]["version"]) & MASK
    expect(delta not in (0, 1 << 63), "checkpoint", "ambiguous active pack")
    p = packs[int(delta < 1 << 63)]
    expect(not p["flags"] & (8 | 16 | 0x2000), "checkpoint", "error/fsck flag set")
    return p


def switched(old, new):
    a, b = active(old), active(new)
    expect(a["slot"] != b["slot"] and b["version"] == (a["version"] + 1) & MASK
           and a == new[a["slot"]], "checkpoint",
           "one dirty syncfs must commit the next generation to the opposite pack")


def held(base, current, ino):
    expect(not base["orphans"] and current["orphans"] == [ino] and not current["flags"] & 1
           and current["inodes"] == base["inodes"] + 1
           and current["nodes"] == base["nodes"] + 1
           and current["blocks"] >= base["blocks"] + 16,
           "orphan", "held inode not checkpointed with its allocated data")


def reclaimed(base, previous, final):
    expect(0 < (final["version"] - previous["version"]) & MASK < (1 << 63)
           and final["flags"] & 1 and not final["orphans"]
           and all(base[k] == final[k] for k in ("inodes", "nodes", "blocks")),
           "orphan", "clean checkpoint did not reclaim inode/node/data to baseline")


def hashfile(p):
    digest = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def regular(name):
    p = Path(name).resolve(strict=True)
    if not stat.S_ISREG(p.stat().st_mode):
        raise ValueError("not a regular file: %s" % name)
    return p


def overlay(directory, userspace, module, helper):
    """Base initramfs is read-only; select our own PID 1 via rdinit."""
    pr = directory / "pr"
    pr.mkdir()
    for src, name in ((module, "delfs.ko"), (helper, "ops"),
                      (HERE / "parity-recovery-init", "init")):
        shutil.copyfile(src, pr / name)
        (pr / name).chmod(0o755)
    with (directory / "initrd").open("xb") as out, userspace.open("rb") as base:
        shutil.copyfileobj(base, out)
        out.write(b"\0" * (-out.tell() % 4))
        out.flush()  # cpio inherits the fd, not Python's buffered file position
        subprocess.run(["cpio", "--quiet", "-o", "-H", "newc", "-R", "0:0"],
                       input=b"pr\npr/init\npr/ops\npr/delfs.ko\n", cwd=directory,
                       stdout=out, check=True)


def command(kernel, token, release, phase, accel):
    return ["qemu-system-x86_64", "-no-user-config", "-nodefaults", "-nic", "none",
            "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
            "-accel", accel, "-cpu", "host" if accel == "kvm" else "max",
            "-smp", "16,sockets=1,cores=16,threads=1", "-m", "2048",
            "-kernel", str(kernel), "-initrd", "initrd",
            "-append", ("rdinit=/pr/init console=ttyS0,115200 loglevel=5 panic=0 "
                        "pr.token=%s pr.release=%s pr.phase=%s" % (token, release, phase)),
            "-drive", "if=none,id=d0,file=data.raw,format=raw,cache=none,discard=unmap",
            "-device", ("virtio-blk-pci,drive=d0,logical_block_size=4096,"
                        "physical_block_size=4096,num-queues=16,serial=pr-" + token)]


def boot(directory, argv, token, phase, timeout, result):
    """Only signal the Popen child, never a supplied PID or a machine by name."""
    prefix = ("PR:%s:" % token).encode()
    pending = b""
    total = 0
    deadline = time.monotonic() + timeout
    with (directory / (phase + ".log")).open("xb") as log:
        p = subprocess.Popen(argv, cwd=directory, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            with selectors.DefaultSelector() as sel:
                sel.register(p.stdout, selectors.EVENT_READ)
                while True:
                    if time.monotonic() >= deadline or not sel.select(deadline - time.monotonic()):
                        raise RuntimeError("%s: boot/ACK timeout" % phase)
                    data = os.read(p.stdout.fileno(), 65536)
                    if not data:
                        raise RuntimeError("%s: guest exited before ACK" % phase)
                    total += len(data)
                    if total > (4 << 20):
                        raise RuntimeError("%s: console limit exceeded" % phase)
                    log.write(data)
                    pending += data
                    lines = pending.split(b"\n")
                    pending = lines.pop()
                    for line in lines:
                        if BAD_LOG.search(line):
                            raise RuntimeError("guest diagnostic: " + line.decode(errors="replace"))
                        if not line.startswith(prefix):
                            continue
                        msg = line[len(prefix):].rstrip(b"\r").decode("ascii")
                        if msg.startswith("error:assert:") and msg[13:] in CELLS:
                            raise Mismatch(msg[13:], msg)
                        if msg.startswith("error:"):
                            raise RuntimeError(msg)
                        if (phase, msg) in (("publish", "remount"), ("restart", "fsync"),
                                            ("orphan", "checkpoint")):
                            result["checks"][msg] = "verified"
                            continue
                        if phase == "orphan" and re.fullmatch(r"orphan:[1-9]\d*", msg):
                            ino = int(msg.split(":")[1])
                        elif phase != "orphan" and msg == "ack:" + phase:
                            ino = None
                        else:
                            raise RuntimeError("unexpected protocol event: " + msg)
                        p.kill()
                        if p.wait(timeout=10) != -signal.SIGKILL:
                            raise RuntimeError("guest exited without external SIGKILL")
                        result["cuts"].append(phase)
                        return ino
        finally:
            if p.poll() is None:
                p.kill()
            p.wait(timeout=10)
            p.stdout.close()


def target(run, version, kernel, module, userspace, helper, args, result):
    directory = run / version
    directory.mkdir()
    token = secrets.token_hex(8)
    result.update(kernel=str(kernel), module=str(module), cuts=[], packs={}, commands={})
    boot_kernel = directory / "bzImage"
    disk = directory / "data.raw"
    try:
        shutil.copyfile(kernel, boot_kernel)
        overlay(directory, userspace, module, helper)
        result.update(kernel_sha256=hashfile(boot_kernel),
                      module_sha256=hashfile(directory / "pr/delfs.ko"))
        with disk.open("xb") as f:
            f.truncate(8 << 30)
        for phase in PHASES:
            result["phase"] = phase
            argv = command(boot_kernel.name, token, version, phase, args.accel)
            result["commands"][phase] = argv
            ino = boot(directory, argv, token, phase, args.timeout, result)
            if phase == "publish":
                continue
            packs = read_packs(disk)
            history = result["packs"]
            history[phase] = packs
            p = active(packs)
            if phase == "restart":
                expect(p["flags"] & 1 and not p["orphans"], "checkpoint", "no clean baseline")
            elif phase in ("rotate1", "rotate2"):
                switched(history["restart" if phase == "rotate1" else "rotate1"], packs)
            elif phase == "orphan":
                result["held_ino"] = ino
                held(active(history["rotate2"]), p, ino)
            elif phase == "reclaim":
                reclaimed(active(history["rotate2"]), active(history["orphan"]), p)
                result["checks"]["orphan"] = "verified"
        if set(result["checks"].values()) != {"verified"}:
            raise RuntimeError("incomplete guest evidence")
        result["status"] = "verified"
    finally:
        for p in (disk, directory / "initrd", boot_kernel):
            p.unlink(missing_ok=True)
        if (directory / "pr").is_dir():
            shutil.rmtree(directory / "pr")


def compare(a, b):
    if "not_measured" in (a, b):
        return "not_compared"
    if a == b:
        return "both_" + a
    return "candidate_regression" if b == "failed" else "original_only_failure"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", nargs=3, action="append", required=True,
                    metavar=("VERSION", "KERNEL", "MODULE"))
    ap.add_argument("--userspace", required=True, help="common trusted initramfs with busybox/mkfs.delfs")
    ap.add_argument("--out", type=Path, required=True, help="new private result directory")
    ap.add_argument("--accel", choices=("kvm", "tcg"), default="kvm",
                    help="TCG is explicit functional smoke only")
    ap.add_argument("--timeout", type=int, default=90, help="seconds per boot (10..180)")
    args = ap.parse_args()
    versions = [t[0] for t in args.target]
    if os.geteuid() == 0 or not 10 <= args.timeout <= 180:
        ap.error("requires unprivileged host user and a bounded timeout")
    if len(set(versions)) != len(versions) or not set(versions) <= set(VERSIONS):
        ap.error("select 6.6.8 and/or 7.2.8 once each")
    run = args.out.resolve()
    os.umask(0o077)
    run.mkdir()  # refuses an existing directory, symlink or image
    results = {v: dict(status="not_measured", checks=dict.fromkeys(CELLS, "not_measured"))
               for v in VERSIONS}
    report = dict(scope="guest termination only; no timing or physical power loss",
                  accel=args.accel, versions=results)
    base, helper = run / "common.cpio", run / "ops"
    try:
        userspace = regular(args.userspace)
        shutil.copyfile(userspace, base)
        report.update(userspace=str(userspace), userspace_sha256=hashfile(base),
                      qemu=subprocess.check_output(["qemu-system-x86_64", "--version"],
                                                   text=True).splitlines()[0])
        report["sources_sha256"] = {name: hashfile(HERE / name) for name in
                                    ("parity-recovery.py", "parity-recovery-init", "parity-recovery-ops.c")}
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
                        str(HERE / "parity-recovery-ops.c"), "-o", str(helper)],
                       env=dict(os.environ, TMPDIR=str(run)), check=True)
        report["ops_sha256"] = hashfile(helper)
        for version, k, m in args.target:
            result = results[version]
            try:
                target(run, version, regular(k), regular(m), base, helper, args, result)
            except Mismatch as exc:
                result.update(status="failed", reason=str(exc))
                result["checks"][exc.cell] = "failed"
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                result.update(status="incomplete", reason=str(exc))
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        report["setup_error"] = str(exc)
    finally:
        base.unlink(missing_ok=True)
        helper.unlink(missing_ok=True)
        comparison = {c: compare(*(results[v]["checks"][c] for v in VERSIONS)) for c in CELLS}
        report["comparison"] = comparison
        (run / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(run / "result.json", comparison, flush=True)
    return int(any(r["status"] != "verified" for r in results.values()))


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    sys.exit(main())
