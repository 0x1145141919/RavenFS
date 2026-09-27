# SPDX-License-Identifier: GPL-2.0
#
# RavenFS Linux KO 构建入口（kbuild 外部模块）
#   独立构建:  make            （用运行内核 /lib/modules/$(uname -r)/build）
#   指定内核:  make KDIR=...  或  make KVER=6.12.0-arch1-1
#   guest 内构建由 vm/share/install-ravenfs-module.sh 走 `make -C $KDIR M=<src>/linux modules`
#
obj-m += ravenfs.o
ravenfs-y := super.o dentry.o dentry_write.o inode_cache.o mdata_cache.o cache_ttl.o bitmap.o name_heap.o iomap.o file.o extent_write.o xattr.o

# v1.2 盘面头在 ../include（C/C++ 双兼容，内部按 __KERNEL__ 选 linux/types.h）
ccflags-y := -I$(src)/../include

ifeq ($(KERNELRELEASE),)

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean
endif
