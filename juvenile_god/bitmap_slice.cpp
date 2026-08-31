/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slice.cpp — 分片位图自治单元实现（FPA BCB 对齐）
 *
 * 本文件负责（Raven 分工）：
 *   bitmap_slice_t 全部成员函数（enable_slice / alloc_hard / alloc_soft
 *   / test_interval / free_interval / disable_slice / get_free_count）
 *
 * 锁纪律：本文件所有函数一律不加锁。临界区由外部用 slice_lock 编排：
 *   try_lock_write() → 操作 → unlock_write()。
 *
 * 坐标约定：所有 base / out_base_idx 均为全局位号（parent_descriptor
 * 视角），片内部换算：片内偏移 = 全局位号 − base_idx。
 *
 * 对齐语义：非末片 slice_bitscount 为 32768（2^15）的倍数 → 片起点
 * 天然 2^15 对齐；alloc_hard/alloc_soft 的 align_log2 按**片内 bit
 * 索引**对齐（调用方约束 ≤16，本实现防御截断）。
 *
 * 错误码约定：
 *   参数非法/越界       → RC_ERR_STATE
 *   片内无空闲          → RC_ERR_NO_BLOCK（片级通用；inode 组调用层
 *                         如需 RC_ERR_NO_INODE 自行映射）
 *   test_interval 不匹配 → RC_ERR_NOT_FOUND（区间非全同 val）
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/ravenfs_v1.1.hpp"

/* ═══════════════════════════════════════════════════════════════
 * 内部轮子（匿名 namespace，字粒度位操作）
 * ═══════════════════════════════════════════════════════════════ */
namespace {
constexpr uint64_t SLICE_NO_HIT = ~0ULL;

inline uint64_t slice_align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

inline bool bit_get(const uint64_t *words, uint64_t idx) {
    return (words[idx >> 6] >> (idx & 63)) & 1ULL;
}
inline void bit_set(uint64_t *words, uint64_t idx) {
    words[idx >> 6] |= 1ULL << (idx & 63);
}
inline void bit_clear(uint64_t *words, uint64_t idx) {
    words[idx >> 6] &= ~(1ULL << (idx & 63));
}

/* 找从 from 起的下一个空闲段 [*out_start, *out_end)（字粒度，整字
 * 占用直接跳过；末字按 nbits 截位）。无空闲段 → false。 */
bool next_free_seg(const uint64_t *words, uint64_t nbits, uint64_t from,
                   uint64_t max_extend,
                   uint64_t *out_start, uint64_t *out_end) {
    uint64_t nwords = (nbits + 63) >> 6;
    uint64_t w = from >> 6;
    if (w >= nwords) return false;

    uint64_t word = ~words[w];
    if (w == nwords - 1 && (nbits & 63)) word &= (1ULL << (nbits & 63)) - 1;
    word &= ~0ULL << (from & 63);
    if (word == 0) {
        for (w++; w < nwords; w++) {
            word = ~words[w];
            if (w == nwords - 1 && (nbits & 63)) word &= (1ULL << (nbits & 63)) - 1;
            if (word) break;
        }
        if (w >= nwords) return false;
    }
    uint64_t start = (w << 6) + __builtin_ctzll(word);

    /* 段尾延伸：找从 start 起第一个占用位（eword 1=空闲，0=占用）。
     * 字粒度：free_mask = 本字从 end 起的空闲位（低位对齐），全 0 →
     * 段结束；否则连续空闲长 = 第一个 0 位偏移 ctzll(~free_mask)
     * （free_mask 全 1 = 整字全空闲 → 64）；到字边界续下一字。
     * ⚠ max_extend>0：段长够 max_extend 即截断 —— 稀疏大位图（如
     * 122M bits inode 图）下 1-bit 分配若延伸到底，每次分配把整片
     * 剩余空间走完（15MB×N 次 = O(n²)，大魔王压测 99.55% CPU 热
     * 点即此）；调用方只需保证截断后的段尾不影响其判定（见调用处）。 */
    uint64_t end = start;
    for (;;) {
        if (end >= nbits) { end = nbits; break; }
        if (max_extend && (end - start) >= max_extend) break;   /* 够用即止 */
        uint64_t ew = end >> 6;
        uint64_t eword = ~words[ew];
        if (ew == nwords - 1 && (nbits & 63)) eword &= (1ULL << (nbits & 63)) - 1;
        uint64_t free_mask = eword >> (end & 63);
        if ((free_mask & 1) == 0) break;   /* end 位占用 → 段结束 */
        /* end 位空闲：连续空闲长 = 第一个 0 位偏移 */
        uint64_t run = (free_mask == ~0ULL) ? 64 : __builtin_ctzll(~free_mask);
        end += run;
        if (end & 63) break;          /* 停在本字内 → 段尾确定 */
        /* 恰好到字边界且本字剩余全空闲 → 下一字继续 */
    }
    if (end > nbits) end = nbits;
    *out_start = start;
    *out_end = end;
    return true;
}

/* 从 from 起找 count 连续空闲位、起始位 align 对齐（片内偏移）；
 * 命中 → 起始片内偏移，无 → SLICE_NO_HIT。段粒度：段内取对齐候选，
 * 段不足直接跳过段尾继续，避免 O(n×count)。 */
uint64_t find_free_run(const uint64_t *words, uint64_t nbits, uint64_t from,
                       uint64_t count, uint64_t align) {
    if (count == 0 || count > nbits) return SLICE_NO_HIT;
    /* 截断上界 count+align：候选起点 = align_up(seg_s,align) ≤ seg_s+align−1，
     * 段长 ≥ count+align 时必有候选可容纳 count —— 截断不丢有效候选，
     * 且避免稀疏大位图下每次 alloc_hard 把整片剩余空间走完（同 O(n²) 坑）。 */
    const uint64_t cap = count + align;
    uint64_t seg_s, seg_e;
    uint64_t probe = from;
    while (next_free_seg(words, nbits, probe, cap, &seg_s, &seg_e)) {
        uint64_t cand = slice_align_up(seg_s, align);
        if (cand < seg_e && cand + count <= seg_e) return cand;
        probe = seg_e;
    }
    return SLICE_NO_HIT;
}
}  // namespace

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slice_t 成员函数（全部不加锁，锁由外部编排）
 * ═══════════════════════════════════════════════════════════════ */

/* 绑定位图子段并扫片统计 free_count（O(片大小)，mount 时一次性）。
 * 校验：slice_base 须落在全局位图内、4K 对齐、片区间不越界。 */
result_code_t RavenFS_juvenile::bitmap_slice_t::enable_slice(
    fs_bitmap_descriptor_t *global_descriptor, slice_config_t config) {
    if (!global_descriptor || !config.slice_base) return RC_ERR_STATE;
    const uint8_t *g_base = reinterpret_cast<const uint8_t *>(global_descriptor->bitmap_raw);
    const uint8_t *s_base = config.slice_base;
    if (s_base < g_base) return RC_ERR_STATE;
    uint64_t byte_off = (uint64_t)(s_base - g_base);
    if (byte_off & 4095) return RC_ERR_STATE;              /* slice_base 须 4K 对齐 */
    uint64_t base = byte_off << 3;
    if (base >= global_descriptor->bits_count) return RC_ERR_STATE;      /* 片首越界 */
    if (config.slice_bitscount > global_descriptor->bits_count - base)
        return RC_ERR_STATE;                               /* 片尾越界 */

    parent_descriptor = global_descriptor;
    slice_base = config.slice_base;
    slice_bitscount = config.slice_bitscount;
    base_idx = base;
    last_finish_hint = 0;

    const uint64_t *words = reinterpret_cast<const uint64_t *>(slice_base);
    free_count = 0;
    for (uint64_t i = 0; i < slice_bitscount; i++)
        if (!bit_get(words, i)) free_count++;
    return RC_OK;
}

void RavenFS_juvenile::bitmap_slice_t::disable_slice() {
    slice_base = nullptr;
    base_idx = 0;
    slice_bitscount = 0;
    parent_descriptor = nullptr;
    free_count = 0;
    last_finish_hint = 0;
}

uint64_t RavenFS_juvenile::bitmap_slice_t::get_free_count() {
    return free_count;
}

/* 测试 hack：读/写 last_finish_hint（片内偏移）。set 越界忽略。 */
uint64_t RavenFS_juvenile::bitmap_slice_t::get_hint() {
    return last_finish_hint;
}

void RavenFS_juvenile::bitmap_slice_t::set_hint(uint64_t new_hint) {
    if (new_hint <= slice_bitscount) last_finish_hint = new_hint;
}

/* 硬分配：片内找 bitscount 连续空闲位，起始片内偏移 2^align_log2 对齐；
 * 从 last_finish_hint 起扫，段尾绕回 [0, hint) 再扫一次；不部分分配。 */
result_code_t RavenFS_juvenile::bitmap_slice_t::alloc_hard(
    uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx) {
    if (!slice_base || !parent_descriptor) return RC_ERR_STATE;
    if (bitscount == 0 || bitscount > slice_bitscount) return RC_ERR_STATE;
    uint64_t align = 1ULL << (align_log2 > 16 ? 16 : align_log2);   /* 防御截断 */
    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);

    uint64_t hit = find_free_run(words, slice_bitscount, last_finish_hint, bitscount, align);
    if (hit == SLICE_NO_HIT)
        hit = find_free_run(words, last_finish_hint, 0, bitscount, align);   /* 绕回 */
    if (hit == SLICE_NO_HIT) return RC_ERR_NO_BLOCK;

    for (uint64_t i = 0; i < bitscount; i++) bit_set(words, hit + i);
    free_count -= bitscount;
    last_finish_hint = hit + bitscount;
    out_base_idx = base_idx + hit;
    return RC_OK;
}

/* 软分配：第一个空闲段取对齐候选，实际 = min(段长, bitscount)；
 * 缺口回传 bits_left_to_be_allocated；无空闲 → RC_ERR_NO_BLOCK 且缺口=bitscount。 */
result_code_t RavenFS_juvenile::bitmap_slice_t::alloc_soft(
    uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx,
    uint64_t &bits_left_to_be_allocated) {
    bits_left_to_be_allocated = bitscount;
    if (!slice_base || !parent_descriptor) return RC_ERR_STATE;
    if (bitscount == 0) return RC_ERR_STATE;
    uint64_t align = 1ULL << (align_log2 > 16 ? 16 : align_log2);
    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
    /* 截断上界 max(bitscount, align)：候选 = align_up(seg_s,align) ≤ seg_s+align−1，
     * 段长 ≥ 上界时必有 ≥bitscount 空闲且候选在段内 —— 够用即止，
     * 避免稀疏大位图（122M bits inode 图）下 1-bit 分配每次走完整片
     * 剩余空间的 O(n²) 灾难。 */
    const uint64_t cap = bitscount > align ? bitscount : align;

    uint64_t seg_s, seg_e, hit = SLICE_NO_HIT;
    if (next_free_seg(words, slice_bitscount, last_finish_hint, cap, &seg_s, &seg_e)) {
        uint64_t cand = slice_align_up(seg_s, align);
        if (cand < seg_e) hit = cand;
    }
    if (hit == SLICE_NO_HIT) {                          /* 绕回 */
        if (next_free_seg(words, slice_bitscount, 0, cap, &seg_s, &seg_e)) {
            uint64_t cand = slice_align_up(seg_s, align);
            if (cand < seg_e) hit = cand;
        }
    }
    if (hit == SLICE_NO_HIT) return RC_ERR_NO_BLOCK;

    uint64_t got = seg_e - hit;
    if (got > bitscount) got = bitscount;
    for (uint64_t i = 0; i < got; i++) bit_set(words, hit + i);
    free_count -= got;
    last_finish_hint = hit + got;
    out_base_idx = base_idx + hit;
    bits_left_to_be_allocated = bitscount - got;
    return RC_OK;
}

/* 区间测试：parent_descriptor 视角 [base, base+len) 全同 val → RC_OK。
 * 越界 → RC_ERR_STATE；存在异位 → RC_ERR_NOT_FOUND。 */
result_code_t RavenFS_juvenile::bitmap_slice_t::test_interval(
    bool val, uint64_t base, uint64_t len) {
    if (!slice_base || !parent_descriptor) return RC_ERR_STATE;
    if (base < base_idx) return RC_ERR_STATE;
    uint64_t off = base - base_idx;
    if (off + len > slice_bitscount) return RC_ERR_STATE;
    if (len == 0) return RC_OK;
    const uint64_t *words = reinterpret_cast<const uint64_t *>(slice_base);
    for (uint64_t i = 0; i < len; i++)
        if (bit_get(words, off + i) != (val ? 1ULL : 0ULL)) return RC_ERR_NOT_FOUND;
    return RC_OK;
}

/* 区间释放：parent_descriptor 视角 [base, base+len) 清位，free_count
 * 按实际清掉的 1→0 位计数（重复释放不虚高）。 */
result_code_t RavenFS_juvenile::bitmap_slice_t::free_interval(
    uint64_t base, uint64_t len) {
    if (!slice_base || !parent_descriptor) return RC_ERR_STATE;
    if (base < base_idx) return RC_ERR_STATE;
    uint64_t off = base - base_idx;
    if (off + len > slice_bitscount) return RC_ERR_STATE;
    if (len == 0) return RC_OK;
    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
    uint64_t cleared = 0;
    for (uint64_t i = 0; i < len; i++) {
        if (bit_get(words, off + i)) { bit_clear(words, off + i); cleared++; }
    }
    free_count += cleared;
    return RC_OK;
}
