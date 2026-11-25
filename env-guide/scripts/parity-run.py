#!/usr/bin/env python3
"""Capture ONE cell on a fresh, already verified dedicated DeLFS guest mount.

matrix: required comparison keys. run: stage profiles, execute, validate and save
one sample. extract: revalidate raw evidence offline and print its six-column CSV.
No format/mount/unmount, global cache controls, deletion or reuse of datasets.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
import random
import re
import shutil
import stat
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
WORKERS = ("1", "2", "4", "8", "16")
FIO = ("rand-write", "seq-write", "start", "rand-read", "seq-read", "direct-rand-read",
       "direct-seq-read", "25-write", "50-write", "75-write", "table6-90", "table6-99")
FILEBENCH = ("myfileserver", "myvarmail", "myvideoserver", "mywebserver", "myoltp")
YCSB = {"myworkloada": ("READ", "UPDATE"), "myworkloadb": ("READ", "UPDATE"),
        "myworkloadf": ("READ", "UPDATE", "READ-MODIFY-WRITE"), "myworkloadu": ("UPDATE",)}
SUITES = {"fio": FIO, "filebench": FILEBENCH, "ycsb": YCSB}
KEYS = ("suite", "workload", "workers", "operation")
COLUMNS = KEYS + ("throughput", "p99_us")
IO_TYPES = set("read writewholefile appendfilerand openfile createfile closefile readwholefile "
               "deletefile statfile aiowrite aiowait".split())
CONTROL = set("delay bwlimit semblock sempost hog eventlimit".split())
MIB = 1 << 20
PROFILE_SET_SHA256 = "c15b20eea1162869760d013b7cd2d147bafdd4822198c61e2e1b08d4f1fb4923"


class Invalid(ValueError):
    pass


class Indeterminate(Invalid):
    pass


def need(truth, why):
    if not truth:
        raise Invalid(why)


def positive(value, name):
    n = float(value)
    need(math.isfinite(n) and n > 0, "missing/non-positive/non-finite " + name)
    return n


def count(value, name):
    need(re.fullmatch(r"[0-9]+", str(value)) is not None, "invalid count: " + name)
    return int(value)


def profile_path(repo, suite, work):
    need(suite in SUITES and work in SUITES[suite], "unknown or raw-device profile")
    folder, ext = {"fio": ("fio", ".fio"), "filebench": ("filebench", ".f"),
                   "ycsb": ("RocksDB/workloads", "")}[suite]
    return repo / "script" / folder / (work + ext)


def verify_profiles(repo):
    # Filebench profiles can contain arbitrary commands. Never execute a modified
    # script merely because its basename matches an approved one.
    h = hashlib.sha256()
    for suite, works in SUITES.items():
        for work in works:
            h.update(work.encode() + b"\0" + profile_path(repo, suite, work).read_bytes() + b"\0")
    need(h.hexdigest() == PROFILE_SET_SHA256, "profile set differs from approved repository inputs")


def operations(suite, work, repo=REPO):
    path = profile_path(repo, suite, work)
    if suite == "fio":
        return ["read" if work.endswith("-read") else "write"]
    if suite == "ycsb":
        return YCSB[work]
    names = []
    for line in path.read_text().splitlines():
        if not line.lstrip().startswith("flowop "):
            continue
        match = re.match(r"\s*flowop (\w+) name=([\w-]+)(?:,|\s*$)", line)
        need(match and match[1] in IO_TYPES | CONTROL, "unclassified flowop: " + line)
        if match[1] in IO_TYPES:
            need(match[2] not in names, "duplicate flowop name")
            names.append(match[2])
    need(names, "no named I/O operations")
    return names


def matrix(repo):
    yield KEYS
    for suite, works in SUITES.items():
        for work in works:
            for workers in (("profile",) if suite == "filebench" else WORKERS):
                for op in operations(suite, work, repo):
                    yield suite, work, workers, op


def csv_text(rows):
    buf = io.StringIO()
    csv.writer(buf, lineterminator="\n").writerows(rows)
    return buf.getvalue()


def quantile(bins):
    rank = (99 * sum(bins.values()) + 99) // 100
    need(rank > 0, "empty latency histogram")
    for bucket, n in sorted(bins.items()):
        rank -= n
        if rank <= 0:
            return bucket


def fio_metrics(output, op, workers, total):
    """Ungrouped json+: merge completion-latency bins, never worker percentiles."""
    jobs = json.loads(output)["jobs"]
    need(len(jobs) == int(workers), "missing fio worker or unexpected group reporting")
    all_bins = Counter()
    max_ms = 0
    for job in jobs:
        need(job["error"] == 0, "failed fio job")
        d = job[op]
        other = job["read" if op == "write" else "write"]
        need(other["io_bytes"] == 0 and d["io_bytes"] == total // len(jobs), "wrong fio work")
        ios = count(d["total_ios"], "fio total_ios")
        need(ios * 4096 == d["io_bytes"] and ios, "bad completed fio I/O count")
        clat = d["clat_ns"]
        p99 = positive(clat["percentile"]["99.000000"], "fio clat_ns p99")
        bins = {count(k, "clat_ns bin"): count(v, "fio bin count") for k, v in clat["bins"].items()}
        # fio 3.16 exports total_ios and the bins, but not the redundant clat.N field.
        need(sum(bins.values()) == ios and ("N" not in clat or clat["N"] == ios)
             and p99 == quantile(bins), "partial/inconsistent fio clat_ns histogram")
        need(d["short_ios"] == d["drop_ios"] == 0, "short/dropped fio I/O")
        all_bins.update(bins)
        max_ms = max(max_ms, positive(d["runtime"], "fio direction runtime(ms)"))
    # Same group denominator as fio: bytes and completions across workers / longest direction time.
    seconds = max_ms / 1000
    return {op: (total / MIB / seconds, positive(quantile(all_bins) / 1000, "fio p99(us)"))}, {
        "io_bytes": total, "ios": sum(all_bins.values()), "iops": sum(all_bins.values()) / seconds,
        "direction_runtime_ms": max_ms}


def checked_log(text):
    need(not re.search(r"(?:\bfailed\b|\bexception\b|\b\w+Exception\b|\bERROR\b|could not obtain|"
                       r"NO VALID RESULTS|terminated prematurely|segmentation fault)", text, re.I),
         "benchmark reported an error (see raw log)")


def filebench_metrics(text, expected, seed):
    checked_log(text)
    summary = re.findall(r"\bIO Summary:\s+(\d+) ops\s+([\d.]+) ops/s\b", text)
    need(len(summary) == 1, "missing/duplicate Filebench IO Summary")
    need(re.findall(r"\bRun took (\d+) seconds\b", text) == ["60"], "incomplete 60s Filebench run")
    reported, rate = int(summary[0][0]), positive(summary[0][1], "Filebench ops/s")
    lines = re.findall(r"^\s*([\w-]+)\s+(\d+)ops\s+[\d.]+ops/s\b", text, re.M)
    counts = {name: int(n) for name, n in lines}
    need(len(counts) == len(lines) and all(counts.get(op, 0) > 0 for op in expected),
         "missing/empty/duplicate Filebench flowop result")
    need(sum(counts[op] for op in expected) == reported, "IO Summary does not match completed I/Os")
    hist = defaultdict(dict)
    for line in text.splitlines():
        if line.startswith("PARITY-HIST"):
            fields = line.split()
            need(len(fields) == 4 and fields[0] == "PARITY-HIST-v1", "unknown Filebench collector")
            op, idx, n = fields[1], count(fields[2], "bucket"), count(fields[3], "bucket count")
            need(op in expected and idx < 4096 and idx not in hist[op] and n, "invalid Filebench bin")
            hist[op][idx] = n
    if not hist and re.search(r"\[\s*(?:\d+\s+){63}\d+\s*\]", text):
        raise Indeterminate("stock power-of-two Filebench histogram is too coarse for a 10% p99 gate")
    need(re.findall(r"^PARITY-SEED-v1 (\d+)$", text, re.M) == [str(seed)], "unverified Filebench seed")
    rows, bounds = {}, {}
    for op in expected:
        need(sum(hist[op].values()) == counts[op], "missing/partial high-precision histogram: " + op)
        i = quantile(hist[op])
        if i == 4095:
            raise Indeterminate("Filebench p99 in overflow bucket: " + op)
        shift = max(0, i // 128 - 1)
        lo = i if i < 128 else ((128 + i % 128) << shift)
        hi = lo + (1 << shift) - 1
        rows[op] = rate, positive(hi / 1000, "Filebench p99(us)")
        bounds[op] = [lo, hi]
    return rows, {"completed_ios": reported, "ops_by_flow": counts, "p99_interval_ns": bounds}


def check_ycsb_log(text):
    checked_log(re.sub(r"^DBWrapper: report latency for each error is false and specific error codes "
                       r"to track for latency are: \[\]$", "", text, flags=re.M))


def ycsb_metrics(text, ops, completed, seed, workers):
    check_ycsb_log(text)
    seeds = re.findall(r"^PARITY-SEED-v1 (\d+) (\d+)$", text, re.M)
    need(sorted((int(s), int(i)) for s, i in seeds) == [(seed, i) for i in range(int(workers))],
         "wrong YCSB seed, uninstrumented client or missing worker")
    groups = defaultdict(dict)
    for group, key, value in re.findall(r"^\[([^]\n]+)\], ([^,\n]+), ([^\n]+)$", text, re.M):
        need(key not in groups[group], "duplicate YCSB metric: " + group + "/" + key)
        groups[group][key] = value
        need(not key.startswith("Return=") or key == "Return=OK" or value == "0", "failed YCSB operation")
    actual = {name for name, metrics in groups.items() if "Operations" in metrics}
    need(actual - {"CLEANUP"} == set(ops), "missing/unexpected YCSB operation")
    ms = positive(groups["OVERALL"]["RunTime(ms)"], "YCSB run time")
    rate = positive(groups["OVERALL"]["Throughput(ops/sec)"], "YCSB throughput")
    need(math.isclose(rate, completed * 1000 / ms, rel_tol=1e-8), "YCSB throughput/completions disagree")
    rows, counts = {}, {}
    for op in ops:
        g = groups[op]
        n = counts[op] = count(g["Operations"], op)
        need(n and (op == "READ-MODIFY-WRITE" or count(g["Return=OK"], "OK") == n),
             "incomplete YCSB status/count: " + op)
        # HDR's verbose export retains each occupied high-resolution (microsecond) bin in the log.
        bins = {int(k): float(v) for k, v in g.items() if k.isdecimal()}
        p99 = positive(g["99thPercentileLatency(us)"], "YCSB p99(us)")
        need(all(v > 0 and v.is_integer() for v in bins.values()) and sum(bins.values()) == n
             and p99 == quantile(bins), "missing/inconsistent HDR buckets: " + op)
        rows[op] = rate, p99
    if "READ-MODIFY-WRITE" in counts:
        need(counts["READ"] == completed and counts["UPDATE"] == counts["READ-MODIFY-WRITE"]
             <= completed, "incomplete workload F (nested RMW read/update)")
    else:
        need(sum(counts.values()) == completed, "incorrect completed YCSB operations")
    return rows, {"logical_operations": completed, "operation_counts": counts, "runtime_ms": ms}


def sub_once(pattern, replacement, text):
    text, n = re.subn(pattern, lambda _: replacement, text, flags=re.M)
    need(n == 1, "unsupported profile near " + pattern)
    return text


def render(m, src):
    suite, work, workers, data = (m[k] for k in ("suite", "workload", "workers", "data"))
    if suite == "fio":
        # Reject profiles with unexpected keys rather than letting fio interpret raw targets etc.
        active = [s for s in src.splitlines() if s.strip() and not s.startswith(";")]
        need(len(active) == 9 and active[0:2] == ["[global]", "directory=/mnt/test"] and
             active[-2] == "[file1]" and active[3:7] ==
             ["bs=4K", "direct=" + str(int(work.startswith("direct-"))), "numjobs=128", "group_reporting"]
             and active[2] in ("rw=read", "rw=randread", "rw=write", "rw=randwrite")
             and re.fullmatch(r"size=\d+M", active[-1]), "unsupported fio profile")
        op = operations(suite, work)[0]
        need(active[2].endswith(op), "fio direction mismatch")
        total = m["total_mib"] * MIB
        need(total > 0 and total % (int(workers) * 4096) == 0, "fio total must divide into whole blocks")
        opts = ["[global]", "directory=" + data, active[2], active[3], active[4],
                "numjobs=" + workers, "randseed=" + str(m["seed"]), "invalidate=1", "clat_percentiles=1",
                "percentile_list=99", "exitall_on_error=1", "[file1]", "size=" + str(total // int(workers))]
        job = "\n".join(opts) + "\n"
        files = {"run.fio": job}
        if op == "read":
            files["prefill.fio"] = sub_once(r"^rw=.*$", "rw=write\nend_fsync=1", job)
        return files
    if suite == "filebench":
        size = m["fb_size_bytes"]
        need(size > 0 and src.count("run 60") == 1, "invalid Filebench size or runtime")
        profile = sub_once(r"^set \$dir=/mnt/test$", "set $dir=" + data, src)
        if work in ("myvideoserver", "myoltp"):
            profile = sub_once(r"^set \$filesize=\d+[mg]$", "set $filesize=" + str(size), profile)
        else:
            profile = sub_once(r"mean:10737414;", "mean:%d;" % size, profile)
        return {"run.f": "enable lathist\n" + profile}
    need(m["records"] >= int(workers) and m["operations"] >= int(workers), "not enough YCSB work per client")
    props = src.replace("fieldlenght=", "fieldlength=")
    for k, v in {"threadcount": workers, "fieldcount": 10, "fieldlength": 4096,
                 "recordcount": m["records"], "operationcount": m["operations"]}.items():
        props = sub_once("^" + k + r"=\d+$", "%s=%s" % (k, v), props)
    # HTrace's default ID queries the network interfaces. The guest has no NIC.
    props += ("\nrocksdb.dir=%s/db\nparity.seed=%s\nhtrace.tracer.id=parity-offline\nmeasurementtype=hdrhistogram\n"
              "hdrhistogram.percentiles=99\nmeasurement.histogram.verbose=true\n") % (data, m["seed"])
    return {"workload.properties": props}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def load_json(path):
    return json.loads(path.read_text())


def stage(raw, m, repo):
    source = profile_path(repo, m["suite"], m["workload"])
    m["source_sha256"] = digest(source)
    shutil.copyfile(source, raw / "source.profile")
    for name, text in render(m, source.read_text()).items():
        (raw / name).write_text(text)
    save_json(raw / "manifest.json", m)


def check_cell(m):
    suite = m["suite"]
    need(suite in SUITES and m["workload"] in SUITES[suite], "invalid suite/workload")
    need(m["workers"] in (("profile",) if suite == "filebench" else WORKERS), "invalid workers")
    need(isinstance(m["seed"], int) and 0 < m["seed"] < 2**32, "expected seed in [1, 2^32-1]")
    need(m["fill_mib"] >= 0 and m["reserve_mib"] >= 1, "invalid fill/reserve")


def validated(raw, repo):
    m = load_json(raw / "manifest.json")
    check_cell(m)
    suite, work, w = m["suite"], m["workload"], m["workers"]
    source = profile_path(repo, suite, work)
    need(digest(raw / "source.profile") == digest(source) == m["source_sha256"], "profile identity mismatch")
    for name, text in render(m, source.read_text()).items():
        need((raw / name).read_text() == text, "effective profile differs: " + name)
    status = load_json(raw / "execution.json")
    opkeys = operations(suite, work, repo)

    def log(phase):
        record = status[phase]
        need(record["returncode"] == 0, phase + " command failed")
        if suite == "ycsb" and record.get("separate_stderr"):
            check_ycsb_log((raw / (phase + ".stderr")).read_text())
        return (raw / (phase + ".log")).read_text()

    if suite == "fio":
        total = m["total_mib"] * MIB
        if opkeys[0] == "read":
            fio_metrics(log("prefill"), "write", w, total)
        rows, details = fio_metrics(log("run"), opkeys[0], w, total)
        if opkeys[0] == "write":
            need(status["drain"]["files_synced"] == int(w), "incomplete final fsync")
            wall = positive(status["drain"]["fio_start_to_fsync_end_ns"], "sync inclusive wall time")
            details["sync_inclusive_wall_mib_s"] = m["total_mib"] * 1e9 / wall
    elif suite == "filebench":
        rows, details = filebench_metrics(log("run"), opkeys, m["seed"])
    else:
        _, load = ycsb_metrics(log("load"), ["INSERT"], m["records"], m["seed"], w)
        rows, details = ycsb_metrics(log("run"), opkeys, m["operations"], m["seed"], w)
        details["load"] = load
    return csv_text([COLUMNS] + [(suite, work, w, op, *rows[op]) for op in opkeys]), details


def verified_mount(mount, device, out):
    # Mirror the guest provisioner's dedicated virtio serial, not a guess based on /dev/vd* alone.
    need(mount.is_absolute() and mount.resolve(strict=True) == mount and mount != Path("/"),
         "expected an absolute non-root, non-symlink mountpoint")
    need("parity.private=1" in Path("/proc/cmdline").read_text().split(), "not a private parity guest")
    dev = device.resolve(strict=True)
    ds = dev.stat()
    need(device.is_absolute() and stat.S_ISBLK(ds.st_mode), "expected a guest block device")
    need((Path("/sys/class/block") / dev.name / "serial").read_text().strip() == "PARITY-DATA",
         "not the guest PARITY-DATA disk")
    cols = subprocess.check_output(
        ["findmnt", "-nr", "-M", str(mount), "-o", "MAJ:MIN,FSTYPE,FSROOT,TARGET"], text=True).split()
    devno = "%d:%d" % (os.major(ds.st_rdev), os.minor(ds.st_rdev))
    need(cols == [devno, "delfs", "/", str(mount)] and mount.stat().st_dev == ds.st_rdev
         and mount.stat().st_dev != Path("/").stat().st_dev, "not an exact dedicated DeLFS mount")
    for entry in mount.iterdir():
        need(entry.name == "lost+found" and not entry.is_symlink() and entry.is_dir() and not any(entry.iterdir()),
             "requires freshly provisioned empty filesystem; never cleans up")
    need(out.parent.resolve(strict=True).stat().st_dev != ds.st_rdev, "store logs outside benchmark device")
    return devno


def planned_bytes(m):
    if m["suite"] == "fio":
        return m["total_mib"] * MIB
    if m["suite"] == "ycsb":
        return 2 * m["records"] * 10 * 4096  # room for load plus compaction (not an upper bound)
    counts = {"mywebserver": 25601, "myfileserver": 12800, "myvarmail": 25600,
              "myvideoserver": 64, "myoltp": 128}
    return counts[m["workload"]] * m["fb_size_bytes"] + (200 * MIB if m["workload"] == "myoltp" else 0)


def make_fill(data, mib, seed):
    if mib:
        rng = random.Random(seed)
        with (data / "occupancy.bin").open("xb") as f:
            for _ in range(mib):
                f.write(rng.getrandbits(8 * MIB).to_bytes(MIB, "little"))
            f.flush()
            os.fsync(f.fileno())
            os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)


def execute(raw, data, m, exe):
    suite, w = m["suite"], m["workers"]
    records = {}
    env = dict(os.environ, LC_ALL="C", PARITY_SEED=str(m["seed"]))
    if suite == "ycsb":
        env["YCSB_HOME"] = str(exe.parent.parent)

    def command(phase, args, separate_stderr=False):
        args = [str(exe)] + list(map(str, args))
        start = time.monotonic_ns()
        with (raw / (phase + ".log")).open("x") as stdout, (raw / (phase + ".stderr")).open("x") as stderr:
            rc = subprocess.run(args, cwd=data, env=env, stdin=subprocess.DEVNULL, stdout=stdout,
                                stderr=stderr if separate_stderr else subprocess.STDOUT).returncode
        records[phase] = {"argv": args, "returncode": rc, "wall_ns": time.monotonic_ns() - start}
        if suite == "ycsb":
            records[phase]["separate_stderr"] = separate_stderr
        save_json(raw / "execution.json", records)
        need(rc == 0, phase + " exited %s" % rc)
        return start

    if suite == "fio":
        def fio(phase):
            return command(phase, ["--output-format=json+", raw / (phase + ".fio")], True)
        if (raw / "prefill.fio").exists():
            fio("prefill")
            fio_metrics((raw / "prefill.log").read_text(), "write", w, m["total_mib"] * MIB)
        start = fio("run")
        if operations("fio", m["workload"])[0] == "write":
            for i in range(int(w)):
                with (data / ("file1.%d.0" % i)).open("r+b") as f:
                    os.fsync(f.fileno())
            fd = os.open(str(data), os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            records["drain"] = {"files_synced": int(w), "fio_start_to_fsync_end_ns": time.monotonic_ns() - start}
            save_json(raw / "execution.json", records)
    elif suite == "filebench":
        need(Path("/proc/sys/kernel/randomize_va_space").read_text().strip() == "0",
             "prepare ASLR=0 in dedicated guest; runner does not change global settings")
        command("run", ["-f", raw / "run.f"])
    else:
        for phase in ("load", "run"):
            # Concurrent Java stdout/stderr writes can split the worker seed receipts on a merged fd.
            command(phase, [phase, "rocksdb", "-P", raw / "workload.properties", "-threads", w], True)
            check_ycsb_log((raw / (phase + ".stderr")).read_text())
            if phase == "load":
                ycsb_metrics((raw / "load.log").read_text(), ["INSERT"], m["records"], m["seed"], w)


def run(a):
    suite, raw, mount = a.suite, a.out.absolute(), a.mount
    need(a.dedicated_guest, "--dedicated-guest requires prior operator verification")
    data = mount / "parity-data"
    for path in (data, raw):
        need(re.fullmatch(r"/[A-Za-z0-9._/-]+", str(path)) and ".." not in path.parts,
             "unsupported characters in workload or output path")
    m = {k: getattr(a, k) for k in ("suite", "workload", "workers", "seed", "total_mib", "fb_size_bytes",
                                      "records", "operations", "fill_mib", "reserve_mib")}
    check_cell(m)
    required = {"fio": ["total_mib"], "filebench": ["fb_size_bytes"], "ycsb": ["records", "operations"]}
    for key in ("total_mib", "fb_size_bytes", "records", "operations"):
        need((m[key] is not None) == (key in required[suite]), "missing or inapplicable option: " + key)
    devno = verified_mount(mount, a.device, raw)
    exe = a.binary.resolve(strict=True)
    need(exe.is_file() and os.access(exe, os.X_OK), "--binary must identify executable")
    space = shutil.disk_usage(mount)
    m.update(data=str(data), device=str(a.device), devno=devno, capacity_bytes=space.total,
             free_before_bytes=space.free, binary=str(exe), binary_sha256=digest(exe),
             cache_policy={"fio": "invalidate=1; full end_fsync write pass before reads",
                           "filebench": "profile preallocation/cache/pacing; no extra warm-up",
                           "ycsb": "load then measured run; no extra warm-up"}[suite])
    need(planned_bytes(m) + (m["fill_mib"] + m["reserve_mib"]) * MIB <= space.free,
         "dataset/fill/free-space reserve exceed available storage")
    raw.mkdir()  # exclusive; never leave stale metrics from an earlier attempt
    try:
        stage(raw, m, a.repo)
        data.mkdir(mode=0o700)
        make_fill(data, m["fill_mib"], m["seed"])
        execute(raw, data, m, exe)
        result, details = validated(raw, a.repo)
        save_json(raw / "validation.json", {"status": "valid", "details": details})
        pending = raw / "sample.csv.partial"
        pending.write_text(result)
        pending.rename(raw / "sample.csv")
    except Exception as exc:
        state = "indeterminate" if isinstance(exc, Indeterminate) else "invalid"
        save_json(raw / "validation.json", {"status": state, "reason": str(exc)})
        raise


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo", type=Path, default=REPO, help="repo containing script/ profiles")
    cmd = p.add_subparsers(dest="cmd", required=True)
    cmd.add_parser("matrix", help="print canonical comparison keys as CSV")
    cmd.add_parser("extract", help="revalidate one raw sample; CSV to stdout").add_argument("sample", type=Path)
    r = cmd.add_parser("run", help="capture ONE cell; fresh mount and new output dir required")
    r.add_argument("suite", choices=SUITES)
    r.add_argument("workload", help="repo workload basename, without extension")
    r.add_argument("workers", choices=WORKERS + ("profile",))
    r.add_argument("--dedicated-guest", action="store_true", required=True,
                   help="assert that the operator verified the guest and scratch disk are exclusively dedicated")
    for opt in ("mount", "device", "out", "binary"):
        r.add_argument("--" + opt, type=Path, required=True)
    r.add_argument("--seed", type=int, required=True)
    r.add_argument("--fill-mib", type=int, required=True, help="real occupancy prefill, excluding workload")
    r.add_argument("--reserve-mib", type=int, required=True, help="minimum planning headroom")
    r.add_argument("--total-mib", type=int, help="fio total across all jobs, not per job")
    r.add_argument("--fb-size-bytes", type=int, help="Filebench file size or gamma mean (not file count)")
    r.add_argument("--records", type=int, help="YCSB load records of 10 x 4096 bytes")
    r.add_argument("--operations", type=int, help="YCSB measured logical run operations")
    a = p.parse_args(argv)
    try:
        verify_profiles(a.repo)
        if a.cmd == "matrix":
            sys.stdout.write(csv_text(matrix(a.repo)))
        elif a.cmd == "extract":
            sys.stdout.write(validated(a.sample, a.repo)[0])
        else:
            run(a)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        state, code = ("indeterminate", 3) if isinstance(exc, Indeterminate) else ("invalid", 2)
        print("%s: %s" % (state, exc), file=sys.stderr)
        return code
    return 0


if __name__ == "__main__":
    sys.exit(main())
