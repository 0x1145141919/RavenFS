# SPDX-License-Identifier: GPL-2.0
#
# RavenFS Linux KO 构建入口（kbuild 外部模块）
#   独立构建:  make            （用运行内核 /lib/modules/$(uname -r)/build）
#   指定内核:  make KDIR=...  或  make KVER=6.12.0-arch1-1
#   guest 内构建由 vm/share/install-ravenfs-module.sh 走 `make -C $KDIR M=<src>/linux modules`
#
obj-m += ravenfs.o
ravenfs-y := version.o super.o dentry.o dentry_tree.o dentry_node.o inode_cache.o mdata_cache.o cache_ttl.o bitmap_adapter.o name_heap.o iomap.o file.o extent_write.o xattr.o \
             bitmap_core.o bitmap_mem.o bitmap_chunk.o

# ── 版本：以打包版本为唯一真值（PKGBUILD pkgver）─────────────────────────────
#   宿主 dev 构建 ：从 PKGBUILD 提取 pkgver（PKGBUILD 已升格至 linux/ 本目录）；
#   dkms 构建     ：dkms.conf 传 RAVENFS_VERSION=${PACKAGE_VERSION}（命令行优先）；
#   两者都无（独立拷出的源）：退化为 "dev"。
#   结果经 -DRAVENFS_KO_VERSION 注入 version.c 的 MODULE_VERSION → `modinfo -F version`。
ifeq ($(origin RAVENFS_VERSION), undefined)
RAVENFS_VERSION := $(shell sed -n 's/^pkgver=//p' $(if $(src),$(src),$(CURDIR))/PKGBUILD 2>/dev/null | head -1)
endif
RAVENFS_VERSION := $(strip $(RAVENFS_VERSION))
ifeq ($(RAVENFS_VERSION),)
RAVENFS_VERSION := dev
endif

# 头文件集中化（2026-09-30）：ko 自有头全部移入 ../include/linux（与 god 的 ../include/ravenfs 并列）；
# v1.2 盘面头 / magics 仍在 ../include 根（C/C++ 双兼容，内部按 __KERNEL__ 选 linux/types.h）。
# 注意：-I../include 使 <linux/...> 会先探 ../include/linux → 该目录只准放 ravenfs*.h（勿放与内核同名的头，会遮蔽内核头）。
ccflags-y := -I$(src)/../include -I$(src)/../include/linux -DRAVENFS_KO_VERSION='"$(RAVENFS_VERSION)"'

ifeq ($(KERNELRELEASE),)

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	# 编译完顺手刷新 IDE compile db（失败不挡构建）
	-@python3 $(PWD)/../tools/ide/gen_compile_commands.py
	@echo "[ravenfs] built version = $(RAVENFS_VERSION)"

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean
endif
