import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location("score", Path(__file__).with_name("parity-score.py"))
score = importlib.util.module_from_spec(spec)
spec.loader.exec_module(score)


class ParityScoreTests(unittest.TestCase):
    def test_median_boundaries_not_just_point_estimate(self):
        ci = score.median_ci
        self.assertEqual(score.verdict(ci([.95] * 6), ci([1.10] * 6)), "PASS")
        self.assertEqual(score.verdict(ci([.90] + [1.] * 5), ci([1.] * 6)), "INCONCLUSIVE")
        self.assertEqual(score.verdict(ci([1.] * 6), ci([1.11] * 6)), "FAIL")
        with self.assertRaisesRegex(ValueError, "six"):
            ci([1.] * 5)

    def test_missing_duplicate_and_invalid_observations(self):
        expected = {("fio", "rand-write", "1", "write")}
        with tempfile.TemporaryDirectory() as temp:
            d = Path(temp)
            path = d / "sample.csv"
            header = "suite,workload,workers,operation,throughput,p99_us\n"
            row = "fio,rand-write,1,write,128,64\n"
            for data in ("", row + row, row.replace("128", "nan")):
                path.write_text(header + data)
                with self.assertRaises(ValueError):
                    score.measurements(d, expected)
            path.write_text(header + row)
            self.assertEqual(score.measurements(d, expected)[next(iter(expected))], (128., 64.))

    def test_paired_matrix_and_build_identity(self):
        expected = {("fio", "rand-write", "1", "write")}
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for i in range(6):
                for k, version in enumerate(score.VERSIONS):
                    d = root / str(i) / version
                    d.mkdir(parents=True)
                    meta = {"kernel": version + "-delfs", "fs": "delfs", "calclock": "ON",
                            "accelerator": "kvm", "vcpus": 16, "verified": True,
                            "kernel_sha256": str(k)*64, "module_sha256": str(k)*64,
                            "common_sha256": "f"*64, "run_id": f"{i}-{k}"}
                    (d / "environment.json").write_text(json.dumps(meta))
                    (d / "sample.csv").write_text(
                        "suite,workload,workers,operation,throughput,p99_us\n"
                        "fio,rand-write,1,write,128,64\n")
            self.assertEqual(list(score.compare(root, expected, "ON"))[0][-1], "PASS")
            with self.assertRaisesRegex(ValueError, "incompatible"):
                list(score.compare(root, expected, "OFF"))
            meta["module_sha256"] = "a"*64
            (d / "environment.json").write_text(json.dumps(meta))
            with self.assertRaisesRegex(ValueError, "build changed"):
                list(score.compare(root, expected, "ON"))
