#!/usr/bin/env python3
"""Run counterbalanced pairs in private guests. No raw host block devices."""
import argparse
import csv
import hashlib
import io
import json
import os
import shlex
import subprocess
import sys
import tarfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
RUNNER = HERE / "parity-run.py"
VM = HERE / "parity-env-vm.py"
VERSIONS = {"6": "6.6.8", "7": "7.2.8"}
BIN = {"fio": "/usr/bin/fio", "filebench": "/opt/parity-filebench/bin/filebench",
       "ycsb": "/opt/parity-ycsb/bin/ycsb.sh"}
FILE_SIZES = {"myfileserver": 409600, "myvarmail": 204800, "mywebserver": 204800,
              "myvideoserver": 128 << 20, "myoltp": 40 << 20}
FIO_MIB = {"25-write": 5*1024, "50-write": 10*1024, "75-write": 15*1024,
           "table6-90": 2000, "table6-99": 200}
PILOT = [("fio", "rand-write", "1"), ("fio", "rand-write", "16"),
         ("filebench", "myfileserver", "profile"), ("ycsb", "myworkloada", "1")]


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def options(cell):
    suite, work, _ = cell
    if suite == "fio":
        return ["--total-mib", str(FIO_MIB.get(work, 1024))]
    if suite == "filebench":
        return ["--fb-size-bytes", str(FILE_SIZES[work])]
    return ["--records", "75000", "--operations", "128000"]


def matrix():
    text = subprocess.check_output([sys.executable, "-B", RUNNER, "matrix"], text=True)
    rows = list(csv.DictReader(io.StringIO(text)))
    return text, list(dict.fromkeys((r["suite"], r["workload"], r["workers"]) for r in rows))


def write_job(cell, path, seed):
    args = ["python3", "-B", "/opt/repo/env-guide/scripts/parity-run.py",
            "run", *cell, "--dedicated-guest", "--mount", "/mnt/test",
            "--device", "/dev/vda", "--out", "/results/sample",
            "--binary", BIN[cell[0]], "--seed", str(seed), "--fill-mib", "0",
            "--reserve-mib", "512", *options(cell)]
    path.write_text("set -eu\n" + " ".join(map(shlex.quote, args)) + "\n")


def revalidate(vmout, output):
    """Extract only regular capture records in a flat sample directory, never tar paths."""
    raw = vmout / "capture"
    raw.mkdir()
    with tarfile.open(vmout / "results.tar.gz", "r:gz") as tar:
        for m in tar:
            p = Path(m.name)
            if p.parent != Path("sample") or not m.isfile():
                continue
            if not 0 <= m.size <= 128 * 1024 * 1024:
                raise ValueError("unexpected evidence size: " + m.name)
            with (raw / p.name).open("xb") as f:
                with tar.extractfile(m) as src:
                    for chunk in iter(lambda: src.read(1024 * 1024), b""):
                        f.write(chunk)
    text = subprocess.check_output([sys.executable, "-B", RUNNER, "extract", raw])
    if text != (raw / "sample.csv").read_bytes():
        raise ValueError(f"offline revalidation disagrees: {raw}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as f:
        f.write(text)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("builds", type=Path, help="private parent containing 6/, 7/, guest/common.cpio.gz")
    p.add_argument("out", type=Path, help="new private campaign directory")
    p.add_argument("--mode", choices=["on", "off"], required=True)
    p.add_argument("--scope", choices=["pilot", "full"], required=True)
    p.add_argument("--pairs", type=int, required=True)
    p.add_argument("--case", action="append", help="pilot only: suite:workload:workers (repeatable)")
    p.add_argument("--exclusive", action="store_true", help="operator confirms exclusive host CPU and storage")
    a = p.parse_args()
    if (a.pairs < 1 or (a.scope == "full" and
                         (a.pairs < 6 or not a.exclusive or a.case))):
        p.error("full qualification requires >=6 pairs, an exclusive host and the full matrix")
    if os.geteuid() == 0:
        p.error("launch as an unprivileged user with KVM access")
    os.umask(0o077)
    text, all_cells = matrix()
    cases = ([tuple(c.split(":")) for c in a.case] if a.case else
             (all_cells if a.scope == "full" else PILOT))
    if not cases or len(set(cases)) != len(cases) or not set(cases) <= set(all_cells):
        p.error("unknown or duplicate cell")
    builds, dest = a.builds.resolve(strict=True), a.out.absolute()
    dest.mkdir(mode=0o700)
    image = builds / "guest/common.cpio.gz"
    (dest / "operations.csv").write_text(text)
    with tarfile.open(dest / "payload.tar", "x") as tar:
        tar.add(RUNNER, arcname="opt/repo/env-guide/scripts/parity-run.py")
    protocol = {"scope": a.scope, "mode": a.mode, "pairs": a.pairs, "exclusive": a.exclusive,
                "cases": {":".join(c): options(c) for c in cases}, "seed0": 728000,
                "disk_gib": 20, "initramfs": sha(image), "workload_capture": sha(RUNNER),
                "expected_keys": sha(dest / "operations.csv"), "launcher": sha(VM),
                "guest_init": sha(HERE / "parity-env-init.sh"), "host": os.uname().nodename}
    manifest = dest / "protocol.json"
    manifest.write_text(json.dumps(protocol, sort_keys=True, indent=2) + "\n")
    buildsigs = {k: (sha(builds / k / "obj/arch/x86/boot/bzImage"),
                     sha(builds / k / a.mode / "delfs.ko")) for k in VERSIONS}
    for pair in range(1, a.pairs + 1):
        for n, cell in enumerate(cases):
            # Alternate first version between pairs; stagger order between cells.
            order = ("6", "7") if (pair+n) % 2 else ("7", "6")
            tag = "-".join(cell)
            job = dest / f"pair{pair}-{tag}.sh"
            write_job(cell, job, protocol["seed0"] + pair)
            for ver in order:
                target = dest / "series" / f"pair{pair:02d}" / VERSIONS[ver]
                vmout = dest / f"boot-{pair}-{tag}-{ver}"
                print(f"START pair={pair} case={tag} version={VERSIONS[ver]}", flush=True)
                subprocess.run([sys.executable, "-B", VM, builds / ver, image, a.mode, vmout,
                                "--job", job, "--payload", dest / "payload.tar",
                                "--disk-gib", "20", "--timeout", "900"], check=True)
                revalidate(vmout, target / tag / "sample.csv")
                receipt = json.loads((vmout / "vm.json").read_text())
                if not (receipt["kvm"]["enabled"] and len(receipt["cpus"]) == 16
                        and receipt["release"].startswith(VERSIONS[ver] + "-")
                        and receipt["mode"] == a.mode):
                    raise ValueError("VM qualification mismatch")
                if n == len(cases) - 1:
                    env = {"kernel": receipt["release"], "fs": "delfs", "vcpus": 16,
                           "accelerator": "kvm", "calclock": a.mode.upper(),
                           "common_sha256": sha(manifest), "kernel_sha256": buildsigs[ver][0],
                           "module_sha256": buildsigs[ver][1], "run_id": f"{dest.name}-{pair}-{ver}",
                           "verified": a.scope == "full" and a.exclusive}
                    (target / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
    print(f"Completed {a.scope} captures at {dest}", flush=True)


if __name__ == "__main__":
    main()
