#pragma once
#include <cstdint>
/* ═══════════════════════════════════════════════════════════════════════════
 * alloc_mode.hpp — 分配器初始化模式（轻量运行时头，零盘 / 零 STL 依赖）
 *
 * 为什么单独成头（而非塞进 fs_allocator.hpp）：
 *   上帝类的构造结构体 `linux_userspace_mount_t`（ravenfs_juvenile.hpp）要能携带
 *   本模式，而 ravenfs_juvenile.hpp 刻意**不** include fs_allocator.hpp —— 它对
 *   `fs_allocator_t` 只做前置声明，避免把 <set>/<mutex>/boost 等拉进
 *   `#define private public` 的测试 TU。故把纯枚举下沉到本独立头，两侧共同
 *   include（同 `cache_mgr_mode.hpp` 的做法）。
 *
 * 语义（slices_cfg_t.mode 的取值）：
 *   RUNTIME（默认）：运行时模仿——多分片 + preferred 亲和路由 + 片内 best-fit
 *                    （alloc_hard）；
 *   DEFRAG         ：碎片缓解——block 组**只装配单分片**（无 preferred 轮转 /
 *                    跨片），且 block_alloc_hard 落到片内 alloc_hard_longest_first
 *                    （最长优先 + 头部上挖），供离线碎片缓解运维工具使用。
 * ═══════════════════════════════════════════════════════════════════════════ */
enum slices_mode_t : uint8_t {
    SLICES_MODE_RUNTIME = 0,   /* 运行时模仿（默认） */
    SLICES_MODE_DEFRAG  = 1,   /* 碎片缓解（离线运维工具） */
};
