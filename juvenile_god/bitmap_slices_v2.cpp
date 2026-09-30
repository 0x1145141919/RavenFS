/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slices_v2.cpp — 双位图分片分配器「窗口版」v2 slice 自治单元实现
 *
 * 2026-09-30 新建（与 include/ravenfs/bitmap_slices_v2.hpp 同日顶出）。
 * 两段：① chunk_slice_mgr_t（片自治单元，本文件主体）；② 文末追加的路由侧
 * bitmap_slices_mgr_t_v2（全局 ↔ 局部换算收口，同 TU，2026-09-30 第二刀）。
 *
 * 契约权威 = bitmap_slices_v2.hpp（坐标 / 窗口 / 返还 / 记账 / 锁 / 状态机
 * 逐条照抄，不改头语义）。窗口化规格 = Docs/runtime_tech/03_chunk_backed_slices.md
 * §3–§9（索引合成 / 换片状态机 / record / 2bit 摘要 / 窗口化 / 尾块）。
 * 位运算轮子移植自 juvenile_god/bitmap_slices.cpp（v1，2026-09-03）。
 * 窗口寻址参考 linux/bitmap.c CHUNK 段（2026-09-28 已跑通）。
 *
 * 与 ko 版的两处刻意差异（god 版身份，锁/坐标以本头为准）：
 *   ① 锁纪律：**一把 std::mutex slice_lock 覆盖全片**（含盘 I/O——可睡故允许，
 *      无 chg_mu / 池锁）。alloc 走 try_lock（拿不到 = 本片忙 → RC_ERR_BUSY，
 *      mgr 换片）；free / test / occupy / chunks_asyn / enable / shutdown 走
 *      阻塞 lock（定点 / 释放不可丢）。
 *   ② 坐标：**片内局部（0 起）单坐标系**；in_global_base_idx 只存不消费，
 *      全局 ↔ 局部换算留给上层 mgr。
 *
 * 实现期关键决定（2026-09-30，注明理由）：
 *   · win_alloc_hard/soft 的 from/to/out = **chunk 内局部**（头注释明文
 *     "from/to/out 均本 chunk 内偏移"）；片内全局换算（wlo + local）收在
 *     alloc_* 外壳一处。win_soft_occupy 的 wlo/whi/base 按参数命名与 ko
 *     同构 = **片内偏移**（窗界与锚点须同坐标系才能做窗口守卫，否则 wlo 恒 0
 *     失去意义）。两处坐标系差异为头文件字面要求，非擅改。
 *   · 归还链按 chunk 边界切段、前插 mgrs[ctl].rec_head（**在线与否一律挂链**，
 *     头文件 ★ 返还明文）；合并点 = 该窗被使用之前：在线窗 = 本次操作扫描到
 *     它时（drain_chunk），离线窗 = ensure_online 的 load 后 publish 前。
 *     挂链即 free_count += len；drain 只清位不动计数（★ 记账）。
 *   · 挂链节点 new(std::nothrow)：堆失败 → RC_ERR_STATE（本段既不挂账也不
 *     记账，已挂段保持自洽 —— 泄漏方向，与 §1.5 同构；v2 不搞节点池，
 *     头文件裁决"容量无限"）。
 *   · chunks_asyn 失败可重试：读 → 覆盖 → 清位（**节点暂不摘**）→ 写；
 *     写成功才释放节点 / 重算摘要 / 校准 free_count / 清脏；写失败记录仍在
 *     链上（且在线窗已保守置脏），重跑幂等。
 *   · shutdown 走同一轮落盘，**无论 I/O 成败都收资源并转 UNWORKABLE**
 *     （I/O 失败返 RC_ERR_IO；记录丢失 = 泄漏方向，fsck 可收）。
 *   · 对齐上界（2026-09-30 契约微调）：align_log2 **仅支持 0..15**，alloc_hard/
 *     alloc_soft 入口 >15 直接 RC_ERR_STATE（不静默截断）⇒ win_alloc_* 可直接
 *     1ULL<<align_log2。对齐基准 = 窗首，窗首恒 32768 倍数 ⇒ ≤15 的对齐与
 *     片内对齐完全等价（chunk 局部坐标无歧义）。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/bitmap_slices_v2.hpp"

#include <cstdlib>   /* posix_memalign / free */
#include <cstring>   /* memcpy / memset */
#include <new>       /* std::nothrow */

/* ═══════════════════════════════════════════════════════════════
 * 内部轮子（匿名 namespace；v1 bitmap_slices.cpp 移植 + 窗口化增补）
 *
 * 几何前提（头文件 §索引合成 / 03 文档 §3，全链路成立）：
 *   slice_bits 恒 32768 倍数、尾块恒 ≥1 整页 ⇒ 所有窗口位数恒 64 倍数，
 *   **无残字特判**（v1 里针对片尾 (nbits&63) 的掩码分支全链路不需要）。
 * ═══════════════════════════════════════════════════════════════ */
namespace {

constexpr uint64_t SEG_NO_HIT = ~0ULL;

/* win_* 私有返回码（int；头未枚举，本文件内部约定） */
constexpr int WIN_OK    =  0;   /* 认领 ≥1 位（soft_occupy） */
constexpr int WIN_BAD   = -1;   /* 参数非法（调用方转 RC_ERR_STATE） */
constexpr int WIN_EMPTY = -2;   /* 本窗无货（认领 0 位；≠ 片完） */

inline uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

inline bool bit_get(const uint64_t *words, uint64_t idx) {
    return (words[idx >> 6] >> (idx & 63)) & 1ULL;
}
inline void bit_set(uint64_t *words, uint64_t idx) {
    words[idx >> 6] |= 1ULL << (idx & 63);
}

/* 置 1 计数：nbits 恒 64 倍数 ⇒ 整字 popcount，零残字分支 */
uint64_t count_ones(const uint64_t *words, uint64_t nbits) {
    uint64_t ones = 0;
    const uint64_t nwords = nbits >> 6;
    for (uint64_t w = 0; w < nwords; w++) ones += (uint64_t)__builtin_popcountll(words[w]);
    return ones;
}
uint64_t count_zeros(const uint64_t *words, uint64_t nbits) {
    return nbits - count_ones(words, nbits);
}

/* ── v1 移植：找从 from 起的下一个空闲段 [*out_start, *out_end)（字粒度，
 * 整字占用跳过；末字按 nbits 截位）。无 → false。 ── */
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
    uint64_t start = (w << 6) + (uint64_t)__builtin_ctzll(word);

    /* 段尾延伸：够 max_extend 即截断（防稀疏大位图 O(n²) 热点，v1 注记保留） */
    uint64_t end = start;
    for (;;) {
        if (end >= nbits) { end = nbits; break; }
        if (max_extend && (end - start) >= max_extend) break;
        uint64_t ew = end >> 6;
        uint64_t eword = ~words[ew];
        if (ew == nwords - 1 && (nbits & 63)) eword &= (1ULL << (nbits & 63)) - 1;
        uint64_t free_mask = eword >> (end & 63);
        if ((free_mask & 1) == 0) break;                 /* end 位占用 → 段结束 */
        uint64_t run = (free_mask == ~0ULL) ? 64 : (uint64_t)__builtin_ctzll(~free_mask);
        end += run;
        if (end & 63) break;
    }
    if (end > nbits) end = nbits;
    *out_start = start;
    *out_end = end;
    return true;
}

/* ── v1 移植：从 from 起找 count 连续空闲位、起始位 align 对齐；段粒度取
 * 对齐候选（段不足跳段尾），避免 O(n×count)。无 → SEG_NO_HIT。 ── */
uint64_t find_free_run(const uint64_t *words, uint64_t nbits, uint64_t from,
                       uint64_t count, uint64_t align) {
    if (count == 0 || count > nbits) return SEG_NO_HIT;
    const uint64_t cap = count + align;
    uint64_t seg_s, seg_e;
    uint64_t probe = from;
    while (next_free_seg(words, nbits, probe, cap, &seg_s, &seg_e)) {
        uint64_t cand = align_up(seg_s, align);
        if (cand < seg_e && cand + count <= seg_e) return cand;
        probe = seg_e;
    }
    return SEG_NO_HIT;
}

/* ── v1 soft_occupy 镜像：从 off 起的连续空闲前缀长 k（≤ cap；撞占用位停）。
 * 窗口位数恒 64 倍数且 off+cap ≤ nbits ⇒ 读字下标恒在窗内，无越界掩码。 ── */
uint64_t greedy_free_prefix(const uint64_t *words, uint64_t off, uint64_t cap) {
    uint64_t k = 0;
    while (k < cap) {
        const uint64_t pos = (off + k) & 63;
        const uint64_t free_mask = (~words[(off + k) >> 6]) >> pos;  /* 1 = 空闲 */
        if ((free_mask & 1) == 0) break;                /* 当前位占用 → 前缀断 */
        uint64_t run = (free_mask == ~0ULL) ? (64 - pos)
                                            : (uint64_t)__builtin_ctzll(~free_mask);
        k += run;
        if (k > cap) k = cap;
    }
    return k;
}

/* ── 2bit 摘要重算（03 §7）：链上有记录 → 恒 MIXED（防"盘态 ALL_USED 其实
 * 有待还空闲"被免载入跳过 → 错误 ENOSPC，§10.5）；否则按 src（下标 = 窗首起）
 * 统计全 1 / 全 0 / 混合。同步 HAS_RECS 位与链实况一致。 ── */
void summarize_window(chunk_mgr &m, const void *src, uint64_t nbits) {
    uint64_t sum;
    if (m.rec_head) {
        sum = CHUNK_SUM_MIXED;
    } else {
        const uint64_t ones = count_ones(reinterpret_cast<const uint64_t *>(src), nbits);
        sum = (ones == nbits) ? CHUNK_SUM_ALL_USED
            : (ones == 0)     ? CHUNK_SUM_ALL_FREE
                              : CHUNK_SUM_MIXED;
    }
    uint64_t cw = m.compact_word & ~(CHUNK_SUMMARY_MASK | CHUNK_FLAG_HAS_RECS);
    cw |= sum;
    if (m.rec_head) cw |= CHUNK_FLAG_HAS_RECS;
    m.compact_word = cw;
}

/* ── 归还链清位（**不摘节点**）：words 以 idx_base 定位 —— 整片暂存缓冲传 0
 * （片内坐标直接当字下标），在线窗缓冲传 wlo（chunk 内局部 = 片内 − 窗首）。
 * 节点摘除由 free_recs 负责（asyn/shutdown 先清位、写回成功才摘账，见文件顶决定）。 ── */
void apply_recs(const rec_seg_t *head, uint64_t *words, uint64_t idx_base) {
    for (const rec_seg_t *n = head; n; n = n->next) {
        for (uint64_t i = 0; i < n->len; i++) {
            const uint64_t idx = n->off + i - idx_base;
            words[idx >> 6] &= ~(1ULL << (idx & 63));
        }
    }
}

/* ── 摘链 + 释放全部节点 + 同步 HAS_RECS（链操作全在 slice_lock 内） ── */
void free_recs(chunk_mgr &m) {
    rec_seg_t *p = m.rec_head;
    while (p) {
        rec_seg_t *nx = p->next;
        delete p;
        p = nx;
    }
    m.rec_head = nullptr;
    m.compact_word &= ~CHUNK_FLAG_HAS_RECS;
}

/* ── 4K 对齐堆分配（buffer / 临时缓冲共用；用户态 host libc 分配器） ── */
void *alloc_aligned_4k(size_t bytes) {
    if (bytes == 0) return nullptr;
    void *p = nullptr;
    if (posix_memalign(&p, 4096, bytes) != 0) return nullptr;
    return p;
}

/* ── 收资源（enable 失败回滚 / shutdown 共用）：全片链节点 + mgrs[] + buffer ── */
void release_resources(chunk_mgr *mgrs, uint32_t nwin, uint8_t *buf) {
    if (mgrs) {
        for (uint32_t i = 0; i < nwin; i++) free_recs(mgrs[i]);
        delete[] mgrs;
    }
    if (buf) free(buf);
}

}  // namespace

/* ═══════════════════════════════════════════════════════════════
 * 读数 / 游标（无门闸语义：门闸只管服务调用；shutdown 后字段已归零）
 * ═══════════════════════════════════════════════════════════════ */

uint64_t chunk_slice_mgr_t::get_free_count() {
    std::lock_guard<std::mutex> lk(slice_lock);
    return free_count;
}

uint64_t chunk_slice_mgr_t::get_hint() {
    std::lock_guard<std::mutex> lk(slice_lock);
    return in_slice_hint;
}

/* 显式设游标（mgr 可用；alloc 自身的推进见 alloc_*）。越界忽略 = v1 口径；
 * "只进不退"是 alloc 的自动推进纪律，显式 setter 允许回拨（绕回/换片用）。 */
void chunk_slice_mgr_t::set_hint(uint64_t new_hint) {
    std::lock_guard<std::mutex> lk(slice_lock);
    if (new_hint <= slice_bits_) in_slice_hint = new_hint;
}

/* ═══════════════════════════════════════════════════════════════
 * alloc_hard — 硬分配（**仅 WORKABLE**；片内 try_lock；窗口迭代两趟）
 *
 * 窗口序列（03 §8）：hint 窗起正向到片尾 → 绕回 [0, hint)。尾窗用 limit_of()
 * 夹断；每窗 = 快/慢路径（在线先 drain / 离线摘要裁决 → ensure_online）+
 * 单窗 win_alloc_hard（chunk 局部坐标）。失败不重试（一次操作扫描期间锁内
 * 互斥、不会有新记录救场——头文件 ★ 返还）。
 * 对齐：align_log2 仅 0..15（头契约），入口 >15 → RC_ERR_STATE（不截断）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::alloc_hard(uint8_t align_log2, uint64_t bitscount,
                                            uint64_t &out_slice_base_idx) {
    /* 无锁预检：非 WORKABLE 直接 STATE（免得忙锁把 STATE 说成 BUSY）；
     * 锁内复检防 enable/shutdown 竞态（状态迁移在 slice_lock 内）。 */
    if (!is_workable()) return RC_ERR_STATE;
    if (align_log2 > 15) return RC_ERR_STATE;   /* 对齐仅 0..15：越界拒，勿静默截断 */
    std::unique_lock<std::mutex> lk(slice_lock, std::try_to_lock);
    if (!lk.owns_lock()) return RC_ERR_BUSY;          /* 本片忙 → mgr 换片 */
    if (!is_workable()) return RC_ERR_STATE;
    if (!mgrs || !active_chunk_buffer) return RC_ERR_STATE;
    if (bitscount == 0 || bitscount > slice_bits_) return RC_ERR_STATE;
    if (bitscount > free_count) return RC_ERR_NO_BLOCK;   /* free_count gate 保真 */

    const uint32_t sh = 15 + static_properties.chunk_order;
    const uint64_t hint = in_slice_hint;
    const uint32_t hctl = (hint >= slice_bits_) ? chunks_per_slice_ - 1u
                                                : (uint32_t)(hint >> sh);

    for (int pass = 0; pass < 2; pass++) {
        const uint32_t c_begin = (pass == 0) ? hctl : 0;
        const uint32_t c_end   = (pass == 0) ? chunks_per_slice_ : hctl + 1u;  /* 绕回 [0, hctl] */
        for (uint32_t c = c_begin; c < c_end; c++) {
            const uint64_t wlo = (uint64_t)c << sh;
            const uint64_t whi = wlo + limit_of(c);       /* 尾窗 limit_of() 夹断 */
            uint64_t from = wlo, to = whi;
            if (pass == 0 && c == hctl && hint > wlo) from = hint;
            if (pass == 1 && c == hctl) to = (hint < whi) ? hint : whi;
            if (from >= to) continue;

            if (online_idx_ == c) {
                if (mgrs[c].rec_head) drain_chunk(c);     /* 已在线窗：操作开启时先还账 */
            } else {
                const uint64_t cw = mgrs[c].compact_word;
                /* 摘要裁决（仅离线窗可信；在线窗摘要可因在线分配变陈旧）：
                 * ALL_USED 且无待还记录 → 免载入跳窗，不触发换片 */
                if (!mgrs[c].rec_head && (cw & CHUNK_SUMMARY_MASK) == CHUNK_SUM_ALL_USED)
                    continue;
                if (ensure_online(c) != 0) return RC_ERR_IO;
            }
            uint64_t hit_c = 0;                          /* chunk 内局部 */
            if (!win_alloc_hard(from - wlo, to - wlo, align_log2, bitscount, &hit_c))
                continue;
            mgrs[c].compact_word |= CHUNK_FLAG_DIRTY;    /* 置位 = 变更（丢 = 双分配） */
            const uint64_t hit = wlo + hit_c;            /* → 片内坐标 */
            in_slice_hint = hit + bitscount;
            out_slice_base_idx = hit;
            return RC_OK;
        }
    }
    return RC_ERR_NO_BLOCK;                              /* 全片扫毕（含绕回） */
}

/* ═══════════════════════════════════════════════════════════════
 * alloc_soft — 软分配（**仅 WORKABLE**；try_lock；首空闲段取对齐候选，
 * 实际 = min(段长, bitscount)，缺口回传；窗口迭代同 alloc_hard 两趟）。
 * 对齐：align_log2 仅 0..15（头契约），入口 >15 → RC_ERR_STATE（不截断）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::alloc_soft(uint8_t align_log2, uint64_t bitscount,
                                            uint64_t &out_slice_base_idx,
                                            uint64_t &bits_left_to_be_allocated) {
    bits_left_to_be_allocated = bitscount;               /* 缺口预置（v1 口径） */
    if (!is_workable()) return RC_ERR_STATE;
    if (align_log2 > 15) return RC_ERR_STATE;   /* 对齐仅 0..15：越界拒，勿静默截断 */
    std::unique_lock<std::mutex> lk(slice_lock, std::try_to_lock);
    if (!lk.owns_lock()) return RC_ERR_BUSY;
    if (!is_workable()) return RC_ERR_STATE;
    if (!mgrs || !active_chunk_buffer) return RC_ERR_STATE;
    if (bitscount == 0) return RC_ERR_STATE;
    if (free_count == 0) return RC_ERR_NO_BLOCK;

    const uint32_t sh = 15 + static_properties.chunk_order;
    const uint64_t hint = in_slice_hint;
    const uint32_t hctl = (hint >= slice_bits_) ? chunks_per_slice_ - 1u
                                                : (uint32_t)(hint >> sh);

    for (int pass = 0; pass < 2; pass++) {
        const uint32_t c_begin = (pass == 0) ? hctl : 0;
        const uint32_t c_end   = (pass == 0) ? chunks_per_slice_ : hctl + 1u;
        for (uint32_t c = c_begin; c < c_end; c++) {
            const uint64_t wlo = (uint64_t)c << sh;
            const uint64_t whi = wlo + limit_of(c);
            uint64_t from = wlo, to = whi;
            if (pass == 0 && c == hctl && hint > wlo) from = hint;
            if (pass == 1 && c == hctl) to = (hint < whi) ? hint : whi;
            if (from >= to) continue;

            if (online_idx_ == c) {
                if (mgrs[c].rec_head) drain_chunk(c);
            } else {
                const uint64_t cw = mgrs[c].compact_word;
                if (!mgrs[c].rec_head && (cw & CHUNK_SUMMARY_MASK) == CHUNK_SUM_ALL_USED)
                    continue;
                if (ensure_online(c) != 0) return RC_ERR_IO;
            }
            uint64_t hit_c = 0, left = bitscount;
            if (!win_alloc_soft(from - wlo, to - wlo, align_log2, bitscount, &hit_c, &left))
                continue;
            mgrs[c].compact_word |= CHUNK_FLAG_DIRTY;
            const uint64_t got = bitscount - left;
            const uint64_t hit = wlo + hit_c;
            in_slice_hint = hit + got;
            out_slice_base_idx = hit;
            bits_left_to_be_allocated = left;
            return RC_OK;
        }
    }
    return RC_ERR_NO_BLOCK;                              /* 无货：缺口 = bitscount */
}

/* ═══════════════════════════════════════════════════════════════
 * test_interval — 区间测试（**仅 WORKABLE**；阻塞 lock；片内坐标）
 *
 * 快/慢路径分派（03 §8 test 行）：
 *   在线窗 → 先 drain 归还链（逻辑真相优先，摘要可能陈旧）→ 直读 buffer；
 *   离线窗 → 摘要裁决（ALL_USED/ALL_FREE 命 val 免载入跳窗）→ 否则换片上线查。
 * 异位 → RC_ERR_NOT_FOUND；换片 I/O 失败 → RC_ERR_IO（不猜盘态）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::test_interval(bool val, uint64_t slice_base, uint64_t len) {
    std::lock_guard<std::mutex> lk(slice_lock);
    if (!is_workable() || !mgrs || !active_chunk_buffer) return RC_ERR_STATE;
    if (slice_base > slice_bits_ || len > slice_bits_ - slice_base) return RC_ERR_STATE;
    if (len == 0) return RC_OK;

    const uint32_t sh = 15 + static_properties.chunk_order;
    const uint64_t end = slice_base + len;
    uint64_t pos = slice_base;
    while (pos < end) {
        const uint32_t c = ctl_of(pos);
        const uint64_t wlo = (uint64_t)c << sh;
        const uint64_t n = ((end <= wlo + limit_of(c)) ? end : wlo + limit_of(c)) - pos;

        if (online_idx_ == c) {
            if (mgrs[c].rec_head) drain_chunk(c);        /* 在线窗：先还账再读 */
        } else {
            const uint64_t cw = mgrs[c].compact_word;
            if (!mgrs[c].rec_head) {
                const uint64_t sum = cw & CHUNK_SUMMARY_MASK;
                if (sum == CHUNK_SUM_ALL_USED || sum == CHUNK_SUM_ALL_FREE) {
                    const bool all_ones = (sum == CHUNK_SUM_ALL_USED);
                    if (all_ones != val) return RC_ERR_NOT_FOUND;
                    pos += n;                            /* 摘要命 val：免载入整窗跳过 */
                    continue;
                }
            }
            if (ensure_online(c) != 0) return RC_ERR_IO;
        }
        const uint64_t *words = reinterpret_cast<const uint64_t *>(active_chunk_buffer);
        const uint64_t lo = pos - wlo;
        for (uint64_t i = 0; i < n; i++)
            if (bit_get(words, lo + i) != val) return RC_ERR_NOT_FOUND;
        pos += n;
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * free_interval — 区间释放（**仅 WORKABLE**；阻塞 lock；统一挂链）
 *
 * 无论目标 chunk 在线与否：按 chunk 边界切段 → 前插该 chunk 归还链（O(1)、
 * 堆节点、链操作全在 slice_lock 内不加额外锁）→ 摘要降级 MIXED + 置
 * HAS_RECS → free_count 挂链即记账 +n（drain 不再动）。零 I/O。
 * 越界 → RC_ERR_STATE；len==0 → RC_OK。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::free_interval(uint64_t slice_base, uint64_t len) {
    std::lock_guard<std::mutex> lk(slice_lock);
    if (!is_workable() || !mgrs) return RC_ERR_STATE;
    if (slice_base > slice_bits_ || len > slice_bits_ - slice_base) return RC_ERR_STATE;
    if (len == 0) return RC_OK;

    const uint32_t sh = 15 + static_properties.chunk_order;
    const uint64_t end = slice_base + len;
    uint64_t pos = slice_base;
    while (pos < end) {
        const uint32_t c = ctl_of(pos);
        const uint64_t wlo = (uint64_t)c << sh;
        const uint64_t whi = wlo + limit_of(c);
        const uint64_t n = (end <= whi) ? end - pos : whi - pos;   /* chunk 边界切段 */

        rec_seg_t *node = new (std::nothrow) rec_seg_t{pos, n, mgrs[c].rec_head};
        if (!node) return RC_ERR_STATE;   /* 堆失败：本段未挂账未记账（泄漏方向，自洽） */
        mgrs[c].rec_head = node;
        mgrs[c].compact_word = (mgrs[c].compact_word & ~(CHUNK_SUMMARY_MASK | CHUNK_FLAG_HAS_RECS))
                             | CHUNK_SUM_MIXED | CHUNK_FLAG_HAS_RECS;
        free_count += n;                                    /* 挂链即记账（★ 记账） */
        pos += n;
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * soft_occupy — 锚定软占领（**仅 WORKABLE**；阻塞 lock；片内贪心前缀）
 *
 * 口径与 v1/ko 完全一致：*rest 入 = 已由其它途径满足量，real = len − *rest
 * 为本锚点上限；出 = len − 本调用累计认领（总缺口）。从锚点窗起逐窗贪心，
 * 顶到窗尾续下一窗（跨 chunk 续圈 = 原跨片续圈同构）；撞占用 / 摘要 ALL_USED
 * 如实停手；顶片尾如实回传。认领 ≥1 → RC_OK；0 → RC_ERR_NO_BLOCK（rest = len）。
 * hint 只进不退（认领尾 > hint 才推进——v1 政策注记）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::soft_occupy(uint64_t slice_base, uint64_t len,
                                             uint64_t *rest_bits) {
    std::lock_guard<std::mutex> lk(slice_lock);
    if (!is_workable() || !mgrs || !active_chunk_buffer) return RC_ERR_STATE;
    if (!rest_bits || *rest_bits > len) return RC_ERR_STATE;
    if (len == 0) return RC_OK;
    if (slice_base >= slice_bits_) return RC_ERR_STATE;
    if (*rest_bits == len) return RC_OK;                  /* real == 0：已全满足 */
    const uint64_t real = len - *rest_bits;

    const uint32_t sh = 15 + static_properties.chunk_order;
    uint64_t anchor = slice_base;
    uint64_t gap = real;                                  /* 本锚点剩余目标 */
    uint64_t claimed = 0;
    uint32_t c = ctl_of(anchor);

    while (c < chunks_per_slice_) {
        const uint64_t wlo = (uint64_t)c << sh;
        const uint64_t whi = wlo + limit_of(c);
        if (anchor >= whi) { c++; continue; }

        if (online_idx_ == c) {
            if (mgrs[c].rec_head) drain_chunk(c);
        } else {
            const uint64_t cw = mgrs[c].compact_word;
            if (!mgrs[c].rec_head && (cw & CHUNK_SUMMARY_MASK) == CHUNK_SUM_ALL_USED)
                break;                                    /* 锚点窗无货 → 前缀断（免载入） */
            if (ensure_online(c) != 0) {
                *rest_bits = len - claimed;               /* 已认领如实回传 */
                return (claimed > 0) ? RC_OK : RC_ERR_IO;
            }
        }
        uint64_t wr = 0;                                  /* 出 = gap − 本窗认领 */
        const int rc = win_soft_occupy(wlo, whi, anchor, gap, &wr);
        const uint64_t k = gap - wr;
        if (rc == WIN_BAD) {
            *rest_bits = len - claimed;
            return RC_ERR_STATE;
        }
        if (k > 0) {
            mgrs[c].compact_word |= CHUNK_FLAG_DIRTY;     /* 认领 = 变更 */
            claimed += k;
            gap = wr;
            anchor += k;
        }
        if (k == 0 || gap == 0) break;                    /* 锚点即占用 / 全额认领 */
        if (anchor < whi) break;                          /* 撞占用停：不跨窗硬续 */
        c++;                                              /* 顶到窗尾 → 下一窗续圈 */
    }

    if (anchor > in_slice_hint) in_slice_hint = anchor;   /* 只进不退 */
    *rest_bits = len - claimed;
    return (claimed > 0) ? RC_OK : RC_ERR_NO_BLOCK;
}

/* ═══════════════════════════════════════════════════════════════
 * enable — UNWORKABLE → WORKABLE（阻塞 lock）
 * 拷 soul → 派生几何（slice_bits_ = chunk_blk_length<<15 ⇒ 尾块公式恒成立）
 * → 建 mgrs[] / active_chunk_buffer（4K 对齐）→ 一次整片全量读（堆上临时
 * 缓冲）实测 free_count + 填每 chunk 2bit 摘要 → WORKABLE。
 * 任一步失败：收资源、几何归零、保持 UNWORKABLE。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::enable(slice_soul_t *soul, uint64_t in_base_idx) {
    /* 参数校验先于上锁（纯读 soul，无共享态） */
    if (!soul || !soul->read_block || !soul->write_block) return RC_ERR_STATE;
    if (soul->chunk_blk_length == 0 || soul->chunk_blk_length >= (1ULL << 49))
        return RC_ERR_STATE;                       /* 移位/体量上界：防 slice_bits_ 溢出 */
    if (soul->chunk_order > 16) return RC_ERR_STATE;   /* 缓冲 4096<<order ≤ 256MB；防移位 UB */

    std::lock_guard<std::mutex> lk(slice_lock);
    if (slice_state == SLICE_WORKABLE) return RC_ERR_STATE;   /* 双 enable 拒 */

    static_properties = *soul;                     /* 拷 soul（本片不变面） */
    in_global_base_idx = in_base_idx;              /* 只存不消费（换算在上层） */

    /* ── 派生几何：slice_bits = 总块数 × 32768 位（1 FS_block = 4096B 位图区）
     *    ⇒ chunks = ceil(slice_bits/chunk_bits)、tail = 32768×(k mod 2^order)
     *    ——与头文件 §索引合成公式逐字一致；尾块恒 ≥1 整页。 ── */
    const uint64_t nblocks = soul->chunk_blk_length;
    slice_bits_ = nblocks << 15;
    const uint64_t per = 1ULL << soul->chunk_order;        /* 每 chunk 占 FS_block 数 */
    const uint64_t nwin = (nblocks + per - 1) >> soul->chunk_order;
    if (nwin == 0 || nwin > 0xFFFFFFFFULL) return RC_ERR_STATE;
    chunks_per_slice_ = (uint32_t)nwin;
    tail_bits_ = slice_bits_ - (uint64_t)(chunks_per_slice_ - 1) * chunk_bits();

    /* ── 建控制器数组（默认初始化：compact_word=MIXED、链头空）+ 在线窗缓冲 ── */
    mgrs = new (std::nothrow) chunk_mgr[chunks_per_slice_];
    if (!mgrs) { mgrs = nullptr; return RC_ERR_STATE; }
    void *buf = alloc_aligned_4k((size_t)4096 << soul->chunk_order);
    if (!buf) {
        delete[] mgrs;
        mgrs = nullptr;
        return RC_ERR_STATE;
    }
    active_chunk_buffer = static_cast<uint8_t *>(buf);

    /* ── 一次整片全量读（mount 期一次性 I/O；堆上临时缓冲，读完即释放）── */
    uint8_t *tmp = static_cast<uint8_t *>(alloc_aligned_4k((size_t)(slice_bits_ >> 3)));
    if (!tmp) {
        release_resources(mgrs, chunks_per_slice_, active_chunk_buffer);
        mgrs = nullptr;
        active_chunk_buffer = nullptr;
        return RC_ERR_STATE;
    }
    if (static_properties.read_block(static_properties.io_ctx, soul->chunk_base_blk,
                                     nblocks, tmp) != RC_OK) {
        free(tmp);
        release_resources(mgrs, chunks_per_slice_, active_chunk_buffer);
        mgrs = nullptr;
        active_chunk_buffer = nullptr;
        return RC_ERR_IO;
    }

    free_count = count_zeros(reinterpret_cast<const uint64_t *>(tmp), slice_bits_);
    const uint32_t sh = 15 + soul->chunk_order;
    for (uint32_t c = 0; c < chunks_per_slice_; c++) {
        summarize_window(mgrs[c], tmp + (((uint64_t)c << sh) >> 3), limit_of(c));
    }
    free(tmp);

    online_idx_ = SLICE_CHUNK_NONE;
    gen_ = 0;
    in_slice_hint = 0;
    slice_state = SLICE_WORKABLE;                 /* 最后一步才开门 */
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * shutdown — 须 WORKABLE →（锁内跑完一轮工作）→ UNWORKABLE（阻塞 lock）
 * 一轮 = 在线脏窗写回 + 全部归还链合并落盘（读整片 → 在线窗覆盖 → 全链清位
 * → 写整片）→ 释放 buffer / mgrs / 残留链节点 → 归零几何 → UNWORKABLE。
 * I/O 失败仍完成收资源与状态迁移（记录丢失 = 泄漏方向，§1.5 同构）返 RC_ERR_IO。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::shutdown() {
    std::lock_guard<std::mutex> lk(slice_lock);
    if (!is_workable()) return RC_ERR_STATE;

    result_code_t rc = RC_OK;
    bool need_io = false;
    for (uint32_t c = 0; c < chunks_per_slice_; c++)
        if (mgrs[c].rec_head || (mgrs[c].compact_word & CHUNK_FLAG_DIRTY)) need_io = true;

    if (need_io) {
        uint8_t *tmp = static_cast<uint8_t *>(alloc_aligned_4k((size_t)(slice_bits_ >> 3)));
        bool ok = false;
        if (tmp && static_properties.read_block(static_properties.io_ctx,
                                                static_properties.chunk_base_blk,
                                                static_properties.chunk_blk_length,
                                                tmp) == RC_OK) {
            const uint32_t sh = 15 + static_properties.chunk_order;
            if (online_idx_ != SLICE_CHUNK_NONE) {
                const uint32_t oc = online_idx_;
                const uint64_t wlo = (uint64_t)oc << sh;
                if (mgrs[oc].rec_head) {
                    apply_recs(mgrs[oc].rec_head,
                               reinterpret_cast<uint64_t *>(active_chunk_buffer), wlo);
                    mgrs[oc].compact_word |= CHUNK_FLAG_DIRTY;   /* 兜底：写失败也须留脏 */
                }
                std::memcpy(tmp + (wlo >> 3), active_chunk_buffer, limit_of(oc) >> 3);
            }
            for (uint32_t c = 0; c < chunks_per_slice_; c++)
                if (mgrs[c].rec_head)
                    apply_recs(mgrs[c].rec_head, reinterpret_cast<uint64_t *>(tmp), 0);
            ok = static_properties.write_block(static_properties.io_ctx,
                                               static_properties.chunk_base_blk,
                                               static_properties.chunk_blk_length, tmp) == RC_OK;
        }
        free(tmp);
        if (!ok) rc = RC_ERR_IO;      /* 记录未落盘：泄漏方向（fsck 可收），照收资源 */
    }

    release_resources(mgrs, chunks_per_slice_, active_chunk_buffer);
    mgrs = nullptr;
    active_chunk_buffer = nullptr;
    online_idx_ = SLICE_CHUNK_NONE;
    gen_ = 0;
    slice_bits_ = 0;
    chunks_per_slice_ = 0;
    tail_bits_ = 0;
    free_count = 0;
    in_slice_hint = 0;
    slice_state = SLICE_UNWORKABLE;
    return rc;
}

/* ═══════════════════════════════════════════════════════════════
 * chunks_asyn — TTL/周期同步（**仅 WORKABLE**；持 slice_lock 一整轮全量）
 * 一次读整片（堆上临时缓冲 slice_bits/8）→ 在线窗：其归还链先落进在线缓冲
 *（buffer 是 evict/overlay 的真相，否则 overlay 会把待还位盖回来）→ 覆盖进
 * 暂存 → 合并本片全部 chunk 归还链（逐节点清位，**节点暂不摘**）→ 一次写回
 * → 成功才摘账 + 重算全窗摘要 + 整片重扫校准 free_count + 清在线脏标 → 释放缓冲。
 * 失败（读/写）：链原样保留、状态不动，重跑幂等（写失败时在线窗已保守置脏）。
 * 校准回报：out_calib 非空 → 回填 {校准前记账值, 整片重扫实测值}（★ 记账：
 * slice 层允许重复 free 致 free_count 虚高，asyn 纠偏并把漂移上报外部）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t chunk_slice_mgr_t::chunks_asyn(free_count_calib_t *out_calib) {
    std::lock_guard<std::mutex> lk(slice_lock);
    if (!is_workable() || !mgrs || !active_chunk_buffer) return RC_ERR_STATE;

    uint8_t *tmp = static_cast<uint8_t *>(alloc_aligned_4k((size_t)(slice_bits_ >> 3)));
    if (!tmp) return RC_ERR_STATE;

    /* ① 一次读入本片全量位图 */
    if (static_properties.read_block(static_properties.io_ctx, static_properties.chunk_base_blk,
                                     static_properties.chunk_blk_length, tmp) != RC_OK) {
        free(tmp);
        return RC_ERR_IO;
    }

    const uint32_t sh = 15 + static_properties.chunk_order;

    /* ② 在线窗：先还账进在线缓冲 → 覆盖暂存中对应窗（顺序不可倒：
     *    先覆盖后清位，否则 overlay 把待还位盖回来 = 记录失效）。 */
    if (online_idx_ != SLICE_CHUNK_NONE) {
        const uint32_t oc = online_idx_;
        const uint64_t wlo = (uint64_t)oc << sh;
        if (mgrs[oc].rec_head) {
            apply_recs(mgrs[oc].rec_head, reinterpret_cast<uint64_t *>(active_chunk_buffer), wlo);
            mgrs[oc].compact_word |= CHUNK_FLAG_DIRTY;      /* 写失败兜底：清位须留脏 */
        }
        std::memcpy(tmp + (wlo >> 3), active_chunk_buffer, limit_of(oc) >> 3);
    }

    /* ③ 合并其余全部 chunk 归还链（逐节点清位；idx_base=0 = 片内坐标直接索引暂存） */
    for (uint32_t c = 0; c < chunks_per_slice_; c++)
        if (mgrs[c].rec_head)
            apply_recs(mgrs[c].rec_head, reinterpret_cast<uint64_t *>(tmp), 0);

    /* ④ 一次整片写回 */
    if (static_properties.write_block(static_properties.io_ctx, static_properties.chunk_base_blk,
                                      static_properties.chunk_blk_length, tmp) != RC_OK) {
        free(tmp);
        return RC_ERR_IO;             /* 记录仍在链上 → 下次重跑（幂等） */
    }

    /* ⑤ 摘账 → 重算摘要 → 整片重扫校准 free_count → 清在线脏标 */
    for (uint32_t c = 0; c < chunks_per_slice_; c++) {
        free_recs(mgrs[c]);
        summarize_window(mgrs[c], tmp + (((uint64_t)c << sh) >> 3), limit_of(c));
    }
    if (online_idx_ != SLICE_CHUNK_NONE)
        mgrs[online_idx_].compact_word &= ~CHUNK_FLAG_DIRTY;   /* 写回后盘态 = 内存 */
    const uint64_t calib_old = free_count;             /* 校准前记账值（可能已漂移） */
    free_count = count_zeros(reinterpret_cast<const uint64_t *>(tmp), slice_bits_);
    if (out_calib) {                                   /* ★ 记账：[旧, 新] 回报外部 */
        out_calib->old_free_count = calib_old;
        out_calib->new_free_count = free_count;
    }
    free(tmp);
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 换片状态机（全程持 slice_lock；可睡故允许持锁等盘 I/O）
 * ═══════════════════════════════════════════════════════════════ */

/* ensure_online — 窗不在线时的慢路径全序列（03 §5/§6 铁律顺序）：
 *   evict 旧窗（脏则写回 + 重算摘要）→ load 目标窗（摘要 ALL_FREE 且链空可
 *   省读 memset 0）→ ★ drain 归还链（必须在 publish 之前，否则 alloc 把盘上
 *   仍为 1 的位判成已用 → 永久泄漏）→ publish(online_idx_=ctl, gen_++) →
 *   按 drained 置 dirty。
 * 返回 0 = 已在线；非 0 = 失败（I/O 错；状态零损坏，可重试）。 */
int chunk_slice_mgr_t::ensure_online(uint32_t ctl) {
    if (!mgrs || !active_chunk_buffer || ctl >= chunks_per_slice_) return -1;
    if (online_idx_ == ctl) return 0;                       /* 复检：已在位 */

    if (online_idx_ != SLICE_CHUNK_NONE) {
        evict_online();
        if (online_idx_ != SLICE_CHUNK_NONE) return -1;     /* 旧窗写回失败：保持在线 */
    }

    /* load：摘要 ALL_FREE 且本 chunk 链空 → 盘态全 0，免读直置空窗 */
    const uint64_t nbits = limit_of(ctl);
    const uint64_t cw = mgrs[ctl].compact_word;
    if ((cw & CHUNK_SUMMARY_MASK) == CHUNK_SUM_ALL_FREE && !mgrs[ctl].rec_head) {
        std::memset(active_chunk_buffer, 0, (size_t)(nbits >> 3));
    } else {
        const uint64_t nblocks = nbits >> 15;               /* nbits 恒 32768 倍数 = 整页 */
        if (static_properties.read_block(static_properties.io_ctx, chunk_disk_blk(ctl),
                                         nblocks, active_chunk_buffer) != RC_OK)
            return -1;                                      /* 在线仍为 NONE，可重试 */
    }

    /* ★ drain 归还链（publish 之前）→ publish */
    const bool drained = (mgrs[ctl].rec_head != nullptr);
    drain_chunk(ctl);
    online_idx_ = ctl;
    gen_++;                                                 /* 换片代号（发布递增） */
    if (drained) mgrs[ctl].compact_word |= CHUNK_FLAG_DIRTY; /* 清位已发生 → 须落盘 */
    return 0;
}

/* drain_chunk — 合并本 chunk 归还链（须持 slice_lock，且该窗内容已在
 * active_chunk_buffer：load 后 publish 前 / 已在线两种时机）。
 * 逐节点清位 + 释放节点。free_count 挂链时已记账，此处不动（★ 记账）。 */
void chunk_slice_mgr_t::drain_chunk(uint32_t ctl) {
    if (!mgrs || !active_chunk_buffer || ctl >= chunks_per_slice_) return;
    if (!mgrs[ctl].rec_head) return;
    const uint64_t wlo = (uint64_t)ctl << (15 + static_properties.chunk_order);
    apply_recs(mgrs[ctl].rec_head, reinterpret_cast<uint64_t *>(active_chunk_buffer), wlo);
    free_recs(mgrs[ctl]);
    /* 清位 = 变更：必须置脏，否则该窗若此前已被 asyn/evict 清脏（且后续无其它
     * 改动），evict_online 会按 dirty==0 跳过回写 → 盘上陈旧（清掉的位仍为 1）→
     * asyn 重扫把 free_count 校准偏小 + test_interval 把空闲位误判占用（泄漏）。
     * 【2026-09-30 压测组发现（adversarial leak_drain_no_dirty）。】（摘要留 evict/
     * 重算更新：清位只增空闲，MIXED 是保守态，不产生错误跳过。） */
    mgrs[ctl].compact_word |= CHUNK_FLAG_DIRTY;
}

/* evict_online — 下线当前在线窗（须持 slice_lock）：脏则快照写盘（失败保持
 * 在线 + 脏标不落，由 ensure_online 判错——buffer 仍是真相，状态零损坏）→
 * 重算摘要 → online_idx_=NONE、gen_++。
 * 不 drain 归还链：链随 chunk 留存、下次上线 publish 前再还（省一次写放大；
 * 期间 evict 若写盘，摘要因链在而恒 MIXED，不会产生错误 ENOSPC）。 */
void chunk_slice_mgr_t::evict_online() {
    const uint32_t old = online_idx_;
    if (old == SLICE_CHUNK_NONE || !mgrs || !active_chunk_buffer) return;
    chunk_mgr &cm = mgrs[old];
    if (cm.compact_word & CHUNK_FLAG_DIRTY) {
        const uint64_t nbits = limit_of(old);
        if (static_properties.write_block(static_properties.io_ctx, chunk_disk_blk(old),
                                          nbits >> 15, active_chunk_buffer) != RC_OK)
            return;                                        /* 写失败：保持在线 + 保留脏 */
        cm.compact_word &= ~CHUNK_FLAG_DIRTY;
    }
    recompute_summary(old, active_chunk_buffer);           /* 从 buffer 重算（§7） */
    online_idx_ = SLICE_CHUNK_NONE;
    gen_++;                                                /* 下线代号递增 */
}

/* recompute_summary — 摘要重算（须持 slice_lock；src 下标 = 窗首起）。
 * 链上有待还记录 → 恒 MIXED（防错误 ENOSPC）。 */
void chunk_slice_mgr_t::recompute_summary(uint32_t ctl, const void *src) {
    if (!mgrs || ctl >= chunks_per_slice_) return;
    summarize_window(mgrs[ctl], src, limit_of(ctl));
}

/* ═══════════════════════════════════════════════════════════════
 * 单窗原语（须持 slice_lock 且窗在线；win_alloc_* 的 from/to/out = chunk 内
 * 局部坐标 —— 头注释明文；free_count 就地扣减（alloc 成功扣减纪律），片内
 * hint / 脏标 / 坐标换算由 alloc_* 外壳统一收口（win_* 不知窗首 wlo）。）
 * ═══════════════════════════════════════════════════════════════ */

/* 硬分配单窗：[from,to) 内找 count 连续空闲位、起始 2^align_log2 对齐；
 * 不部分分配。命中 → 置位 + free_count 扣减 + *out = chunk 内局部命中位。 */
bool chunk_slice_mgr_t::win_alloc_hard(uint64_t from, uint64_t to, uint8_t align_log2,
                                        uint64_t count, uint64_t *out) {
    if (!out || !active_chunk_buffer) return false;
    if (from >= to || count == 0 || count > to - from) return false;   /* 窗内放不下 */
    const uint64_t align = 1ULL << align_log2;   /* 入口已限 0..15（alloc_* 预检） */
    uint64_t *words = reinterpret_cast<uint64_t *>(active_chunk_buffer);

    const uint64_t hit = find_free_run(words, to, from, count, align);
    if (hit == SEG_NO_HIT) return false;
    for (uint64_t i = 0; i < count; i++) bit_set(words, hit + i);
    free_count -= count;                                    /* gate 已保 count ≤ free_count */
    *out = hit;
    return true;
}

/* 软分配单窗：首空闲段取对齐候选，实际 = min(段长, count)；缺口经 *left 回传。 */
bool chunk_slice_mgr_t::win_alloc_soft(uint64_t from, uint64_t to, uint8_t align_log2,
                                       uint64_t count, uint64_t *out, uint64_t *left) {
    if (!out || !left || !active_chunk_buffer) return false;
    *left = count;
    if (from >= to || count == 0) return false;
    const uint64_t align = 1ULL << align_log2;   /* 入口已限 0..15（alloc_* 预检） */
    uint64_t *words = reinterpret_cast<uint64_t *>(active_chunk_buffer);
    const uint64_t cap = (count > align) ? count : align;

    uint64_t seg_s = 0, seg_e = 0;
    if (!next_free_seg(words, to, from, cap, &seg_s, &seg_e)) return false;
    const uint64_t cand = align_up(seg_s, align);
    if (cand >= seg_e) return false;
    uint64_t got = seg_e - cand;
    if (got > count) got = count;
    for (uint64_t i = 0; i < got; i++) bit_set(words, cand + i);
    free_count -= got;
    *out = cand;
    *left = count - got;
    return true;
}

/* 锚定占领单窗（wlo/whi/base = 片内偏移，ko 同构——窗界与锚点须同坐标系）：
 * 从 base 起贪心认领连续空闲前缀（≤ min(窗尾, slice 尾, len−*rest)），
 * 撞占用/窗尾停。*rest 口径同 v1：入 = 已满足，出 = len − 本窗认领。
 * 返回 WIN_OK（认领 ≥1）/ WIN_EMPTY（0 位）/ WIN_BAD（参数）。 */
int chunk_slice_mgr_t::win_soft_occupy(uint64_t wlo, uint64_t whi, uint64_t base,
                                       uint64_t len, uint64_t *rest) {
    if (!rest || !active_chunk_buffer) return WIN_BAD;
    if (*rest > len) return WIN_BAD;
    if (len == 0) return WIN_OK;
    const uint64_t real = len - *rest;
    if (real == 0) return WIN_OK;
    if (base < wlo || base >= whi || base >= slice_bits_) return WIN_BAD;

    const uint64_t whi_eff = (whi < slice_bits_) ? whi : slice_bits_;
    uint64_t cap = whi_eff - base;                          /* 窗尾夹断（顶窗尾即停） */
    if (cap > real) cap = real;
    if (cap == 0) { *rest = len; return WIN_EMPTY; }

    uint64_t *words = reinterpret_cast<uint64_t *>(active_chunk_buffer);
    const uint64_t off = base - wlo;                        /* → chunk 内局部 */
    const uint64_t k = greedy_free_prefix(words, off, cap);
    if (k == 0) { *rest = len; return WIN_EMPTY; }
    for (uint64_t i = 0; i < k; i++) bit_set(words, off + i);
    free_count -= k;
    *rest = len - k;
    return WIN_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slices_mgr_t_v2 — 路由侧（全局 ↔ 局部换算收口；同 TU 追加）
 *
 * 2026-09-30 第二刀：头（include/ravenfs/bitmap_slices_v2.hpp）内仅声明的
 * mgr 在此落地。契约权威 = 头注释；路由骨架 / 几何推导 / preferred 亲和 /
 * 跨片 free·occupy 续圈 / 审计逐条移植自 juvenile_god/bitmap_slices.cpp 的
 * bitmap_slices_mgr_t（v1，2026-09-03）。
 *
 * 与 v1 的结构差（皆因 v2 片自带 slice_lock + 单一局部坐标）：
 *   ① mgr 不再编排锁：alloc 走**片内 try_lock**（拿不到 → 片返 RC_ERR_BUSY
 *      → 按 preferred 换片）；free / occupy / test 走片内阻塞锁（定点/释放
 *      不可丢）。路由层零锁（v1 的 try_lock_write / 自旋抢锁全部下沉片内）。
 *   ② 片入出参是**片内局部**（0 起）⇒ 全局 ↔ 局部换算全收本层（v1 片持全局
 *      坐标、mgr 只做路由）：定位片 = g / per_slice_bits、
 *      局部 = g − idx×per、全局 = idx×per + 局部。
 *   ③ per_slice_bits 不入头（group_t 仅 slices/preferred）⇒ 运行期由片 1 的
 *      in_global_base_idx（enable 写入 = 1×per）恢复；单片退化 per=0、idx 恒 0。
 *      同理头无 processor_count / cpu_slots_ 成员 ⇒ cpu 索引与 preferred_of
 *      均**不做上界校验**（宿主须保证 cfg.processor_count ≥ 真实逻辑 CPU 数，
 *      否则 OOB）—— 报告遗留点：建议下一刀在头里补 cpu_slots_ 成员。
 *   ④ 不做 v1.1 的「片0 hint=1」根保留（v1.2 root=16 起、无保留区）：inode 组
 *      hint 恒 0 起，首枚 inode_alloc 即可取 0。
 * ═══════════════════════════════════════════════════════════════ */

extern "C" uint32_t get_processor_id();   /* v1 同款平台合约：cpuid；
                                           * 用户态 cpu_id_stub.cpp weak 桩 / 测试强定义 */

namespace {

/* ── per_slice_bits 恢复：片 i 的 in_global_base_idx = i×per（enable 写入）
 *    ⇒ 片 1 基址即 per（片数 ≥2）；单片时任何合法坐标 idx 恒 0，per 取 0
 *    退化值即可（无需真值）。 ── */
inline uint64_t recover_per(const chunk_slice_mgr_t *slices, uint32_t slice_count) {
    return (slices && slice_count > 1) ? slices[1].in_global_base_idx : 0;
}
/* 全局位号 → 片下标（定位片 = g / per_slice_bits；单片 per=0 恒 0） */
inline uint64_t slice_idx_of(uint64_t g, uint64_t per) {
    return per ? g / per : 0;
}
/* 全局位号 → 片内局部（g − idx×per；单片即 g 本身） */
inline uint64_t local_of(uint64_t g, uint64_t per, uint64_t idx) {
    return g - idx * per;
}
/* 片内局部 → 全局位号（idx×per + local；单片即 local） */
inline uint64_t global_of(uint64_t idx, uint64_t per, uint64_t local) {
    return idx * per + local;
}

}  // namespace

/* ═══════════════════════════════════════════════════════════════
 * ctor — 即装配（CHUNK-only）：slice_count_ 推导（v1 同款，两组共享）→
 * per-CPU preferred → 逐组逐片构造 soul 并 enable。失败 *res_code 给错
 * 且自清理（成员回空态：数组已还、slice_count_=0）。
 * ═══════════════════════════════════════════════════════════════ */
bitmap_slices_mgr_t_v2::bitmap_slices_mgr_t_v2(const bitmap_slices_v2cfg_t &cfg,
                                               uint64_t *res_code) {
    if (res_code) *res_code = RC_ERR_STATE;        /* 默认失败态（v1 口径） */

    /* ── 调用方参数预检：两组 I/O 回调非空 + 两段位图非空 + CPU 槽 ≥1。
     *    processor_count=0 无从兜底（头无 cpu_slots_ 成员，内部 % 会除零）⇒ 拒。 */
    if (!cfg.read_block || !cfg.write_block ||
        cfg.block_bitmap_blocks == 0 || cfg.inode_bitmap_blocks == 0 ||
        cfg.processor_count == 0)
        return;                                    /* 保持空态 */

    /* ── 位数由块数推导（1 FS_block = 4096B 位图区 = 32768 位；头不传位数）── */
    const uint64_t blk_bits = (uint64_t)cfg.block_bitmap_blocks << 15;
    const uint64_t ino_bits = (uint64_t)cfg.inode_bitmap_blocks << 15;

    /* ── slice_count_ 推导（v1 逐字移植；两组共享一个 slice_count_）：
     *    2^n 上界(cpu) ∩ 双组满片数约束（每片 ≥32768 位）→ 末片非空迭代 ── */
    uint64_t n = 1;
    while (n < cfg.processor_count && n < (1ULL << 30)) n <<= 1;
    const uint64_t blk_cap = blk_bits / 32768;
    const uint64_t ino_cap = ino_bits / 32768;
    uint64_t cap = blk_cap < ino_cap ? blk_cap : ino_cap;
    if (cap == 0) cap = 1;
    if (n > cap) n = cap;
    for (;;) {
        uint64_t per_b = ((blk_bits + n - 1) / n + 32767) / 32768 * 32768;
        uint64_t per_i = ((ino_bits + n - 1) / n + 32767) / 32768 * 32768;
        if (per_b == 0) per_b = 32768;
        if (per_i == 0) per_i = 32768;
        if ((n - 1) * per_b < blk_bits && (n - 1) * per_i < ino_bits) break;
        if (n == 1) break;
        n >>= 1;
    }
    slice_count_ = (uint32_t)n;

    /* ── 失败自清理（头 ctor 注释：成员回空态）：逐片 shutdown（未 enable 的
     *    返 STATE 无害）→ delete[] 数组 → slice_count_ 归 0 ── */
    auto cleanup = [this]() {
        group_t *gs[2] = {&blk_, &ino_};
        for (group_t *g : gs) {
            if (g->slices) {
                for (uint32_t i = 0; i < slice_count_; i++)
                    (void)g->slices[i].shutdown();
                delete[] g->slices;
                g->slices = nullptr;
            }
            delete[] g->preferred;
            g->preferred = nullptr;
        }
        slice_count_ = 0;
    };

    /* ── per-CPU preferred：长 = cfg.processor_count；初值 i % slice_count_ ── */
    try {
        blk_.preferred = new uint32_t[cfg.processor_count];
        ino_.preferred = new uint32_t[cfg.processor_count];
    } catch (const std::bad_alloc &) {
        blk_.preferred = nullptr;
        ino_.preferred = nullptr;
        cleanup();
        return;
    }
    for (uint32_t i = 0; i < cfg.processor_count; i++) {
        blk_.preferred[i] = (uint32_t)(i % slice_count_);
        ino_.preferred[i] = (uint32_t)(i % slice_count_);
    }

    /* ── 一组的装配（v1 setup_group 同款 per 推导 + 逐片 enable）：
     *    per_slice_bits = nbits/slice_count 向上取整到 32768 倍数（片下限）；
     *    片 i：in_global_base_idx = i×per、chunk_base_blk = 组起始 + i×per_blocks、
     *    chunk_blk_length = 该片盘上占块数（末片收窄）、chunk_order = 组 order、
     *    read/write/io_ctx 取自 cfg（两组共用一对回调）。
     *    失败不在此收尾（交 ctor 统一 cleanup，含前一组已 enable 的片）。 */
    auto setup_group = [this, &cfg](group_t &g, uint64_t base_blk, uint64_t blocks,
                                    uint64_t nbits, uint8_t order) -> result_code_t {
        uint64_t per = ((nbits + slice_count_ - 1) / slice_count_ + 32767) / 32768 * 32768;
        if (per == 0) per = 32768;                 /* v1 口径（nbits=0 已在预检挡） */
        const uint64_t per_blocks = per >> 15;      /* 每片盘上占块数（per 恒 32768 倍数） */

        try {
            g.slices = new chunk_slice_mgr_t[slice_count_];
        } catch (const std::bad_alloc &) {
            g.slices = nullptr;
            return RC_ERR_STATE;
        }
        for (uint32_t i = 0; i < slice_count_; i++) {
            const uint64_t begin_blk = (uint64_t)i * per_blocks;
            if (begin_blk >= blocks) return RC_ERR_STATE;   /* 防御（末片非空迭代已保证） */
            uint64_t nblk = blocks - begin_blk;
            if (nblk > per_blocks) nblk = per_blocks;       /* 末片收窄 */

            slice_soul_t soul{};
            soul.read_block       = cfg.read_block;
            soul.write_block      = cfg.write_block;
            soul.io_ctx           = cfg.io_ctx;
            soul.chunk_base_blk   = base_blk + begin_blk;   /* 组起始 FS_block + i×per_blocks */
            soul.chunk_blk_length = nblk;
            soul.chunk_order      = order;
            const result_code_t rc = g.slices[i].enable(&soul, (uint64_t)i * per);
            if (rc != RC_OK) return rc;                     /* 部分成功 → ctor cleanup 统一收 */
        }
        return RC_OK;
    };

    result_code_t rc = setup_group(blk_, cfg.block_bitmap_base_FS_block,
                                   cfg.block_bitmap_blocks, blk_bits, cfg.blkbm_chunk_order);
    if (rc == RC_OK)
        rc = setup_group(ino_, cfg.inode_bitmap_base_FS_block,
                         cfg.inode_bitmap_blocks, ino_bits, cfg.inobm_chunk_order);
    if (rc != RC_OK) { cleanup(); return; }        /* 自清理 → 空态 */

    if (res_code) *res_code = RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * dtor — 逐片 shutdown（在线脏窗 + 归还链一轮落盘；I/O 失败也收资源）→
 * delete[] 片数组 → 还 preferred。未 enable / 已 shutdown 的片返 STATE，无害。
 * ═══════════════════════════════════════════════════════════════ */
bitmap_slices_mgr_t_v2::~bitmap_slices_mgr_t_v2() {
    for (uint32_t i = 0; i < slice_count_; i++) {
        if (blk_.slices) (void)blk_.slices[i].shutdown();
        if (ino_.slices) (void)ino_.slices[i].shutdown();
    }
    delete[] blk_.slices;     blk_.slices = nullptr;
    delete[] ino_.slices;     ino_.slices = nullptr;
    delete[] blk_.preferred;  blk_.preferred = nullptr;
    delete[] ino_.preferred;  ino_.preferred = nullptr;
    slice_count_ = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * block 组（路由：全局位号 ↔ 片内局部换算全在本层；锁在片内）
 * ═══════════════════════════════════════════════════════════════ */

/* 硬分配：count 连续 + 2^align 对齐，**单片内完成**（不跨片）；失败 PTR_INVALID。
 * 路由：本 CPU（get_processor_id()）preferred 片起轮询 slice_count_ 片；
 * 片内 try_lock 拿不到 → RC_ERR_BUSY → 换片（v1 mgr 层 try_lock_write 的等价物，
 * 锁已下沉片内）；RC_OK → 写回 preferred 并把片内命中折成**全局块号**返出。 */
uint64_t bitmap_slices_mgr_t_v2::block_alloc_hard(uint64_t require_count,
                                                   uint8_t align_log2) {
    if (require_count == 0 || !blk_.slices || !blk_.preferred || slice_count_ == 0)
        return PTR_INVALID;
    const uint64_t per = recover_per(blk_.slices, slice_count_);
    const uint32_t cpu = get_processor_id();   /* 头无 processor_count：不做 % 上界（见文件顶 ③） */
    for (uint32_t round = 0; round < slice_count_; round++) {
        const uint32_t idx = (uint32_t)((blk_.preferred[cpu] + round) % slice_count_);
        uint64_t out_local = 0;
        const result_code_t rc =
            blk_.slices[idx].alloc_hard(align_log2, require_count, out_local);
        if (rc != RC_OK) continue;   /* BUSY=本片忙换片；NO_BLOCK/STATE 同 v1 继续扫 */
        blk_.preferred[cpu] = idx;
        return global_of(idx, per, out_local);        /* → 全局块号 */
    }
    return PTR_INVALID;                              /* 全败 */
}

/* 软分配：一次只从单片取（preferred 起轮询；**一片为止**，不跨片续取）；
 * 实际 = min(段长, require)，缺口经 *remain_count 回传；一块没有 → PTR_INVALID。
 * align 恒 0（v1 同款——软分配本就取「首空闲段」，对齐无意义）。 */
uint64_t bitmap_slices_mgr_t_v2::block_alloc_soft(uint64_t require_count,
                                                   uint64_t *remain_count) {
    if (remain_count) *remain_count = require_count;      /* 缺口预置（v1 口径） */
    if (require_count == 0 || !blk_.slices || !blk_.preferred || slice_count_ == 0)
        return PTR_INVALID;
    const uint64_t per = recover_per(blk_.slices, slice_count_);
    const uint32_t cpu = get_processor_id();
    for (uint32_t round = 0; round < slice_count_; round++) {
        const uint32_t idx = (uint32_t)((blk_.preferred[cpu] + round) % slice_count_);
        uint64_t out_local = 0, left = require_count;
        if (blk_.slices[idx].alloc_soft(0, require_count, out_local, left) != RC_OK)
            continue;                                 /* BUSY / 无货 → 换片（v1 同款） */
        blk_.preferred[cpu] = idx;
        if (remain_count) *remain_count = left;       /* 缺口回传 */
        return global_of(idx, per, out_local);
    }
    return PTR_INVALID;
}

/* 软锚定占领（定点对·路由版）：blkbase 起认领 ≤ require_count 连续空闲块。
 * 锚点片唯一 → 片内**阻塞**锁（不可换片，纪律同 block_free——v1 自旋抢锁的
 * 等价物）；片尾跨界在循环顶重定位续圈（下一片 base 0，逐片前缀链 = 真实连续）；
 * 返回 blkbase（原址，认领 ≥1）/ PTR_INVALID；*rest_count 出 = 缺口。 */
uint64_t bitmap_slices_mgr_t_v2::block_occupy_soft(uint64_t blkbase,
                                                    uint64_t require_count,
                                                    uint64_t *rest_count) {
    if (rest_count) *rest_count = require_count;      /* 缺口预置（v1 口径） */
    if (require_count == 0 || !blk_.slices || slice_count_ == 0)
        return PTR_INVALID;
    const uint64_t per = recover_per(blk_.slices, slice_count_);

    uint64_t base = blkbase, remain = require_count;
    while (remain > 0) {
        const uint64_t idx = slice_idx_of(base, per);
        if (idx >= slice_count_) break;               /* 越界（含末片片尾之外）：如实停 */
        uint64_t left = 0;
        const result_code_t rc =
            blk_.slices[idx].soft_occupy(local_of(base, per, idx), remain, &left);
        if (rc != RC_OK) break;                       /* 锚点即占用 / 已至片尾外：前缀断 */
        base += remain - left;                        /* claimed = remain − left ≥ 1 */
        remain = left;
        if (remain == 0) break;
        if (idx + 1 < slice_count_) {                 /* 非末片：片尾精确判（v1 同款） */
            const uint64_t slice_end = (idx + 1) * per;
            if (base != slice_end) break;             /* 中途撞占用停：跨界无意义 */
        }
        /* 恰至片尾（或末片）→ 回循环顶重定位：下一片 base 0 续圈；末片片尾 =
         * nbits 未外化（头无字段）⇒ 重定位后 soft_occupy 返 STATE / idx≥片数
         * 即断，与 v1 的 base != slice_end 判等价。 */
    }
    if (rest_count) *rest_count = remain;
    return (require_count - remain > 0) ? blkbase : PTR_INVALID;
}

/* 区间释放（跨片循环）：blkbase 起 count 块逐段定位所属片释放（物理连续的
 * run 可能跨片——occupy 的跨界产物）。释放无连续要求，片内阻塞锁逐段清位，
 * 无需跨片双持。单片路径与 v1 行为一致。 */
void bitmap_slices_mgr_t_v2::block_free(uint64_t blkbase, uint64_t count) {
    if (count == 0 || !blk_.slices || slice_count_ == 0) return;
    const uint64_t per = recover_per(blk_.slices, slice_count_);
    while (count > 0) {
        const uint64_t idx = slice_idx_of(blkbase, per);
        if (idx >= slice_count_) return;              /* 防御：越界剩余放弃（v1 口径） */
        uint64_t n = count;
        if (idx + 1 < slice_count_) {                 /* 非末片：片尾夹断 → 跨片逐段 */
            const uint64_t slice_end = (idx + 1) * per;
            n = (count <= slice_end - blkbase) ? count : slice_end - blkbase;
            if (n == 0) return;
        }
        /* 末片：nbits 未外化 ⇒ 整段交片内 free_interval 范围校验兜底
         * （越界 → STATE 且一位不落，断链不自旋）；合法输入恒在界内。 */
        if (blk_.slices[idx].free_interval(local_of(blkbase, per, idx), n) != RC_OK)
            return;
        blkbase += n;
        count -= n;
    }
}

/* 区间测试：定位片 + 片内 test_interval（v1 直读 raw 无锁 → v2 无 raw，
 * 经片查、片内阻塞锁）。越界 / 未装配 → false。 */
bool bitmap_slices_mgr_t_v2::block_test(uint64_t blk) const {
    if (!blk_.slices || slice_count_ == 0) return false;
    const uint64_t per = recover_per(blk_.slices, slice_count_);
    const uint64_t idx = slice_idx_of(blk, per);
    if (idx >= slice_count_) return false;
    return blk_.slices[idx].test_interval(true, local_of(blk, per, idx), 1) == RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * inode 组（同构；**不做** v1.1 的「片0 hint=1」根保留——v1.2 root=16 起、
 * 无保留区 ⇒ hint 恒 0 起，首枚 inode_alloc 即可取 0）
 * ═══════════════════════════════════════════════════════════════ */

uint64_t bitmap_slices_mgr_t_v2::inode_alloc() {
    if (!ino_.slices || !ino_.preferred || slice_count_ == 0) return PTR_INVALID;
    const uint64_t per = recover_per(ino_.slices, slice_count_);
    const uint32_t cpu = get_processor_id();
    for (uint32_t round = 0; round < slice_count_; round++) {
        const uint32_t idx = (uint32_t)((ino_.preferred[cpu] + round) % slice_count_);
        uint64_t out_local = 0, left = 0;
        if (ino_.slices[idx].alloc_soft(0, 1, out_local, left) != RC_OK) continue;
        ino_.preferred[cpu] = idx;
        return global_of(idx, per, out_local);
    }
    return PTR_INVALID;
}

void bitmap_slices_mgr_t_v2::inode_free(uint64_t inum) {
    if (!ino_.slices || slice_count_ == 0) return;
    const uint64_t per = recover_per(ino_.slices, slice_count_);
    const uint64_t idx = slice_idx_of(inum, per);
    if (idx >= slice_count_) return;                  /* 防御：越界放弃 */
    (void)ino_.slices[idx].free_interval(local_of(inum, per, idx), 1);   /* 片内阻塞锁 */
}

bool bitmap_slices_mgr_t_v2::inode_test(uint64_t inum) const {
    if (!ino_.slices || slice_count_ == 0) return false;
    const uint64_t per = recover_per(ino_.slices, slice_count_);
    const uint64_t idx = slice_idx_of(inum, per);
    if (idx >= slice_count_) return false;
    return ino_.slices[idx].test_interval(true, local_of(inum, per, idx), 1) == RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 几何 / 审计
 * ═══════════════════════════════════════════════════════════════ */

uint64_t bitmap_slices_mgr_t_v2::block_free_total() const {
    uint64_t sum = 0;
    if (!blk_.slices) return 0;
    for (uint32_t i = 0; i < slice_count_; i++) sum += blk_.slices[i].get_free_count();
    return sum;                                      /* 各片 free_count 求和 */
}

uint64_t bitmap_slices_mgr_t_v2::inode_free_total() const {
    uint64_t sum = 0;
    if (!ino_.slices) return 0;
    for (uint32_t i = 0; i < slice_count_; i++) sum += ino_.slices[i].get_free_count();
    return sum;
}

/* 直返 preferred[cpu]（dump_info 用）：头无 processor_count 成员 ⇒ 无从做
 * cpu 越界校验（v1 有 cpu_slots_ 可比）——仅挡 ctor 失败态的空指针。
 * 宿主须保证 cpu < cfg.processor_count。 */
uint32_t bitmap_slices_mgr_t_v2::block_preferred_of(uint32_t cpu) const {
    return blk_.preferred ? blk_.preferred[cpu] : 0u;
}

uint32_t bitmap_slices_mgr_t_v2::inode_preferred_of(uint32_t cpu) const {
    return ino_.preferred ? ino_.preferred[cpu] : 0u;
}
