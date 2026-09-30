// SPDX-License-Identifier: GPL-2.0
/*
 * version.c —— 模块版本元数据（构建期注入）
 *
 * 真值 = 打包版本（packaging/arch/PKGBUILD 的 pkgver）：
 *   · 宿主 dev 构建：Makefile 从 PKGBUILD 提取 pkgver 注入；
 *   · dkms 构建    ：dkms.conf 传 RAVENFS_VERSION=${PACKAGE_VERSION}。
 * 目的：`modinfo ravenfs -F version` 能自证版本，宿主 / guest 两边逐一对账。
 *
 * 单独成文件，避免动 super.c（其余 MODULE_* 元数据在那里）。
 */
#include <linux/module.h>

#ifndef RAVENFS_KO_VERSION
#define RAVENFS_KO_VERSION "dev"
#endif

MODULE_VERSION(RAVENFS_KO_VERSION);
