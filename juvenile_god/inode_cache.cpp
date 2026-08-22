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
 * 按 bits_len_of_inodes_count 逐档固化 per_level_width：
 * 数组和 = bits - 4（余 4bit = 行内引索），逐层 cut_high 递归切割。
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
 * 构造：模式由 bits 推导；FULL_RAM 在此全量载入
 * ═══════════════════════════════════════════════════════════════════ */
Inode_cache_mgr_t::Inode_cache_mgr_t(const Inode_cache_cfg_t &cfg, result_code_t *rc_out)
    : cfg_(cfg), mode_(inode_cache_mode_of(cfg.bits_len_of_inodes_count)) {
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

/* 析构：全生命周期收口 — 脏页回写 → 释放 */
Inode_cache_mgr_t::~Inode_cache_mgr_t() {
    switch (mode_) {
    case CACHE_MODE_FULL_RAM:
        flush_full_ram();
        cfg_.free(cfg_.io_ctx, full_ram_dirty_);
        cfg_.free(cfg_.io_ctx, full_ram_array_);
        break;
    case CACHE_MODE_CTRL_ARRAY:
        flush_ctrl_array();
        cfg_.free(cfg_.io_ctx, data_pages_);
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
    full_ram_dirty_ = (uint8_t *)cfg_.alloc(cfg_.io_ctx, (pages + 7) / 8);
    if (!full_ram_array_ || !full_ram_dirty_) return RC_ERR_NO_BLOCK;
    for (uint64_t i = 0; i < (pages + 7) / 8; i++) full_ram_dirty_[i] = 0;
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
    data_pages_ = (raven_Inode_t *)cfg_.alloc(cfg_.io_ctx,
                                               n * INODES_PER_PAGE * sizeof(raven_Inode_t));
    if (!ctrl_array_ || !data_pages_) return RC_ERR_NO_BLOCK;
    for (uint64_t i = 0; i < n; i++) {
        Inode_cache_ctrl_t *c = &ctrl_array_[i];
        c->cache_data     = &data_pages_[i * INODES_PER_PAGE];  /* I/O 目标，位置即身份 */
        c->accss_stamp    = 0;
        c->numof_accessor = 0;
        c->flags          = 0;
        c->state          = idle;    /* 未载入；CAS 状态机见 get() */
    }
    return RC_OK;
}

/* ── get：三模式统一入口 ── */
raven_Inode_t *Inode_cache_mgr_t::get(uint64_t inum) {
    if (inum >= cfg_.inodes_count) return nullptr;   /* 防御：上层保证合法 */

    switch (mode_) {
    case CACHE_MODE_FULL_RAM:
        /* 全量常驻：直接数组寻址，恒命中，零 I/O 零锁 */
        return &full_ram_array_[inum];

    case CACHE_MODE_CTRL_ARRAY: {
        const uint64_t line = line_of(inum);
        Inode_cache_ctrl_t *c = &ctrl_array_[line];
        /* 懒加载状态机：idle → io_ing(CAS) → I/O（不持锁）→ working
         * 控制器全量常驻、数据页加载后常驻不淘汰 → 无 victim、
         * 无 tag 比较，位置即身份 */
        for (;;) {
            uint8_t s = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
            if (s == working) break;                        /* 已就绪 */
            if (s == idle) {
                uint8_t expect = idle;
                if (__atomic_compare_exchange_n(&c->state, &expect, io_ing, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    /* 本线程拿到加载权：I/O 不持锁 */
                    result_code_t rc = cfg_.read_page(cfg_.io_ctx, blk_of(line), c->cache_data);
                    if (rc != RC_OK) {
                        /* 失败回 idle，他人可重试；本次调用返回空 */
                        __atomic_store_n(&c->state, idle, __ATOMIC_RELEASE);
                        return nullptr;
                    }
                    __atomic_store_n(&c->state, working, __ATOMIC_RELEASE);
                    break;
                }
                /* CAS 失败 → 他人已接管，回循环顶部看最新状态 */
            } else {                                        /* io_ing：他人 I/O 中，忙等 */
                cpu_relax();
            }
        }
        __atomic_add_fetch(&c->numof_accessor, 1, __ATOMIC_RELAXED);
        c->accss_stamp = rdtsc_timestamp();
        return &c->cache_data[row_of(inum)];
    }

    default: /* RADIX_SHARD：草稿未实装 */
        return nullptr;
    }
}

/* ── put：accessor 计数 -1（须与 get 配对，上层保证） ── */
void Inode_cache_mgr_t::put(uint64_t inum) {
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
    case CACHE_MODE_FULL_RAM: {
        const uint64_t line = line_of(inum);
        __atomic_or_fetch(&full_ram_dirty_[line >> 3], (uint8_t)(1u << (line & 7)),
                          __ATOMIC_RELEASE);
        break;
    }
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
    const uint64_t pages = ctrl_count();
    for (uint64_t i = 0; i < pages; i++) {
        if (__atomic_load_n(&full_ram_dirty_[i >> 3], __ATOMIC_ACQUIRE) & (1u << (i & 7))) {
            cfg_.write_page(cfg_.io_ctx, blk_of(i), &full_ram_array_[i * INODES_PER_PAGE]);
            __atomic_and_fetch(&full_ram_dirty_[i >> 3], (uint8_t)~(1u << (i & 7)),
                               __ATOMIC_RELEASE);
        }
    }
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
