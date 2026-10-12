#pragma once
/* ═══════════════════════════════════════════════════════════════════════════
 * [FORK/LEAKSCAN] 临时“发现一个泄露→修复一个泄露”胶水（--free-leaks）。
 *
 * 语义：收口（相位 C reconcile_block）判定某块「位图亮但不可达（疑似泄漏）」
 * 时，**就地清掉该块在磁盘块位图里的分配位** —— 把这块空间还给分配器。
 *
 * 为什么安全：泄漏块 = 已被分配但**没有任何元数据引用**它（shadow=0 ∧ disk=1
 * ∧ 非 backup SB ∧ < bitmap_start）。清位不会踩任何可达数据，只是回收空间。
 *
 * ⚠️ 临时件：这是审计器的**写**能力，违反 audit「只读」纪律（audit/DISCIPLINE.md）。
 *    仅供本次损坏研究回收空间使用；默认**关闭**，必须显式 `--free-leaks`。
 *    运行前必须保证该盘**未挂载**（否则与在飞写并发，危险）。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <cstdint>

namespace rfs_audit {

/* CLI --free-leaks 置位；默认 false。 */
extern bool     g_free_leaks;

/* 修复用的 O_RDWR fd（由 audit_entry 打开）；<0 = 未启用修复。 */
extern int      g_repair_fd;

/* 位图几何（修复定位用；由 audit_entry 从 SB 几何填入）。 */
extern uint64_t g_repair_bitmap_start;   /* bitmap 区首 FS_block */
extern uint64_t g_repair_total_blocks;

/* 修复计数（[AUDIT][SUM][LEAKREPAIR] 打印）。 */
extern uint64_t g_leaks_repaired;
extern uint64_t g_leaks_repair_failed;

/* 清掉 blk 的分配位（read-modify-write 一个位图字节）。成功/已是 0 返 true。 */
bool leak_repair_clear(uint64_t blk);

} /* namespace rfs_audit */
