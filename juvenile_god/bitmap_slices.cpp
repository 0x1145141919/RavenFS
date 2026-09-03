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

    if (!cfg.block_bits_raw || !cfg.inode_bits_raw ||
        cfg.block_bits_count == 0 || cfg.inode_bits_count == 0) {
        return;   /* 调用方参数缺失：保持空态，res_code=RC_ERR_STATE */
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
    if (!blk_.desc.bitmap_raw) return false;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(blk_.desc.bitmap_raw);
    return (raw[blk / 8] & (1u << (blk % 8))) != 0;
}

/* ── inode 组 ── */

uint64_t bitmap_slices_mgr_t::inode_alloc() {
    if (!ino_.slices || slice_count_ == 0 ||
        cpu_slots_ == 0 || !ino_.preferred) return PTR_INVALID;
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
    uint64_t idx = inum / ino_.per_slice_bits;
    if (idx >= slice_count_) return;
    bitmap_slice_t &s = ino_.slices[idx];
    while (!s.slice_lock.try_lock_write()) { }            /* 释放不可丢，自旋等 */
    s.free_interval(inum, 1);
    s.slice_lock.unlock_write();
}

bool bitmap_slices_mgr_t::inode_test(uint64_t inum) const {
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
