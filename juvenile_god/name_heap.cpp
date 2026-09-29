#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 负载片会话 */
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession：inode 访问统一走缓存 */
#include "../include/util/bitmap_base.hpp"            /* 位图视图轮子（移植自 kernel util/bitmap.h） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * name_heap.cpp — 名字堆（name heap）原语（disk_layout_v1.1.md §7）
 *
 * 职责：长名（>40B）外置部分的分配 / 释放 / 读写。
 *   - 每目录固定 10 个 name_heap_controller_t（raven_dir_Inode_t 内联区）
 *   - 档位级懒：未激活档位零开销；激活 = 位图面+数据面按容量一次分配
 *     （各自 block_alloc_hard 连续区间、2^1 对齐；位图面清零）
 *   - 位图 bit[i] 管辖数据区 [i×2^gran_log2, (i+1)×2^gran_log2)，
 *     物理位置 = data_base_block_ptr + i×2^gran_log2（数据面单一连续区间）
 *   - 分配 = 找 N 个连续空闲 bit 并置位；释放 = 连续区间清位
 *   - 位图面/数据面均为标准 8K 负载片，访问统一走 mdata_piece_caches
 *
 * 位图操作（分片内存载入）：alloc/free 时把位图按 8K 片载入栈缓冲
 * （≤32KB = 4 片），绑定 bitmap_base 视图后：
 *   找连续空闲 = 按 u64 字扫描（~0ULL 整字跳过 + ctz 枚举字内 0 位），
 *   置位/清位 = bitmap_base operator[]（带越界 boomer）。
 *
 * 分档表（尾部 = name_len − 40，≤512B）：
 *   粒度 8B  承接 1~40B   （≤5 单元）   槽 0..2（8k/16k/32k）
 *   粒度 64B 承接 41~64B  （1 单元）    槽 3..4（8k/16k）
 *   粒度 128B 承接 65~128B（1 单元）    槽 5..6（8k/16k）
 *   粒度 256B 承接 129~256B（1 单元）   槽 7..8（8k/16k）
 *   粒度 512B 承接 257~512B（1 单元）   槽 9（8k）
 * 同粒度多档 = 容量扩容关系（位图无连续空闲单元才激活下一档）。
 * ═══════════════════════════════════════════════════════════════════ */

namespace {

/* 10 个堆槽的静态几何（与 raven_dir_Inode_t 字段顺序一一对应） */
constexpr uint8_t kGranLog2[10] = { 3,3,3,  6,6,  7,7,  8,8,  9 };  /* 8B/64B/128B/256B/512B */
constexpr uint8_t kBitsLog2[10] = { 16,17,18, 16,17, 16,17, 16,17, 16 };  /* 8k/16k/32k 位图 */

/* 粒度组边界（槽号区间 [lo, hi)）：8B / 64B / 128B / 256B / 512B */
constexpr uint16_t kGroupSlotLo[5]  = { 0, 3, 5, 7, 9 };
constexpr uint16_t kGroupSlotHi[5]  = { 3, 5, 7, 9, 10 };

/* 尾部长度 → 粒度组 + 单元数 + 粒度 log2 */
uint16_t name_heap_pick(uint16_t tail_len, uint16_t *units, uint8_t *gran_log2)
{
    if (tail_len <= 40)  { *units = (uint16_t)((tail_len + 7) / 8); *gran_log2 = 3; return 0; }
    if (tail_len <= 64)  { *units = 1; *gran_log2 = 6; return 1; }
    if (tail_len <= 128) { *units = 1; *gran_log2 = 7; return 2; }
    if (tail_len <= 256) { *units = 1; *gran_log2 = 8; return 3; }
    *units = 1; *gran_log2 = 9; return 4;
}

/* 位图面块数 / 数据面块数（由两个 log2 推导，disk_layout_v1.1.md §7.4 容量表） */
inline uint64_t heap_bmp_blocks(uint8_t bits_log2)  { return 1ULL << (bits_log2 - 15); }  /* 2/4/8 */
inline uint64_t heap_data_blocks(uint8_t bits_log2, uint8_t gran_log2)
{ return 1ULL << (bits_log2 + gran_log2 - 12); }

/* 位图总 bit 数（= 数据面容量 / 粒度） */
inline uint64_t heap_total_bits(uint8_t bits_log2)  { return 1ULL << bits_log2; }

/* 每 8K 负载片 bit 数（8192B × 8） */
constexpr uint64_t BITS_PER_PIECE = 65536;

/* 在位图视图上找 [start, start+units) 全空闲的第一个 start。
 * 按 u64 字扫描：整字 ~0ULL 跳过；字内用 ctz 枚举 0 位做候选，
 * 每个候选检查连续 units（跨字候选由视图 operator[] 直接访问）。
 * 找不到 → UINT64_MAX。 */
uint64_t bitmap_find_free(bitmap_base &bmp, uint64_t total_bits, uint16_t units)
{
    uint64_t *w = bmp.get_base();
    const uint64_t words = total_bits / 64;
    for (uint64_t wi = 0; wi < words; wi++) {
        uint64_t avail = ~w[wi];              /* 字内空闲位 mask */
        while (avail) {
            const uint64_t z = (uint64_t)__builtin_ctzll(avail);
            const uint64_t cand = wi * 64 + z;
            if (cand + units <= total_bits) {
                bool ok = true;
                for (uint16_t u = 1; u < units; u++) {
                    if (bmp[cand + u]) { ok = false; break; }
                }
                if (ok) return cand;
            }
            avail &= avail - 1;               /* 清当前候选位，继续找下一个 0 位 */
        }
    }
    return UINT64_MAX;
}

}   /* namespace */

/* ═══════════════════════════════════════════════════════════════════
 * name_heap_activate — 激活堆槽：位图面 + 数据面按容量一次分配
 *
 * 分配失败回滚：数据面失败 → 释放已分配位图面；位图清零失败 → 全释放。
 * 控制器填充后**不落盘**——写回由调用方持有的 InodeSession dirty() 负责
 * （no_cache 脏写回 / offload 直改内核页缓存切片）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_heap_activate(raven_dir_Inode_t *dino, uint16_t slot, uint64_t dir_inode_idx)
{
    (void)dir_inode_idx;   /* 写回由调用方 session 负责，本函数只填充控制器 */
    if (!dino || slot >= 10) return RC_ERR_STATE;
    name_heap_controller_t *ctrl = (name_heap_controller_t *)&dino->heaps_8B_8k + slot;
    if (RFS_NHC_BITMAP(*ctrl) != NAME_HEAP_INACTIVE) return RC_OK;   /* 已激活 */

    const uint8_t gran_log2  = kGranLog2[slot];
    const uint8_t bits_log2  = kBitsLog2[slot];
    const uint64_t bmp_blks  = heap_bmp_blocks(bits_log2);
    const uint64_t data_blks = heap_data_blocks(bits_log2, gran_log2);

    /* 位图面 + 数据面：各自连续、2^1 对齐（8K 对齐，mdata_piece 先决条件） */
    uint64_t bmp_base = block_alloc_hard(bmp_blks, 1);
    if (bmp_base == PTR_INVALID) return RC_ERR_NO_BLOCK;
    uint64_t data_base = block_alloc_hard(data_blks, 1);
    if (data_base == PTR_INVALID) {
        block_free(bmp_base, bmp_blks);
        return RC_ERR_NO_BLOCK;
    }

    /* 位图面清零（alloc 返回的块内容未知；位图必须初始全 0 = 全空闲） */
    for (uint64_t p = 0; p < bmp_blks / 2; p++) {
        mdata_piece_session sess(mdata_piece_mgr, bmp_base + p * 2);
        if (sess.index_error() || !sess) {
            block_free(bmp_base, bmp_blks);
            block_free(data_base, data_blks);
            return sess.index_error() ? RC_ERR_STATE : RC_ERR_IO;
        }
        memset(sess.data, 0, 8192);
        sess.dirty();
    }

    RFS_NHC_SET_GRAN(*ctrl, gran_log2);
    RFS_NHC_SET_BITS(*ctrl, bits_log2);
    RFS_NHC_SET_BITMAP(*ctrl, bmp_base);
    RFS_NHC_SET_DATA(*ctrl, data_base);
    return RC_OK;   /* 调用方 session dirty 落盘 */
}

/* ═══════════════════════════════════════════════════════════════════
 * name_heap_alloc — 分配尾部存储（tail_len ∈ [1, 512]）
 *
 * 流程：读 dir inode（InodeSession）→ 按尾部长度定粒度组 → 组内槽位
 *   从低档起：未激活 → 激活（懒）；已激活 → 位图分片载入栈缓冲 →
 *   bitmap_base 视图 + 字扫描找连续空闲 → 置位 → 分片写回 →
 *   物理定位（data_base + bit_start×粒度）
 * 组内所有槽满 → RC_ERR_NO_BLOCK（512B 档仅 1 槽，满即耗尽）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_heap_alloc(uint64_t dir_inode_idx, uint16_t tail_len, name_heap_loc_t *out)
{
    if (!out || tail_len == 0 || tail_len > 512) return RC_ERR_STATE;

    InodeSession ino_sess(inode_cache_mgr, dir_inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;

    uint16_t units = 0;
    uint8_t  gran_log2 = 0;
    const uint16_t group = name_heap_pick(tail_len, &units, &gran_log2);
    const uint16_t slot_lo = kGroupSlotLo[group];
    const uint16_t slot_hi = kGroupSlotHi[group];

    for (uint16_t s = slot_lo; s < slot_hi; s++) {
        name_heap_controller_t *ctrl = (name_heap_controller_t *)&dino->heaps_8B_8k + s;

        if (RFS_NHC_BITMAP(*ctrl) == NAME_HEAP_INACTIVE) {
            /* 懒激活：失败（空间不足）直接返回——同粒度下一档容量更大，不可能成功 */
            result_code_t rc = name_heap_activate(dino, s, dir_inode_idx);
            if (rc != RC_OK) return rc;
            ino_sess.dirty();   /* 控制器落盘（no_cache 写回 / offload 直改窗口） */
            ctrl = (name_heap_controller_t *)&dino->heaps_8B_8k + s;   /* 同 session 内存，指针稳定 */
        }

        const uint64_t bits_log2 = RFS_NHC_BITS(*ctrl);
        const uint64_t total_bits = heap_total_bits((uint8_t)bits_log2);
        const uint64_t bmp_blks = heap_bmp_blocks((uint8_t)bits_log2);

        /* 分片载入位图到栈缓冲（≤32KB = 4 片），绑定 bitmap_base 视图 */
        alignas(uint64_t) uint8_t bmp[32768];
        for (uint64_t p = 0; p < bmp_blks / 2; p++) {
            mdata_piece_session sess(mdata_piece_mgr, RFS_NHC_BITMAP(*ctrl) + p * 2);
            if (sess.index_error()) return RC_ERR_STATE;
            if (!sess) return RC_ERR_IO;
            memcpy(bmp + p * 8192, sess.data, 8192);
        }
        bitmap_base view;
        view.enable((uint64_t *)bmp, total_bits);

        /* 字扫描找连续空闲；找不到 → 同粒度下一档 */
        const uint64_t bit_start = bitmap_find_free(view, total_bits, units);
        if (bit_start == UINT64_MAX) continue;

        /* 置位（≤5 bits，跨片最多 2 片） */
        for (uint16_t u = 0; u < units; u++) view[bit_start + u] = true;
        for (uint64_t p = 0; p < bmp_blks / 2; p++) {
            const uint64_t lo = p * BITS_PER_PIECE;
            const uint64_t hi = lo + BITS_PER_PIECE;
            if (bit_start < hi && bit_start + units > lo) {
                mdata_piece_session sess(mdata_piece_mgr, RFS_NHC_BITMAP(*ctrl) + p * 2);
                if (sess.index_error()) return RC_ERR_STATE;
                if (!sess) return RC_ERR_IO;
                memcpy(sess.data, bmp + p * 8192, 8192);
                sess.dirty();
            }
        }

        /* 物理定位：byte_off = bit_start × 粒度，连续落在数据面内 */
        const uint64_t byte_off = bit_start << gran_log2;
        out->fblk   = RFS_NHC_DATA(*ctrl) + (byte_off >> 12);
        out->offset = (uint16_t)(byte_off & 0xFFF);
        out->slot   = (uint8_t)s;
        return RC_OK;
    }
    return RC_ERR_NO_BLOCK;   /* 该粒度档全部耗尽 */
}

/* ═══════════════════════════════════════════════════════════════════
 * name_heap_free — 释放尾部存储（由 dentry 信息反推，无需额外字段）
 *
 * 定位：tail = name_len−40 → 粒度组；块号 ∈ 组内某堆数据区区间
 * （各堆数据区互不重叠，唯一命中）；bit_start = (物理 − data_base)/粒度。
 * 清位：分片载入 → bitmap_base 绑定 → 区间清位 → 写回。
 * 数据面不收缩、位图不回收（回收策略见 dir_name_heap_release）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_heap_free(uint64_t dir_inode_idx, uint16_t name_len, uint64_t full_char_remain_ptr)
{
    if (name_len <= DENTRY_INLINE_NAME_MAX) return RC_OK;
    if (full_char_remain_ptr == PTR_INVALID) return RC_ERR_STATE;

    const uint16_t tail = name_len - DENTRY_INLINE_NAME_MAX;
    const uint64_t blk = full_char_remain_ptr >> 12;
    const uint16_t offset = (uint16_t)(full_char_remain_ptr & 0xFFF);

    uint16_t units = 0;
    uint8_t  gran_log2 = 0;
    const uint16_t group = name_heap_pick(tail, &units, &gran_log2);
    const uint16_t slot_lo = kGroupSlotLo[group];
    const uint16_t slot_hi = kGroupSlotHi[group];

    InodeSession ino_sess(inode_cache_mgr, dir_inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;

    for (uint16_t s = slot_lo; s < slot_hi; s++) {
        name_heap_controller_t *ctrl = (name_heap_controller_t *)&dino->heaps_8B_8k + s;
        if (RFS_NHC_BITMAP(*ctrl) == NAME_HEAP_INACTIVE) continue;   /* 未激活 */

        const uint64_t data_blks = heap_data_blocks((uint8_t)RFS_NHC_BITS(*ctrl),
                                                    (uint8_t)RFS_NHC_GRAN(*ctrl));
        if (blk < RFS_NHC_DATA(*ctrl) ||
            blk >= RFS_NHC_DATA(*ctrl) + data_blks) continue;   /* 不在本堆数据区 */

        /* 命中：反推 bit 区间并清位（按片载入 → 视图 → 区间清位 → 写回） */
        const uint64_t byte_off = (blk - RFS_NHC_DATA(*ctrl)) * 4096 + offset;
        const uint64_t bit_start = byte_off >> RFS_NHC_GRAN(*ctrl);
        const uint64_t bits_log2 = RFS_NHC_BITS(*ctrl);
        const uint64_t bmp_blks = heap_bmp_blocks((uint8_t)bits_log2);

        for (uint64_t p = 0; p < bmp_blks / 2; p++) {
            const uint64_t lo = p * BITS_PER_PIECE;
            const uint64_t hi = lo + BITS_PER_PIECE;
            if (bit_start < hi && bit_start + units > lo) {
                mdata_piece_session sess(mdata_piece_mgr, RFS_NHC_BITMAP(*ctrl) + p * 2);
                if (sess.index_error()) return RC_ERR_STATE;
                if (!sess) return RC_ERR_IO;
                bitmap_base view;
                view.enable((uint64_t *)sess.data, BITS_PER_PIECE);
                for (uint16_t u = 0; u < units; u++) {
                    const uint64_t bit = bit_start + u;
                    if (bit >= lo && bit < hi) view[bit - lo] = false;
                }
                sess.dirty();
            }
        }
        return RC_OK;
    }
    return RC_ERR_STATE;   /* 块号不落在任何堆数据区：状态异常（调用方 bug） */
}

/* ═══════════════════════════════════════════════════════════════════
 * name_heap_read / name_heap_write — 尾部数据读写（8K 片两段拷贝）
 *
 * 尾部 ≤512B；起点在片内第二块高偏移时可能跨 8K 片（≤2 片），
 * 按两段拷贝处理：第一段 [in_piece, 8192)，第二段片起点 0 起。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_heap_read(uint16_t name_len, uint64_t full_char_remain_ptr, char *out_tail)
{
    if (name_len <= DENTRY_INLINE_NAME_MAX || !out_tail) return RC_ERR_STATE;
    const uint16_t tail = name_len - DENTRY_INLINE_NAME_MAX;
    const uint64_t blk = full_char_remain_ptr >> 12;
    const uint16_t offset = (uint16_t)(full_char_remain_ptr & 0xFFF);
    const uint64_t piece = blk & ~1ULL;
    const uint64_t in_piece = ((blk & 1ULL) << 12) + offset;

    mdata_piece_session sess(mdata_piece_mgr, piece);
    if (sess.index_error()) return RC_ERR_STATE;
    if (!sess) return RC_ERR_IO;

    if (in_piece + tail <= 8192) {
        memcpy(out_tail, sess.data + in_piece, tail);
    } else {
        const size_t first = (size_t)(8192 - in_piece);
        memcpy(out_tail, sess.data + in_piece, first);
        mdata_piece_session sess2(mdata_piece_mgr, piece + 2);
        if (sess2.index_error()) return RC_ERR_STATE;
        if (!sess2) return RC_ERR_IO;
        memcpy(out_tail + first, sess2.data, tail - first);
    }
    return RC_OK;
}

result_code_t RavenFS_juvenile::name_heap_write(uint16_t name_len, uint64_t full_char_remain_ptr, const char *tail)
{
    if (name_len <= DENTRY_INLINE_NAME_MAX || !tail) return RC_ERR_STATE;
    const uint16_t len = name_len - DENTRY_INLINE_NAME_MAX;
    const uint64_t blk = full_char_remain_ptr >> 12;
    const uint16_t offset = (uint16_t)(full_char_remain_ptr & 0xFFF);
    const uint64_t piece = blk & ~1ULL;
    const uint64_t in_piece = ((blk & 1ULL) << 12) + offset;

    mdata_piece_session sess(mdata_piece_mgr, piece);
    if (sess.index_error()) return RC_ERR_STATE;
    if (!sess) return RC_ERR_IO;

    if (in_piece + len <= 8192) {
        memcpy(sess.data + in_piece, tail, len);
        sess.dirty();
    } else {
        const size_t first = (size_t)(8192 - in_piece);
        memcpy(sess.data + in_piece, tail, first);
        sess.dirty();
        mdata_piece_session sess2(mdata_piece_mgr, piece + 2);
        if (sess2.index_error()) return RC_ERR_STATE;
        if (!sess2) return RC_ERR_IO;
        memcpy(sess2.data, tail + first, len - first);
        sess2.dirty();
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * dir_name_heap_release — 释放目录的全部已空名字堆（堆回收）
 *
 * 前置：外部持 logical_inode_mutex（单目录互斥；跨目录无共享）。
 * 遍历 10 个控制器：位图面无任何置位（全空）→ 位图面+数据面归还全局
 * bitmap → 控制器重置未激活哨兵；非空堆跳过（数据面不收缩）。
 * 有堆被释放 → 调用方 session dirty 落盘。释放后堆块内容作废（再分配即覆写）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_name_heap_release(uint64_t inode_idx)
{
    InodeSession ino_sess(inode_cache_mgr, inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;

    bool changed = false;
    for (uint16_t s = 0; s < 10; s++) {
        name_heap_controller_t *ctrl = (name_heap_controller_t *)&dino->heaps_8B_8k + s;
        if (RFS_NHC_BITMAP(*ctrl) == NAME_HEAP_INACTIVE) continue;   /* 未激活 */

        const uint8_t bits_log2 = (uint8_t)RFS_NHC_BITS(*ctrl);
        const uint8_t gran_log2 = (uint8_t)RFS_NHC_GRAN(*ctrl);
        const uint64_t bmp_blks  = heap_bmp_blocks(bits_log2);
        const uint64_t data_blks = heap_data_blocks(bits_log2, gran_log2);

        /* 全空判定：位图面全部字节 == 0（无任何分配单元） */
        bool empty = true;
        for (uint64_t p = 0; p < bmp_blks / 2 && empty; p++) {
            mdata_piece_session sess(mdata_piece_mgr, RFS_NHC_BITMAP(*ctrl) + p * 2);
            if (sess.index_error()) return RC_ERR_STATE;
            if (!sess) return RC_ERR_IO;
            const uint64_t *w = (const uint64_t *)sess.data;
            for (int i = 0; i < 1024 && empty; i++)
                if (w[i] != 0) empty = false;
        }
        if (!empty) continue;

        /* 归还 + 重置哨兵 */
        block_free(RFS_NHC_BITMAP(*ctrl), bmp_blks);
        block_free(RFS_NHC_DATA(*ctrl), data_blks);
        RFS_NHC_SET_BITMAP(*ctrl, NAME_HEAP_INACTIVE);
        RFS_NHC_SET_DATA(*ctrl, NAME_HEAP_INACTIVE);
        RFS_NHC_SET_GRAN(*ctrl, 0);
        RFS_NHC_SET_BITS(*ctrl, 0);
        changed = true;
    }
    if (changed) ino_sess.dirty();   /* 控制器重置落盘 */
    return RC_OK;
}
