#!/usr/bin/env python3
"""Host-only oracle tests: regular files and our own child processes only."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest
import uuid
import zlib

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("recovery", HERE / "parity-recovery.py")
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.d = Path.cwd() / ("parity-recovery-test-" + uuid.uuid4().hex)
        self.d.mkdir(mode=0o700)
        self.addCleanup(shutil.rmtree, self.d)

    def test_inode_names_size_and_contents_are_independent(self):
        helper = self.d / "ops"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(HERE / "parity-recovery-ops.c"),
                        "-o", str(helper)], env=dict(os.environ, TMPDIR=str(self.d)), check=True)

        def op(verb, want=0):
            p = subprocess.run([helper, verb, "cut"], cwd=self.d, capture_output=True)
            self.assertEqual(p.returncode, want, p.stderr)

        op("put")
        op("get")
        source = self.d / "cut/src/staging"
        source.touch()
        op("get", 1)
        source.unlink()
        src = source.parent
        src.rename(self.d / "substitute")
        src.symlink_to(self.d / "substitute", target_is_directory=True)
        op("get", 1)
        src.unlink()
        (self.d / "substitute").rename(src)
        dst = self.d / "cut/dst/published"
        data = dst.read_bytes()
        dst.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        op("get", 1)
        dst.write_bytes(data[:-1])
        op("get", 1)

    def test_offline_reader_rejects_torn_pack_and_corrupt_tail(self):
        img = self.d / "disk"
        # Small synthetic metadata image, not a filesystem mount.
        sb = bytearray(3072)
        for pos, val in [(0, r.MAGIC), (16, 12), (20, 9), (76, 2)]:
            struct.pack_into("<I", sb, pos, val)

        def hdr(version):
            h = bytearray(4 * 4096)
            for pos, val in [(0, version), (40, 9), (44, 4), (68, 16380)]:
                struct.pack_into("<I", h, pos, val)
            crc = zlib.crc32(h[:-4], r.MAGIC ^ 0xffffffff) ^ 0xffffffff
            struct.pack_into("<I", h, 16380, crc)
            return h

        with img.open("wb") as f:
            f.seek(1024)
            f.write(sb)
            for start, version in [(2, 99), (1026, 100)]:
                for block in (start, start + 5):
                    f.seek(block * 4096)
                    f.write(hdr(version))
        self.assertEqual(r.active(r.read_packs(img))["slot"], 1)
        tail = (1026 + 5) * 4096
        with img.open("r+b") as f:
            f.seek(tail)
            f.write(hdr(99))  # CRC valid but stale tail version
        with self.assertRaisesRegex(r.Mismatch, "torn"):
            r.read_packs(img)
        with img.open("r+b") as f:
            f.seek(tail + 12)
            f.write(b"\xff")
        with self.assertRaisesRegex(r.Mismatch, "CRC"):
            r.read_packs(img)

    def test_slot_and_space_oracles_do_not_accept_cosmetic_changes(self):
        base = dict(slot=0, version=12, flags=0, inodes=10, nodes=21, blocks=37, orphans=[])
        old = [base, dict(base, slot=1, version=11)]
        new = [base, dict(base, slot=1, version=13)]
        r.switched(old, new)
        with self.assertRaisesRegex(r.Mismatch, "opposite pack"):
            r.switched(old, [dict(base, version=14), old[1]])
        with self.assertRaisesRegex(r.Mismatch, "opposite pack"):
            r.switched(old, [dict(base, nodes=0), new[1]])
        live = dict(base, version=14, flags=2, inodes=11, nodes=22, blocks=54, orphans=[81])
        r.held(base, live, 81)
        with self.assertRaisesRegex(r.Mismatch, "held inode"):
            r.held(base, live, 82)
        clean = dict(base, version=15, flags=1)
        r.reclaimed(base, live, clean)
        with self.assertRaisesRegex(r.Mismatch, "did not reclaim"):
            r.reclaimed(base, live, dict(clean, blocks=54))

    def test_serial_ack_then_external_kill_only(self):
        res = dict(cuts=[], checks={})
        def child(msg):
            return [sys.executable, "-c", "import time; print(%r, flush=True); time.sleep(5)" % msg]
        r.boot(self.d, child("PR:nonce:ack:publish"), "nonce", "publish", 2, res)
        self.assertEqual(res["cuts"], ["publish"])
        with self.assertRaisesRegex(RuntimeError, "timeout"):
            r.boot(self.d, child("kernel cmdline PR:nonce:ack:restart"), "nonce", "restart", .2, res)
        self.assertEqual(res["cuts"], ["publish"])
        with self.assertRaises(r.Mismatch) as err:
            r.boot(self.d, child("PR:nonce:error:assert:checkpoint"), "nonce", "rotate1", 2, res)
        self.assertEqual(err.exception.cell, "checkpoint")
        self.assertEqual(r.compare("verified", "not_measured"), "not_compared")
        self.assertEqual(r.compare("verified", "failed"), "candidate_regression")


if __name__ == "__main__":
    unittest.main()
