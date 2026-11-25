"""Raw-capture fixture tests. No benchmark, device, mount, or root privilege needed."""
import contextlib
import csv
import importlib.util
import io
import json
import shutil
import unittest
import uuid
from pathlib import Path
from unittest.mock import patch


HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("parity_raw", HERE / "parity-run.py")
raw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(raw)


def fio_job(bins, ms, op="read"):
    n = sum(bins.values())
    return {"error": 0, op: {"io_bytes": n * 4096, "runtime": ms, "total_ios": n,
                             "drop_ios": 0, "short_ios": 0,
                             "clat_ns": {"percentile": {"99.000000": raw.quantile(bins)}, "bins": bins}},
            ("write" if op == "read" else "read"): {"io_bytes": 0}}


def ycsb_log(counts, logical, ms, workers=2):
    lines = ["DBWrapper: report latency for each error is false and specific error codes to track for latency are: []"]
    lines += ["PARITY-SEED-v1 5 %d" % i for i in range(workers)]
    lines += ["[OVERALL], RunTime(ms), %d" % ms, "[OVERALL], Throughput(ops/sec), %s" % (logical * 1000 / ms)]
    for op, n in counts.items():
        lines += ["[%s], Operations, %d" % (op, n), "[%s], 99thPercentileLatency(us), 57" % op,
                  "[%s], 57, %s.0" % (op, n)]
        if op != "READ-MODIFY-WRITE":
            lines.append("[%s], Return=OK, %d" % (op, n))
    return "\n".join(lines) + "\n"


def fb_log():
    # Each bucket is <0.8% wide, not a power-of-two interval nor a rounded ms value.
    return """PARITY-SEED-v1 5
1.000: Run took 60 seconds...
PARITY-HIST-v1 vidreader 1400 98
PARITY-HIST-v1 vidreader 1401 2
61.001: Per-Operation Breakdown
vidreader     100ops        2ops/s   0.0mb/s     1.0ms/op [0.01ms - 3.00ms]
serverlimit   800ops       13ops/s   0.0mb/s     2.0ms/op [0.01ms - 3.00ms]
61.002: IO Summary:   100 ops 1.666 ops/s 2/0 rd/wr 0.0mb/s 1.0ms/op
"""


class Metrics(unittest.TestCase):
    def test_fio_pools_bins_not_percentiles_and_uses_group_elapsed_time(self):
        jobs = [fio_job({1000: 256}, 2000), fio_job({1000: 253, 9000: 3}, 1000)]
        # Worker 2 p99 = 9us, but only 3 out of 512 IOs are slow; group p99 = 1us.
        rows, details = raw.fio_metrics(json.dumps({"jobs": jobs}), "read", "2", 2 * raw.MIB)
        self.assertEqual(rows, {"read": (1.0, 1.0)})
        self.assertEqual(details["iops"], 256)

    def test_fio_failed_job_missing_p99_partial_io_or_wrong_units_are_invalid(self):
        for mutation in ("failed", "p99", "truncated", "units", "short"):
            with self.subTest(mutation=mutation):
                j = fio_job({1000: 256}, 1000)
                if mutation == "failed":
                    j["error"] = 5
                elif mutation == "p99":
                    j["read"]["clat_ns"]["percentile"].clear()
                elif mutation == "truncated":
                    j["read"]["clat_ns"]["bins"][1000] -= 1
                elif mutation == "units":
                    j["read"]["clat_us"] = j["read"].pop("clat_ns")
                else:
                    j["read"]["short_ios"] = 1
                with self.assertRaises((ValueError, KeyError)):
                    raw.fio_metrics(json.dumps({"jobs": [j]}), "read", "1", raw.MIB)

    def test_filebench_p99_is_high_precision_from_named_io_only(self):
        rows, details = raw.filebench_metrics(fb_log(), ["vidreader"], 5)
        self.assertEqual(rows, {"vidreader": (1.666, 127.999)})
        self.assertEqual(details["p99_interval_ns"]["vidreader"], [127488, 127999])

    def test_filebench_distinguishes_coarse_from_broken_or_missing_collection(self):
        fine = fb_log()
        coarse = "\n".join(l for l in fine.splitlines() if not l.startswith("PARITY-HIST"))
        with self.assertRaises(raw.Indeterminate):
            raw.filebench_metrics(coarse + "\n[ " + "0 " * 63 + "100 ]", ["vidreader"], 5)
        for invalid in (coarse, fine.replace("1401 2", "1401 1"), fine.replace("1.666 ops/s", "0 ops/s"),
                        fine.replace("Run took 60", "Run took 4"), fine + "\nflowop vidreader failed"):
            with self.assertRaises(raw.Invalid) as err:
                raw.filebench_metrics(invalid, ["vidreader"], 5)
            self.assertNotIsInstance(err.exception, raw.Indeterminate)
        with self.assertRaises(raw.Indeterminate):
            raw.filebench_metrics(fine.replace("1401 2", "4095 2"), ["vidreader"], 5)

    def test_ycsb_f_nested_counts_hdr_and_run_return_status(self):
        ops = raw.YCSB["myworkloadf"]
        good = ycsb_log(dict(zip(ops, (100, 51, 51))), 100, 500)
        self.assertEqual(raw.ycsb_metrics(good, ops, 100, 5, "2")[0], {k: (200, 57) for k in ops})
        for bad in (good.replace("Return=OK, 51", "Return=ERROR, 51"),
                    good.replace("99thPercentileLatency(us)", "99thPercentileLatency(ms)"),
                    good.replace("Operations, 51", "Operations, 50"), good.replace(", 57, ", ", 57000, "),
                    good.replace("200.0", "201"), good.replace("PARITY-SEED-v1", "IGNORED")):
            with self.assertRaises((ValueError, KeyError)):
                raw.ycsb_metrics(bad, ops, 100, 5, "2")


class Sample(unittest.TestCase):
    def setUp(self):
        self.out = Path.cwd() / (".parity-fixture-" + uuid.uuid4().hex)
        self.out.mkdir()
        self.addCleanup(shutil.rmtree, self.out)
        self.m = dict(suite="ycsb", workload="myworkloadf", workers="2", seed=5,
                      total_mib=None, fb_size_bytes=None, records=400, operations=100,
                      fill_mib=0, reserve_mib=1, data="/mnt/test/parity-data")

    def test_matrix_has_no_raw_or_pacing_operations(self):
        matrix = list(raw.matrix(raw.REPO))
        self.assertEqual(len(matrix) - 1, len(set(matrix[1:])))
        self.assertEqual(len({row[:3] for row in matrix[1:]}), 85)
        by_oltp = {r[-1] for r in matrix if r[:3] == ("filebench", "myoltp", "profile")}
        self.assertEqual(by_oltp, {"lg-write", "lg-aiowait", "dbwrite-a", "dbwr-aiowait", "shadowread"})
        self.assertTrue(all(not row[1].startswith("raw") for row in matrix))
        self.assertNotIn("fsyncfile2", raw.operations("filebench", "myvarmail"))

    def test_effective_profiles_use_correct_fields_sizes_and_fixed_thread_roles(self):
        for work in raw.YCSB:
            m = dict(self.m, workload=work)
            text = raw.render(m, raw.profile_path(raw.REPO, "ycsb", work).read_text())["workload.properties"]
            self.assertIn("\nfieldlength=4096\n", text)
            self.assertIn("\nfieldcount=10\n", text)
            self.assertIn("\nhtrace.tracer.id=parity-offline\n", text)
            self.assertNotIn("fieldlenght", text)
        for work in raw.FIO:
            m = dict(self.m, suite="fio", workload=work, workers="16", total_mib=128)
            f = raw.render(m, raw.profile_path(raw.REPO, "fio", work).read_text())
            self.assertIn("size=8388608\n", f["run.fio"])
            self.assertEqual("prefill.fio" in f, work.endswith("-read"))
            if "prefill.fio" in f:
                self.assertIn("\nrw=write\nend_fsync=1\n", f["prefill.fio"])
        for work in raw.FILEBENCH:
            m = dict(self.m, suite="filebench", workload=work, workers="profile", fb_size_bytes=409600)
            src = raw.profile_path(raw.REPO, "filebench", work).read_text()
            text = raw.render(m, src)["run.f"]
            for line in src.splitlines():
                if "instances=" in line or "flowop " in line:
                    self.assertIn(line, text)

    def test_offline_extract_validates_load_then_run_never_substitutes_load_rate(self):
        raw.stage(self.out, self.m, raw.REPO)
        (self.out / "load.log").write_text(ycsb_log({"INSERT": 400}, 400, 500))
        (self.out / "run.log").write_text(ycsb_log({"READ": 100, "UPDATE": 48, "READ-MODIFY-WRITE": 48},
                                                  100, 4000))
        rc = {"returncode": 0}
        raw.save_json(self.out / "execution.json", {"load": rc, "run": rc})
        text, details = raw.validated(self.out, raw.REPO)
        self.assertEqual(text.splitlines()[0], "suite,workload,workers,operation,throughput,p99_us")
        self.assertEqual({r["throughput"] for r in csv.DictReader(io.StringIO(text))}, {"25.0"})
        self.assertEqual(details["load"]["logical_operations"], 400)
        raw.save_json(self.out / "execution.json", {"load": {"returncode": 1}, "run": rc})
        with contextlib.redirect_stdout(io.StringIO()) as stdout, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(raw.main(["extract", str(self.out)]), 2)
        self.assertFalse(stdout.getvalue())

    def test_run_refuses_ordinary_directory_even_with_explicit_opt_in(self):
        with patch.object(raw, "execute") as execute, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(raw.main(["run", "fio", "seq-read", "1", "--mount", str(self.out),
                                       "--dedicated-guest", "--device", str(self.out),
                                       "--out", str(self.out / "out"), "--binary", "/bin/false",
                                       "--seed", "5", "--fill-mib", "0", "--reserve-mib", "10",
                                       "--total-mib", "1"]), 2)
        execute.assert_not_called()
        self.assertEqual(list(self.out.iterdir()), [])

    def test_ycsb_seed_receipts_are_not_interleaved_with_stderr_and_errors_still_fail(self):
        raw.stage(self.out, self.m, raw.REPO)
        def fake_ycsb(cmd, **kw):
            self.assertIsNot(kw["stderr"], raw.subprocess.STDOUT)
            load = cmd[1] == "load"
            kw["stdout"].write(ycsb_log({"INSERT": 400} if load else
                                {"READ": 100, "UPDATE": 48, "READ-MODIFY-WRITE": 48},
                                400 if load else 100, 500))
            kw["stderr"].write("[Thread-2] INFO client - opening database\n")
            return raw.subprocess.CompletedProcess(cmd, 0)
        with patch.object(raw.subprocess, "run", side_effect=fake_ycsb):
            raw.execute(self.out, self.out, self.m, Path("/opt/bin/ycsb"))
        raw.validated(self.out, raw.REPO)
        (self.out / "run.stderr").write_text("[Thread-2] ERROR: write failed\n")
        with self.assertRaisesRegex(ValueError, "reported an error"):
            raw.validated(self.out, raw.REPO)

    def test_failed_prefill_stops_before_measurement_even_with_exit_zero(self):
        mount = self.out / "mock-mount"
        mount.mkdir()
        sample = self.out / "case"
        args = ["run", "fio", "seq-read", "1", "--mount", str(mount), "--dedicated-guest",
                "--device", "/dev/unused", "--out", str(sample), "--binary", "/bin/false",
                "--seed", "123", "--fill-mib", "0", "--reserve-mib", "1", "--total-mib", "1"]

        def fake_fio(cmd, **kw):
            self.assertTrue(cmd[-1].endswith("/prefill.fio"))
            job = fio_job({1000: 256}, 1000, "write")
            job["error"] = 28
            kw["stdout"].write(json.dumps({"jobs": [job]}))
            return raw.subprocess.CompletedProcess(cmd, 0)

        with patch.object(raw, "verified_mount", return_value="mock"), \
                patch.object(raw.subprocess, "run", side_effect=fake_fio) as launched, \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(raw.main(args), 2)
        self.assertEqual(launched.call_count, 1)
        self.assertEqual(raw.load_json(sample / "validation.json")["status"], "invalid")
        self.assertFalse((sample / "sample.csv").exists())


if __name__ == "__main__":
    unittest.main()
