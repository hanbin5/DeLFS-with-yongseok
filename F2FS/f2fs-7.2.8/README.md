# F2FS 7.2.8 baseline

This directory is the F2FS 7.2.8 baseline for porting DeLFS from Linux 6.6.8
to Linux 7.2.8.

- `linux-7.2.8/`: Linux 7.2.8 kernel source, including `fs/f2fs/`.
- `module_f2fs/`: a copy of that kernel's `fs/f2fs/`, with an out-of-tree
  build Makefile and a local `.gitignore`. The C sources, headers, and Kconfig
  match the in-tree F2FS files.
- `config-arm64-vm`: the arm64 VM kernel configuration, using
  `CONFIG_LOCALVERSION="-f2fs"` and `CONFIG_F2FS_FS=m`.

The kernel version is recorded in `linux-7.2.8/Makefile`. The upstream stable
kernel repository is <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/>.
This snapshot preserves the local source tree; no independent comparison with
an upstream release has been performed.

Kernel licensing is described in [COPYING](linux-7.2.8/COPYING) and
[LICENSES](linux-7.2.8/LICENSES/). Build outputs are not versioned.

## Build and install the kernel

From the repository root:

```bash
cd F2FS/f2fs-7.2.8/linux-7.2.8
cp ../config-arm64-vm .config
make olddefconfig
make -j "$(nproc)"
sudo make modules_install INSTALL_MOD_STRIP=1
sudo make install
```

Reboot into the installed kernel and check `uname -r`; the expected release
is `7.2.8-f2fs`.

## Build and load the F2FS module

From the repository root, after booting the matching kernel:

```bash
cd F2FS/f2fs-7.2.8/module_f2fs
make -j "$(nproc)"
```

To select a different configured kernel build tree, use `make KDIR=<path>`.
The out-of-tree module and in-tree module both use the name `f2fs`.
Unmount any F2FS file systems before replacing the loaded module:

```bash
sudo modprobe -r f2fs
sudo modprobe -a lz4_compress lz4hc_compress
sudo insmod f2fs.ko
```

`insmod` does not automatically resolve module dependencies.
