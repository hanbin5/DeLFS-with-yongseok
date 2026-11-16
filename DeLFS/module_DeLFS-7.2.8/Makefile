# SPDX-License-Identifier: GPL-2.0
#
# DeLFS for Linux 7.2.8: out-of-tree module "delfs.ko".
# Base: Linux 7.2.8 fs/f2fs.  Optional objects follow the CONFIG_F2FS_*
# values of the kernel it is built against.
#
# Build against the patched kernel (DeLFS/linux-7.2.8), not a stock one:
# dflush needs the symbols that patch exports.
#   make KDIR=<7.2.8-delfs build dir>
obj-m += delfs.o

delfs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
delfs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o
delfs-y		+= shrinker.o extent_cache.o sysfs.o
delfs-y		+= oldinv.o	# DeLFS deLLC (B)
delfs-$(CONFIG_F2FS_STAT_FS) += debug.o
delfs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
delfs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
delfs-$(CONFIG_FS_VERITY) += verity.o
delfs-$(CONFIG_F2FS_FS_COMPRESSION) += compress.o
delfs-$(CONFIG_F2FS_IOSTAT) += iostat.o

# DeLFS dflush (see dflush.h)
delfs-y		+= mm/filemap.o mm/page-writeback.o fs/fs-writeback.o

# DeLFS calclock (see calc/README.md): off by default, as in 6.6.8 DeLFS.
#   make CALCLOCK=1   measure, and print the results to dmesg at rmmod
ifeq ($(CALCLOCK),1)
ccflags-y	+= -DCONFIG_CALCLOCK
delfs-y		+= calc/calclock.o
endif

KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
