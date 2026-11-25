"""Boot one fixed, private KVM guest. No TCG, host share, or disk path input."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def qmp(conn, command):
    conn.write((json.dumps({"execute": command}) + "\n").encode())
    conn.flush()
    while True:
        item = json.loads(conn.readline())
        if "return" in item:
            return item["return"]
        if "error" in item:
            raise RuntimeError(item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="parity-env-build.sh output")
    parser.add_argument("common", type=Path, help="parity-env-userspace.sh common.cpio.gz")
    parser.add_argument("mode", choices=["on", "off"])
    parser.add_argument("run", type=Path, help="fresh subdirectory under a trusted private parent")
    parser.add_argument("--job", type=Path, help="trusted bash job; default: non-timed functional check")
    parser.add_argument("--payload", type=Path, help="trusted uncompressed tar unpacked inside guest /")
    parser.add_argument("--disk-gib", type=int, default=8)
    parser.add_argument("--keep-images", action="store_true",
                        help="retain per-boot private data disk + initrd even after success")
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    os.umask(0o077)
    if os.geteuid() == 0 or not set(range(18)).issubset(os.sched_getaffinity(0)):
        parser.error("non-root user with access to CPUs 0-17 required")
    with os.fdopen(os.open("/dev/kvm", os.O_RDWR), "rb", buffering=0) as kvm:
        if fcntl.ioctl(kvm.fileno(), 0xAE00) != 12:
            parser.error("KVM API 12 required")
    run = args.run.absolute()
    if not 1 <= args.disk_gib <= 512 or shutil.disk_usage(run.parent).free < (args.disk_gib + 8) * 2**30:
        parser.error("need logical disk capacity + 8 GiB reserve on run filesystem")
    if args.timeout < 1:
        parser.error("positive timeout required")
    run.mkdir(mode=0o700)
    build = args.build.resolve(strict=True)
    common = args.common.resolve(strict=True)
    kernel = build / "obj/arch/x86/boot/bzImage"
    module = build / args.mode / "delfs.ko"
    release = build / "obj/include/config/kernel.release"
    init = Path(__file__).resolve().with_name("parity-env-init.sh")
    markers = {"expected-release": release.read_text(), "mode": args.mode}
    for name, value in markers.items():
        (run / name).write_text(value.strip() + "\n")
    files = {"/init": init, "/parity/delfs.ko": module}
    files.update({"/parity/" + name: run / name for name in markers})
    if args.job:
        files["/parity/job"] = args.job.resolve(strict=True)
    if args.payload:
        files["/parity/payload.tar"] = args.payload.resolve(strict=True)
    manifest = "nod /dev/console 0600 0 0 c 5 1\n"
    for dest, src in files.items():
        if not src.is_file() or any(c.isspace() or c == "$" for c in str(src)):
            parser.error("regular inputs at whitespace- and dollar-free paths required")
        manifest += f"file {dest} {src} {'0755' if dest == '/init' else '0644'} 0 0\n"
    (run / "archive.list").write_text(manifest)
    with (run / "boot.cpio").open("xb") as boot, common.open("rb") as base:
        shutil.copyfileobj(base, boot)
        boot.flush()
        # Each newc member begins at a 4-byte boundary.
        boot.write(b"\0" * (-boot.tell() % 4))
        boot.flush()
        subprocess.run([build / "obj/usr/gen_init_cpio", run / "archive.list"], stdout=boot, check=True)
    with (run / "data.raw").open("xb") as disk:
        disk.truncate(args.disk_gib * 2**30)
    command = [
        "/usr/bin/qemu-system-x86_64", "-nodefaults", "-no-user-config", "-no-reboot",
        "-machine", "pc-i440fx-4.2,accel=kvm", "-cpu", "host", "-smp", "16,sockets=1,cores=16,threads=1",
        "-m", "4096", "-display", "none", "-nic", "none", "-kernel", str(kernel), "-initrd", "boot.cpio",
        "-append", "console=ttyS0 rdinit=/init parity.private=1 preempt=full panic=-1 loglevel=4",
        "-serial", "file:serial.log", "-qmp", "unix:qmp.sock,server,nowait", "-S",
        "-object", "iothread,id=io0",
        "-drive", "file=data.raw,id=data,if=none,format=raw,cache=none,aio=native,discard=unmap",
        "-device", "virtio-blk-pci,drive=data,serial=PARITY-DATA,iothread=io0,num-queues=16",
        "-chardev", "file,id=results,path=results.tar.gz", "-device", "virtio-serial-pci",
        "-device", "virtserialport,chardev=results,name=parity.results",
    ]
    proof = {
        "uid": os.getuid(), "gid": os.getgid(), "argv": command,
        "qemu_version": subprocess.check_output([command[0], "--version"], text=True).splitlines()[0],
        "hashes": {str(f): digest(f) for f in [kernel, module, common, init, build / "obj/.config"]
                   + ([args.job] if args.job else []) + ([args.payload] if args.payload else [])},
        "disk_gib": args.disk_gib, "release": markers["expected-release"].strip(), "mode": args.mode,
    }
    with (run / "qemu.log").open("xb") as log, socket.socket(socket.AF_UNIX) as sock:
        # Child initially runs all threads on CPU 17; move vCPUs and I/O off
        # that CPU only after QMP provides their actual host thread IDs.
        proc = subprocess.Popen(command, cwd=run, stderr=log, stdout=log, stdin=subprocess.DEVNULL,
                                preexec_fn=lambda: os.sched_setaffinity(0, {17}))
        try:
            # Relative socket path: sockaddr_un.sun_path is limited to 108 bytes.
            os.chdir(run)
            deadline = time.monotonic() + 15
            while True:
                try:
                    sock.connect("qmp.sock")
                    break
                except (ConnectionRefusedError, FileNotFoundError):
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f"QMP startup failed; see {run}/qemu.log")
                    time.sleep(0.05)
            sock.settimeout(15)
            with sock.makefile("rwb") as conn:
                json.loads(conn.readline())["QMP"]
                qmp(conn, "qmp_capabilities")
                proof["kvm"] = qmp(conn, "query-kvm")
                if not proof["kvm"]["enabled"]:
                    raise RuntimeError("QMP reports acceleration is disabled")
                cpus = qmp(conn, "query-cpus-fast")
                io, = qmp(conn, "query-iothreads")
                if sorted(cpu["cpu-index"] for cpu in cpus) != list(range(16)) or io["id"] != "io0":
                    raise RuntimeError("unexpected CPU indices or I/O backend")
                for tid, core in [(c["thread-id"], c["cpu-index"]) for c in cpus] + [(io["thread-id"], 16)]:
                    os.sched_setaffinity(tid, {core})
                    if os.sched_getaffinity(tid) != {core}:
                        raise RuntimeError(f"pinning thread {tid} to {core} failed")
                proof.update(cpus=cpus, iothread=io, block=qmp(conn, "query-block"))
                (run / "vm.json").write_text(json.dumps(proof, indent=2) + "\n")
                qmp(conn, "cont")
            rc = proc.wait(timeout=args.timeout)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    serial = (run / "serial.log").read_bytes().replace(b"\r", b"").splitlines()
    if rc or b"PARITY_ENV_PASS" not in serial or any(b"PARITY_ENV_FAIL" in ln for ln in serial):
        raise SystemExit(f"guest failed (qemu={rc}): {run}/serial.log")
    subprocess.run(["gzip", "-t", str(run / "results.tar.gz")], check=True)
    if not args.keep_images:
        (run / "data.raw").unlink()
        (run / "boot.cpio").unlink()
    print(f"verified private KVM boot: {run}")


if __name__ == "__main__":
    main()
