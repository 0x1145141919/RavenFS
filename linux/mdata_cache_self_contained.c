// SPDX-License-Identifier: GPL-2.0
/*
 * mdata_cache_self_contained.c —— 兼容空 TU（无符号）
 *
 * 原「自包含」实现与同名兼容头均已删除；平台中立核心的唯一入口是
 * `include/linux/ravenfs_mdata_cache_core.h` / `linux/mdata_cache_core.c`。
 *
 * 保留本文件仅为一个目的：
 *   · 既有 CMake / 用户态验收命令若仍列出 mdata_cache_self_contained.o，
 *     编译不破（本 TU 不产生任何符号）。
 *
 * 不要在本文件重新实现缓存逻辑：平台中立核心只有 mdata_cache_core.c 一份。
 */
#include "ravenfs_mdata_cache_core.h"
