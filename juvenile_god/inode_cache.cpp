/* ═══════════════════════════════════════════════════════════════════
 * inode_cache.cpp — InodeSession 实现 + 共享位域轮子（v2.0）
 *
 * 声明见 include/ravenfs/inode_cache_radix.hpp。本文件保留两类内容：
 *   1. 共享轮子 cut_high / decode（inode 会话与 mdata_piece 缓存共用；
 *      声明在 cache_wheel.hpp，mdata_piece_cache.cpp 也在此链接消费）。
 *   2. InodeSession 私有静态：piece_blk_of（翻译取物理 8K 片基）/
 *      inode_slot_of（片内取 inode 槽，需 raven_Inode_t 完整定义）。
 *
 * v2.0 起 Inode_cache_mgr_t 的三后端自管缓存机械（FULL_RAM/CTRL_ARRAY/RADIX、
 * statey 策略表、ctrl_word 状态机）**整体退役**——inode 存储是全局弹性数据面，
 * 缓存统一走 mdata_piece_caches_mgr（8K 片粒度）。
 * ═══════════════════════════════════════════════════════════════════ */
#include "ravenfs/inode_cache_radix.hpp"
#include "disk_struct_v2.0.h"   /* raven_Inode_t 完整定义（inode_slot_of 取址用） */

/* ═══════════════════════════════════════════════════════════════════
 * cut_high — 位域切割原语（左对齐方案）
 *
 * raw 须已左对齐（位域最高位 = bit 63）；从位域顶端向下切 bits_to_cut 个 bit：
 *   cut_out_section = 切下的高位段（右对齐到低位）
 *   main            = 剩余位域（保持左对齐，供 decode 递归继续从 bit 63 切）
 * ═══════════════════════════════════════════════════════════════════ */
cut_result_t cut_high(uint64_t raw_metarial, uint8_t bits_to_cut) {
    cut_result_t r;
    if (bits_to_cut >= 64) {
        r.cut_out_section = raw_metarial;   /* 全切 */
        r.main            = 0;
    } else {
        r.cut_out_section = raw_metarial >> (64 - bits_to_cut);
        r.main            = raw_metarial << bits_to_cut;
    }
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * decode — 按 plan 逐层切分索引位域（inode 缓存 / mdata_piece 缓存共用）
 *
 * chunks[0..total_levels-1] = 各层索引（chunks[0] = mgr 层 slice 选择），
 * chunks[total_levels]      = 行内索引。
 * row_bits 由 plan 累积派生（bits_len - sum(level_bits)），plan 为唯一真相源；
 * raw 内部左对齐到位域顶端后递归 cut_high。约束：total_levels ≤ 6。
 * ═══════════════════════════════════════════════════════════════════ */
bitfield_decoded_result_t decode(uint64_t raw, bitfield_decode_plan_t plan) {
    bitfield_decoded_result_t r{};
    r.raw          = raw;
    r.total_levels = plan.total_levels;
    if (plan.total_levels > 6) r.total_levels = 6;   /* 防御：chunks[7] 容量上限 */

    uint64_t cur = raw << (64 - plan.bits_len);      /* 左对齐到位域顶端 */
    uint16_t cut_total = 0;
    for (uint8_t i = 0; i < r.total_levels; i++) {
        cut_result_t c = cut_high(cur, plan.level_bits[i]);
        r.chunks[i]    = (uint16_t)c.cut_out_section;
        cur            = c.main;
        cut_total     += plan.level_bits[i];
    }
    const uint8_t row_bits = plan.bits_len - cut_total;
    r.chunks[r.total_levels] = row_bits
        ? (uint16_t)raw & ((1ull << row_bits) - 1)
        : 0;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * InodeSession 私有静态
 * ═══════════════════════════════════════════════════════════════════ */

/* piece_blk_of — inum 经数据面翻译取物理 8K 片基（偶 FS_block）。
 * 输入非法 / 翻译失败 → ~0（奇，mdata_piece_session 判 index_error）。 */
uint64_t InodeSession::piece_blk_of(const inode_io_t *io, uint64_t inum) {
    if (io == nullptr || io->plane == nullptr) return ~0ULL;
    inode_location_t loc{};
    if (io->plane->inode_locate(inum, &loc) != RC_OK) return ~0ULL;
    return loc.blkbase;
}

/* inode_slot_of — 8K 片缓冲取第 idx 个 inode 槽（idx = inum & 31）。 */
raven_Inode_t *InodeSession::inode_slot_of(void *piece_data, uint64_t idx) {
    return &static_cast<raven_Inode_t *>(piece_data)[idx];
}
