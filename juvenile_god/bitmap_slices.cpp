/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slices.cpp — 双位图分片分配器独立模块实现
 *
 * 2026-09-03 从上帝类顶出（原 bitmap_slice.cpp 片自治单元 + ravenfs_juvenile.cpp
 * 的路由/几何推导合编）。模块契约见 include/ravenfs/bitmap_slices.hpp 头注释。
 *
 * 本文件负责：
 *   bitmap_slice_t 全部成员（enable_slice / alloc_hard / alloc_soft /
 *     test_interval / free_interval / soft_occupy / disable_slice + hint/free 读数）
 *   bitmap_slices_mgr_t 全部成员（几何推导 / 双组装配 / 路由 / 审计）
 *
 * 锁纪律：slice 层函数一律不加锁，临界区由 mgr 路由层用 slice_lock 编排：
 *   分配走 try（忙则换片，preferred 亲和）；释放/定点占领走自旋强制抢。
 * 平台身份：get_processor_id() 外部合约（cpp 顶部 extern），用户态由
 * cpu_id_stub.cpp weak 桩 / 压测强定义覆盖；内核端移植必须提供强定义。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/bitmap_slices.hpp"
#include <cstring>
#include <cstdlib>                    /* posix_memalign/free：CHUNK buffer/snapshot（03 §4） */
#include <new>

extern "C" uint32_t get_processor_id();

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
     * ⚠ max_extend>0：段长够 max_extend 即截断 —— 稀疏大位图下 1-bit
     * 分配若延伸到底，每次分配把整片剩余空间走完（O(n²) 热点）。 */
    uint64_t end = start;
    for (;;) {
        if (end >= nbits) { end = nbits; break; }
        if (max_extend && (end - start) >= max_extend) break;   /* 够用即止 */
        uint64_t ew = end >> 6;
        uint64_t eword = ~words[ew];
        if (ew == nwords - 1 && (nbits & 63)) eword &= (1ULL << (nbits & 63)) - 1;
        uint64_t free_mask = eword >> (end & 63);
        if ((free_mask & 1) == 0) break;   /* end 位占用 → 段结束 */
        uint64_t run = (free_mask == ~0ULL) ? 64 : __builtin_ctzll(~free_mask);
        end += run;
        if (end & 63) break;          /* 停在本字内 → 段尾确定 */
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

/* 绑定位图子段并扫片统计 free_count（O(片大小)，装配时一次性）。
 * 校验：slice_base 须落在全局位图内、4K 对齐、片区间不越界。 */
result_code_t bitmap_slice_t::enable_slice(
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

void bitmap_slice_t::disable_slice() {
    slice_base = nullptr;
    base_idx = 0;
    slice_bitscount = 0;
    parent_descriptor = nullptr;
    free_count = 0;
    last_finish_hint = 0;
}

uint64_t bitmap_slice_t::get_free_count() {
    return free_count;
}

/* 测试 hack：读/写 last_finish_hint（片内偏移）。set 越界忽略。 */
uint64_t bitmap_slice_t::get_hint() {
    return last_finish_hint;
}

void bitmap_slice_t::set_hint(uint64_t new_hint) {
    if (new_hint <= slice_bitscount) last_finish_hint = new_hint;
}

/* 硬分配：片内找 bitscount 连续空闲位，起始片内偏移 2^align_log2 对齐；
 * 从 last_finish_hint 起扫，段尾绕回 [0, hint) 再扫一次；不部分分配。 */
result_code_t bitmap_slice_t::alloc_hard(
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
result_code_t bitmap_slice_t::alloc_soft(
    uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx,
    uint64_t &bits_left_to_be_allocated) {
    bits_left_to_be_allocated = bitscount;
    if (!slice_base || !parent_descriptor) return RC_ERR_STATE;
    if (bitscount == 0) return RC_ERR_STATE;
    uint64_t align = 1ULL << (align_log2 > 16 ? 16 : align_log2);
    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
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

/* ═══════════════════════════════════════════════════════════════
 * soft_occupy — 锚定软占领（定点占领对·slice 侧；对偶 free_interval）
 *
 * 与 alloc_hard/alloc_soft 的本质差异：alloc_* 是**搜索式**（从 hint 起
 * 找空闲段）；soft_occupy 是**锚定式**——调用方指定全局位号 base，从
 * base 起贪心认领**连续空闲前缀**，撞第一个占用位或到达额度上限即停。
 *
 * 口径（配合路由层链式续圈）：
 *   *rest_bits 入 = 本次调用前已由其它途径满足的块数（链式记账）；
 *   real_occupy = len − *rest_bits_in = 本锚点目标上限；
 *   实际认领区间 [base, base+claimed)，claimed ≤ real_occupy；
 *   *rest_bits 出 = len − claimed（总缺口，恒等式与入值无关）。
 *   撞片尾（claimed 顶到 slice_bitscount）不是错误——如实回传 rest；
 *   跨界续圈由路由层（block_occupy_soft）按全局位号连续在下一片 base 0
 *   继续；本函数**永不越片**（cap 片内夹断）。
 *
 * 返回：claimed ≥ 1 → RC_OK；claimed == 0（锚点即占用/片内无空间）
 *       → RC_ERR_NO_BLOCK（rest 出 = len）。参数非法 → RC_ERR_STATE。
 * 锁纪律：本函数不加锁，临界区由外部 slice_lock 编排（全文件纪律）。
 * hint 只进不退：认领尾 > 当前 hint 才推进（锚定占领是政策驱动，回卷
 * 游标会打烂搜索分配的空间局部性）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t bitmap_slice_t::soft_occupy(
    uint64_t base, uint64_t len, uint64_t *rest_bits)
{
    if (!slice_base || !parent_descriptor || !rest_bits) return RC_ERR_STATE;
    if (*rest_bits > len) return RC_ERR_STATE;         /* 口径防御：已满足量不能超需求 */
    if (len == 0) return RC_OK;                        /* 无事可做（rest 不动） */
    const uint64_t real = len - *rest_bits;            /* 本锚点目标上限 */
    if (real == 0) return RC_OK;                       /* 已全满足 */

    if (base < base_idx) return RC_ERR_STATE;
    const uint64_t off = base - base_idx;
    if (off >= slice_bitscount) return RC_ERR_STATE;
    const uint64_t cap = (real < slice_bitscount - off) ? real : slice_bitscount - off;
    /* cap ≥ 1（off < slice_bitscount 且 real ≥ 1） */

    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
    const uint64_t nwords = (slice_bitscount + 63) >> 6;

    /* 贪心前缀：从 off 起找第一个占用位（1）；整字全空闲整字吞，
     * 撞占用停（前缀断），cap 夹断。与 next_free_seg 段尾延伸同构，
     * 方向相反（找空闲连续段而非找段起点）。 */
    uint64_t k = 0;
    while (k < cap) {
        const uint64_t wi = (off + k) >> 6;
        if (wi >= nwords) break;                       /* 防御（cap 已夹断，不可达） */
        uint64_t eword = ~words[wi];                   /* 1 = 空闲 */
        if (wi == nwords - 1 && (slice_bitscount & 63))
            eword &= (1ULL << (slice_bitscount & 63)) - 1;  /* 片尾外视为占用 */
        const uint64_t pos = (off + k) & 63;
        const uint64_t free_mask = eword >> pos;
        if ((free_mask & 1) == 0) break;               /* 当前位占用 → 前缀断 */
        /* 字内连续空闲长：第一个占用位偏移（全空闲则走到字尾） */
        uint64_t run = (free_mask == ~0ULL)
                           ? (64 - pos)
                           : __builtin_ctzll(~free_mask);
        k += run;
        if (k > cap) k = cap;
    }
    if (k == 0) { *rest_bits = len; return RC_ERR_NO_BLOCK; }

    for (uint64_t i = 0; i < k; i++) bit_set(words, off + i);
    free_count -= k;
    if (off + k > last_finish_hint) last_finish_hint = off + k;  /* 只进不退 */
    *rest_bits = len - k;
    return RC_OK;
}

/* 区间测试：parent_descriptor 视角 [base, base+len) 全同 val → RC_OK。
 * 越界 → RC_ERR_STATE；存在异位 → RC_ERR_NOT_FOUND。 */
result_code_t bitmap_slice_t::test_interval(
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
result_code_t bitmap_slice_t::free_interval(
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

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slices_mgr_t — 双组装配 + 路由
 * ═══════════════════════════════════════════════════════════════ */

void bitmap_slices_mgr_t::teardown() {
    release_group_chunk(blk_);   /* FULL：空操作（release_backing 早退，现行为零变化）；CHUNK：释放资产 */
    release_group_chunk(ino_);
    delete[] blk_.slices;  blk_.slices = nullptr;
    delete[] blk_.preferred; blk_.preferred = nullptr;
    delete[] ino_.slices;  ino_.slices = nullptr;
    delete[] ino_.preferred; ino_.preferred = nullptr;
}

/* 装配一组：per_slice_bits = nbits/n 向上取整到 min_slice_bits 倍数；
 * 逐片 enable_slice（片 i 覆盖 [i*per, min((i+1)*per, nbits))）。
 * 失败 → 已清理本组自有数组并返回错误码。 */
result_code_t bitmap_slices_mgr_t::setup_group(group_t &g, uint8_t *raw, uint64_t nbits) {
    const uint64_t msb = 32768;   /* 片下限：1 位图块 = 32768 bit（4K 对齐切片约束） */
    uint64_t per = ((nbits + slice_count_ - 1) / slice_count_ + msb - 1) / msb * msb;
    if (per == 0) per = msb;
    g.per_slice_bits = per;
    g.desc.FS_blk_base = 0;
    g.desc.bitmap_raw = reinterpret_cast<uint64_t *>(raw);
    g.desc.bits_count = nbits;

    if (g.backing_mode == BITMAP_BACKING_CHUNK)   /* 03 §2 分叉：CHUNK 装配（刀①③）；
                                                   * FULL 走下述旧码原样（红线） */
        return setup_group_chunk(g, nbits);

    try {
        g.slices = new bitmap_slice_t[slice_count_];
    } catch (const std::bad_alloc &) {
        g.slices = nullptr;
        return RC_ERR_STATE;
    }
    for (uint64_t i = 0; i < slice_count_; i++) {
        uint64_t begin = i * per;
        uint64_t cnt = nbits - begin;
        if (cnt > per) cnt = per;
        slice_config_t cfg;
        cfg.slice_base = raw + begin / 8;   /* begin 恒为 msb(32768) 倍数 → 4K 对齐 */
        cfg.slice_bitscount = cnt;
        if (g.slices[i].enable_slice(&g.desc, cfg) != RC_OK) {
            delete[] g.slices;
            g.slices = nullptr;
            return RC_ERR_STATE;
        }
    }
    return RC_OK;
}

/* ctor 即装配：2^n 上界(cpu) ∩ 双组满片数约束 → 末片非空迭代 → 双组
 * 切片 enable → per-CPU preferred 初始化（片 0 inode hint 跳过 root）。 */
bitmap_slices_mgr_t::bitmap_slices_mgr_t(const bitmap_slices_cfg_t &cfg, uint64_t *res_code) {
    if (res_code) *res_code = RC_ERR_STATE;   /* 默认失败态 */

    /* ── backing 装配期校验（03 §2）：默认 FULL = 现行为零变化（不传新字段的
     * 调用方逐位不变）；CHUNK 必填 chunk_io（原始 fd 后端契约，mmap 不叠用）、
     * chunk_order 有效 0..8，且允许 raw 为 null（全图不驻留，§0 动机）。 */
    const bool chunked = (cfg.backing_mode == BITMAP_BACKING_CHUNK);
    if (cfg.block_bits_count == 0 || cfg.inode_bits_count == 0 ||
        (!chunked && (!cfg.block_bits_raw || !cfg.inode_bits_raw))) {
        return;   /* 调用方参数缺失：保持空态，res_code=RC_ERR_STATE */
    }
    if (chunked && (!cfg.chunk_io || !cfg.chunk_io->read || !cfg.chunk_io->write ||
                    cfg.chunk_order > 8)) {
        return;   /* CHUNK 缺 chunk_io / order 越界 → RC_ERR_STATE（§2 装配期校验） */
    }

    /* 2^n 上界(cpu)（仿 FPA；0 → 退化单槽由 cpu_slots_ 兜底） */
    uint64_t n = 1;
    while (n < cfg.logical_processor_count && n < (1ULL << 30)) n <<= 1;
    /* 满片数约束：每片 ≥ 32768 bit（4K 对齐下限） */
    uint64_t blk_cap = cfg.block_bits_count / 32768;
    uint64_t ino_cap = cfg.inode_bits_count / 32768;
    uint64_t cap = blk_cap < ino_cap ? blk_cap : ino_cap;
    if (cap == 0) cap = 1;
    if (n > cap) n = cap;
    /* 末片非空迭代：per 按 32768 倍数上取整后，双组末片必须非空 */
    for (;;) {
        uint64_t per_b = ((cfg.block_bits_count + n - 1) / n + 32767) / 32768 * 32768;
        uint64_t per_i = ((cfg.inode_bits_count + n - 1) / n + 32767) / 32768 * 32768;
        if (per_b == 0) per_b = 32768;
        if (per_i == 0) per_i = 32768;
        if ((n - 1) * per_b < cfg.block_bits_count &&
            (n - 1) * per_i < cfg.inode_bits_count) break;
        if (n == 1) break;
        n >>= 1;
    }
    slice_count_ = (uint32_t)n;
    cpu_slots_ = cfg.logical_processor_count ? cfg.logical_processor_count : 1;

    /* per-CPU preferred：每"CPU"记住上次成功的亲和片（仿 FPA 每 CPU preferred） */
    try {
        blk_.preferred = new uint32_t[cpu_slots_];
        ino_.preferred = new uint32_t[cpu_slots_];
    } catch (const std::bad_alloc &) {
        blk_.preferred = nullptr;
        ino_.preferred = nullptr;
        teardown();
        return;
    }
    for (uint32_t i = 0; i < cpu_slots_; i++) {
        blk_.preferred[i] = (uint32_t)(i % slice_count_);
        ino_.preferred[i] = (uint32_t)(i % slice_count_);
    }

    /* CHUNK 装配参数下放双组（03 §2；FULL 组全字段惰性不用） */
    blk_.backing_mode = ino_.backing_mode = cfg.backing_mode;
    blk_.is_inode = false;
    ino_.is_inode = true;
    if (chunked) {
        blk_.chunk_order = ino_.chunk_order = cfg.chunk_order;
        blk_.chunk_io = ino_.chunk_io = cfg.chunk_io;
        blk_.chunk_io_ctx = ino_.chunk_io_ctx = cfg.chunk_io_ctx;
        blk_.rec_pool_depth = ino_.rec_pool_depth =
            cfg.rec_pool_depth ? cfg.rec_pool_depth : 64;   /* §6 默认 64 */
        blk_.skip_self_check = ino_.skip_self_check = cfg.skip_self_check;
        blk_.self_check_lo = cfg.self_check_lo;             /* §7.4 自描述校验尾段 */
        blk_.self_check_hi = cfg.self_check_hi;
    }

    if (setup_group(blk_, cfg.block_bits_raw, cfg.block_bits_count) != RC_OK) {
        teardown();
        return;
    }
    if (setup_group(ino_, cfg.inode_bits_raw, cfg.inode_bits_count) != RC_OK) {
        teardown();
        return;
    }
    if (slice_count_ > 0) ino_.slices[0].set_hint(1);   /* inode 0 = root 保留 */

    if (res_code) *res_code = RC_OK;
}

bitmap_slices_mgr_t::~bitmap_slices_mgr_t() {
    teardown();
}

/* ── block 组 ── */

/* 硬分配：count 连续 + 对齐，单片内完成（不跨片）；失败 PTR_INVALID。
 * 路由：本 CPU（get_processor_id()）preferred 片优先 → try 锁失败逐片扫描 */
uint64_t bitmap_slices_mgr_t::block_alloc_hard(uint64_t require_count, uint8_t align_log2) {
    if (require_count == 0 || !blk_.slices || slice_count_ == 0 ||
        cpu_slots_ == 0 || !blk_.preferred) return PTR_INVALID;
    if (blk_.backing_mode == BITMAP_BACKING_CHUNK)        /* 03 §1.2 分叉：FULL 走旧码原样 */
        return chunk_alloc_hard(blk_, require_count, align_log2);
    const uint32_t cpu = get_processor_id() % cpu_slots_;
    for (uint64_t round = 0; round < slice_count_; round++) {
        uint64_t idx = (blk_.preferred[cpu] + round) % slice_count_;
        bitmap_slice_t &s = blk_.slices[idx];
        if (!s.slice_lock.try_lock_write()) continue;      /* 锁忙换片 */
        uint64_t out = 0;
        result_code_t rc = s.alloc_hard(align_log2, require_count, out);
        s.slice_lock.unlock_write();
        if (rc == RC_OK) { blk_.preferred[cpu] = (uint32_t)idx; return out; }
    }
    return PTR_INVALID;
}

/* 软分配：一次只从单片取（preferred 优先），实际 = min(区间长, require)；
 * *remain_count = 缺口数；一块没有 → PTR_INVALID。 */
uint64_t bitmap_slices_mgr_t::block_alloc_soft(uint64_t require_count, uint64_t *remain_count) {
    if (remain_count) *remain_count = require_count;
    if (require_count == 0 || !blk_.slices || slice_count_ == 0 ||
        cpu_slots_ == 0 || !blk_.preferred) return PTR_INVALID;
    if (blk_.backing_mode == BITMAP_BACKING_CHUNK)        /* 03 §1.2 分叉：FULL 走旧码原样 */
        return chunk_alloc_soft(blk_, require_count, remain_count);
    const uint32_t cpu = get_processor_id() % cpu_slots_;
    for (uint64_t round = 0; round < slice_count_; round++) {
        uint64_t idx = (blk_.preferred[cpu] + round) % slice_count_;
        bitmap_slice_t &s = blk_.slices[idx];
        if (!s.slice_lock.try_lock_write()) continue;
        uint64_t out = 0, left = require_count;
        result_code_t rc = s.alloc_soft(0, require_count, out, left);
        s.slice_lock.unlock_write();
        if (rc == RC_OK) {
            blk_.preferred[cpu] = (uint32_t)idx;
            if (remain_count) *remain_count = left;
            return out;
        }
    }
    return PTR_INVALID;
}

/* 软锚定占领（定点对·路由版）：blkbase 起认领 ≤ require_count 连续空闲块。
 * 锚点片唯一 → 自旋强制抢锁（不可换片，纪律同 block_free）；片尾跨界在
 * 内部续圈（逐片单锁前缀链 = 真实连续，见头文件契约）；上层零切片知识。
 * 返回 blkbase（原址）/ PTR_INVALID；*rest_count 出 = 缺口。 */
uint64_t bitmap_slices_mgr_t::block_occupy_soft(uint64_t blkbase,
                                                uint64_t require_count,
                                                uint64_t *rest_count) {
    if (rest_count) *rest_count = require_count;
    if (require_count == 0 || !blk_.slices || slice_count_ == 0)
        return PTR_INVALID;
    if (blk_.backing_mode == BITMAP_BACKING_CHUNK)        /* 03 §1.2 分叉：FULL 走旧码原样 */
        return chunk_occupy_soft(blk_, blkbase, require_count, rest_count);
    const uint64_t nbits = blk_.desc.bits_count;

    uint64_t base = blkbase, remain = require_count;
    while (remain > 0 && base < nbits) {
        const uint64_t idx = base / blk_.per_slice_bits;
        if (idx >= slice_count_) break;
        bitmap_slice_t &s = blk_.slices[idx];
        while (!s.slice_lock.try_lock_write()) { }        /* 自旋强制抢：锚点不可放弃 */
        uint64_t left = 0;
        result_code_t rc = s.soft_occupy(base, remain, &left);
        s.slice_lock.unlock_write();
        if (rc != RC_OK) break;                           /* 锚点即占用：连续前缀断，不跨 */
        const uint64_t claimed = remain - left;
        base += claimed;
        remain = left;
        if (remain == 0) break;
        const uint64_t slice_end = (idx + 1) * blk_.per_slice_bits < nbits
                                       ? (idx + 1) * blk_.per_slice_bits : nbits;
        if (base != slice_end) break;                     /* 中途撞占用停：跨界无意义 */
        /* base == slice_end == 下一片 base 0 → 续圈（循环顶重定位） */
    }

    if (rest_count) *rest_count = remain;
    return (require_count - remain > 0) ? blkbase : PTR_INVALID;
}

/* 区间释放（跨片循环）：blkbase 起 count 块清位。物理连续的 run 可能跨片
 * （block_occupy_soft 的跨界产物）——逐段定位所属片释放；释放无连续
 * 要求，逐段清位即可，无需跨片双持。单片路径与旧版行为一致。 */
void bitmap_slices_mgr_t::block_free(uint64_t blkbase, uint64_t count) {
    if (count == 0 || !blk_.slices || slice_count_ == 0) return;
    if (blk_.backing_mode == BITMAP_BACKING_CHUNK) {      /* 03 §1.2 分叉：FULL 走旧码原样 */
        chunk_free(blk_, blkbase, count);
        return;
    }
    const uint64_t nbits = blk_.desc.bits_count;
    while (count > 0) {
        if (blkbase >= nbits) return;                     /* 防御：越界剩余放弃 */
        const uint64_t idx = blkbase / blk_.per_slice_bits;
        if (idx >= slice_count_) return;
        const uint64_t slice_end = (idx + 1) * blk_.per_slice_bits < nbits
                                       ? (idx + 1) * blk_.per_slice_bits : nbits;
        const uint64_t n = (blkbase + count <= slice_end) ? count : slice_end - blkbase;
        if (n == 0) return;
        bitmap_slice_t &s = blk_.slices[idx];
        while (!s.slice_lock.try_lock_write()) { }        /* 释放不可丢，自旋（原纪律不变） */
        s.free_interval(blkbase, n);
        s.slice_lock.unlock_write();
        blkbase += n;
        count -= n;
    }
}

bool bitmap_slices_mgr_t::block_test(uint64_t blk) const {
    if (blk_.backing_mode == BITMAP_BACKING_CHUNK)        /* 03 §1.1 语义不变：CHUNK 路由裁决 */
        return chunk_test(blk_, blk);
    if (!blk_.desc.bitmap_raw) return false;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(blk_.desc.bitmap_raw);
    return (raw[blk / 8] & (1u << (blk % 8))) != 0;
}

/* ── inode 组 ── */

uint64_t bitmap_slices_mgr_t::inode_alloc() {
    if (!ino_.slices || slice_count_ == 0 ||
        cpu_slots_ == 0 || !ino_.preferred) return PTR_INVALID;
    if (ino_.backing_mode == BITMAP_BACKING_CHUNK)        /* 03 §1.2 分叉：FULL 走旧码原样 */
        return chunk_alloc_soft(ino_, 1, nullptr);         /* 单槽特化 = alloc_soft(0,1)（同 FULL） */
    const uint32_t cpu = get_processor_id() % cpu_slots_;
    for (uint64_t round = 0; round < slice_count_; round++) {
        uint64_t idx = (ino_.preferred[cpu] + round) % slice_count_;
        bitmap_slice_t &s = ino_.slices[idx];
        if (!s.slice_lock.try_lock_write()) continue;
        uint64_t out = 0, left = 0;
        result_code_t rc = s.alloc_soft(0, 1, out, left);
        s.slice_lock.unlock_write();
        if (rc == RC_OK) { ino_.preferred[cpu] = (uint32_t)idx; return out; }
    }
    return PTR_INVALID;
}

void bitmap_slices_mgr_t::inode_free(uint64_t inum) {
    if (!ino_.slices || slice_count_ == 0) return;
    if (ino_.backing_mode == BITMAP_BACKING_CHUNK) {      /* 03 §1.2 分叉：FULL 走旧码原样 */
        chunk_free(ino_, inum, 1);
        return;
    }
    uint64_t idx = inum / ino_.per_slice_bits;
    if (idx >= slice_count_) return;
    bitmap_slice_t &s = ino_.slices[idx];
    while (!s.slice_lock.try_lock_write()) { }            /* 释放不可丢，自旋等 */
    s.free_interval(inum, 1);
    s.slice_lock.unlock_write();
}

bool bitmap_slices_mgr_t::inode_test(uint64_t inum) const {
    if (ino_.backing_mode == BITMAP_BACKING_CHUNK)        /* 03 §1.1 语义不变：CHUNK 路由裁决 */
        return chunk_test(ino_, inum);
    if (!ino_.desc.bitmap_raw) return false;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(ino_.desc.bitmap_raw);
    return (raw[inum / 8] & (1u << (inum % 8))) != 0;
}

/* ── 审计 ── */

uint64_t bitmap_slices_mgr_t::block_free_total() const {
    uint64_t sum = 0;
    if (!blk_.slices) return 0;
    for (uint64_t i = 0; i < slice_count_; i++) sum += blk_.slices[i].get_free_count();
    return sum;
}

uint64_t bitmap_slices_mgr_t::inode_free_total() const {
    uint64_t sum = 0;
    if (!ino_.slices) return 0;
    for (uint64_t i = 0; i < slice_count_; i++) sum += ino_.slices[i].get_free_count();
    return sum;
}

uint32_t bitmap_slices_mgr_t::block_preferred_of(uint32_t cpu) const {
    if (!blk_.preferred || cpu >= cpu_slots_) return 0;
    return blk_.preferred[cpu];
}

uint32_t bitmap_slices_mgr_t::inode_preferred_of(uint32_t cpu) const {
    if (!ino_.preferred || cpu >= cpu_slots_) return 0;
    return ino_.preferred[cpu];
}

/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slice_t — CHUNK 后端（03 §5/§8；2026-09-28 刀①：换片状态机 + 窗口化）
 *
 * 锁纪律例外（头文件已声明）：本节 chunk_* 方法自管锁——try 语义同 god（忙 →
 * RC_CHUNK_BUSY 交路由换片），free/定点走自旋强抢；一切盘 I/O 在 slice_lock 之外、
 * 持 chg_mu 下发起（§10.3 铁律）；快路径不碰 chg_mu（§5，与 FULL 同速）。
 *
 * §10.3 可测化：本节所有 slice_lock 取/放均经 slock_* 包装维护本线程深度计数，
 * 导出 ravenfs_bitmap_in_slice_lock() 供 mock chunk_io 断言「I/O 永不进 slice_lock」。
 * ═══════════════════════════════════════════════════════════════════ */

namespace {
thread_local int t_slock_depth = 0;   /* 本线程 slice_lock 持有深度（审计钩子） */

/* CHUNK 临界区包装（自旋强抢 / try 两种）——取放必须成对，深度才准 */
inline void slock_acquire(bitmap_slice_t &s) {
    while (!s.slice_lock.try_lock_write()) cpu_relax();
    t_slock_depth++;
}
inline bool slock_try(bitmap_slice_t &s) {
    if (!s.slice_lock.try_lock_write()) return false;
    t_slock_depth++;
    return true;
}
inline void slock_release(bitmap_slice_t &s) {
    t_slock_depth--;
    s.slice_lock.unlock_write();
}
}  // namespace

extern "C" bool ravenfs_bitmap_in_slice_lock(void) { return t_slock_depth > 0; }

namespace {
/* §6 record 节点池：per-group 共享 LIFO 的建/拆（预建数组 + 下标空闲链，
 * 头 = {tag32|idx32}）→ slice_lock 内取/还零堆分配零睡眠（§10.3）。 */
rec_pool_t *rec_pool_create(uint32_t depth) {
    if (depth == 0) return nullptr;
    rec_node *nodes = nullptr;
    uint32_t *link = nullptr;
    rec_pool_t *pool = nullptr;
    try {
        nodes = new rec_node[depth];
        link  = new uint32_t[depth];
        pool  = new rec_pool_t();
    } catch (const std::bad_alloc &) {
        delete[] nodes;
        delete[] link;
        delete pool;
        return nullptr;
    }
    pool->nodes = nodes;
    pool->free_link = link;
    pool->cap = depth;
    for (uint32_t i = 0; i + 1 < depth; i++) link[i] = i + 1;
    link[depth - 1] = rec_pool_t::NIL;
    pool->head.store(0, std::memory_order_release);   /* tag=0, idx=0 */
    return pool;
}

void rec_pool_destroy(rec_pool_t *pool) {
    if (!pool) return;
    delete[] pool->nodes;
    delete[] pool->free_link;
    delete pool;
}
}  // namespace

/* 原子 LIFO 取/还节点（slice_lock 内调用：零堆分配零睡眠，§10.3）；
 * 池空 → nullptr，交调用方走“强制上线直清”兜底（不产生第二批记录，§6/§10.9）。 */
rec_node *rec_pool_t::acquire() {
    for (;;) {
        uint64_t h = head.load(std::memory_order_acquire);
        const uint32_t idx = (uint32_t)(h & 0xFFFFFFFFu);
        if (idx == NIL) return nullptr;
        const uint32_t nxt = free_link[idx];
        const uint64_t nh = (((h >> 32) + 1) << 32) | nxt;   /* tag+1 抗 ABA */
        if (head.compare_exchange_weak(h, nh, std::memory_order_acq_rel,
                                       std::memory_order_acquire))
            return &nodes[idx];
    }
}

void rec_pool_t::release(rec_node *n) {
    const uint32_t idx = (uint32_t)(n - nodes);
    if (idx >= cap) return;                                  /* 防御：非本池节点不收 */
    for (;;) {
        uint64_t h = head.load(std::memory_order_acquire);
        free_link[idx] = (uint32_t)(h & 0xFFFFFFFFu);
        const uint64_t nh = (((h >> 32) + 1) << 32) | idx;
        if (head.compare_exchange_weak(h, nh, std::memory_order_acq_rel,
                                       std::memory_order_acquire))
            return;
    }
}

/* ── §4 装配：几何登记 + 挂 backing（不碰常驻全图，raw 可为 null）──
 * 与 enable_slice 的差异：free_count 留 0 由 mount 顺序扫描填（计数不驻留，§7）；
 * slice_base 只是 ctl0 几何占位（online=NONE 期间任何路径不解引用它）。 */
result_code_t bitmap_slice_t::enable_slice_chunked(
    fs_bitmap_descriptor_t *global_descriptor, uint64_t base_bit, uint64_t bitscount,
    slice_backing_t *bk) {
    if (!global_descriptor || !bk || !bk->buffer || !bk->ctls || !bk->io || !bk->pool)
        return RC_ERR_STATE;   /* 池是 drain/挂账的零分配保证（§6），缺则拒装 */
    if (base_bit >= global_descriptor->bits_count) return RC_ERR_STATE;
    if (bitscount == 0 || bitscount > global_descriptor->bits_count - base_bit)
        return RC_ERR_STATE;
    if (base_bit & 32767) return RC_ERR_STATE;               /* 片首 4K 对齐（同 FULL 口径） */
    if (bk->chunks_per_slice == 0) return RC_ERR_STATE;

    parent_descriptor = global_descriptor;
    base_idx = base_bit;
    slice_bitscount = bitscount;
    slice_base = bk->buffer;                                 /* ctl0 占位；发布时按窗重指（§4） */
    free_count = 0;
    last_finish_hint = 0;
    backing = bk;
    bk->online_chunk = BITMAP_CHUNK_NONE;                    /* mount 扫描后仍全离线 */
    bk->gen = 0;
    bk->rec_head = nullptr;
    return RC_OK;
}

/* §3 索引合成：chunk_bits = 32768<<order，位移 15+order；尾窗夹到片尾
 *（tail 恒 0 或 ≥1 整页 → 不存在 sub-page 尾块，位移/掩码全链路安全）。 */
void bitmap_slice_t::ctl_window(uint32_t ctl, uint64_t *wlo, uint64_t *whi) const {
    const uint64_t lo = (uint64_t)ctl << (15 + backing->chunk_order);
    uint64_t hi = lo + ((uint64_t)32768 << backing->chunk_order);
    if (hi > slice_bitscount) hi = slice_bitscount;
    *wlo = lo;
    *whi = hi;
}

/* §7 摘要重算（须持 slice_lock）：链上有待还 record → 恒 MIXED（§10.5 防
 * 错误 ENOSPC）；src = buffer（evict）或 snapshot（TTL 态 2 写回后）。 */
void bitmap_slice_t::recompute_summary_locked(uint32_t ctl, const void *src) {
    slice_backing_t *bk = backing;
    chunk_ctl_t &c = bk->ctls[ctl];
    if (c.has_recs) { c.summary = CHUNK_SUM_MIXED; return; }   /* 降级凭据优先（§10.5） */
    uint64_t wlo = 0, whi = 0;
    ctl_window(ctl, &wlo, &whi);
    const uint64_t *words = reinterpret_cast<const uint64_t *>(src);
    uint64_t ones = 0;
    for (uint64_t w = 0; w < ((whi - wlo) >> 6); w++)
        ones += (uint64_t)__builtin_popcountll(words[w]);
    c.summary = (ones == whi - wlo) ? CHUNK_SUM_ALL_USED
              : (ones == 0)         ? CHUNK_SUM_ALL_FREE
                                    : CHUNK_SUM_MIXED;
}

/* 在线窗直清 [from,to)（须持 slice_lock 且窗在线）：按实际 1→0 计入 free_count
 *（同 FULL 口径，重复释放不虚高）+ 置 dirty（TTL 按脏回写，§9） */
void bitmap_slice_t::clear_bits_locked(uint64_t from, uint64_t to) {
    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
    uint64_t cleared = 0;
    for (uint64_t b = from; b < to; b++) {
        if (bit_get(words, b)) { bit_clear(words, b); cleared++; }
    }
    free_count += cleared;
    const uint32_t sh = (uint32_t)(15 + backing->chunk_order);
    backing->ctls[(uint32_t)(from >> sh)].dirty = 1;
}

/* ★ §5/§6 record 还账核心（须持 slice_lock；零 I/O）：链 ∩ 本窗 1→0 清位；
 * 整段覆盖 → 摘节点、单侧伸出 → 收缩留余、两侧都伸 → 尽力劈分（池空时保留
 * 整节点：清位已做且幂等，下次上线再缩/摘 → 不丢，§10.7）。
 * target_words 下标 = slice_off − wlo（发布时 = buffer；TTL 态 2 = snapshot——
 * buffer 归线上窗所有，绝不触碰）。free_count 已在挂 record 时记账，此处不动。
 * 返回本窗实际 1→0 清位数（>0 ⇒ 发布须置脏，§10.6）。 */
uint64_t bitmap_slice_t::drain_window_locked(uint32_t ctl, uint64_t *target_words) {
    slice_backing_t *bk = backing;
    uint64_t wlo = 0, whi = 0;
    ctl_window(ctl, &wlo, &whi);
    rec_node **pp = &bk->rec_head;
    bool kept = false;
    uint64_t cleared = 0;
    while (*pp) {
        rec_node *n = *pp;
        const uint64_t ns = n->slice_off;
        const uint64_t ne = n->slice_off + n->len;
        if (ne <= wlo || ns >= whi) { pp = &n->next; continue; }
        const uint64_t is = ns > wlo ? ns : wlo;
        const uint64_t ie = ne < whi ? ne : whi;
        for (uint64_t b = is; b < ie; b++)
            if (bit_get(target_words, b - wlo)) { bit_clear(target_words, b - wlo); cleared++; }
        if (ns >= wlo && ne <= whi) {                       /* 整段覆盖 → 摘节点 */
            *pp = n->next;
            bk->pool->release(n);
            continue;                                        /* pp 已指向后继，不推进 */
        }
        if (ns >= wlo) {                                    /* 尾伸窗外 → 收缩留尾 */
            n->slice_off = whi;
            n->len = ne - whi;
            pp = &n->next;
        } else if (ne <= whi) {                             /* 头伸窗外 → 收缩留头 */
            n->len = wlo - ns;
            pp = &n->next;
        } else {                                            /* 两侧伸窗外 → 劈分（§5 收缩的双余特例） */
            rec_node *tail = bk->pool->acquire();
            if (tail) {
                tail->slice_off = whi;
                tail->len = ne - whi;
                tail->next = n->next;
                n->len = wlo - ns;                          /* 头余 [ns, wlo) */
                n->next = tail;
                pp = &tail->next;
            } else {                                        /* 池空：保留整节点（幂等，不丢） */
                kept = true;
                pp = &n->next;
            }
        }
    }
    /* 本窗交集已全部还清（池空保留者仍覆盖 → 保持 1） */
    bk->ctls[ctl].has_recs = kept ? 1 : 0;
    return cleared;
}

/* ══ §5 慢路径：换片状态机（盘 I/O 永不进 slice_lock）═══
 * 全序列串行在 chg_mu 下：下线旧窗（锁内快照+清 dirty，online→NONE）→ evict 写
 *（锁外）→ 读目标窗（锁外）→ ★ 发布前先 drain record（§5/§6 顺序铁律，刀②接账）→
 * 发布。
 * 下线必须置 NONE：否则快路径会在快照后继续改 buffer，随后的 read 覆盖这些并发
 * 变更 = 双分配方向（§10.2 补全；NONE 期间快路径全被 chg_mu 挡住）。
 * 写失败 → 恢复旧窗在线 + 复脏（buffer 仍是旧窗真相，状态零损坏；失败方向 = 泄漏）。 */
result_code_t bitmap_slice_t::chunk_ensure_online(uint32_t ctl) {
    slice_backing_t *bk = backing;
    if (!bk || ctl >= bk->chunks_per_slice) return RC_ERR_STATE;
    std::lock_guard<bitmap_mutex_t> guard(bk->chg_mu);   /* 换片互斥（可睡眠，§4） */

    uint64_t tlo = 0, thi = 0;
    ctl_window(ctl, &tlo, &thi);

    uint32_t old = BITMAP_CHUNK_NONE;
    bool need_write = false;
    slock_acquire(*this);
    if (bk->online_chunk == ctl) {                        /* 复检：别人已换上 ✓（§5） */
        slock_release(*this);
        return RC_OK;
    }
    old = bk->online_chunk;
    if (old != BITMAP_CHUNK_NONE) {
        chunk_ctl_t &oc = bk->ctls[old];
        if (oc.dirty) {
            uint64_t olo = 0, ohi = 0;
            ctl_window(old, &olo, &ohi);
            memcpy(bk->snapshot, bk->buffer, (ohi - olo) / 8);
            oc.dirty = 0;
            need_write = true;
        }
        bk->online_chunk = BITMAP_CHUNK_NONE;             /* 下线（gen++，§4）：buffer 冻结 */
        bk->gen++;
    }
    slock_release(*this);

    if (need_write) {                                     /* evict 写（锁外、持 chg_mu） */
        uint64_t olo = 0, ohi = 0;
        ctl_window(old, &olo, &ohi);
        if (bk->io->write(bk->io_ctx, bk->is_inode, bk->ctls[old].disk_block_rel,
                          bk->snapshot, (ohi - olo) / 32768) != 0) {
            slock_acquire(*this);
            if (bk->online_chunk == BITMAP_CHUNK_NONE) {
                bk->online_chunk = old;
                bk->gen++;
                bk->ctls[old].dirty = 1;
                recompute_summary_locked(old, bk->buffer); /* 保守重算（盘态可能部分写） */
            }
            slock_release(*this);
            return RC_ERR_STATE;
        }
        slock_acquire(*this);
        recompute_summary_locked(old, bk->buffer);         /* evict 时从 buffer 重算（§7） */
        slock_release(*this);
    }

    /* 目标窗读入（锁外、持 chg_mu）；失败 → 旧窗已安全下线，保持 NONE 可重试。
     * §7 摘要 ALL_FREE 且无 pending record → 盘态全 0，免读直置空窗（整段可判
     * 空闲 = §7 摘要表的免载入收益）；否则真实读盘。 */
    if (bk->ctls[ctl].summary == CHUNK_SUM_ALL_FREE && !bk->ctls[ctl].has_recs) {
        memset(bk->buffer, 0, (thi - tlo) / 8);
    } else if (bk->io->read(bk->io_ctx, bk->is_inode, bk->ctls[ctl].disk_block_rel,
                            bk->buffer, (thi - tlo) / 32768) != 0) {
        return RC_ERR_STATE;
    }

    slock_acquire(*this);
    /* ★ 发布前先还账（§5/§6 顺序铁律）：drain 清位数 >0 ⇒ buffer ≠ 盘态 ⇒ 置脏，
     * 否则 evict 按 dirty==0 跳写 = 已还账的 free 永不落盘（§10.6）。 */
    const uint64_t drained =
        drain_window_locked(ctl, reinterpret_cast<uint64_t *>(bk->buffer));
    bk->ctls[ctl].dirty = drained ? 1 : 0;
    bk->online_chunk = ctl;
    bk->gen++;                                            /* 发布（§4） */
    slice_base = bk->buffer - tlo / 8;                     /* §4 重指：slice_base+off 落进 buffer */
    slock_release(*this);
    return RC_OK;
}

/* 快/慢路径外壳（§5）：每窗一次调用。快路径 = 锁内复检在线窗（纯内存 HIT）；
 * 慢路径 = 摘要裁决（ALL_USED 且无 record → 免载入跳过，§7）→ chg_mu 换片 →
 * 重试（有限次，防同片换片打摆）。返回 RC_OK 时**已持 slice_lock 且窗已上线**。 */
result_code_t bitmap_slice_t::chunk_lock_window(uint32_t ctl) {
    slice_backing_t *bk = backing;
    if (!bk || ctl >= bk->chunks_per_slice) return RC_ERR_STATE;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!slock_try(*this)) return RC_CHUNK_BUSY;            /* god try：锁忙换片 */
        if (bk->online_chunk == ctl) return RC_OK;              /* 快路径 HIT */
        const chunk_ctl_t &c = bk->ctls[ctl];
        if (c.summary == CHUNK_SUM_ALL_USED && !c.has_recs) {   /* 摘要跳过（§7/§10.5） */
            slock_release(*this);
            return RC_ERR_NO_BLOCK;
        }
        slock_release(*this);
        if (chunk_ensure_online(ctl) != RC_OK) return RC_ERR_STATE;  /* 慢路径换片 */
    }
    return RC_CHUNK_BUSY;                                       /* 换片反复被抢 → 换片 */
}

/* ══ §8 单窗原语（调用方已持 slice_lock 且窗已上线）：口径与 FULL 版完全
 * 一致，仅扫描区间夹到本窗 —— FULL 函数逐字未动（红线），此为镜像实现。 ══ */

result_code_t bitmap_slice_t::window_alloc_hard(uint64_t from, uint64_t to,
                                                uint8_t align_log2, uint64_t bitscount,
                                                uint64_t &out) {
    if (!slice_base || !parent_descriptor) return RC_ERR_STATE;
    if (bitscount == 0 || bitscount > slice_bitscount) return RC_ERR_STATE;
    if (from >= to || bitscount > to - from) return RC_ERR_NO_BLOCK;  /* 窗内放不下 */
    uint64_t align = 1ULL << (align_log2 > 16 ? 16 : align_log2);     /* 防御截断（同 FULL） */
    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
    const uint64_t hit = find_free_run(words, to, from, bitscount, align);
    if (hit == SLICE_NO_HIT) return RC_ERR_NO_BLOCK;
    for (uint64_t i = 0; i < bitscount; i++) bit_set(words, hit + i);
    free_count -= bitscount;
    last_finish_hint = hit + bitscount;
    out = base_idx + hit;
    return RC_OK;
}

result_code_t bitmap_slice_t::window_alloc_soft(uint64_t from, uint64_t to,
                                                uint8_t align_log2, uint64_t bitscount,
                                                uint64_t &out, uint64_t &bits_left) {
    bits_left = bitscount;
    if (!slice_base || !parent_descriptor) return RC_ERR_STATE;
    if (bitscount == 0) return RC_ERR_STATE;
    if (from >= to) return RC_ERR_NO_BLOCK;
    uint64_t align = 1ULL << (align_log2 > 16 ? 16 : align_log2);
    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
    const uint64_t cap = bitscount > align ? bitscount : align;
    uint64_t seg_s = 0, seg_e = 0, hit = SLICE_NO_HIT;
    if (next_free_seg(words, to, from, cap, &seg_s, &seg_e)) {
        uint64_t cand = slice_align_up(seg_s, align);
        if (cand < seg_e) hit = cand;
    }
    if (hit == SLICE_NO_HIT) return RC_ERR_NO_BLOCK;
    uint64_t got = seg_e - hit;
    if (got > bitscount) got = bitscount;
    for (uint64_t i = 0; i < got; i++) bit_set(words, hit + i);
    free_count -= got;
    last_finish_hint = hit + got;
    out = base_idx + hit;
    bits_left = bitscount - got;
    return RC_OK;
}

/* 贪心前缀（镜像 soft_occupy，差异仅 cap 另受窗尾 whi 夹断，§8 窗口化）：
 * 口径不变 = *rest 入=已满足量、出=len−claimed 总缺口；撞占用停、窗尾如实回传。 */
result_code_t bitmap_slice_t::window_soft_occupy(uint64_t wlo, uint64_t whi,
                                                 uint64_t base, uint64_t len,
                                                 uint64_t *rest_bits) {
    if (!slice_base || !parent_descriptor || !rest_bits) return RC_ERR_STATE;
    if (*rest_bits > len) return RC_ERR_STATE;
    if (len == 0) return RC_OK;
    const uint64_t real = len - *rest_bits;
    if (real == 0) return RC_OK;
    if (base < base_idx) return RC_ERR_STATE;
    const uint64_t off = base - base_idx;
    if (off < wlo || off >= whi || off >= slice_bitscount) return RC_ERR_STATE;  /* 窗口守卫（§4） */
    const uint64_t bound = whi < slice_bitscount ? whi : slice_bitscount;
    const uint64_t cap = (real < bound - off) ? real : bound - off;  /* 窗尾/片尾夹断 */
    if (cap == 0) return RC_ERR_NO_BLOCK;

    uint64_t *words = reinterpret_cast<uint64_t *>(slice_base);
    const uint64_t nwords = (slice_bitscount + 63) >> 6;
    uint64_t k = 0;
    while (k < cap) {
        const uint64_t wi = (off + k) >> 6;
        if (wi >= nwords) break;                           /* 防御（cap 已夹断，不可达） */
        uint64_t eword = ~words[wi];                       /* 1 = 空闲 */
        if (wi == nwords - 1 && (slice_bitscount & 63))
            eword &= (1ULL << (slice_bitscount & 63)) - 1;
        const uint64_t pos = (off + k) & 63;
        const uint64_t free_mask = eword >> pos;
        if ((free_mask & 1) == 0) break;                   /* 当前位占用 → 前缀断 */
        uint64_t run = (free_mask == ~0ULL) ? (64 - pos) : __builtin_ctzll(~free_mask);
        k += run;
        if (k > cap) k = cap;
    }
    if (k == 0) { *rest_bits = len; return RC_ERR_NO_BLOCK; }

    for (uint64_t i = 0; i < k; i++) bit_set(words, off + i);
    free_count -= k;
    if (off + k > last_finish_hint) last_finish_hint = off + k;  /* 只进不退 */
    *rest_bits = len - k;
    return RC_OK;
}

/* ══ §8 路由层窗口迭代器（alloc/occupy 外壳；每窗 = 快/慢路径 + 单窗原语）═══ */

/* 硬分配：hint 窗起顺序向片尾；扫到片尾未中 → 绕回 [0,hint) 窗序列再来一遍
 *（02 §4 绕回语义不变）。单窗 find ⇒ 分配不跨 chunk（§5 单 chunk 上线的推论，
 * A/B 记账项：FULL 无此界）。 */
result_code_t bitmap_slice_t::chunk_alloc_hard(uint8_t align_log2, uint64_t bitscount,
                                               uint64_t &out) {
    if (!backing || !parent_descriptor) return RC_ERR_STATE;
    if (bitscount == 0 || bitscount > slice_bitscount) return RC_ERR_STATE;
    if (backing->chunks_per_slice == 0) return RC_ERR_STATE;
    if (!slock_try(*this)) return RC_CHUNK_BUSY;            /* god try：锁忙换片 */
    const uint64_t hint = last_finish_hint;
    slock_release(*this);

    const uint32_t nwin = backing->chunks_per_slice;
    const uint32_t sh = (uint32_t)(15 + backing->chunk_order);
    const uint32_t hctl = ((uint64_t)hint >= ((uint64_t)nwin << sh))
                              ? nwin - 1 : (uint32_t)(hint >> sh);

    for (uint32_t c = hctl; c < nwin; c++) {                 /* 正向：hint 窗 → 片尾 */
        uint64_t wlo = 0, whi = 0;
        ctl_window(c, &wlo, &whi);
        const uint64_t from = (c == hctl && hint > wlo) ? hint : wlo;
        if (from >= whi) continue;
        result_code_t rc = chunk_lock_window(c);
        if (rc == RC_ERR_NO_BLOCK) continue;                 /* 摘要 ALL_USED 跳过（§7） */
        if (rc != RC_OK) return rc;                          /* BUSY/STATE → 路由换片 */
        rc = window_alloc_hard(from, whi, align_log2, bitscount, out);
        if (rc == RC_OK) backing->ctls[c].dirty = 1;   /* 置位同样是变更（§4/§9，丢了 = 双分配） */
        slock_release(*this);
        if (rc != RC_ERR_NO_BLOCK) return rc;                /* OK / STATE */
    }
    for (uint32_t c = 0; c <= hctl; c++) {                   /* 绕回：[0, hint) 窗序列 */
        uint64_t wlo = 0, whi = 0;
        ctl_window(c, &wlo, &whi);
        const uint64_t to = (c == hctl) ? (hint < whi ? hint : whi) : whi;
        if (wlo >= to) continue;
        result_code_t rc = chunk_lock_window(c);
        if (rc == RC_ERR_NO_BLOCK) continue;
        if (rc != RC_OK) return rc;
        rc = window_alloc_hard(wlo, to, align_log2, bitscount, out);
        if (rc == RC_OK) backing->ctls[c].dirty = 1;   /* 置位同样是变更（§4/§9） */
        slock_release(*this);
        if (rc != RC_ERR_NO_BLOCK) return rc;
    }
    return RC_ERR_NO_BLOCK;                                  /* 全片扫毕（含绕回）= SLICE_EMPTY */
}

/* 软分配：同硬分配的两趟窗序列（02 §4：正向首段命中即中，绕回补 [0,hint)） */
result_code_t bitmap_slice_t::chunk_alloc_soft(uint8_t align_log2, uint64_t bitscount,
                                               uint64_t &out, uint64_t &bits_left) {
    bits_left = bitscount;
    if (!backing || !parent_descriptor) return RC_ERR_STATE;
    if (bitscount == 0) return RC_ERR_STATE;
    if (backing->chunks_per_slice == 0) return RC_ERR_STATE;
    if (!slock_try(*this)) return RC_CHUNK_BUSY;
    const uint64_t hint = last_finish_hint;
    slock_release(*this);

    const uint32_t nwin = backing->chunks_per_slice;
    const uint32_t sh = (uint32_t)(15 + backing->chunk_order);
    const uint32_t hctl = ((uint64_t)hint >= ((uint64_t)nwin << sh))
                              ? nwin - 1 : (uint32_t)(hint >> sh);

    for (uint32_t c = hctl; c < nwin; c++) {
        uint64_t wlo = 0, whi = 0;
        ctl_window(c, &wlo, &whi);
        const uint64_t from = (c == hctl && hint > wlo) ? hint : wlo;
        if (from >= whi) continue;
        result_code_t rc = chunk_lock_window(c);
        if (rc == RC_ERR_NO_BLOCK) continue;
        if (rc != RC_OK) return rc;
        rc = window_alloc_soft(from, whi, align_log2, bitscount, out, bits_left);
        if (rc == RC_OK) backing->ctls[c].dirty = 1;   /* 置位同样是变更（§4/§9） */
        slock_release(*this);
        if (rc != RC_ERR_NO_BLOCK) return rc;
    }
    for (uint32_t c = 0; c <= hctl; c++) {
        uint64_t wlo = 0, whi = 0;
        ctl_window(c, &wlo, &whi);
        const uint64_t to = (c == hctl) ? (hint < whi ? hint : whi) : whi;
        if (wlo >= to) continue;
        result_code_t rc = chunk_lock_window(c);
        if (rc == RC_ERR_NO_BLOCK) continue;
        if (rc != RC_OK) return rc;
        rc = window_alloc_soft(wlo, to, align_log2, bitscount, out, bits_left);
        if (rc == RC_OK) backing->ctls[c].dirty = 1;   /* 置位同样是变更（§4/§9） */
        slock_release(*this);
        if (rc != RC_ERR_NO_BLOCK) return rc;
    }
    return RC_ERR_NO_BLOCK;
}

/* 锚定占领窗口迭代器（§8 occupy 路由）：锚点窗起逐窗贪心前缀，顶到窗尾换下一窗
 *（跨 chunk 续圈 = 原跨片续圈同构）；撞占用（含摘要 ALL_USED 判定的锚点无货）
 * 如实停手不硬跳。*rest_bits 口径与 soft_occupy 完全一致（出 = len − 本链累计认领）。 */
result_code_t bitmap_slice_t::chunk_soft_occupy(uint64_t base, uint64_t len,
                                                uint64_t *rest_bits) {
    if (!backing || !parent_descriptor || !rest_bits) return RC_ERR_STATE;
    if (*rest_bits > len) return RC_ERR_STATE;
    if (base < base_idx) return RC_ERR_STATE;
    if (len == 0) return RC_OK;
    if (*rest_bits == len) return RC_OK;                    /* real_occupy == 0 */
    if (backing->chunks_per_slice == 0) return RC_ERR_STATE;
    const uint64_t start = base - base_idx;
    if (start >= slice_bitscount) return RC_ERR_STATE;

    const uint32_t sh = (uint32_t)(15 + backing->chunk_order);
    const uint64_t rest0 = *rest_bits;
    uint32_t c = (uint32_t)(start >> sh);                   /* 锚点窗起 */
    uint64_t anchor = start;                                /* 当前窗锚点（片内偏移） */
    uint64_t gap = len - rest0;                             /* 本链待认领（real_occupy 口径） */
    while (c < backing->chunks_per_slice) {
        uint64_t wlo = 0, whi = 0;
        ctl_window(c, &wlo, &whi);
        result_code_t rc = chunk_lock_window(c);
        if (rc == RC_ERR_NO_BLOCK) break;                   /* 摘要 ALL_USED：锚点窗无货 → 前缀断 */
        if (rc != RC_OK) return rc;                         /* BUSY/STATE：换片被抢 → 如实回传 */
        uint64_t wr = 0;                                    /* 入 = 本链已满足量 0（FULL 口径：real = len − *rest）；出 = 窗后剩余 */
        rc = window_soft_occupy(wlo, whi, base_idx + anchor, gap, &wr);
        if (rc != RC_ERR_STATE && gap > wr)             /* 认领了位 = 变更（§4/§9 dirty） */
            backing->ctls[c].dirty = 1;
        slock_release(*this);
        if (rc == RC_ERR_STATE) return rc;
        const uint64_t claimed = gap - wr;
        gap = wr;
        *rest_bits = rest0 + gap;                           /* FULL 恒等式：出 = len − 累计认领 */
        if (claimed == 0) break;                            /* 锚点即占用：前缀断（§8） */
        if (gap == 0) return RC_OK;                         /* 全额认领，收工 */
        anchor += claimed;
        if (anchor < whi) return RC_OK;                     /* 撞占用停：不跨窗硬续 */
        c++;                                                /* 顶到窗尾 → 下一窗续圈 */
    }
    return RC_OK;                                           /* rest 如实（片尾/占用断） */
}

/* §6 区间释放：在线直改（dirty=1，零 record 零 I/O）/ 离线挂 record（零 I/O、
 * O(1)）/ 池空兜底强制上线直清（零第二批记录，§6/§10.9）。自管锁（兜底须解锁
 * 走慢路径），纪律同 FULL：自旋强抢、释放不可丢。 */
result_code_t bitmap_slice_t::chunk_free_interval(uint64_t base, uint64_t len) {
    if (!backing || !parent_descriptor) return RC_ERR_STATE;
    if (base < base_idx) return RC_ERR_STATE;
    uint64_t pos = base - base_idx;
    if (pos + len > slice_bitscount) return RC_ERR_STATE;
    if (len == 0) return RC_OK;

    const uint32_t sh = (uint32_t)(15 + backing->chunk_order);
    uint64_t remaining = len;
    int guard = 0;                                          /* 兜底后被换片的有限重试 */
    while (remaining > 0) {
        const uint32_t c = (uint32_t)(pos >> sh);
        uint64_t wlo = 0, whi = 0;
        ctl_window(c, &wlo, &whi);
        const uint64_t nwin = (pos + remaining <= whi) ? remaining : whi - pos;

        slock_acquire(*this);                               /* 释放不可丢：自旋（FULL 同纪律） */
        if (backing->online_chunk == c) {
            /* 在线 free → 直接改内存 + dirty（§1.4；零 record 零 I/O） */
            clear_bits_locked(pos, pos + nwin);
            slock_release(*this);
            pos += nwin; remaining -= nwin; guard = 0;
            continue;
        }
        /* 离线 free → 池内取节点挂片级链（§6；free_count 同步记账）。记录区间
         * 跨连续离线窗合并为单节点（§4：单次 free 每片至多一节点，长 run 也只 1
         * 节点 → 记录数天然有界 = 池深）；若前方有在线窗则只记到窗前
         *（record 绝不覆盖在线窗 —— 在线窗已直改，再记会重复清位）。 */
        const uint32_t on = backing->online_chunk;
        uint64_t span_limit = slice_bitscount;
        if (on != BITMAP_CHUNK_NONE) {
            uint64_t olo = 0, ohi = 0;
            ctl_window(on, &olo, &ohi);
            if (pos < ohi && pos + remaining > olo) span_limit = olo;
        }
        const uint64_t n = (pos + remaining <= span_limit) ? remaining : span_limit - pos;

        rec_node *node = backing->pool ? backing->pool->acquire() : nullptr;
        if (node) {
            node->slice_off = pos;
            node->len = n;
            node->next = backing->rec_head;
            backing->rec_head = node;
            /* 摘要降级（§6 关键正确性）：覆盖到的 ctl 一律 MIXED，否则盘态
             * ALL_USED（其实有待还空闲）被 alloc 免载入跳过 → 错误 ENOSPC（§10.5） */
            const uint32_t c_last = (uint32_t)((pos + n - 1) >> sh);
            for (uint32_t t = c; t <= c_last; t++) {
                backing->ctls[t].has_recs = 1;
                backing->ctls[t].summary = CHUNK_SUM_MIXED;
            }
            free_count += n;            /* 挂账时记账；drain 不再动计数（§5/§6） */
            slock_release(*this);
            pos += n; remaining -= n; guard = 0;
            continue;
        }
        /* 池空兜底（§6/§10.9）：解锁 → 慢路径强制上线 → 重锁 → 直接清位；
         * 此时盘态仍显示已分配（free 未记账），alloc 抢不走它 ⇒ 不越序、
         * 不产生第二批记录、不阻塞他人 */
        slock_release(*this);
        if (chunk_ensure_online(c) != RC_OK) {
            /* I/O 失败且无节点可挂：本段放弃（泄漏方向，§1.5，fsck 可收） */
            pos += nwin; remaining -= nwin; guard = 0;
            continue;
        }
        slock_acquire(*this);
        if (backing->online_chunk != c) {                   /* 竞态被换片 → 重走本段 */
            slock_release(*this);
            if (++guard > 64) { pos += nwin; remaining -= nwin; guard = 0; }
            continue;
        }
        clear_bits_locked(pos, pos + nwin);
        slock_release(*this);
        pos += nwin; remaining -= nwin; guard = 0;
    }
    return RC_OK;
}

/* block_test / inode_test 的 CHUNK 版（§1.1 语义不变）：在线窗直读 / record
 * 覆盖 = 逻辑空闲 / 摘要裁决（ALL_USED、ALL_FREE 免载入）/ MIXED 离线 → 上线查；
 * I/O 失败保守按已用（泄漏方向，绝不误报 free）。注：FULL 的 block_test 无锁直读；
 * CHUNK 需锁内裁决 + 可能的换片 I/O（chg_mu 下），返回值语义与 FULL 逐位一致。 */
bool bitmap_slice_t::chunk_test_bit(uint64_t base) {
    if (!backing || !parent_descriptor) return false;
    if (base < base_idx) return false;
    const uint64_t off = base - base_idx;
    if (off >= slice_bitscount) return false;
    const uint32_t c = (uint32_t)(off >> (15 + backing->chunk_order));

    slock_acquire(*this);
    if (backing->online_chunk == c) {                       /* 在线：直读 buffer */
        const bool v = bit_get(reinterpret_cast<const uint64_t *>(slice_base), off);
        slock_release(*this);
        return v;
    }
    for (rec_node *n = backing->rec_head; n; n = n->next) { /* 离线：record 覆盖 = 逻辑空闲 */
        if (off >= n->slice_off && off < n->slice_off + n->len) {
            slock_release(*this);
            return false;
        }
    }
    const uint8_t sum = backing->ctls[c].summary;           /* 摘要裁决（免载入） */
    slock_release(*this);
    if (sum == CHUNK_SUM_ALL_USED) return true;
    if (sum == CHUNK_SUM_ALL_FREE) return false;
    if (chunk_ensure_online(c) != RC_OK) return true;       /* MIXED 上线查；I/O 失败 → 保守按已用 */
    slock_acquire(*this);
    const bool v = (backing->online_chunk == c)
                       ? bit_get(reinterpret_cast<const uint64_t *>(slice_base), off)
                       : true;
    slock_release(*this);
    return v;
}

/* §7 mount 扫描单窗：chunk_io 顺序读入 → 一趟吃入 free_count + 2bit 摘要 +
 * 自描述三段校验（02 §1）内联同趟做掉，失败 RC_ERR_STATE 拒挂；skip_self_check
 * 供单测合成位图。构造期单线程；持 chg_mu 仅为 §10.3「I/O 只在持 chg_mu 下」
 * 字面成立（FULL ctor 零 I/O 不进本函数）。 */
result_code_t bitmap_slice_t::chunk_mount_scan(uint32_t ctl) {
    slice_backing_t *bk = backing;
    if (!bk || !bk->io || ctl >= bk->chunks_per_slice) return RC_ERR_STATE;
    uint64_t wlo = 0, whi = 0;
    ctl_window(ctl, &wlo, &whi);
    const uint64_t nbits = whi - wlo;
    const uint64_t nblocks = nbits / 32768;                 /* §3：恒整页，无 sub-page 尾块 */
    if (nblocks == 0) return RC_ERR_STATE;

    std::lock_guard<bitmap_mutex_t> guard(bk->chg_mu);      /* §10.3：I/O 持 chg_mu */
    if (bk->io->read(bk->io_ctx, bk->is_inode, bk->ctls[ctl].disk_block_rel,
                     bk->buffer, nblocks) != 0)
        return RC_ERR_IO;

    const uint64_t *words = reinterpret_cast<const uint64_t *>(bk->buffer);
    uint64_t ones = 0;
    for (uint64_t w = 0; w < (nbits >> 6); w++)
        ones += (uint64_t)__builtin_popcountll(words[w]);
    const uint64_t zeros = nbits - ones;
    free_count += zeros;                                    /* 每片 free_count（02 §4 gate 保真） */
    bk->ctls[ctl].summary = (zeros == 0) ? CHUNK_SUM_ALL_USED
                          : (ones == 0)  ? CHUNK_SUM_ALL_FREE
                                         : CHUNK_SUM_MIXED;

    /* §7.4 自描述三段校验（02 §1）：block 组尾巴两段并段 [lo,hi) 全 1 +
     * inode 组 root bit0 亮灯；失败 → RC_ERR_STATE 拒挂（03 §7.4 原文）。 */
    if (bk->skip_self_check) return RC_OK;
    /* ★ 自描述区间在**全局位号**空间（02 §1/§3.4 坐标双视角）：wlo/whi 是片内
     * 偏移，不换算就会在每个片的 ctl0 上误触 root 灯校验、并对片1 的校验区间
     * 越界读 buffer（2026-09-28 刀④ 测试抓出的真 bug）。 */
    const uint64_t g_wlo = base_idx + wlo;
    const uint64_t g_whi = base_idx + whi;
    if (!bk->is_inode) {
        const uint64_t lo = g_wlo > bk->self_check_lo ? g_wlo : bk->self_check_lo;
        const uint64_t hi = g_whi < bk->self_check_hi ? g_whi : bk->self_check_hi;
        for (uint64_t b = lo; b < hi; b++)
            if (!bit_get(words, b - g_wlo)) return RC_ERR_STATE;
    } else if (g_wlo == 0 && nbits > 0) {
        if (!bit_get(words, 0)) return RC_ERR_STATE;         /* root inode 亮灯（仅全局 bit0） */
    }
    return RC_OK;
}

/* §9 单 chunk 三态分派（已持 chg_mu）：ONLINE&&dirty → 锁内快照+清脏（原子）→
 * 锁外写 → 重算；OFFLINE&&链上有覆盖 record → 读 → 应用+摘节点 → 写 → 重算；
 * 其余跳过。写失败：恢复脏标（buffer 是真相）/ 摘要降 MIXED（盘态未知，保守，§1.5）。 */
void bitmap_slice_t::chunk_sync_one(uint32_t ctl) {
    slice_backing_t *bk = backing;
    if (!bk || !bk->io || ctl >= bk->chunks_per_slice) return;
    uint64_t wlo = 0, whi = 0;
    ctl_window(ctl, &wlo, &whi);
    const uint64_t nbits = whi - wlo;
    const uint64_t nblocks = nbits / 32768;

    slock_acquire(*this);
    if (bk->online_chunk == ctl && bk->ctls[ctl].dirty) {   /* 态 1：ONLINE && dirty */
        memcpy(bk->snapshot, bk->buffer, nbits / 8);
        bk->ctls[ctl].dirty = 0;
        slock_release(*this);
        if (bk->io->write(bk->io_ctx, bk->is_inode, bk->ctls[ctl].disk_block_rel,
                          bk->snapshot, nblocks) != 0) {
            slock_acquire(*this);
            bk->ctls[ctl].dirty = 1;                        /* 写失败：buffer 是真相，回滚脏标 */
            slock_release(*this);
            return;
        }
        slock_acquire(*this);
        recompute_summary_locked(ctl, bk->buffer);          /* 写回后盘态 = buffer（§7） */
        slock_release(*this);
        return;
    }
    const bool offline_recs = (bk->online_chunk != ctl) && bk->ctls[ctl].has_recs;
    slock_release(*this);
    if (!offline_recs) return;                              /* 态 3：其余跳过 */

    /* 态 2：OFFLINE&&有账。用 snapshot 而非 buffer（buffer 归线上窗所有，§5）。
     * 读失败 = 记录原样留待下次（不丢，§10.7）；写失败 = 摘要降 MIXED 保守。
     *（若写前被换上线 → 复检发现在线即收手：记录已被那次发布的 drain 消费。） */
    if (bk->io->read(bk->io_ctx, bk->is_inode, bk->ctls[ctl].disk_block_rel,
                     bk->snapshot, nblocks) != 0)
        return;
    slock_acquire(*this);
    if (bk->online_chunk == ctl) { slock_release(*this); return; }
    (void)drain_window_locked(ctl, reinterpret_cast<uint64_t *>(bk->snapshot));
    slock_release(*this);
    if (bk->io->write(bk->io_ctx, bk->is_inode, bk->ctls[ctl].disk_block_rel,
                      bk->snapshot, nblocks) != 0) {
        slock_acquire(*this);
        bk->ctls[ctl].summary = CHUNK_SUM_MIXED;            /* 盘态未知 → 保守降级（§1.5） */
        slock_release(*this);
        return;
    }
    slock_acquire(*this);
    recompute_summary_locked(ctl, bk->snapshot);            /* 写回后盘态 = snapshot */
    slock_release(*this);
}

/* §9 单片 TTL：遍历本片 chunk 数组（不拿全片锁），逐 chunk 三态分派 */
void bitmap_slice_t::chunk_sync() {
    if (!backing) return;                                   /* FULL：现全量行为归 gong 侧 */
    for (uint32_t c = 0; c < backing->chunks_per_slice; c++) {
        std::lock_guard<bitmap_mutex_t> guard(backing->chg_mu);  /* §5 反例依据：I/O 串行 chg_mu */
        chunk_sync_one(c);
    }
}

/* mgr teardown 用：释放 buffer/snapshot/ctls/backing（幂等）。record 池归 group
 * 所有，链上残留节点随池整体释放（节点只在本池内复用，无二次分配）。 */
void bitmap_slice_t::release_backing() {
    if (!backing) return;                                   /* FULL：空操作（现行为零变化） */
    free(backing->buffer);
    free(backing->snapshot);
    delete[] backing->ctls;
    delete backing;
    backing = nullptr;
    slice_base = nullptr;
}

/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slices_mgr_t — CHUNK 分支（03 §13 刀①②③；FULL 路径零改动）
 *
 * 热路径只多一条 backing_mode 常量分支（装配期即定，分支完美预测）；
 * 顶层 8 接口签名/语义不变（§1.1），try/自旋锁纪律两边各保持现行为（§10.10）。
 * ═══════════════════════════════════════════════════════════════════ */

/* §2 装配期校验 + 逐片 backing 建立（刀①；§7 mount 顺序扫描 = 刀③）。
 * 失败 → 已清理本组自有数组（含 backing 资产）并返回错误码。 */
result_code_t bitmap_slices_mgr_t::setup_group_chunk(group_t &g, uint64_t nbits) {
    const uint64_t per = g.per_slice_bits;
    const uint64_t chunk_bytes = 4096ULL << g.chunk_order;
    if (!g.chunk_io || !g.chunk_io->read || !g.chunk_io->write) return RC_ERR_STATE;
    if (chunk_bytes > per / 8) return RC_ERR_STATE;     /* §2：chunk ≤ 最小片字节数 */

    auto fail = [&](result_code_t rc) -> result_code_t {
        release_group_chunk(g);                         /* 已挂 backing 全数安全拆除（null 免疫） */
        delete[] g.slices;
        g.slices = nullptr;
        return rc;
    };

    /* §6 record 池：per-group 共享 LIFO（深度 cfg 可调，默认 64） */
    g.rec_pool = rec_pool_create(g.rec_pool_depth ? g.rec_pool_depth : 64);
    if (!g.rec_pool) return RC_ERR_STATE;

    try {
        g.slices = new bitmap_slice_t[slice_count_];
    } catch (const std::bad_alloc &) {
        g.slices = nullptr;
    }
    if (!g.slices) return fail(RC_ERR_STATE);
    for (uint32_t i = 0; i < slice_count_; i++)
        g.slices[i].backing = nullptr;                  /* 部分失败安全拆的前提（null 免疫） */

    const uint64_t chunk_bits = chunk_bytes * 8;
    for (uint32_t i = 0; i < slice_count_; i++) {
        const uint64_t begin = i * per;
        uint64_t cnt = nbits - begin;
        if (cnt > per) cnt = per;

        slice_backing_t *bk = nullptr;
        try {
            bk = new slice_backing_t();
        } catch (const std::bad_alloc &) {
            bk = nullptr;
        }
        if (!bk) return fail(RC_ERR_STATE);
        g.slices[i].backing = bk;                       /* 先挂后建：任何一步失败都由 fail 拆 */
        bk->mode = BITMAP_BACKING_CHUNK;
        bk->chunk_order = g.chunk_order;
        bk->chunks_per_slice = (uint32_t)((cnt + chunk_bits - 1) / chunk_bits);
        bk->pool = g.rec_pool;
        bk->io = g.chunk_io;
        bk->io_ctx = g.chunk_io_ctx;
        bk->is_inode = g.is_inode;
        bk->skip_self_check = g.skip_self_check;
        bk->self_check_lo = g.self_check_lo;
        bk->self_check_hi = g.self_check_hi;
        bk->online_chunk = BITMAP_CHUNK_NONE;
        if (posix_memalign((void **)&bk->buffer, 4096, chunk_bytes) != 0 ||
            posix_memalign((void **)&bk->snapshot, 4096, chunk_bytes) != 0)
            return fail(RC_ERR_STATE);
        memset(bk->buffer, 0, chunk_bytes);
        memset(bk->snapshot, 0, chunk_bytes);
        try {
            bk->ctls = new chunk_ctl_t[bk->chunks_per_slice];
        } catch (const std::bad_alloc &) {
            bk->ctls = nullptr;
        }
        if (!bk->ctls) return fail(RC_ERR_STATE);

        /* 控制器预构造（§4：上线前即构造好，靠它做一切定位；刀③ mount 扫描落摘要真值） */
        for (uint32_t c = 0; c < bk->chunks_per_slice; c++) {
            const uint64_t wlo = (uint64_t)c << (15 + g.chunk_order);
            uint64_t whi = wlo + chunk_bits;
            if (whi > cnt) whi = cnt;
            bk->ctls[c].disk_block_rel = (begin + wlo) / 32768;  /* 相对本组盘起点（4K 块） */
            bk->ctls[c].tail_bits = whi - wlo;                    /* §3 尾窗有效位（恒整页） */
            bk->ctls[c].summary = CHUNK_SUM_MIXED;                /* 保守值；mount 扫描落真值 */
        }
        if (g.slices[i].enable_slice_chunked(&g.desc, begin, cnt, bk) != RC_OK)
            return fail(RC_ERR_STATE);
    }

    /* §7 mount 顺序扫描（CHUNK ctor 唯一 I/O 源；FULL ctor 零 I/O 不进本函数）：
     * 一趟产出 每片 free_count / 组全局空闲计数（由 free_count 求和）/
     * 每 ctl 2bit 摘要 / 自描述三段校验。 */
    for (uint32_t i = 0; i < slice_count_; i++) {
        for (uint32_t c = 0; c < g.slices[i].backing->chunks_per_slice; c++) {
            const result_code_t rc = g.slices[i].chunk_mount_scan(c);
            if (rc != RC_OK) return fail(rc == RC_ERR_IO ? rc : RC_ERR_STATE);
        }
    }
    return RC_OK;
}

/* 释放 backing 资产与 record 池（幂等）。FULL 组 = 只对 null backing 空转，
 * 现行为零变化（红线）。 */
void bitmap_slices_mgr_t::release_group_chunk(group_t &g) {
    if (g.slices) {
        for (uint32_t i = 0; i < slice_count_; i++) g.slices[i].release_backing();
    }
    rec_pool_destroy(g.rec_pool);
    g.rec_pool = nullptr;
}

/* 硬分配路由（CHUNK 版；循环形态与 FULL 同构）：锁忙/片内无货/I/O 失败 → 换片，
 * preferred 亲和与 try 纪律不变（§10.10）。 */
uint64_t bitmap_slices_mgr_t::chunk_alloc_hard(group_t &g, uint64_t require_count,
                                               uint8_t align_log2) {
    if (require_count == 0 || !g.slices || slice_count_ == 0 ||
        cpu_slots_ == 0 || !g.preferred) return PTR_INVALID;
    const uint32_t cpu = get_processor_id() % cpu_slots_;
    for (uint64_t round = 0; round < slice_count_; round++) {
        uint64_t idx = (g.preferred[cpu] + round) % slice_count_;
        bitmap_slice_t &s = g.slices[idx];
        uint64_t out = 0;
        const result_code_t rc = s.chunk_alloc_hard(align_log2, require_count, out);
        if (rc == RC_OK) { g.preferred[cpu] = (uint32_t)idx; return out; }
        /* RC_ERR_NO_BLOCK（全片扫毕含绕回）/ RC_CHUNK_BUSY / RC_ERR_STATE → 下一片 */
    }
    return PTR_INVALID;
}

/* 软分配路由（CHUNK 版）：一次只从单片取，缺口回传，全片失败 PTR_INVALID */
uint64_t bitmap_slices_mgr_t::chunk_alloc_soft(group_t &g, uint64_t require_count,
                                               uint64_t *remain_count) {
    if (remain_count) *remain_count = require_count;
    if (require_count == 0 || !g.slices || slice_count_ == 0 ||
        cpu_slots_ == 0 || !g.preferred) return PTR_INVALID;
    const uint32_t cpu = get_processor_id() % cpu_slots_;
    for (uint64_t round = 0; round < slice_count_; round++) {
        uint64_t idx = (g.preferred[cpu] + round) % slice_count_;
        bitmap_slice_t &s = g.slices[idx];
        uint64_t out = 0, left = require_count;
        const result_code_t rc = s.chunk_alloc_soft(0, require_count, out, left);
        if (rc == RC_OK) {
            g.preferred[cpu] = (uint32_t)idx;
            if (remain_count) *remain_count = left;
            return out;
        }
    }
    return PTR_INVALID;
}

/* 锚定占领路由（CHUNK 版）：锚点片唯一 → 片内窗迭代自消化（§8）；片尾跨界续圈
 * 归本循环（逐片单锁前缀链 = 真实连续，同 FULL）。 */
uint64_t bitmap_slices_mgr_t::chunk_occupy_soft(group_t &g, uint64_t blkbase,
                                               uint64_t require_count,
                                               uint64_t *rest_count) {
    if (rest_count) *rest_count = require_count;
    if (require_count == 0 || !g.slices || slice_count_ == 0) return PTR_INVALID;
    const uint64_t nbits = g.desc.bits_count;

    uint64_t base = blkbase, remain = require_count;
    while (remain > 0 && base < nbits) {
        const uint64_t idx = base / g.per_slice_bits;
        if (idx >= slice_count_) break;
        bitmap_slice_t &s = g.slices[idx];
        uint64_t left = 0;
        const result_code_t rc = s.chunk_soft_occupy(base, remain, &left);
        if (rc != RC_OK && rc != RC_ERR_NO_BLOCK) break;    /* BUSY/STATE：不硬续 */
        const uint64_t claimed = remain - left;
        base += claimed;
        remain = left;
        if (remain == 0) break;
        const uint64_t slice_end = (idx + 1) * g.per_slice_bits < nbits
                                       ? (idx + 1) * g.per_slice_bits : nbits;
        if (base != slice_end) break;                       /* 中途撞占用停：不跨片（FULL 同款） */
    }
    if (rest_count) *rest_count = remain;
    return (require_count - remain > 0) ? blkbase : PTR_INVALID;
}

/* 区间释放路由（CHUNK 版）：逐片切段同 FULL；片内在线直改/离线兜底由
 * chunk_free_interval 自管锁（兜底须解锁走慢路径，故路由层不持锁）。 */
void bitmap_slices_mgr_t::chunk_free(group_t &g, uint64_t blkbase, uint64_t count) {
    if (count == 0 || !g.slices || slice_count_ == 0) return;
    const uint64_t nbits = g.desc.bits_count;
    while (count > 0) {
        if (blkbase >= nbits) return;                       /* 防御：越界剩余放弃 */
        const uint64_t idx = blkbase / g.per_slice_bits;
        if (idx >= slice_count_) return;
        const uint64_t slice_end = (idx + 1) * g.per_slice_bits < nbits
                                       ? (idx + 1) * g.per_slice_bits : nbits;
        const uint64_t n = (blkbase + count <= slice_end) ? count : slice_end - blkbase;
        if (n == 0) return;
        (void)g.slices[idx].chunk_free_interval(blkbase, n);
        blkbase += n;
        count -= n;
    }
}

/* 单点测试路由（CHUNK 版；block_test/inode_test 的 const 方法经指针成员非 const
 * 语义进入片方法）：返回值语义与 FULL 逐位一致（§1.1）。 */
bool bitmap_slices_mgr_t::chunk_test(const group_t &g, uint64_t bit) const {
    if (!g.slices || slice_count_ == 0 || bit >= g.desc.bits_count) return false;
    const uint64_t idx = bit / g.per_slice_bits;
    if (idx >= slice_count_) return false;
    return g.slices[idx].chunk_test_bit(bit);
}

/* §9 TTL 三态分派（双组遍历）：FULL 组跳过（维持现全量回写行为，本函数空操作）；
 * CHUNK 组只写脏/有待还 record 的 chunk —— 替代 02 §5 的“每 30s 全量 80MB +
 * 全片锁”，online 写盘期间仅同片换片等待，快路径分配不受扰。 */
void bitmap_slices_mgr_t::chunk_ttl_sync() {
    group_t *gs[2] = { &blk_, &ino_ };
    for (group_t *g : gs) {
        if (g->backing_mode != BITMAP_BACKING_CHUNK || !g->slices) continue;
        for (uint32_t i = 0; i < slice_count_; i++) g->slices[i].chunk_sync();
    }
}
