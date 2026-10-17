/* ═══════════════════════════════════════════════════════════════════
 * mdata_piece_cache.cpp — mdata_piece_caches_mgr self_reliat 双档实现
 *
 * 声明见 include/ravenfs/mdata_piece_caches.hpp（本文件为唯一实现 TU）。
 *
 * 定位（2026-08-30 决策）：
 *   - 本模块不是通用块缓存，而是「8K 大小 + 8K 对齐」元数据负载片的专用缓存：
 *     负载 = 2 连续 FS_block（8192B），起始块号恒偶数（hard_alloc(2,1) 契约）。
 *   - 缓存行恒 = 1 个负载（8192B，blk_len 恒 2）。与 inode 缓存的关键差异：
 *     session 构造时 mgr_->blkidx_valid 已阻断尾部不足（blkidx+2 恒在覆盖区间内），
 *     因此 self_reliat 加载直接 read_block(blkidx, 2) —— 没有 inode 版
 *     「行 = 2^row_bits 单元 + 末行 row_blk_len 收窄」的复杂度。
 *
 * 双档（按 bits_len = ceil(log2(Piece_count)) 推导，单真相源）：
 *   - MDP_MODE_CTRL_ARRAY   bits ≤ 13 —— 控制器全量常驻，负载页懒加载不淘汰
 *   - MDP_MODE_RADIX_SHARD  bits > 13 —— statey 表（14~52）+ 多级静态基数树分片
 *     表已按 row_bits=0 调整：每档 sum(level_bits) = bits（无行内索引位），
 *     行内索引恒 0 → 叶表 ctrl 定位后直接返回整 8K 页，blk = base + piece_idx*2 = blkidx。
 *     （B 路线「行 = 2 负载」否决：一行涉及多个控制器 → 淘汰/写回可能同时锁多行，
 *      死锁风险不可忍；A 路线行 = 1 负载，控制器与缓存行一一对应。）
 *
 * 并发/淘汰：ctrl_word 单原子字（state/flags/accessor 打包）+ CAS 状态机与
 * inode 版共用（CTRL_STATE_MASK / CTRL_ACCESSOR_INC / CTRL_FLAG_DIRTY /
 * idle..evicting 见 inode_cache_radix.hpp）；TTL 淘汰绑 x86_64 Invariant TSC
 * （默认 2s，0 = 禁用）。RADIX 分片树：shared_mutex 读写锁 + 树只增不删 +
 * 读锁不跨会话（get 内建链窗口持锁，返回即释；嵌套 session 内层 miss 建链
 * 不自死锁 —— 2026-08-30 与 inode 版同款修复对齐）。
 * ═══════════════════════════════════════════════════════════════════ */
#include "ravenfs/mdata_piece_caches_v2.hpp"
#include "util/lock.h"   /* cpu_relax（忙等） / rdtsc_timestamp（TTL 时间戳） */

#include <cstring>   /* memset：中间表槽位清零 */
#include <new>       /* placement new：分片树 */
#include <cstdlib>   /* aligned_alloc/free：8K 对齐分配器 */
#include <mutex>     /* std::unique_lock：get 建链写锁（RAII） */

/* ═══════════════════════════════════════════════════════════════════
 * statey 位域策略表（RADIX 档 14~52，row_bits=0 版）
 *
 * 按 bits_len（= ceil(log2(Piece_count))）逐档固化 level_bits：
 * 逐层 cut_high 递归切割；行内引索位宽 row_bits = bits - sum(level_bits) 恒 0
 * —— 缓存行 = 1 负载（8192B），无行内索引位，叶表槽即负载控制器。
 * 边界不变量：行 I/O 恒 = 负载 2 块 —— blkidx_valid 已保证 blkidx+2 在覆盖
 * 区间内（无 inode 版末行收窄；脏写回同理）。
 * 命名 mdp_statey_XX 前缀：与 inode_cache.cpp 的 statey_XX 全局数组区分
 * （两 TU 各自链接，同名会多重定义）。
 * ═══════════════════════════════════════════════════════════════════ */
uint8_t mdp_statey_14[]={7,7};
uint8_t mdp_statey_15[]={8,7};
uint8_t mdp_statey_16[]={8,8};
uint8_t mdp_statey_17[]={9,8};
uint8_t mdp_statey_18[]={7,5,6};
uint8_t mdp_statey_19[]={7,6,6};
uint8_t mdp_statey_20[]={8,6,6};
uint8_t mdp_statey_21[]={8,6,7};
uint8_t mdp_statey_22[]={7,5,5,5};
uint8_t mdp_statey_23[]={7,5,5,6};
uint8_t mdp_statey_24[]={7,5,6,6};
uint8_t mdp_statey_25[]={7,6,6,6};
uint8_t mdp_statey_26[]={7,6,6,7};
uint8_t mdp_statey_27[]={8,6,6,7};
uint8_t mdp_statey_28[]={8,7,6,7};
uint8_t mdp_statey_29[]={8,6,7,8};
uint8_t mdp_statey_30[]={8,6,8,8};
uint8_t mdp_statey_31[]={8,7,8,8};
uint8_t mdp_statey_32[]={9,5,6,6,6};
uint8_t mdp_statey_33[]={9,6,6,6,6};
uint8_t mdp_statey_34[]={9,6,6,6,7};
uint8_t mdp_statey_35[]={9,6,6,7,7};
uint8_t mdp_statey_36[]={9,6,7,7,7};
uint8_t mdp_statey_37[]={9,7,7,7,7};
uint8_t mdp_statey_38[]={10,7,7,7,7};
uint8_t mdp_statey_39[]={10,7,7,7,8};
uint8_t mdp_statey_40[]={10,6,6,6,6,6};
uint8_t mdp_statey_41[]={10,6,6,6,6,7};
uint8_t mdp_statey_42[]={10,6,6,6,7,7};
uint8_t mdp_statey_43[]={10,6,6,7,7,7};
uint8_t mdp_statey_44[]={10,6,7,7,7,7};
uint8_t mdp_statey_45[]={10,7,7,7,7,7};
uint8_t mdp_statey_46[]={10,7,7,7,7,8};
uint8_t mdp_statey_47[]={10,8,7,7,7,8};
uint8_t mdp_statey_48[]={10,8,7,7,8,8};
uint8_t mdp_statey_49[]={10,8,7,8,8,8};
uint8_t mdp_statey_50[]={10,8,8,8,8,8};
uint8_t mdp_statey_51[]={10,8,8,8,8,9};
uint8_t mdp_statey_52[]={10,8,8,8,9,9};
