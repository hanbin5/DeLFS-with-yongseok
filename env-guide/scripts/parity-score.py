#!/usr/bin/env python3
"""Compare matched DeLFS measurements; no pooling across workloads or concurrency."""
import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path
from statistics import median


KEY = ["suite", "workload", "workers", "operation"]
METRICS = ["throughput", "p99_us"]
VERSIONS = ("6.6.8", "7.2.8")
FIO = ("rand-write seq-write start rand-read seq-read direct-rand-read "
       "direct-seq-read 25-write 50-write 75-write table6-90 table6-99").split()
FILEBENCH = "myfileserver myvarmail mywebserver myvideoserver myoltp".split()
YCSB = ["myworkload" + suffix for suffix in "abfu"]


def matrix():
    counts = ("1", "2", "4", "8", "16")
    return ({(suite, w, n) for suite, names in [("fio", FIO), ("ycsb", YCSB)]
             for w in names for n in counts}
            | {("filebench", w, "profile") for w in FILEBENCH})


def load_rows(path, header):
    with path.open(newline="") as f:
        rows = csv.DictReader(f)
        if rows.fieldnames != header:
            raise ValueError(f"{path}: expected header {','.join(header)}")
        for row in rows:
            if None in row or any(not x for x in row.values()):
                raise ValueError(f"{path}:{rows.line_num}: incomplete row")
            yield row


def operations(path):
    keys = [tuple(row[k] for k in KEY) for row in load_rows(path, KEY)]
    if len(set(keys)) != len(keys) or {k[:3] for k in keys} != matrix():
        raise ValueError(f"{path}: duplicate key or incomplete official workload matrix")
    return set(keys)


def measurements(directory, expected):
    samples = {}
    for path in sorted(directory.rglob("sample.csv")):
        for row in load_rows(path, KEY + METRICS):
            key = tuple(row[k] for k in KEY)
            values = tuple(float(row[k]) for k in METRICS)
            if key in samples or key not in expected or not all(
                    math.isfinite(x) and x > 0 for x in values):
                raise ValueError(f"{path}: duplicate/unexpected key or invalid value for {key}")
            samples[key] = values
    if set(samples) != expected:
        raise ValueError(f"{directory}: missing {len(expected - samples.keys())} operation(s)")
    return samples


def median_ci(values):
    """Distribution-free two-sided >=95% sign interval for the population median."""
    ordered = sorted(values)
    n = len(ordered)
    tail, rank = 0, 0
    for i in range((n + 1) // 2):
        tail += math.comb(n, i)
        if 40 * tail > 2 ** n:  # P(one tail) > .025
            break
        rank = i + 1
    if rank == 0:
        raise ValueError("need at least six independent pairs for a finite 95% median interval")
    return median(ordered), ordered[rank - 1], ordered[-rank]


def verdict(bw, latency):
    if bw[1] >= .95 and latency[2] <= 1.10:
        return "PASS"
    if bw[2] < .95 or latency[1] > 1.10:
        return "FAIL"
    return "INCONCLUSIVE"


def compare(root, expected, mode, common_sha256=None):
    dirs = sorted(p for p in root.iterdir() if p.is_dir())
    if len(dirs) < 6:
        raise ValueError("need at least six independent pair directories")
    seen, pairs, builds, common = set(), [], {}, common_sha256
    for pair in dirs:
        pairdata = []
        for version in VERSIONS:
            d = pair / version
            meta = json.loads((d / "environment.json").read_text())
            if not (meta["kernel"].startswith(version + "-") and meta["fs"] == "delfs"
                    and meta["accelerator"] == "kvm" and meta["vcpus"] == 16
                    and meta["calclock"] == mode and meta["verified"] is True):
                raise ValueError(f"{d}: unverified/incompatible qualification environment")
            for k in ("kernel_sha256", "module_sha256", "common_sha256"):
                if not re.fullmatch(r"[0-9a-f]{64}", meta[k]):
                    raise ValueError(f"{d}: invalid {k}")
            signature = (meta["kernel_sha256"], meta["module_sha256"])
            if builds.setdefault(version, signature) != signature:
                raise ValueError(f"{d}: build changed within series")
            if common is None:
                common = meta["common_sha256"]
            if common != meta["common_sha256"]:
                raise ValueError(f"{d}: shared experimental conditions changed")
            run = meta["run_id"]
            if not isinstance(run, str) or not run.strip() or run in seen:
                raise ValueError(f"{d}: duplicate or missing run identifier")
            seen.add(run)
            pairdata.append(measurements(d, expected))
        pairs.append(pairdata)
    for key in sorted(expected):
        bw, lat = [median_ci([b[key][i] / a[key][i] for a, b in pairs]) for i in range(2)]
        yield (*key, len(pairs), *bw, *lat, verdict(bw, lat))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("root", type=Path)
    p.add_argument("expected_operations", type=Path)
    p.add_argument("--mode", choices=["ON", "OFF"], required=True)
    args = p.parse_args()
    try:
        rows = list(compare(args.root, operations(args.expected_operations), args.mode))
    except (ValueError, OSError, KeyError, TypeError) as e:
        p.exit(2, f"invalid series: {e}\n")
    writer = csv.writer(sys.stdout)
    writer.writerow(KEY + ["pairs", "throughput_ratio", "throughput_lo", "throughput_hi",
                           "p99_ratio", "p99_lo", "p99_hi", "cell"])
    writer.writerows(rows)
    # This exit code covers measured performance only, not the recovery gate.
    return int(any(row[-1] != "PASS" for row in rows))


if __name__ == "__main__":
    sys.exit(main())
