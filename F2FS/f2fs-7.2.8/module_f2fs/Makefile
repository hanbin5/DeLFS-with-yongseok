# SPDX-License-Identifier: GPL-2.0
# Vanilla F2FS from Linux 7.2.8 (fs/f2fs), built as an out-of-tree module.
#
#   make                     # build f2fs.ko for the running kernel
#   make KDIR=<kernel tree>  # build against a specific kernel build tree
#
# The kernel itself is configured with CONFIG_F2FS_FS=m, so its in-tree
# f2fs.ko has the same module name. Unload it (modprobe -r f2fs) before
# running "insmod f2fs.ko" from this directory.
obj-m += f2fs.o

f2fs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
f2fs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o
f2fs-y		+= shrinker.o extent_cache.o sysfs.o
f2fs-$(CONFIG_F2FS_STAT_FS) += debug.o
f2fs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
f2fs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
f2fs-$(CONFIG_FS_VERITY) += verity.o
f2fs-$(CONFIG_F2FS_FS_COMPRESSION) += compress.o
f2fs-$(CONFIG_F2FS_IOSTAT) += iostat.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
