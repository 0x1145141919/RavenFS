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
        ttl_ticks_ = cfg.ttl_ticks;
    } else {
        tsc_info_t tsc;
        if (tsc_probe(&tsc)) ttl_ticks_ = tsc_ticks_of_seconds(2, tsc);
    }

    result_code_t rc = RC_OK;
    switch (mode_) {
    case CACHE_MODE_FULL_RAM:   rc = ctor_full_ram();   break;
    case CACHE_MODE_CTRL_ARRAY: rc = ctor_ctrl_array(); break;
    case CACHE_MODE_RADIX_SHARD:
        /* 草稿：按 statey 表解析 global_slice_plan（cut_high 逐层切），
         * 分片树/片 RW 锁/准入淘汰待实装 */
        break;
    }
    ready_ = (rc == RC_OK);
    if (rc_out) *rc_out = rc;
}

/* set_ttl_ticks：外部调整缓存行 TTL（tick）。slice 持 &ttl_ticks_ 共享，即时生效，无需逐片同步 */
void Inode_cache_mgr_t::set_ttl_ticks(uint64_t ticks) {
    ttl_ticks_ = ticks;
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
    default:
        break;   /* RADIX：逐片析构，待实装 */
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
        c->cache_data     = nullptr;   /* 懒分配：get 的 io_ing 态内 alloc+read 原子完成 */
        c->accss_stamp    = 0;
        c->numof_accessor = 0;
        c->flags          = 0;
        c->state          = idle;    /* 未载入；CAS 状态机见 get() */
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
        const uint64_t line = line_of(inum);
        Inode_cache_ctrl_t *c = &ctrl_array_[line];
        /* 懒加载状态机：idle → io_ing(CAS) → I/O（不持锁）→ working；
         * TTL 淘汰（reap_expired）可能并发：working 行被 CAS 接管为 evicting
         * （写回+释放进行中）。故命中后必须「先占坑(+accessor) 再复核 state」：
         * 复核失败说明已被接管，退坑重试；加载完成也统一回顶部走占坑分支。
         * evicting / io_ing 均忙等（罕见路径，reap 完成即回 idle）。 */
        for (;;) {
            uint8_t s = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
            if (s == working) {
                __atomic_add_fetch(&c->numof_accessor, 1, __ATOMIC_RELAXED);
                if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == working) {
                    __atomic_store_n(&c->accss_stamp, rdtsc_timestamp(), __ATOMIC_RELAXED);
                    break;
                }
                __atomic_sub_fetch(&c->numof_accessor, 1, __ATOMIC_RELAXED);   /* 退坑 */
                cpu_relax();
                continue;
            }
            if (s == idle) {
                uint8_t expect = idle;
                if (__atomic_compare_exchange_n(&c->state, &expect, io_ing, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    /* 本线程拿到加载权：alloc + read 原子完成，I/O 不持锁 */
                    raven_Inode_t *page = (raven_Inode_t *)cfg_.alloc(
                        cfg_.io_ctx, INODES_PER_PAGE * sizeof(raven_Inode_t));
                    if (!page) {
                        __atomic_store_n(&c->state, idle, __ATOMIC_RELEASE);
                        return nullptr;
                    }
                    result_code_t rc = cfg_.read_page(cfg_.io_ctx, blk_of(line), page);
                    if (rc != RC_OK) {
                        cfg_.free(cfg_.io_ctx, page);
                        c->cache_data = nullptr;
                        __atomic_store_n(&c->state, idle, __ATOMIC_RELEASE);
                        return nullptr;
                    }
                    c->cache_data = page;   /* working 发布前写定，ACQUIRE 观察方可见 */
                    __atomic_store_n(&c->state, working, __ATOMIC_RELEASE);
                    continue;               /* 统一回顶部走占坑分支 */
                }
                /* CAS 失败 → 他人已接管，回循环顶部看最新状态 */
            } else {                        /* io_ing：他人加载中；evicting：淘汰接管中 */
                cpu_relax();
            }
        }
        return &c->cache_data[row_of(inum)];
    }

    default: /* RADIX_SHARD：草稿未实装 */
        return nullptr;
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
        __atomic_sub_fetch(&ctrl_array_[line_of(inum)].numof_accessor, 1, __ATOMIC_RELAXED);
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
        __atomic_or_fetch(&c->flags, (uint16_t)CTRL_FLAG_DIRTY, __ATOMIC_RELEASE);
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
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == working &&
            (__atomic_load_n(&c->flags, __ATOMIC_ACQUIRE) & CTRL_FLAG_DIRTY)) {
            cfg_.write_page(cfg_.io_ctx, blk_of(i), c->cache_data);
            __atomic_and_fetch(&c->flags, (uint16_t)~CTRL_FLAG_DIRTY, __ATOMIC_RELEASE);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * reap_expired — 专用淘汰线程周期调用：回收全部「超时 && 无访问者」的行
 *
 * TTL 淘汰（非压力淘汰）：inode 缓存行小（4K/8K），真正需要压力淘汰的是
 * 文件数据缓存；这里只做时间戳超时回收。
 * 并发安全（与 get 共用 CAS 状态机）：
 *   - 仅 working && accessor==0 && 超时 的行可被接管（get 已占坑的必不满足）
 *   - 接管 = CAS working→evicting，之后无锁 I/O（写回+释放），完成后回 idle
 *   - get 在 evicting 上忙等，回 idle 后重新懒加载
 * 注意：与 flush_all 不同，reap 不要求与 get 互斥；但宿主应在卸载前先停淘汰线程。
 * ═══════════════════════════════════════════════════════════════════ */
void Inode_cache_mgr_t::reap_expired() {
    if (!ttl_ticks_) return;              /* 无 TTL 时钟（TSC 不可靠）：淘汰禁用 */
    const uint64_t now = rdtsc_timestamp();

    switch (mode_) {
    case CACHE_MODE_CTRL_ARRAY: {
        const uint64_t n = ctrl_count();
        for (uint64_t i = 0; i < n; i++) {
            Inode_cache_ctrl_t *c = &ctrl_array_[i];
            if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != working) continue;
            if (__atomic_load_n(&c->numof_accessor, __ATOMIC_RELAXED) != 0) continue;
            if (now - __atomic_load_n(&c->accss_stamp, __ATOMIC_RELAXED) <= ttl_ticks_) continue;

            uint8_t expect = working;
            if (__atomic_compare_exchange_n(&c->state, &expect, evicting, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                /* 接管成功：无锁 I/O 写回 → 释放 → 回 idle（get 忙等后重载） */
                if (__atomic_load_n(&c->flags, __ATOMIC_ACQUIRE) & CTRL_FLAG_DIRTY)
                    cfg_.write_page(cfg_.io_ctx, blk_of(i), c->cache_data);
                cfg_.free(cfg_.io_ctx, c->cache_data);
                c->cache_data = nullptr;
                __atomic_and_fetch(&c->flags, (uint16_t)~CTRL_FLAG_DIRTY, __ATOMIC_RELEASE);
                __atomic_store_n(&c->state, idle, __ATOMIC_RELEASE);
            }
        }
        break;
    }
    case CACHE_MODE_RADIX_SHARD:
        /* 逐片转发：RADIX 实装后 for 每片 slice->reap_expired()（TODO） */
        break;
    default:
        break;   /* FULL_RAM：全量常驻，无淘汰 */
    }
}
