/* ═══════════════════════════════════════════════════════════════════
 * inode_cache.cpp — Inode_cache_mgr_t 三模式实现
 *
 * 声明见 include/ravenfs/inode_cache_radix.hpp（本文件为唯一实现 TU）。
 *
 * 分支现状：
 *   - CACHE_MODE_FULL_RAM    [1,12]  已实现：构造全量载入，get 零 I/O 零锁
 *   - CACHE_MODE_CTRL_ARRAY  [13,18] 已实现：控制器全量常驻，页懒加载不淘汰
 *   - CACHE_MODE_RADIX_SHARD [19,52] 草稿：策略表在下方，分片树待实装
 * ═══════════════════════════════════════════════════════════════════ */
#include "ravenfs/inode_cache_radix.hpp"
#include "util/lock.h"   /* cpu_relax（忙等） / rdtsc_timestamp（LRU 时间戳） */

#include <cstring>   /* memset：中间表槽位清零 */
#include <new>       /* placement new：分片树 */

/* ═══════════════════════════════════════════════════════════════════
 * statey 位域策略表（Sparrow 亲笔，从头文件原样迁入）
 *
 * 按 bits_len_of_inodes_count 逐档固化 level_bits（per_level_width）：
 * 逐层 cut_high 递归切割；行内引索位宽 row_bits = bits - sum(level_bits)，
 * 绝大多数档 4bit(16 entries=4KB=1块)，但 bits=25 / bits=33 两档为
 * 5bit(32 entries=8KB=2块) —— row_bits 由 decode 派生，不做独立常量。
 * statey_array 尚未接线（RADIX 分支实装时按 bits 查表解析 global_slice_plan）。
 * ═══════════════════════════════════════════════════════════════════ */
uint8_t **statey_array={
    nullptr,
};
uint8_t statey_19[]={8,7};
uint8_t statey_20[]={9,7};
uint8_t statey_21[]={9,8};
uint8_t statey_22[]={9,9};
uint8_t statey_23[]={9,10};
uint8_t statey_24[]={9,11};
uint8_t statey_25[]={9,11};
uint8_t statey_26[]={7,9,6};
uint8_t statey_27[]={7,9,7};
uint8_t statey_28[]={8,9,7};
uint8_t statey_29[]={8,10,7};
uint8_t statey_30[]={9,10,7};
uint8_t statey_31[]={9,10,8};
uint8_t statey_32[]={9,10,9};
uint8_t statey_33[]={9,10,9};
uint8_t statey_34[]={7,9,9,5};
uint8_t statey_35[]={7,9,9,6};
uint8_t statey_36[]={7,9,9,7};
uint8_t statey_37[]={8,9,9,7};
uint8_t statey_38[]={8,10,9,7};
uint8_t statey_39[]={8,10,10,7};
uint8_t statey_40[]={8,10,10,8};
uint8_t statey_41[]={8,10,10,9};
uint8_t statey_42[]={9,10,10,9};
uint8_t statey_43[]={9,10,11,9};
uint8_t statey_44[]={9,11,11,9};
uint8_t statey_45[]={7,9,9,9,7};
uint8_t statey_46[]={8,9,9,9,7};
uint8_t statey_47[]={8,9,9,10,7};
uint8_t statey_48[]={8,9,10,10,7};
uint8_t statey_49[]={8,10,10,10,7};
uint8_t statey_50[]={9,10,10,10,7};
uint8_t statey_51[]={9,10,10,11,7};
uint8_t statey_52[]={9,10,11,11,7};

/* statey_levels_of — 按 bits_len_of_inodes_count 查策略表
 * 返回 level_bits 数组（指向 statey_XX 全局数组，生命周期全程有效），
 * out_levels 收层数；非法档返回 nullptr（bits < 19 或 > 52 不会走到：
 * 模式判定已把 ≤18 归入 FULL_RAM/CTRL_ARRAY） */
uint8_t *Inode_cache_mgr_t::statey_levels_of(uint8_t bits, uint8_t *out_levels) {
    uint8_t *lv = nullptr;
    switch (bits) {
    case 19: lv = statey_19; *out_levels = 2; break;
    case 20: lv = statey_20; *out_levels = 2; break;
    case 21: lv = statey_21; *out_levels = 2; break;
    case 22: lv = statey_22; *out_levels = 2; break;
    case 23: lv = statey_23; *out_levels = 2; break;
    case 24: lv = statey_24; *out_levels = 2; break;
    case 25: lv = statey_25; *out_levels = 2; break;
    case 26: lv = statey_26; *out_levels = 3; break;
    case 27: lv = statey_27; *out_levels = 3; break;
    case 28: lv = statey_28; *out_levels = 3; break;
    case 29: lv = statey_29; *out_levels = 3; break;
    case 30: lv = statey_30; *out_levels = 3; break;
    case 31: lv = statey_31; *out_levels = 3; break;
    case 32: lv = statey_32; *out_levels = 3; break;
    case 33: lv = statey_33; *out_levels = 3; break;
    case 34: lv = statey_34; *out_levels = 4; break;
    case 35: lv = statey_35; *out_levels = 4; break;
    case 36: lv = statey_36; *out_levels = 4; break;
    case 37: lv = statey_37; *out_levels = 4; break;
    case 38: lv = statey_38; *out_levels = 4; break;
    case 39: lv = statey_39; *out_levels = 4; break;
    case 40: lv = statey_40; *out_levels = 4; break;
    case 41: lv = statey_41; *out_levels = 4; break;
    case 42: lv = statey_42; *out_levels = 4; break;
    case 43: lv = statey_43; *out_levels = 4; break;
    case 44: lv = statey_44; *out_levels = 4; break;
    case 45: lv = statey_45; *out_levels = 5; break;
    case 46: lv = statey_46; *out_levels = 5; break;
    case 47: lv = statey_47; *out_levels = 5; break;
    case 48: lv = statey_48; *out_levels = 5; break;
    case 49: lv = statey_49; *out_levels = 5; break;
    case 50: lv = statey_50; *out_levels = 5; break;
    case 51: lv = statey_51; *out_levels = 5; break;
    case 52: lv = statey_52; *out_levels = 5; break;
    default:  *out_levels = 0; break;
    }
    return lv;
}

/* ═══════════════════════════════════════════════════════════════════
 * cut_high — 位域切割原语（左对齐方案）
 *
 * raw 须已左对齐（位域最高位 = bit 63）；从位域顶端向下切 bits_to_cut 个 bit：
 *   cut_out_section = 切下的高位段（右对齐到低位）
 *   main            = 剩余位域（保持左对齐，供 decode 递归继续从 bit 63 切）
 * 通用轮子：块缓存（block cache）等其它位域分片场景可复用。
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
 * decode — 按 plan 逐层切分 inode 号位域
 *
 * chunks[0..total_levels-1] = 各层索引（chunks[0] = mgr 层 slice 选择），
 * chunks[total_levels]      = 行内索引。
 * row_bits 由 plan 累积派生（bits_len - sum(level_bits)），不依赖外部常量，
 * plan 为唯一真相源；raw 内部左对齐到位域顶端后递归 cut_high。
 * 约束：plan.total_levels ≤ 6（chunks[7] 容量，现表最大 5 层）。
 * ═══════════════════════════════════════════════════════════════════ */
Inode_cache_mgr_t::bitfield_decoded_result_t Inode_cache_mgr_t::decode(uint64_t raw, bitfield_decode_plan_t plan) {
    bitfield_decoded_result_t r{};
    r.raw         = raw;
    r.total_levels = plan.total_levels;

    if (plan.total_levels > 6) {
        r.total_levels = 6;   /* 防御：chunks[7] 容量上限（正常表最大 5 层） */
    }

    uint64_t cur = raw << (64 - plan.bits_len);   /* 左对齐到位域顶端 */
    uint16_t cut_total = 0;
    for (uint8_t i = 0; i < r.total_levels; i++) {
        cut_result_t c = cut_high(cur, plan.level_bits[i]);
        r.chunks[i]    = (uint16_t)c.cut_out_section;
        cur            = c.main;
        cut_total     += plan.level_bits[i];
    }

    /* 行内索引：剩余位宽 = 总位宽 - 已切总和，掩码提取 */
    const uint8_t row_bits = plan.bits_len - cut_total;
    r.chunks[r.total_levels] = row_bits
        ? (uint16_t)raw & ((1ull << row_bits) - 1)
        : 0;   /* 防御：row_bits=0 时无行内位（实际表 ≥4） */
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * 构造：模式由 bits 推导；FULL_RAM 在此全量载入
 * ═══════════════════════════════════════════════════════════════════ */
Inode_cache_mgr_t::Inode_cache_mgr_t(const Inode_cache_cfg_t &cfg, result_code_t *rc_out)
    : cfg_(cfg), mode_(inode_cache_mode_of(cfg.bits_len_of_inodes_count)) {
    /* TTL 初始化：cfg.ttl_ticks 优先；否则按 Invariant TSC 频率换算默认 2s。
     * TSC 不可靠/无频率 → ttl_ticks_ 保持 0 = TTL 淘汰禁用（reap_expired 空转）。
     * （绑死 x86_64：换算逻辑见 include/util/tsc.h，对齐 kernel tsc.cpp BARE_METAL 分支） */
    if (cfg.ttl_ticks) {
        ttl_ticks_.store(cfg.ttl_ticks, std::memory_order_relaxed);
    } else {
        tsc_info_t tsc;
        if (tsc_probe(&tsc)) ttl_ticks_.store(tsc_ticks_of_seconds(2, tsc), std::memory_order_relaxed);
    }

    result_code_t rc = RC_OK;
    switch (mode_) {
    case CACHE_MODE_FULL_RAM:   rc = ctor_full_ram();   break;
    case CACHE_MODE_CTRL_ARRAY: rc = ctor_ctrl_array(); break;
    case CACHE_MODE_RADIX_SHARD: {
        /* 按 statey 表解析 global_slice_plan（level_bits 指向 statey_XX 全局数组）。
         * level_bits[0] = mgr 层分片选择位宽（slice 数 = 2^w0），level_bits[1..]
         * = 树内逐层位宽；row_bits = bits - Σlevel_bits（行内索引，多数 4，25/33 档 5）。
         * 每片 placement new 一棵 slice_radix_tree（ctor 即建根表）。 */
        global_slice_plan.bits_len = cfg_.bits_len_of_inodes_count;
        global_slice_plan.level_bits = statey_levels_of(cfg_.bits_len_of_inodes_count,
                                                        &global_slice_plan.total_levels);
        if (!global_slice_plan.level_bits || global_slice_plan.total_levels == 0) {
            rc = RC_ERR_NO_BLOCK;
            break;
        }
        uint16_t sum = 0;
        for (uint8_t i = 0; i < global_slice_plan.total_levels; i++)
            sum += global_slice_plan.level_bits[i];
        if (sum > global_slice_plan.bits_len) { rc = RC_ERR_NO_BLOCK; break; }  /* 防御：位宽合计越界 */
        const uint8_t row_bits = (uint8_t)(global_slice_plan.bits_len - sum);
        n_slices_ = 1ull << global_slice_plan.level_bits[0];
        slices_ = (slice_radix_tree **)cfg_.alloc(cfg_.io_ctx, n_slices_ * sizeof(slice_radix_tree *));
        if (!slices_) { rc = RC_ERR_NO_BLOCK; break; }
        memset(slices_, 0, n_slices_ * sizeof(slice_radix_tree *));
        slice_radix_tree_cfg_t scfg;
        scfg.host_cfg  = &cfg_;
        scfg.plan      = global_slice_plan;
        scfg.row_bits  = row_bits;
        scfg.ttl_ticks = &ttl_ticks_;
        for (uint64_t i = 0; i < n_slices_; i++) {
            void *mem = cfg_.alloc(cfg_.io_ctx, sizeof(slice_radix_tree));
            if (!mem) { rc = RC_ERR_NO_BLOCK; break; }
            slices_[i] = new (mem) slice_radix_tree(scfg);
        }
        break;
    }
    }
    ready_ = (rc == RC_OK);
    if (rc_out) *rc_out = rc;
}

/* set_ttl_ticks：外部调整缓存行 TTL（tick）。slice 持 &ttl_ticks_ 共享，即时生效，无需逐片同步 */
void Inode_cache_mgr_t::set_ttl_ticks(uint64_t ticks) {
    ttl_ticks_.store(ticks, std::memory_order_relaxed);
}

/* 析构：全生命周期收口 — 脏页回写 → 释放 */
Inode_cache_mgr_t::~Inode_cache_mgr_t() {
    switch (mode_) {
    case CACHE_MODE_FULL_RAM:
        flush_full_ram();
        cfg_.free(cfg_.io_ctx, full_ram_array_);
        break;
    case CACHE_MODE_CTRL_ARRAY:
        flush_ctrl_array();
        for (uint64_t i = 0; i < ctrl_count(); i++) {   /* 懒分配页逐个释放 */
            Inode_cache_ctrl_t *c = &ctrl_array_[i];
            if (c->cache_data) cfg_.free(cfg_.io_ctx, c->cache_data);
        }
        cfg_.free(cfg_.io_ctx, ctrl_array_);
        break;
    case CACHE_MODE_RADIX_SHARD:
        /* 逐片析构（slice dtor 内部：脏页写回 + 释放全部表）。
         * 宿主须先停淘汰线程（注释见 reap_expired）。 */
        for (uint64_t i = 0; i < n_slices_; i++) {
            if (slices_[i]) {
                slices_[i]->~slice_radix_tree();
                cfg_.free(cfg_.io_ctx, slices_[i]);
            }
        }
        cfg_.free(cfg_.io_ctx, slices_);
        break;
    default:
        break;
    }
}

/* ── FULL_RAM 构造：全量载入，之后 get 零 I/O 零锁 ── */
result_code_t Inode_cache_mgr_t::ctor_full_ram() {
    const uint64_t n     = cfg_.inodes_count ? cfg_.inodes_count : 1;
    const uint64_t pages = ctrl_count();
    full_ram_array_ = (raven_Inode_t *)cfg_.alloc(cfg_.io_ctx, n * sizeof(raven_Inode_t));
    if (!full_ram_array_) return RC_ERR_NO_BLOCK;
    for (uint64_t i = 0; i < pages; i++) {
        result_code_t rc = cfg_.read_page(cfg_.io_ctx, blk_of(i),
                                          &full_ram_array_[i * INODES_PER_PAGE]);
        if (rc != RC_OK) return rc;
    }
    return RC_OK;
}

/* ── CTRL_ARRAY 构造：控制器全量建好（状态全 idle），数据页懒加载 ── */
result_code_t Inode_cache_mgr_t::ctor_ctrl_array() {
    const uint64_t n = ctrl_count();
    ctrl_array_ = (Inode_cache_ctrl_t *)cfg_.alloc(cfg_.io_ctx, n * sizeof(Inode_cache_ctrl_t));
    if (!ctrl_array_) return RC_ERR_NO_BLOCK;
    for (uint64_t i = 0; i < n; i++) {
        Inode_cache_ctrl_t *c = &ctrl_array_[i];
        c->cache_data = nullptr;                  /* 懒分配：get 的 io_ing 态内 alloc+read 原子完成 */
        c->accss_stamp.store(0, std::memory_order_relaxed);
        c->ctrl_word.store(idle, std::memory_order_relaxed);   /* 未载入；CAS 状态机见 get() */
    }
    return RC_OK;
}

/* ── get：三模式统一入口 ── */
raven_Inode_t *Inode_cache_mgr_t::get(uint64_t inum, slice_cache_acces_token_t *out_token) {
    if (out_token) out_token->slice_idx = 0;   /* FULL_RAM/CTRL_ARRAY 单分片；RADIX 实装时填真值 */
    if (inum >= cfg_.inodes_count) return nullptr;   /* 防御：上层保证合法 */

    switch (mode_) {
    case CACHE_MODE_FULL_RAM:
        /* 全量常驻：直接数组寻址，恒命中，零 I/O 零锁 */
        return &full_ram_array_[inum];

    case CACHE_MODE_CTRL_ARRAY: {
        /* 懒加载状态机（公共原语 ctrl_acquire）：idle → io_ing(CAS) → I/O（不持锁）→ working；
         * TTL 淘汰（reap_expired）可能并发：working 行被 CAS 接管为 evicting（写回+释放中）。
         * 占坑（+accessor）与 state 打包在同一原子字 ctrl_word —— reap「accessor==0 才能接管」
         * 与 get 占坑是同一原子操作，无 TOCTOU 窗口（详见头文件 ctrl_word 位域注释）。
         * evicting / io_ing 均忙等（罕见路径，reap/加载完成即回可占坑态）。 */
        const uint64_t line = line_of(inum);
        Inode_cache_ctrl_t *c = &ctrl_array_[line];
        raven_Inode_t *page = ctrl_acquire(c, blk_of(line),
                                           INODES_PER_PAGE * sizeof(raven_Inode_t), cfg_);
        if (!page) return nullptr;
        return &page[row_of(inum)];
    }

    case CACHE_MODE_RADIX_SHARD: {
        /* 分片树：decode 位域 → chunks[0] 选片，树内逐层寻址（见 slice_radix_tree::get）。
         * token 携带 slice_idx（put 释放片读锁用）。 */
        bitfield_decoded_result_t d = decode(inum, global_slice_plan);
        if (out_token) out_token->slice_idx = d.chunks[0];
        slice_radix_tree *s = slices_[d.chunks[0]];
        if (!s) return nullptr;   /* ctor 部分失败防御 */
        raven_Inode_t *page = s->get(&d);
        if (!page) return nullptr;
        return &page[d.chunks[d.total_levels]];   /* 行内索引 */
    }
    default:
        return nullptr;   /* 不可达：模式判定已覆盖全部枚举 */
    }
}

/* ── put：accessor 计数 -1（须与 get 配对，上层保证；token 供 RADIX 定位分片释放读锁） ── */
void Inode_cache_mgr_t::put(uint64_t inum, const slice_cache_acces_token_t &token) {
    (void)token;
    if (inum >= cfg_.inodes_count) return;
    switch (mode_) {
    case CACHE_MODE_FULL_RAM:
        /* 无淘汰无计数：no-op（接口占位，保持三模式一致） */
        break;
    case CACHE_MODE_CTRL_ARRAY:
        ctrl_array_[line_of(inum)].ctrl_word.fetch_sub(CTRL_ACCESSOR_INC, std::memory_order_relaxed);
        break;
    case CACHE_MODE_RADIX_SHARD:
        /* 重新 decode 定位分片（与 get 一致）；slice::put 内 accessor-1 + 释放片读锁 */
        if (token.slice_idx < n_slices_ && slices_[token.slice_idx]) {
            bitfield_decoded_result_t d = decode(inum, global_slice_plan);
            slices_[token.slice_idx]->put(&d);
        }
        break;
    default:
        break;
    }
}

/* ── mark_dirty：页粒度脏标记（I/O 恒为整页回写） ── */
void Inode_cache_mgr_t::mark_dirty(uint64_t inum) {
    if (inum >= cfg_.inodes_count) return;
    switch (mode_) {
    case CACHE_MODE_FULL_RAM:
        /* 全量写回方案：无需脏标记，no-op */
        break;
    case CACHE_MODE_CTRL_ARRAY: {
        /* 仅 working 有效：会话内调用（InodeSession::dirty），上层保证 */
        Inode_cache_ctrl_t *c = &ctrl_array_[line_of(inum)];
        c->ctrl_word.fetch_or(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT, std::memory_order_release);
        break;
    }
    case CACHE_MODE_RADIX_SHARD: {
        /* 会话内调用（本线程已持片读锁）→ slice::mark_dirty 无锁走路径安全 */
        bitfield_decoded_result_t d = decode(inum, global_slice_plan);
        if (d.chunks[0] < n_slices_ && slices_[d.chunks[0]])
            slices_[d.chunks[0]]->mark_dirty(&d);
        break;
    }
    default:
        break;
    }
}

/* ── flush_all：全量脏页回写（挂载/卸载同步点） ── */
result_code_t Inode_cache_mgr_t::flush_all() {
    switch (mode_) {
    case CACHE_MODE_FULL_RAM:   flush_full_ram();   break;
    case CACHE_MODE_CTRL_ARRAY: flush_ctrl_array(); break;
    case CACHE_MODE_RADIX_SHARD:
        /* 逐片读锁 DFS 写回（slice::flush_all）。宿主应保证与 reap 不并发
         * （卸载路径先停淘汰线程）；并发双写同页是良性的（内容一致） */
        for (uint64_t i = 0; i < n_slices_; i++)
            if (slices_[i]) slices_[i]->flush_all();
        break;
    default: break;
    }
    return RC_OK;   /* 草稿：回写失败暂不聚合，待细化 */
}

void Inode_cache_mgr_t::flush_full_ram() {
    /* FULL_RAM ≤ 2^12 inode = 1MB：无条件全量写回，不要脏页位图 */
    const uint64_t pages = ctrl_count();
    for (uint64_t i = 0; i < pages; i++)
        cfg_.write_page(cfg_.io_ctx, blk_of(i), &full_ram_array_[i * INODES_PER_PAGE]);
}

void Inode_cache_mgr_t::flush_ctrl_array() {
    const uint64_t n = ctrl_count();
    for (uint64_t i = 0; i < n; i++) {
        Inode_cache_ctrl_t *c = &ctrl_array_[i];
        const uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
        if ((w & CTRL_STATE_MASK) == working &&
            ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY)) {
            cfg_.write_page(cfg_.io_ctx, blk_of(i), c->cache_data);
            c->ctrl_word.fetch_and(~(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT), std::memory_order_release);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * reap_expired — 专用淘汰线程周期调用：回收全部「超时 && 无访问者」的行
 *
 * TTL 淘汰（非压力淘汰）：inode 缓存行小（4K/8K），真正需要压力淘汰的是
 * 文件数据缓存；这里只做时间戳超时回收。
 * 并发安全（与 get 共用 ctrl_word CAS 状态机）：
 *   - 仅 working && accessor==0 && 超时 的行可被接管（CAS 前提原子包含 accessor==0，
 *     已占坑的 get 必不满足；无复核窗口）
 *   - 接管 = CAS working→evicting，之后无锁 I/O（写回+释放），完成后回 idle
 *   - get 在 evicting 上忙等，回 idle 后重新懒加载
 * 注意：与 flush_all 不同，reap 不要求与 get 互斥；但宿主应在卸载前先停淘汰线程。
 * ═══════════════════════════════════════════════════════════════════ */
void Inode_cache_mgr_t::reap_expired() {
    const uint64_t ttl = ttl_ticks_.load(std::memory_order_relaxed);
    if (!ttl) return;             /* 无 TTL 时钟（TSC 不可靠）：淘汰禁用 */
    const uint64_t now = rdtsc_timestamp();

    switch (mode_) {
    case CACHE_MODE_CTRL_ARRAY: {
        const uint64_t n = ctrl_count();
        for (uint64_t i = 0; i < n; i++)
            ctrl_reap_one(&ctrl_array_[i], cfg_, now, ttl);
        break;
    }
    case CACHE_MODE_RADIX_SHARD:
        for (uint64_t i = 0; i < n_slices_; i++)
            if (slices_[i]) slices_[i]->reap_expired();
        break;
    default:
        break;   /* FULL_RAM：全量常驻，无淘汰 */
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * ctrl_acquire — 缓存行懒加载状态机（CTRL_ARRAY / RADIX 共用）
 *
 * 返回 working 且 accessor 已 +1 的页基址；alloc/read 失败回 idle 并返回
 * nullptr（调用方解锁/返回）。out_loaded 置位 = 本调用实际完成页加载。
 * 忙等 io_ing（他人加载中）/ evicting（淘汰接管中，reap 完成即回 idle）。
 * c->blk 由加载者在 CAS 成功后写定（read 用；working 发布后 reap/flush 可读）。
 * ═══════════════════════════════════════════════════════════════════ */
raven_Inode_t *Inode_cache_mgr_t::ctrl_acquire(Inode_cache_ctrl_t *c, uint64_t blk,
                                               uint64_t page_bytes,
                                               const Inode_cache_cfg_t &cfg,
                                               bool *out_loaded) {
    for (;;) {
        uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
        const uint8_t s = (uint8_t)(w & CTRL_STATE_MASK);
        if (s == working) {
            /* 占坑：CAS 加 accessor —— 与 reap 接管互斥在同一原子操作上，无需复核 */
            if (c->ctrl_word.compare_exchange_weak(w, w + CTRL_ACCESSOR_INC,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                c->accss_stamp.store(rdtsc_timestamp(), std::memory_order_relaxed);
                if (out_loaded) *out_loaded = false;
                return c->cache_data;
            }
            continue;   /* CAS 失败（他人占坑/被接管）→ 重读 */
        }
        if (s == idle) {
            if (c->ctrl_word.compare_exchange_weak(w, (w & ~CTRL_STATE_MASK) | io_ing,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                /* 本线程拿到加载权：alloc + read 原子完成，I/O 不持锁 */
                c->blk = blk;   /* 加载者独占写；working 发布后 reap/flush 读 */
                raven_Inode_t *page = (raven_Inode_t *)cfg.alloc(cfg.io_ctx, page_bytes);
                if (!page) {
                    c->ctrl_word.store((w & ~CTRL_STATE_MASK) | idle, std::memory_order_release);
                    return nullptr;
                }
                result_code_t rc = cfg.read_page(cfg.io_ctx, blk, page);
                if (rc != RC_OK) {
                    cfg.free(cfg.io_ctx, page);
                    c->cache_data = nullptr;
                    c->ctrl_word.store((w & ~CTRL_STATE_MASK) | idle, std::memory_order_release);
                    return nullptr;
                }
                c->cache_data = page;   /* working 发布前写定，ACQUIRE 观察方可见 */
                c->ctrl_word.store((w & ~CTRL_STATE_MASK) | working, std::memory_order_release);
                if (out_loaded) *out_loaded = true;
                continue;               /* 回顶部走占坑分支（统一入口） */
            }
            continue;   /* CAS 失败 → 他人已接管，回循环顶部看最新状态 */
        }
        cpu_relax();    /* io_ing：他人加载中；evicting：淘汰接管中 */
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * ctrl_reap_one — 单行 TTL 淘汰尝试（CTRL_ARRAY / RADIX 共用）
 *
 * 仅 working && accessor==0 && 超时 的行可被 CAS 接管（前提原子打包，
 * 无 get 占坑窗口）；成功后：脏写回 → 释放页 → 回 idle。
 * 返回 true = 本行已被淘汰（调用方做统计记账）。
 * ═══════════════════════════════════════════════════════════════════ */
bool Inode_cache_mgr_t::ctrl_reap_one(Inode_cache_ctrl_t *c, const Inode_cache_cfg_t &cfg,
                                      uint64_t now, uint64_t ttl) {
    uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
    if ((w & CTRL_STATE_MASK) != working) return false;
    if ((w >> CTRL_ACCESSOR_SHIFT) != 0) return false;
    if (now - c->accss_stamp.load(std::memory_order_relaxed) <= ttl) return false;

    const uint64_t evict_w = (w & ~CTRL_STATE_MASK) | evicting;
    if (!c->ctrl_word.compare_exchange_weak(w, evict_w,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        return false;   /* 被 get 占坑/他人接管 → 放弃 */

    /* 接管成功：无锁 I/O 写回 → 释放 → 回 idle（get 忙等后重载） */
    if ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY)
        cfg.write_page(cfg.io_ctx, c->blk, c->cache_data);
    cfg.free(cfg.io_ctx, c->cache_data);
    c->cache_data = nullptr;
    const uint64_t back_idle = ((evict_w & ~CTRL_STATE_MASK) &
                                ~(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT)) | idle;
    c->ctrl_word.store(back_idle, std::memory_order_release);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * slice_radix_tree — 单分片基数树（RADIX 分支 [19,52]）
 *
 * 拓扑（由 plan 推导，见头文件注释）：
 *   - 树内层数 tree_levels_ = plan_.total_levels - 1（level_bits[0] 是 mgr 层分片）
 *   - 层 j（0=根）表宽 = 2^level_bits[1+j]；槽索引 = chunks[1+j]
 *   - 叶子层（j == tree_levels_-1）表 = Inode_cache_ctrl_t 数组（槽即 ctrl_t 实体），
 *     叶表槽索引 = chunks[total_levels-1]，行内索引 = chunks[total_levels]
 *   - 中间层表 = 指针槽数组（8B/槽，nullptr = 未建）
 *   - 表类型由层深决定，无需运行时标记
 * 根表在 ctor 建好（tree_levels_==1 → 根即叶表）；中间/叶表按需懒建（写锁内）。
 * 寻址：chunks[0] 选片（mgr 层）→ 树内逐层 → 叶表 ctrl_t → cache_data[行内]。
 * blk = base + ((inum >> row_bits_) << (row_bits_-4))（25/33 档 row_bits=5 → 8KB 两连块）。
 * ═══════════════════════════════════════════════════════════════════ */
Inode_cache_mgr_t::slice_radix_tree::slice_radix_tree(const slice_radix_tree_cfg_t &cfg)
    : host_cfg_(cfg.host_cfg), plan_(cfg.plan), row_bits_(cfg.row_bits),
      ttl_ticks_(cfg.ttl_ticks) {
    tree_levels_ = (uint8_t)(plan_.total_levels - 1);   /* level_bits[0] 是 mgr 层，不进树 */
    if (tree_levels_ == 1) {
        root_ = alloc_leaf();   /* 根即叶表（19~25 档全属此类） */
    } else {
        root_ = alloc_internal(0);   /* 根 = 中间表（指针槽数组） */
    }
}

/* 析构：全部缓存脏页写回 + 非脏丢弃 + 释放全部表（宿主须先停淘汰线程） */
Inode_cache_mgr_t::slice_radix_tree::~slice_radix_tree() {
    if (root_) dfs_destroy(root_, 0);
}

/* alloc_leaf — 叶表：ctrl_t 数组全 idle（写锁内调用） */
void *Inode_cache_mgr_t::slice_radix_tree::alloc_leaf() {
    const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
    Inode_cache_ctrl_t *leaf = (Inode_cache_ctrl_t *)host_cfg_->alloc(
        host_cfg_->io_ctx, n * sizeof(Inode_cache_ctrl_t));
    if (!leaf) return nullptr;
    for (uint64_t i = 0; i < n; i++) {
        Inode_cache_ctrl_t *c = &leaf[i];
        c->cache_data = nullptr;
        c->accss_stamp.store(0, std::memory_order_relaxed);
        c->ctrl_word.store(idle, std::memory_order_relaxed);
    }
    table_size_all_accumulate += n * sizeof(Inode_cache_ctrl_t);   /* 写锁内 */
    return leaf;
}

/* alloc_internal — 中间表：指针槽数组清零（写锁内调用） */
void *Inode_cache_mgr_t::slice_radix_tree::alloc_internal(uint8_t level) {
    const uint64_t n = 1ull << plan_.level_bits[1 + level];
    void **t = (void **)host_cfg_->alloc(host_cfg_->io_ctx, n * sizeof(void *));
    if (!t) return nullptr;
    memset(t, 0, n * sizeof(void *));
    table_size_all_accumulate += n * sizeof(void *);   /* 写锁内 */
    return t;
}

/* ensure_path — 写锁内：沿 decoded 路径补建缺失的中间表/叶表（无 I/O） */
void Inode_cache_mgr_t::slice_radix_tree::ensure_path(bitfield_decoded_result_t *d) {
    void **slot = (void **)&root_;
    for (uint8_t j = 0; j < tree_levels_; j++) {
        if (!*slot) {
            *slot = (j == tree_levels_ - 1) ? alloc_leaf() : alloc_internal(j);
            if (!*slot) return;   /* OOM：路径建不动，get 重走时 miss 报错 */
        }
        if (j == tree_levels_ - 1) break;
        void **t = (void **)*slot;
        slot = &t[d->chunks[1 + j]];
    }
}

/* locate — 读路径寻址（读锁内 / 会话内无锁皆可；树只增不删） */
Inode_cache_ctrl_t *
Inode_cache_mgr_t::slice_radix_tree::locate(bitfield_decoded_result_t *d) {
    if (!root_) return nullptr;
    void *cur = root_;
    for (uint8_t j = 0; j < tree_levels_; j++) {
        if (j == tree_levels_ - 1)
            return &((Inode_cache_ctrl_t *)cur)[d->chunks[plan_.total_levels - 1]];
        void **t = (void **)cur;
        cur = t[d->chunks[1 + j]];
        if (!cur) return nullptr;   /* 路径未建（正常：miss 走建链；异常：OOM） */
    }
    return nullptr;   /* 不可达 */
}

/* get：读锁假定一次命中；miss → 写锁建链（无 I/O）→ 回头读锁。读锁泄露到 put 释放 */
raven_Inode_t *Inode_cache_mgr_t::slice_radix_tree::get(bitfield_decoded_result_t *d) {
    lock_.lock_shared();
    Inode_cache_ctrl_t *c = locate(d);
    if (!c) {
        lock_.unlock_shared();
        lock_.lock();
        c = locate(d);
        if (!c) ensure_path(d);
        c = locate(d);   /* 建链后必命中（OOM 除外） */
        lock_.unlock();
        if (!c) return nullptr;   /* 建链 OOM：上层看到 nullptr，走错误路径 */
        lock_.lock_shared();
    }
    const uint64_t page_bytes = (1ull << row_bits_) * sizeof(raven_Inode_t);
    const uint64_t blk = host_cfg_->inode_array_base +
                         ((d->raw >> row_bits_) << (row_bits_ - 4));
    bool loaded = false;
    raven_Inode_t *page = Inode_cache_mgr_t::ctrl_acquire(c, blk, page_bytes, *host_cfg_, &loaded);
    if (!page) { lock_.unlock_shared(); return nullptr; }
    if (loaded) real_cache_accumulate.fetch_add(page_bytes, std::memory_order_relaxed);
    return page;   /* 读锁保持到 put */
}

/* put：accessor-1 + 释放 get 泄露的读锁。
 * 会话内（本线程持读锁）→ 无锁走路径安全：建链写锁被本线程读锁阻塞，树冻结。 */
void Inode_cache_mgr_t::slice_radix_tree::put(bitfield_decoded_result_t *d) {
    Inode_cache_ctrl_t *c = locate(d);
    if (c) c->ctrl_word.fetch_sub(CTRL_ACCESSOR_INC, std::memory_order_relaxed);
    lock_.unlock_shared();
}

/* mark_dirty：会话内调用（调用方已持本片读锁）→ 无锁走路径置 DIRTY */
void Inode_cache_mgr_t::slice_radix_tree::mark_dirty(bitfield_decoded_result_t *d) {
    Inode_cache_ctrl_t *c = locate(d);
    if (c) c->ctrl_word.fetch_or(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT, std::memory_order_release);
}

/* flush_all：读锁内 DFS 全量脏页写回（挂载/卸载同步点；宿主保证与 reap 不并发） */
void Inode_cache_mgr_t::slice_radix_tree::flush_all() {
    std::shared_lock lk(lock_);
    if (!root_) return;
    dfs_flush(root_, 0);
}

void Inode_cache_mgr_t::slice_radix_tree::dfs_flush(void *table, uint8_t level) {
    if (level == tree_levels_ - 1) {
        Inode_cache_ctrl_t *leaf = (Inode_cache_ctrl_t *)table;
        const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
        for (uint64_t i = 0; i < n; i++) {
            Inode_cache_ctrl_t *c = &leaf[i];
            const uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
            if ((w & CTRL_STATE_MASK) == working &&
                ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY)) {
                host_cfg_->write_page(host_cfg_->io_ctx, c->blk, c->cache_data);
                c->ctrl_word.fetch_and(~(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT),
                                       std::memory_order_release);
            }
        }
    } else {
        void **t = (void **)table;
        const uint64_t w = 1ull << plan_.level_bits[1 + level];
        for (uint64_t i = 0; i < w; i++)
            if (t[i]) dfs_flush(t[i], level + 1);
    }
}

/* reap_expired：读锁内 DFS 淘汰（不摘表 —— 摘表需写锁，会与 get 的
 * evicting 忙等构成死锁；表只增不删，读锁遍历安全） */
void Inode_cache_mgr_t::slice_radix_tree::reap_expired() {
    if (!ttl_ticks_) return;
    const uint64_t ttl = ttl_ticks_->load(std::memory_order_relaxed);
    if (!ttl) return;
    std::shared_lock lk(lock_);
    if (!root_) return;
    const uint64_t now = rdtsc_timestamp();
    dfs_reap(root_, 0, now, ttl);
}

void Inode_cache_mgr_t::slice_radix_tree::dfs_reap(void *table, uint8_t level,
                                                   uint64_t now, uint64_t ttl) {
    const uint64_t page_bytes = (1ull << row_bits_) * sizeof(raven_Inode_t);
    if (level == tree_levels_ - 1) {
        Inode_cache_ctrl_t *leaf = (Inode_cache_ctrl_t *)table;
        const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
        for (uint64_t i = 0; i < n; i++) {
            if (Inode_cache_mgr_t::ctrl_reap_one(&leaf[i], *host_cfg_, now, ttl))
                real_cache_accumulate.fetch_sub(page_bytes, std::memory_order_relaxed);
        }
    } else {
        void **t = (void **)table;
        const uint64_t w = 1ull << plan_.level_bits[1 + level];
        for (uint64_t i = 0; i < w; i++)
            if (t[i]) dfs_reap(t[i], level + 1, now, ttl);
    }
}

/* dfs_destroy：脏页写回 + 非脏丢弃 + 释放全部表（析构，无并发） */
void Inode_cache_mgr_t::slice_radix_tree::dfs_destroy(void *table, uint8_t level) {
    if (level == tree_levels_ - 1) {
        Inode_cache_ctrl_t *leaf = (Inode_cache_ctrl_t *)table;
        const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
        for (uint64_t i = 0; i < n; i++) {
            Inode_cache_ctrl_t *c = &leaf[i];
            const uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
            if ((w & CTRL_STATE_MASK) == working && c->cache_data) {
                if ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY)
                    host_cfg_->write_page(host_cfg_->io_ctx, c->blk, c->cache_data);
                host_cfg_->free(host_cfg_->io_ctx, c->cache_data);
            }
            /* io_ing/evicting：无并发析构下不可能残留页（cache_data 应为空），防御跳过 */
        }
        host_cfg_->free(host_cfg_->io_ctx, leaf);
    } else {
        void **t = (void **)table;
        const uint64_t w = 1ull << plan_.level_bits[1 + level];
        for (uint64_t i = 0; i < w; i++)
            if (t[i]) dfs_destroy(t[i], level + 1);
        host_cfg_->free(host_cfg_->io_ctx, t);
    }
}
