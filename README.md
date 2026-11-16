# DeLFS: A Decentralized Log-Structured File System for Manycores

This repository contains the DeLFS source code, comparison file systems, formatting tools, and evaluation scripts.

## Contents

- [Repository structure](#repository-structure)
- [Team VM environment guide](env-guide/README.md)
- [Linux 7.2.8 Quick Start](#linux-728-quick-start)
- [Build and install DeLFS](#build-and-install-delfs)
- [Run the benchmarks](#run-the-benchmarks)

## Repository structure

```text
DeLFS/
├── DeLFS/
│   ├── linux-6.6.8/
│   ├── linux-7.2.8/
│   ├── module_DeLFS/
│   └── module_DeLFS-7.2.8/
├── DeLFS-CrashMonkey/
├── DeLFS-Git-Aging-Benchmark/
├── delfs-mkfs/
├── env-guide/
│   ├── README.md
│   └── scripts/
├── F2FSJ/
│   ├── f3fsj-6.6.8/
│   ├── f3fsj-6.6.8-SC/
│   └── f3fsj-6.6.8-SC-SF/
├── MAX/
├── ScaleCache/
│   ├── f2fs-6.6.8-SC/
│   └── f2fs-6.6.8-SC-SF/
├── ScaleLFS/
│   ├── ScaleLFS-6.6.8/
│   ├── ScaleLFS-6.6.8-SC/
│   └── ScaleLFS-6.6.8-SC-SF/
└── script/
    ├── filebench/
    ├── fio/
    └── RocksDB/
```

Each configuration under `F2FSJ/` contains `linux-6.6.8/` and `module_f3fsj/`. Each configuration under `ScaleCache/` and `ScaleLFS/` contains `linux-6.6.8/` and `module_f3fs/`.

The commands below start from the repository root unless stated otherwise.

For the team's development and test VM setup, including the scaled benchmarks for a 20 GB nvme-loop device, follow the [environment guide](env-guide/README.md). It uses the profiles and workloads in `script/` with the runners in `env-guide/scripts/`.

## Linux 7.2.8 Quick Start

1. **Build and install the patched 7.2.8 kernel** (then reboot into `7.2.8-delfs`):

```bash
cd DeLFS/linux-7.2.8
cp ../linux-6.6.8/oldconfig .config   # or /boot/config-$(uname -r)
scripts/config --set-str LOCALVERSION -delfs \
    --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
    --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT --disable DEBUG_INFO_DWARF4 \
    --disable DEBUG_INFO_DWARF5 --disable DEBUG_INFO_BTF --enable DEBUG_INFO_NONE
make olddefconfig
make -j "$(nproc)" && make -j "$(nproc)" modules
sudo make -j "$(nproc)" modules_install INSTALL_MOD_STRIP=1
sudo make -j "$(nproc)" install
cd ../..
```

2. **Build `mkfs.delfs` and format the target NVMe device** (run on the target machine so `mkfs.delfs` matches online CPU count):

```bash
cd delfs-mkfs && ./autogen.sh && ./configure && make -j "$(nproc)" && cd ..
cd DeLFS && ./mkfs.sh nvme0n1 && cd ..
```

3. **Build, load, and mount the 7.2.8 DeLFS module**:

```bash
cd DeLFS/module_DeLFS-7.2.8
make -j "$(nproc)"                    # add CALCLOCK=1 to enable calclock profiling

sudo rmmod f2fs 2>/dev/null || true
sudo modprobe -v lz4_compress lz4hc_compress jbd2
sudo insmod delfs.ko

sudo mkdir -p /mnt/test
sudo mount -t delfs /dev/nvme0n1 /mnt/test
cd ../..
```

Then run the benchmarks in [`script/`](#run-the-benchmarks) (or the 20 GB `nvme-loop` test scripts in [`env-guide/`](env-guide/README.md)).

## Build and install DeLFS

### 1. Build and install the kernel

```bash
cd DeLFS/linux-6.6.8
cp config-scalecache .config
make oldconfig

make -j "$(nproc)"
make -j "$(nproc)" modules
sudo make -j "$(nproc)" modules_install INSTALL_MOD_STRIP=1
sudo make -j "$(nproc)" install

cd ../..
```

Reboot into the installed kernel before building the DeLFS module. Check the running kernel with:

```bash
uname -r
```

### 2. Build the formatting tool

```bash
cd delfs-mkfs
./autogen.sh
./configure
make -j "$(nproc)"
cd ..
```

### 3. Format the SSD

DeLFS uses an Intel SSD in this setup. Identify the intended device before formatting it:

```bash
cd DeLFS
./check_model
```

Pass the device name **without `/dev/`** to `mkfs.sh`. For example, if `check_model` identifies the intended Intel SSD as `/dev/nvme0n1`:

```bash
./mkfs.sh nvme0n1
```

**Formatting destroys the existing data on the selected device. Verify the device name before running `mkfs.sh`.**

### 4. Build and load the DeLFS module

From `DeLFS/`:

```bash
cd module_DeLFS
make -j "$(nproc)"

sudo modprobe -v lz4_compress
sudo modprobe -v lz4hc_compress
sudo modprobe -v jbd2
sudo insmod delfs.ko
```

### 5. Mount DeLFS

The supplied Filebench and RocksDB workloads use `/mnt/test`. Mount the formatted SSD there, replacing `nvme0n1` with the device selected above:

```bash
sudo mkdir -p /mnt/test
sudo mount -t delfs /dev/nvme0n1 /mnt/test
findmnt --mountpoint /mnt/test

cd ../..
```

At this point, the working directory is the repository root.

## Run the benchmarks

### fio: microbenchmark

```bash
cd script/fio
sudo fio rand-write.fio
```

### Filebench: macrobenchmarks

The repository provides three Filebench profiles used for the fileserver, varmail, and videoserver workloads. All three profiles use `/mnt/test` and specify a 60-second run.

Install Filebench separately if it is not already available. On the evaluation machine, its executable is `/usr/local/bin/filebench`.

From the repository root:

```bash
cd script/filebench
findmnt --mountpoint /mnt/test

sudo /usr/local/bin/filebench -f myfileserver.f
sudo /usr/local/bin/filebench -f myvarmail.f
sudo /usr/local/bin/filebench -f myvideoserver.f
```

Run each profile as a separate experiment and prepare the file system in the same initial state when comparing results.

### RocksDB with YCSB: application benchmark

The RocksDB scripts store data under `/mnt/test/ycsb-rocksdb-data`. Mount DeLFS at `/mnt/test` before running them. Use `sudo` because the mounted directory requires root permission in this setup.

From the repository root:

```bash
cd script/RocksDB
findmnt --mountpoint /mnt/test

sudo ./load_rocksdb.sh
sudo ./run_rocksdb.sh
```

Run the load phase before the run phase. The paper evaluates YCSB workloads A, B, F, and update-only with 1–128 client threads, 10 million records, 10 fields of 4 KB per record, and 1.28 million operations per workload. Check the RocksDB scripts and their workload configuration files to select and reproduce each of these cases.
