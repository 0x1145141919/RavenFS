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
#include "ravenfs/mdata_piece_caches.hpp"
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

/* statey_levels_of — 按 bits_len 查 RADIX 策略表（14~52）
 * 返回 level_bits 数组（指向 mdp_statey_XX 全局数组，生命周期全程有效），
 * out_levels 收层数；非法档返回 nullptr（bits < 14 或 > 52 不会走到：
 * 档位判定已把 ≤13 归入 CTRL_ARRAY，>52 超出负载计数上限防御拒）。
 * 层数 = 数组长度（sizeof 派生，唯一真相源）——微调 statey 表（改层数）时
 * 本函数**无需同步**改计数，杜绝"表改了层数没改"导致的 sum!=bits 静默拒挂载。 */
uint8_t *mdata_piece_caches_mgr::statey_levels_of(uint8_t bits, uint8_t *out_levels) {
    uint8_t *lv = nullptr;
#define MDP_STATEY_CASE(b) \
    case b: lv = mdp_statey_##b; \
            *out_levels = (uint8_t)(sizeof(mdp_statey_##b) / sizeof(mdp_statey_##b[0])); break;
    switch (bits) {
    MDP_STATEY_CASE(14) MDP_STATEY_CASE(15) MDP_STATEY_CASE(16) MDP_STATEY_CASE(17)
    MDP_STATEY_CASE(18) MDP_STATEY_CASE(19) MDP_STATEY_CASE(20) MDP_STATEY_CASE(21)
    MDP_STATEY_CASE(22) MDP_STATEY_CASE(23) MDP_STATEY_CASE(24) MDP_STATEY_CASE(25)
    MDP_STATEY_CASE(26) MDP_STATEY_CASE(27) MDP_STATEY_CASE(28) MDP_STATEY_CASE(29)
    MDP_STATEY_CASE(30) MDP_STATEY_CASE(31) MDP_STATEY_CASE(32) MDP_STATEY_CASE(33)
    MDP_STATEY_CASE(34) MDP_STATEY_CASE(35) MDP_STATEY_CASE(36) MDP_STATEY_CASE(37)
    MDP_STATEY_CASE(38) MDP_STATEY_CASE(39) MDP_STATEY_CASE(40) MDP_STATEY_CASE(41)
    MDP_STATEY_CASE(42) MDP_STATEY_CASE(43) MDP_STATEY_CASE(44) MDP_STATEY_CASE(45)
    MDP_STATEY_CASE(46) MDP_STATEY_CASE(47) MDP_STATEY_CASE(48) MDP_STATEY_CASE(49)
    MDP_STATEY_CASE(50) MDP_STATEY_CASE(51) MDP_STATEY_CASE(52)
    default: *out_levels = 0; break;
    }
#undef MDP_STATEY_CASE
    return lv;
}

/* ═══════════════════════════════════════════════════════════════════
 * 文件级辅助：bits 推导 + 8K 对齐分配器
 * ═══════════════════════════════════════════════════════════════════ */
/* ceil(log2(n))：v2 cfg 只给 Piece_count，bits 由它推导（档位判定唯一真相源）。
 * n 上限 2^52（bits 14~52 档）；n=1 → bits=1（CTRL_ARRAY 档）。 */
static uint8_t bits_len_of(uint64_t count) {
    uint8_t b = 0;
    while ((1ull << b) < count) b++;
    return b;
}

/* 8K 对齐分配：负载页恒 8192B，O_DIRECT 直读需要 8K 对齐（no_cache 会话缓冲
 * 同规格）。表结构（ctrl_t 数组 / 指针槽数组）大小不定，统一 8K 对齐上取整。
 * 移植（sparrowos / linux 内核）时改这两处即可，mgr 核心零改动。 */
void *mdata_piece_caches_mgr::mdp_alloc(uint64_t size) {
    return std::aligned_alloc(8192, (size + 8191) & ~8191ull);
}
void mdata_piece_caches_mgr::mdp_free(void *ptr) {
    std::free(ptr);
}

/* ═══════════════════════════════════════════════════════════════════
 * 内部装配：self_reliat 后端（几何/回调成员已由 v2 ctor 写定）
 *
 * 组装 rt_ 快照（v2 派生：I/O 回调 + io_ctx + 几何，分片树持值拷贝），
 * TTL 换算（ttl_ticks_cfg 非 0 直接用；0 → 按 Invariant TSC 换算默认 2s，
 * TSC 不可靠/无频率 → 0 = TTL 淘汰禁用，reap 空转），然后按 bits 跑双档：
 *   bits ≤ 13 → CTRL_ARRAY（控制器全量常驻，懒加载不淘汰）
 *   bits > 13 → RADIX（statey 表 + 分片树；sum(level_bits) 必须 = bits，
 *               row_bits=0 保证行 = 1 负载 —— 这是 A 路线的硬约束）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t mdata_piece_caches_mgr::ctor_self_reliat(uint64_t ttl_ticks_cfg) {
    if (!piece_count_) { MDP_STAT1(stat_, MDP_INIT_FAIL); return RC_ERR_STATE; }   /* 防御：退化配置（v2 ctor 已查基址偶数） */
    rt_.read_block    = read_block;
    rt_.write_block   = write_block;
    rt_.io_ctx        = io_ctx;
    rt_.pieces_count  = piece_count_;
    rt_.pieces_base   = piece_base_;
    bits_len_ = bits_len_of(piece_count_);
    mode_ = (bits_len_ <= 13) ? MDP_MODE_CTRL_ARRAY : MDP_MODE_RADIX_SHARD;

    /* TTL 初始化：显式 tick 优先；否则按 Invariant TSC 频率换算默认 2s。
     * TSC 不可靠/无频率 → ttl_ticks_ 保持 0 = TTL 淘汰禁用（reap_expired 空转）。 */
    if (ttl_ticks_cfg) {
        ttl_ticks_.store(ttl_ticks_cfg, std::memory_order_relaxed);
    } else {
        tsc_info_t tsc;
        if (tsc_probe(&tsc)) ttl_ticks_.store(tsc_ticks_of_seconds(2, tsc), std::memory_order_relaxed);
    }

    result_code_t rc = RC_OK;
    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY:
        rc = ctor_ctrl_array();
        break;
    case MDP_MODE_RADIX_SHARD: {
        /* 按 statey 表解析 global_slice_plan（level_bits 指向 mdp_statey_XX 全局数组）。
         * level_bits[0] = mgr 层分片选择位宽（slice 数 = 2^w0），level_bits[1..]
         * = 树内逐层位宽；row_bits = bits - Σlevel_bits 必须 = 0（行 = 1 负载）。 */
        global_slice_plan.bits_len = bits_len_;
        global_slice_plan.level_bits = statey_levels_of(bits_len_,
                                                        &global_slice_plan.total_levels);
        if (!global_slice_plan.level_bits || global_slice_plan.total_levels == 0) {
            rc = RC_ERR_NO_BLOCK;
            break;
        }
        uint16_t sum = 0;
        for (uint8_t i = 0; i < global_slice_plan.total_levels; i++)
            sum += global_slice_plan.level_bits[i];
        if (sum != global_slice_plan.bits_len) { rc = RC_ERR_NO_BLOCK; break; }  /* 表必须 row_bits=0（A 路线硬约束） */
        n_slices_ = 1ull << global_slice_plan.level_bits[0];
        slices_ = (slice_radix_tree **)mdp_alloc(n_slices_ * sizeof(slice_radix_tree *));
        if (!slices_) { rc = RC_ERR_NO_BLOCK; break; }
        memset(slices_, 0, n_slices_ * sizeof(slice_radix_tree *));
        slice_radix_tree_cfg_t scfg;
        scfg.rt        = rt_;        /* 值拷贝：每片独立持一份快照，无共享指针/悬垂 */
        scfg.plan      = global_slice_plan;
        scfg.ttl_ticks = &ttl_ticks_;
        scfg.stat      = stat_;      /* md_ 计数钩子下传各片（片内埋点）*/
        for (uint64_t i = 0; i < n_slices_; i++) {
            void *mem = mdp_alloc(sizeof(slice_radix_tree));
            if (!mem) { rc = RC_ERR_NO_BLOCK; break; }
            slices_[i] = new (mem) slice_radix_tree(scfg);
        }
        break;
    }
    }
    if (rc == RC_OK) {   /* 装配成功：上报档位与分片数 */
        MDP_STAT1(stat_, MDP_INIT_OK);
        MDP_STAT1(stat_, mode_ == MDP_MODE_RADIX_SHARD ? MDP_INIT_RADIX : MDP_INIT_CTRL_ARRAY);
        if (mode_ == MDP_MODE_RADIX_SHARD) MDP_STAT(stat_, MDP_SLICES, n_slices_);
    } else {
        MDP_STAT1(stat_, MDP_INIT_FAIL);
    }
    return rc;
}

/* ── CTRL_ARRAY 构造：控制器全量建好（状态全 idle），数据页懒加载 ── */
result_code_t mdata_piece_caches_mgr::ctor_ctrl_array() {
    const uint64_t n = piece_count_;
    ctrl_array_ = (mdata_piece_ctrl_t *)mdp_alloc(n * sizeof(mdata_piece_ctrl_t));
    if (!ctrl_array_) return RC_ERR_NO_BLOCK;
    for (uint64_t i = 0; i < n; i++) {
        mdata_piece_ctrl_t *c = &ctrl_array_[i];
        c->cache_data = nullptr;                  /* 懒分配：get 的 io_ing 态内 alloc+read 原子完成 */
        c->accss_stamp.store(0, std::memory_order_relaxed);
        c->latest_syn_stamp.store(0, std::memory_order_relaxed);   /* 未 load：无同步时刻 */
        c->ctrl_word.store(idle, std::memory_order_relaxed);   /* 未载入；CAS 状态机见 get() */
        c->blk = 0;
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * self_reliat_get — self_reliat 后端统一入口（CTRL_ARRAY / RADIX 双档表面一致）
 *
 * 仅 backed_mode == self_reliat_cache 时有意义；其余后端一律 nullptr
 * （会话按模式分派，不会走到；守卫纯防御）。
 * 加载前提：调用方（session）已用 blkidx_valid 校验 —— 负载 2 块恒完整，
 * 直接 read_block(blkidx, 2)；blk 即 blkidx，无 inode 版 base+line 派生。
 * ═══════════════════════════════════════════════════════════════════ */
uint8_t *mdata_piece_caches_mgr::self_reliat_get(uint64_t blkidx) {
    if (backed_mode != cache_work_mode_t::self_reliat_cache) return nullptr;
    if (!blkidx_valid(blkidx)) return nullptr;   /* 防御：session 已校验，双保险 */
    const uint64_t piece_idx = (blkidx - piece_base_) >> 1;   /* 负载序数 */

    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY: {
        /* 懒加载状态机（公共原语 mdp_ctrl_acquire）：idle → io_ing(CAS) → I/O（不持锁）→ working；
         * 占坑（+accessor）与 state 打包在同一原子字 ctrl_word —— reap「accessor==0 才能接管」
         * 与 get 占坑是同一原子操作，无 TOCTOU 窗口（位域注释见 inode_cache_radix.hpp）。
         * 控制器位置即身份：line = piece_idx（行 = 1 负载）。 */
        mdata_piece_ctrl_t *c = &ctrl_array_[piece_idx];
        return mdp_ctrl_acquire(c, blkidx, rt_, nullptr, nullptr, &stat_);
    }
    case MDP_MODE_RADIX_SHARD: {
        /* 分片树：decode 位域（raw = piece_idx）→ chunks[0] 选片，树内逐层寻址。
         * row_bits=0：chunks[total_levels] 恒 0（行 = 1 负载），叶表 ctrl 即负载控制器，
         * 命中返回整 8K 页，无行内偏移。 */
        bitfield_decoded_result_t d = decode(piece_idx, global_slice_plan);
        slice_radix_tree *s = slices_[d.chunks[0]];
        if (!s) return nullptr;   /* ctor 部分失败防御 */
        return s->get(&d);
    }
    default:
        return nullptr;   /* 不可达：档位判定已覆盖全部枚举 */
    }
}

/* ── self_reliat_put：accessor 计数 -1（须与 get 配对，上层保证）。
 * 无需 token：blkidx 重新 decode 定位分片（与 inode 版 token 等价，接口更简）。 ── */
void mdata_piece_caches_mgr::self_reliat_put(uint64_t blkidx) {
    if (backed_mode != cache_work_mode_t::self_reliat_cache) return;
    if (!blkidx_valid(blkidx)) return;
    const uint64_t piece_idx = (blkidx - piece_base_) >> 1;
    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY:
        ctrl_array_[piece_idx].ctrl_word.fetch_sub(CTRL_ACCESSOR_INC, std::memory_order_release);   /* release：与 reap 的 acquire 同步，保「占坑-访问」happens-before 于 free */
        MDP_STAT1(stat_, MDP_PUT);
        break;
    case MDP_MODE_RADIX_SHARD:
        /* 重新 decode 定位分片（与 get 一致）；slice::put 内 accessor-1（无锁） */
        {
            bitfield_decoded_result_t d = decode(piece_idx, global_slice_plan);
            if (d.chunks[0] < n_slices_ && slices_[d.chunks[0]])
                slices_[d.chunks[0]]->put(&d);
        }
        break;
    default:
        break;
    }
}

/* ── self_reliat_mark_dirty：页粒度脏标记（I/O 恒为整 8K 页回写） ── */
void mdata_piece_caches_mgr::self_reliat_mark_dirty(uint64_t blkidx) {
    if (backed_mode != cache_work_mode_t::self_reliat_cache) return;
    if (!blkidx_valid(blkidx)) return;
    const uint64_t piece_idx = (blkidx - piece_base_) >> 1;
    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY: {
        /* 仅 working 有效：会话内调用（mdata_piece_session::dirty），上层保证 */
        mdata_piece_ctrl_t *c = &ctrl_array_[piece_idx];
        c->ctrl_word.fetch_or(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT, std::memory_order_release);
        MDP_STAT1(stat_, MDP_DIRTY);
        break;
    }
    case MDP_MODE_RADIX_SHARD: {
        /* 会话内调用；slice::mark_dirty 无锁走路径安全（树只增不删，读锁不跨会话） */
        bitfield_decoded_result_t d = decode(piece_idx, global_slice_plan);
        if (d.chunks[0] < n_slices_ && slices_[d.chunks[0]])
            slices_[d.chunks[0]]->mark_dirty(&d);
        break;
    }
    default:
        break;
    }
}

/* self_reliat_set_ttl_ticks：外部调整缓存行 TTL（tick）。slice 持 &ttl_ticks_ 共享，
 * 即时生效，无需逐片同步；0 = 禁用淘汰。 */
void mdata_piece_caches_mgr::self_reliat_set_ttl_ticks(uint64_t ticks) {
    ttl_ticks_.store(ticks, std::memory_order_relaxed);
}

/* self_reliat_reap_expired — 专用淘汰线程周期调用：回收「超时 && 无访问者」的负载行
 * 并发安全（与 get 共用 ctrl_word CAS 状态机，注释见 inode_cache.cpp 同名单函数）：
 *   - 仅 working && accessor==0 && 超时 的行可被接管（CAS 前提原子包含 accessor==0）
 *   - 接管 = CAS working→evicting，之后无锁 I/O（写回+释放），完成后回 idle
 * 与 flush_all 的关系：RADIX 档由片写锁互斥；CTRL_ARRAY 档无片锁 —— 宿主仍应在
 * 卸载前先停淘汰线程，保证 flush_all 处于静默点（见 flush_self_reliat 注释）。 */
void mdata_piece_caches_mgr::self_reliat_reap_expired() {
    if (backed_mode != cache_work_mode_t::self_reliat_cache) return;
    const uint64_t ttl = ttl_ticks_.load(std::memory_order_relaxed);
    if (!ttl) return;             /* 无 TTL 时钟（TSC 不可靠）：淘汰禁用 */
    const uint64_t now = rdtsc_timestamp();

    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY:
        for (uint64_t i = 0; i < piece_count_; i++)
            mdp_ctrl_reap_one(&ctrl_array_[i], rt_, now, ttl, &stat_);
        break;
    case MDP_MODE_RADIX_SHARD:
        for (uint64_t i = 0; i < n_slices_; i++)
            if (slices_[i]) slices_[i]->reap_expired();
        break;
    }
}

/* self_reliat_reap_expired_strong — 强淘汰（整树回收；低频重操作）。力度见 mdp_squeeze_gear_t：
 *   PRESSURE：无视 TTL，把 0-ref 行全踹 + 回收空表（内存压力驱动）；
 *   LOWLOAD ：在强刷回 + 回收空表基础上，只踹 TTL 到期的 0-ref 冷行（ioctl 低载触发）。
 * CTRL_ARRAY：无表可摘（控制数组永久）→ 逐控制器 reap_one（PRESSURE 传 ttl=0 无视；LOWLOAD 传 ttl）。
 * RADIX     ：逐片 slice::reap_expired_strong（写锁内后序：先踹叶表 0-ref 行，再由下到上摘空表）。
 * 并发：与 get 的探测段（读锁）+ ensure_path 写锁互斥；在飞 I/O 段其行恒 io_ing
 * → 被 dfs_reap_strong 保留，绝不误释放。触发时机归宿主，不接周期路径。 */
void mdata_piece_caches_mgr::self_reliat_reap_expired_strong(mdp_squeeze_gear_t gear) {
    if (backed_mode != cache_work_mode_t::self_reliat_cache) return;
    const bool respect_ttl = (gear == MDP_SQUEEZE_LOWLOAD);
    const uint64_t ttl = respect_ttl ? ttl_ticks_.load(std::memory_order_relaxed) : 0;
    if (respect_ttl && !ttl) return;   /* 无 TTL 时钟：LOWLOAD 判不了冷，退化为 no-op */
    const uint64_t now = rdtsc_timestamp();
    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY:
        for (uint64_t i = 0; i < piece_count_; i++)
            mdp_ctrl_reap_one(&ctrl_array_[i], rt_, now, ttl, &stat_, /*strong=*/true);   /* CTRL_ARRAY 强淘汰记 MDP_REAP_STRONG */
        break;
    case MDP_MODE_RADIX_SHARD:
        for (uint64_t i = 0; i < n_slices_; i++)
            if (slices_[i]) slices_[i]->reap_expired_strong(gear);
        break;
    }
}

/* self_reliat_mode：内部缓存档位（仅 self_reliat 后端有意义；其余后端返回占位值，勿依赖） */
mdata_piece_mode_t mdata_piece_caches_mgr::self_reliat_mode() const {
    return mode_;
}

/* ═══════════════════════════════════════════════════════════════════
 * mdp_ctrl_flush_one — flush_all 单行语义（CTRL_ARRAY / RADIX 共用）
 *
 *   脏行：写回 + 清 DIRTY，保持驻留；非脏行：CAS 接管（working && accessor==0）
 *   → 释放页 → 回 idle（用户态丢弃）。与 mdp_ctrl_reap_one 的差异：无 TTL 前提。
 * 并发：RADIX 档由 slice::flush_all 的**片写锁**独占（卸载静默点，无并发
 * get/put/mark_dirty/reap）→ 脏写回期间页不会被改写/释放，H2/H3 关闭；
 * CTRL_ARRAY 档无片锁，仍依赖宿主「卸载时无访问者」契约。
 * 返回 true = 本行已丢弃（调用方做统计记账）。
 * ═══════════════════════════════════════════════════════════════════ */
bool mdata_piece_caches_mgr::mdp_ctrl_flush_one(mdata_piece_ctrl_t *c, const mdata_piece_rt_t &rt,
                                                const mdp_stat_hook *stat) {
    uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
    if ((w & CTRL_STATE_MASK) != working) return false;
    if ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY) {
        rt.write_block(rt.io_ctx, c->blk, 2, c->cache_data);   /* 负载恒 2 块整写回 */
        c->ctrl_word.fetch_and(~(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT), std::memory_order_release);
        if (stat) MDP_STAT1(*stat, MDP_FLUSH_WB);   /* 脏行写回（驻留）*/
        return false;   /* 写回后驻留，未丢弃 */
    }
    if ((w >> CTRL_ACCESSOR_SHIFT) != 0) return false;   /* 会话占用中：不丢 */
    const uint64_t evict_w = (w & ~CTRL_STATE_MASK) | evicting;
    if (!c->ctrl_word.compare_exchange_weak(w, evict_w, std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        return false;   /* 被 get 占坑 → 放弃（下轮 flush 再丢） */
    mdp_free(c->cache_data);
    c->cache_data = nullptr;
    c->ctrl_word.store((evict_w & ~CTRL_STATE_MASK) | idle, std::memory_order_release);
    if (stat) {   /* 非脏行丢弃 */ MDP_STAT1(*stat, MDP_FLUSH_DROP); MDP_STAT(*stat, MDP_RESIDENT_DEL_BYTES, 8192); }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * mdp_ctrl_acquire — 缓存行懒加载状态机（CTRL_ARRAY / RADIX 共用）
 *
 * 返回 working 且 accessor 已 +1 的 8K 页基址；alloc/read 失败回 idle 并返回
 * nullptr（调用方解锁/返回）。out_loaded 置位 = 本调用实际完成页加载。
 * blk_len 恒 2、页字节恒 8192：行 = 1 负载，blkidx_valid 已保证负载完整，
 * 直接 read_block(blk, 2)，无末行收窄（A 路线核心简化）。
 *
 * held（RADIX）：调用方（slice::get 的句柄）持有的分片读锁 —— **只在探测段
 * （load + CAS）持锁**，防止 strong reap 在 CAS 前连叶表一起释放；一旦 CAS 到
 * io_ing（或占坑成功），即放锁：状态机 io_ing 与 working|accessor>0 全程钉住叶表，
 * 故 I/O 与发布无需持锁（加载是高频路径，不把慢 I/O 兜在锁里）。
 * CTRL_ARRAY 传 nullptr（控制数组永不释放，无需锁）。
 * ═══════════════════════════════════════════════════════════════════ */
uint8_t *mdata_piece_caches_mgr::mdp_ctrl_acquire(mdata_piece_ctrl_t *c, uint64_t blk,
                                                  const mdata_piece_rt_t &rt,
                                                  bool *out_loaded,
                                                  std::shared_lock<std::shared_mutex> *held,
                                                  const mdp_stat_hook *stat) {
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
                if (stat) MDP_STAT1(*stat, MDP_GET_HIT);   /* 快路径命中 */
                return c->cache_data;   /* held 锁由调用方句柄析构释放 */
            }
            continue;   /* CAS 失败（他人占坑/被接管）→ 重读 */
        }
        if (s == idle) {
            if (c->ctrl_word.compare_exchange_weak(w, (w & ~CTRL_STATE_MASK) | io_ing,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                /* 本线程拿到加载权（idle→io_ing）。此后**整段放锁**：状态机 io_ing
                 * 及随后的 working|accessor>0 全程钉住叶表 —— reap/flush 见非 working
                 * 跳过，strong reap 对 io_ing/占用行保留叶表且不碰缓冲。故 I/O 与发布
                 * 均无需持锁（避免"放锁后重持"与等待写者形成环）。 */
                c->blk = blk;           /* 加载者独占写；working 发布后 reap/flush 读 */
                if (held) held->unlock();
                uint8_t *page = (uint8_t *)mdp_alloc(8192);
                if (page && rt.read_block(rt.io_ctx, blk, 2, page) != RC_OK) {   /* 负载恒 2 块 */
                    mdp_free(page);
                    page = nullptr;
                }
                if (!page) {
                    c->ctrl_word.store((w & ~CTRL_STATE_MASK) | idle, std::memory_order_release);
                    if (stat) MDP_STAT1(*stat, MDP_ERR);   /* OOM / 盘读失败 */
                    return nullptr;     /* 回 idle；此后不再访问 c */
                }
                c->cache_data = page;   /* working 发布前写定，ACQUIRE 观察方可见 */
                const uint64_t now_ts = rdtsc_timestamp();
                c->accss_stamp.store(now_ts, std::memory_order_relaxed);   /* load 亦是一次访问：盖访问戳（否则新载入行 accss=0 → 立刻被判冷，H4）*/
                c->latest_syn_stamp.store(now_ts, std::memory_order_relaxed);   /* 首次 load：内存 == 盘，记同步时刻 */
                /* 发布即占坑：working | accessor=1 —— 一次 store 同时发布并就位，
                 * 系统里**不存在** working&&accessor==0 的可见态（关掉 strong reap 的空窗）。 */
                c->ctrl_word.store((uint64_t)working | CTRL_ACCESSOR_INC, std::memory_order_release);
                if (out_loaded) *out_loaded = true;
                if (stat) {   /* 未命中 → 走盘读 */
                    MDP_STAT1(*stat, MDP_GET_MISS);
                    MDP_STAT1(*stat, MDP_NODE_LOAD);
                    MDP_STAT(*stat, MDP_RESIDENT_ADD_BYTES, 8192);
                }
                return page;            /* 无需重持锁：io_ing/accessor 已钉住叶表 */
            }
            continue;   /* CAS 失败 → 他人已接管，回循环顶部看最新状态 */
        }
        cpu_relax();    /* io_ing：他人加载中（其 I/O 段不持锁）；evicting：淘汰接管中 */
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * mdp_ctrl_reap_one — 单行 TTL 淘汰尝试（CTRL_ARRAY / RADIX 共用）
 *
 * 仅 working && accessor==0 && 超时 的行可被 CAS 接管（前提原子打包，
 * 无 get 占坑窗口）；成功后：脏写回 → 释放页 → 回 idle。
 * 返回 true = 本行已被淘汰（调用方做统计记账）。
 * ═══════════════════════════════════════════════════════════════════ */
bool mdata_piece_caches_mgr::mdp_ctrl_reap_one(mdata_piece_ctrl_t *c, const mdata_piece_rt_t &rt,
                                               uint64_t now, uint64_t ttl,
                                               const mdp_stat_hook *stat, bool strong) {
    uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
    if ((w & CTRL_STATE_MASK) != working) return false;
    if ((w >> CTRL_ACCESSOR_SHIFT) != 0) return false;
    if (now - c->accss_stamp.load(std::memory_order_relaxed) <= ttl) return false;

    const uint64_t evict_w = (w & ~CTRL_STATE_MASK) | evicting;
    if (!c->ctrl_word.compare_exchange_weak(w, evict_w,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        return false;   /* 被 get 占坑/他人接管 → 放弃 */

    /* 接管成功：无锁 I/O 写回（负载恒 2 块，blkidx_valid 保证不越界）→ 释放 → 回 idle */
    const bool dirty = (w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY;
    if (dirty)
        rt.write_block(rt.io_ctx, c->blk, 2, c->cache_data);
    mdp_free(c->cache_data);
    c->cache_data = nullptr;
    const uint64_t back_idle = ((evict_w & ~CTRL_STATE_MASK) &
                                ~(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT)) | idle;
    c->ctrl_word.store(back_idle, std::memory_order_release);
    if (stat) {   /* 淘汰一行（脏则含一次写回）；strong 档记入 MDP_REAP_STRONG */
        MDP_STAT1(*stat, strong ? MDP_REAP_STRONG : MDP_REAP);
        if (dirty) MDP_STAT1(*stat, MDP_NODE_WB);
        MDP_STAT(*stat, MDP_RESIDENT_DEL_BYTES, 8192);
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * flush_self_reliat — self_reliat 同步点（挂载/卸载用；脏写回驻留 + 非脏丢弃）
 *
 * CTRL_ARRAY：控制数组全量常驻、无片锁 —— 安全边界甩给上层：宿主须保证本调用
 *   处于静默点（卸载/显式同步，无 get/put/mark_dirty/reap 并发）。小盘（bits≤13）
 *   才走本档，契约成本可接受；H2/H3 在该档仍按「契约门控」记录。
 * RADIX     ：逐片调用 slice::flush_all（片写锁独占）—— 机制上排除并发
 *   get/put/mark_dirty/reap，RADIX 档的 H2/H3 已关闭。
 * ═══════════════════════════════════════════════════════════════════ */
void mdata_piece_caches_mgr::flush_self_reliat() {
    if (backed_mode != cache_work_mode_t::self_reliat_cache) return;
    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY:
        for (uint64_t i = 0; i < piece_count_; i++)
            mdp_ctrl_flush_one(&ctrl_array_[i], rt_, &stat_);   /* 脏写回驻留 + 非脏丢弃（行级 CAS） */
        break;
    case MDP_MODE_RADIX_SHARD:
        /* 逐片写锁 DFS（slice::flush_all）：卸载时刻独占该分片；宿主仍应在
         * 卸载前先停淘汰线程（REAP 线程的生命周期不归本模块管）。 */
        for (uint64_t i = 0; i < n_slices_; i++)
            if (slices_[i]) slices_[i]->flush_all();
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * teardown_self_reliat — 析构收口：脏页回写 → 释放全部（非 self_reliat 后端空操作）
 * ═══════════════════════════════════════════════════════════════════ */
void mdata_piece_caches_mgr::teardown_self_reliat() {
    if (backed_mode != cache_work_mode_t::self_reliat_cache) return;
    switch (mode_) {
    case MDP_MODE_CTRL_ARRAY:
        if (ctrl_array_) {
            for (uint64_t i = 0; i < piece_count_; i++) {
                mdata_piece_ctrl_t *c = &ctrl_array_[i];
                const uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
                if ((w & CTRL_STATE_MASK) == working && c->cache_data) {
                    if ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY)
                        rt_.write_block(rt_.io_ctx, c->blk, 2, c->cache_data);
                    mdp_free(c->cache_data);
                }
                /* io_ing/evicting：无并发析构下不可能残留页（cache_data 应为空），防御跳过 */
            }
            mdp_free(ctrl_array_);
            ctrl_array_ = nullptr;
        }
        break;
    case MDP_MODE_RADIX_SHARD:
        /* 逐片析构（slice dtor 内部：脏页写回 + 释放全部表）。宿主须先停淘汰线程。 */
        for (uint64_t i = 0; i < n_slices_; i++) {
            if (slices_[i]) {
                slices_[i]->~slice_radix_tree();
                mdp_free(slices_[i]);
            }
        }
        if (slices_) mdp_free(slices_);
        slices_ = nullptr;
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * slice_radix_tree — 单分片基数树（RADIX 分支 bits > 13）
 *
 * 拓扑（由 plan 推导，与 inode 版同构）：
 *   - 树内层数 tree_levels_ = plan_.total_levels - 1（level_bits[0] 是 mgr 层分片）
 *   - 层 j（0=根）表宽 = 2^level_bits[1+j]；槽索引 = chunks[1+j]
 *   - 叶子层表 = mdata_piece_ctrl_t 数组（槽即 ctrl_t 实体），
 *     叶表槽索引 = chunks[total_levels-1]
 *   - 中间层表 = 指针槽数组（8B/槽，nullptr = 未建）
 * 与 inode 版的差异（A 路线，行 = 1 负载）：
 *   - row_bits 恒 0：无行内索引（chunks[total_levels] 恒 0），
 *     叶表 ctrl 即负载控制器，get 返回整 8K 页
 *   - blk = pieces_base + (raw << 1)（raw = piece_idx），即 blkidx 本身；
 *     blk_len 恒 2，无 row_blk_len 末行收窄
 * ═══════════════════════════════════════════════════════════════════ */
mdata_piece_caches_mgr::slice_radix_tree::slice_radix_tree(const slice_radix_tree_cfg_t &cfg)
    : rt_(cfg.rt), plan_(cfg.plan), ttl_ticks_(cfg.ttl_ticks), stat_(cfg.stat) {
    tree_levels_ = (uint8_t)(plan_.total_levels - 1);   /* level_bits[0] 是 mgr 层，不进树 */
    if (tree_levels_ == 1) {
        root_ = alloc_leaf();   /* 根即叶表（14~20 档全属此类） */
    } else {
        root_ = alloc_internal(0);   /* 根 = 中间表（指针槽数组） */
    }
}

/* 析构：全部缓存脏页写回 + 非脏丢弃 + 释放全部表（宿主须先停淘汰线程） */
mdata_piece_caches_mgr::slice_radix_tree::~slice_radix_tree() {
    if (root_) dfs_destroy(root_, 0);
}

/* alloc_leaf — 叶表：ctrl_t 数组全 idle（写锁内调用） */
void *mdata_piece_caches_mgr::slice_radix_tree::alloc_leaf() {
    const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
    mdata_piece_ctrl_t *leaf = (mdata_piece_ctrl_t *)mdp_alloc(n * sizeof(mdata_piece_ctrl_t));
    if (!leaf) return nullptr;
    for (uint64_t i = 0; i < n; i++) {
        mdata_piece_ctrl_t *c = &leaf[i];
        c->cache_data = nullptr;
        c->accss_stamp.store(0, std::memory_order_relaxed);
        c->latest_syn_stamp.store(0, std::memory_order_relaxed);   /* 未 load：无同步时刻 */
        c->ctrl_word.store(idle, std::memory_order_relaxed);
        c->blk = 0;
    }
    table_size_all_accumulate += n * sizeof(mdata_piece_ctrl_t);   /* 写锁内 */
    MDP_STAT1(stat_, MDP_TABLE_ALLOC_LEAF);
    MDP_STAT(stat_, MDP_TABLE_ADD_BYTES, n * sizeof(mdata_piece_ctrl_t));
    return leaf;
}

/* alloc_internal — 中间表：指针槽数组清零（写锁内调用） */
void *mdata_piece_caches_mgr::slice_radix_tree::alloc_internal(uint8_t level) {
    const uint64_t n = 1ull << plan_.level_bits[1 + level];
    void **t = (void **)mdp_alloc(n * sizeof(void *));
    if (!t) return nullptr;
    memset(t, 0, n * sizeof(void *));
    table_size_all_accumulate += n * sizeof(void *);   /* 写锁内 */
    MDP_STAT1(stat_, MDP_TABLE_ALLOC_INTERNAL);
    MDP_STAT(stat_, MDP_TABLE_ADD_BYTES, n * sizeof(void *));
    return t;
}

/* ensure_path — 写锁内：沿 decoded 路径补建缺失的中间表/叶表（无 I/O） */
void mdata_piece_caches_mgr::slice_radix_tree::ensure_path(bitfield_decoded_result_t *d) {
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
mdata_piece_caches_mgr::mdata_piece_ctrl_t *
mdata_piece_caches_mgr::slice_radix_tree::locate(bitfield_decoded_result_t *d) {
    if (!root_) return nullptr;
    void *cur = root_;
    for (uint8_t j = 0; j < tree_levels_; j++) {
        if (j == tree_levels_ - 1)
            return &((mdata_piece_ctrl_t *)cur)[d->chunks[plan_.total_levels - 1]];
        void **t = (void **)cur;
        cur = t[d->chunks[1 + j]];
        if (!cur) return nullptr;   /* 路径未建（正常：miss 走建链；异常：OOM） */
    }
    return nullptr;   /* 不可达 */
}

/* locate_with_rdlock — 读锁内翻译 decoded → ctrl*；命中返回**持读锁的句柄**
 * （锁跨 mdp_ctrl_acquire，保护叶表不被 strong reap 释放）；未命中返回空句柄
 * （不持锁，好让 ensure_path_with_wlock 取写锁）。 */
mdata_piece_caches_mgr::slice_radix_tree::ctrl_ref_t
mdata_piece_caches_mgr::slice_radix_tree::locate_with_rdlock(bitfield_decoded_result_t *d) {
    std::shared_lock sh(lock_);
    mdata_piece_ctrl_t *c = locate(d);
    if (!c) return ctrl_ref_t{};             /* 未命中：不持锁返回 */
    return ctrl_ref_t{c, std::move(sh)};     /* 命中：读锁随句柄交出 */
}

/* ensure_path_with_wlock — 写锁内补建 decoded 路径（无 I/O）；返回 false = OOM。
 * 返回即释写锁；调用方（get）拿到 true 后重试 locate_with_rdlock 取持锁句柄。 */
bool mdata_piece_caches_mgr::slice_radix_tree::ensure_path_with_wlock(bitfield_decoded_result_t *d) {
    std::unique_lock ex(lock_);
    if (!locate(d)) ensure_path(d);          /* 缺链才 alloc 中间表/叶表 */
    return locate(d) != nullptr;             /* 建链后必命中（OOM 除外） */
}

/* get：吃解析好的位域结果（树不解码）。并发安全委托两个带锁接口，get 内不手动玩锁：
 *   命中句柄持读锁跨 mdp_ctrl_acquire（acquire 只在 read_block 的 I/O 段临时放锁）。
 * 行 = 1 负载：blk = pieces_base + (raw << 1)（raw = piece_idx），blk_len 恒 2。 */
uint8_t *mdata_piece_caches_mgr::slice_radix_tree::get(bitfield_decoded_result_t *d) {
    const uint64_t blk = rt_.pieces_base + (d->raw << 1);   /* raw = piece_idx；行 = 1 负载 */
    for (;;) {
        ctrl_ref_t r = locate_with_rdlock(d);
        if (!r) {
            if (!ensure_path_with_wlock(d)) { MDP_STAT1(stat_, MDP_ERR); return nullptr; }  /* 建链 OOM */
            MDP_STAT1(stat_, MDP_RETRY);   /* miss 建链后重定位（或被强淘汰摘表后重建）*/
            continue;
        }
        bool loaded = false;
        uint8_t *page = mdata_piece_caches_mgr::mdp_ctrl_acquire(r.ctrl, blk, rt_, &loaded, &r.lock, &stat_);
        if (!page) return nullptr;
        if (loaded) real_cache_accumulate.fetch_add(8192, std::memory_order_relaxed);
        return page;   /* r 析构释读锁 */
    }
}

/* put：accessor-1。locate_with_rdlock 短持读锁取 ctrl；递减是 ctrl_word 原子操作，无需跨锁。 */
void mdata_piece_caches_mgr::slice_radix_tree::put(bitfield_decoded_result_t *d) {
    ctrl_ref_t r = locate_with_rdlock(d);
    if (r) {
        r.ctrl->ctrl_word.fetch_sub(CTRL_ACCESSOR_INC, std::memory_order_release);   /* release：与 reap 的 acquire 同步（H1）*/
        MDP_STAT1(stat_, MDP_PUT);
    }
}

/* mark_dirty：会话内调用——与 get/put 同款，locate_with_rdlock 短持读锁取 ctrl 置 DIRTY。 */
void mdata_piece_caches_mgr::slice_radix_tree::mark_dirty(bitfield_decoded_result_t *d) {
    ctrl_ref_t r = locate_with_rdlock(d);
    if (r) {
        r.ctrl->ctrl_word.fetch_or(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT, std::memory_order_release);
        MDP_STAT1(stat_, MDP_DIRTY);
    }
}

/* flush_all：**写锁**内 DFS 全量同步（挂载/卸载同步点）——
 * 脏行写回（驻留）+ 非脏行丢弃（mdp_ctrl_flush_one 行级 CAS）；不摘表。
 * 独占语义：flush_all 是卸载时刻动作，宿主已保证无访问者；这里用 unique_lock 把
 * 该契约落成机制 —— 与 get 探测段 / put / mark_dirty / reap_expired / 强淘汰全部
 * 串行，写回 I/O 期间任何行都不会被改写或释放（RADIX 档 H2 丢更新 / H3 页 UAF
 * 由此关闭）。锁跨磁盘 I/O 是可以接受的：本函数只允许在静默点调用，
 * 放周期路径会阻塞全部读者。 */
void mdata_piece_caches_mgr::slice_radix_tree::flush_all() {
    std::unique_lock lk(lock_);
    if (!root_) return;
    dfs_flush(root_, 0);
}

void mdata_piece_caches_mgr::slice_radix_tree::dfs_flush(void *table, uint8_t level) {
    if (level == tree_levels_ - 1) {
        mdata_piece_ctrl_t *leaf = (mdata_piece_ctrl_t *)table;
        const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
        for (uint64_t i = 0; i < n; i++)
            if (mdata_piece_caches_mgr::mdp_ctrl_flush_one(&leaf[i], rt_, &stat_))   /* 丢弃非脏行 → 记账 */
                real_cache_accumulate.fetch_sub(8192, std::memory_order_relaxed);
    } else {
        void **t = (void **)table;
        const uint64_t w = 1ull << plan_.level_bits[1 + level];
        for (uint64_t i = 0; i < w; i++)
            if (t[i]) dfs_flush(t[i], level + 1);
    }
}

/* reap_expired：读锁内 DFS 淘汰（不摘表 —— 摘表需写锁，会与 get 的
 * evicting 忙等构成死锁；表只增不删，读锁遍历安全） */
void mdata_piece_caches_mgr::slice_radix_tree::reap_expired() {
    if (!ttl_ticks_) return;
    const uint64_t ttl = ttl_ticks_->load(std::memory_order_relaxed);
    if (!ttl) return;
    std::shared_lock lk(lock_);
    if (!root_) return;
    const uint64_t now = rdtsc_timestamp();
    dfs_reap(root_, 0, now, ttl);
}

void mdata_piece_caches_mgr::slice_radix_tree::dfs_reap(void *table, uint8_t level,
                                                        uint64_t now, uint64_t ttl) {
    if (level == tree_levels_ - 1) {
        mdata_piece_ctrl_t *leaf = (mdata_piece_ctrl_t *)table;
        const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
        for (uint64_t i = 0; i < n; i++) {
            if (mdata_piece_caches_mgr::mdp_ctrl_reap_one(&leaf[i], rt_, now, ttl, &stat_))
                real_cache_accumulate.fetch_sub(8192, std::memory_order_relaxed);
        }
    } else {
        void **t = (void **)table;
        const uint64_t w = 1ull << plan_.level_bits[1 + level];
        for (uint64_t i = 0; i < w; i++)
            if (t[i]) dfs_reap(t[i], level + 1, now, ttl);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * reap_expired_strong — 强淘汰（写锁内整树回收）。力度见 mdp_squeeze_gear_t：
 *
 *   MDP_SQUEEZE_PRESSURE（内存压力驱动）：
 *     · **无视 TTL**：所有 0-ref（accessor==0）working 行强制写回 + 释放（热行也踹）；
 *     · 回收空表。
 *   MDP_SQUEEZE_LOWLOAD（ioctl 低载主动）：
 *     · 只踹 **TTL 已到期**（accss_stamp 距今 > ttl）的 0-ref 冷行 —— 放过热度高
 *       （剩余 TTL 多）的明星行；其余同 PRESSURE（强刷回 + 回收空表）。
 *
 * 两档都只动 accessor==0 的行（in-use 页绝不释放）。与 get 的并发安全：get 的句柄只在
 * acquire 的**探测段**（load+CAS）持读锁，本函数取写锁即排除该段（无 in-flight CAS）；
 * 探测外的 I/O/发布段其行恒 io_ing 或 working|accessor>0，dfs_reap_strong 见非
 * 「working && accessor==0（且 TTL 到期/无视 TTL）」即保留叶表 → 绝不误释放在用叶表。
 * 低频重操作（全局写锁下做 I/O），勿放周期路径。
 * ═══════════════════════════════════════════════════════════════════ */
void mdata_piece_caches_mgr::slice_radix_tree::reap_expired_strong(mdp_squeeze_gear_t gear) {
    const bool respect_ttl = (gear == MDP_SQUEEZE_LOWLOAD);
    uint64_t ttl = 0;
    if (respect_ttl) {
        if (!ttl_ticks_) return;
        ttl = ttl_ticks_->load(std::memory_order_relaxed);
        if (!ttl) return;   /* 无 TTL 时钟：LOWLOAD 判不了冷，no-op */
    }
    const uint64_t now = rdtsc_timestamp();
    std::unique_lock ex(lock_);   /* 写锁：独占整树 */
    if (!root_) return;
    if (!dfs_reap_strong(root_, 0, now, ttl, respect_ttl)) return;   /* 根未空：已就地踹掉冷/0-ref 行 */

    /* 全树已空 → 释放根表，回落未建链态（后续 get 走 ensure_path 重建） */
    const uint64_t root_bytes = (tree_levels_ == 1)
        ? (1ull << plan_.level_bits[plan_.total_levels - 1]) * sizeof(mdata_piece_ctrl_t)
        : (1ull << plan_.level_bits[1]) * sizeof(void *);
    table_size_all_accumulate -= root_bytes;
    mdp_free(root_);
    root_ = nullptr;
    MDP_STAT1(stat_, MDP_TABLE_FREE);
    MDP_STAT(stat_, MDP_TABLE_DEL_BYTES, root_bytes);
}

/* dfs_reap_strong — 后序：先到叶表把 0-ref 的 working 行写回/释放，回溯时释放「已空」的
 * 子表、父槽置 null。返回 true = 本表已空（父可回收本表）。
 * respect_ttl=false（PRESSURE）→ 无视 TTL 全踹；true（LOWLOAD）→ 只踹 TTL 到期的冷行。
 * 调用前提：已持写锁（独占）→ 状态稳定；I/O 段放锁的在飞加载其行恒 io_ing → 保留。 */
bool mdata_piece_caches_mgr::slice_radix_tree::dfs_reap_strong(void *table, uint8_t level,
                                                               uint64_t now, uint64_t ttl,
                                                               bool respect_ttl) {
    if (level == tree_levels_ - 1) {
        mdata_piece_ctrl_t *leaf = (mdata_piece_ctrl_t *)table;
        const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
        bool empty = true;
        for (uint64_t i = 0; i < n; i++) {
            mdata_piece_ctrl_t *c = &leaf[i];
            const uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
            const uint8_t s = (uint8_t)(w & CTRL_STATE_MASK);
            if (s == idle) continue;                      /* 无驻留：不阻止本叶表回收 */
            if (s != working || (w >> CTRL_ACCESSOR_SHIFT) != 0) {
                empty = false;                            /* io_ing/evicting/会话占用：保留 */
                continue;
            }
            if (respect_ttl && now - c->accss_stamp.load(std::memory_order_relaxed) <= ttl) {
                empty = false;                            /* LOWLOAD：热行（剩余 TTL 多）放过 */
                continue;
            }
            /* 0-ref（且 TTL 到期 / PRESSURE）：强制写回脏行 → 释放页 → 回 idle（写锁内无需 CAS） */
            const bool dirty = (w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY;
            if (dirty)
                rt_.write_block(rt_.io_ctx, c->blk, 2, c->cache_data);
            mdp_free(c->cache_data);
            c->cache_data = nullptr;
            c->ctrl_word.store(idle, std::memory_order_release);
            real_cache_accumulate.fetch_sub(8192, std::memory_order_relaxed);
            MDP_STAT1(stat_, MDP_REAP_STRONG);
            if (dirty) MDP_STAT1(stat_, MDP_NODE_WB);
            MDP_STAT(stat_, MDP_RESIDENT_DEL_BYTES, 8192);
        }
        return empty;
    }
    void **t = (void **)table;
    const uint64_t w = 1ull << plan_.level_bits[1 + level];
    bool empty = true;
    for (uint64_t i = 0; i < w; i++) {
        if (!t[i]) continue;
        if (dfs_reap_strong(t[i], level + 1, now, ttl, respect_ttl)) {
            const uint8_t cl = (uint8_t)(level + 1);
            const uint64_t bytes = (cl == tree_levels_ - 1)
                ? (1ull << plan_.level_bits[plan_.total_levels - 1]) * sizeof(mdata_piece_ctrl_t)
                : (1ull << plan_.level_bits[1 + cl]) * sizeof(void *);
            table_size_all_accumulate -= bytes;
            mdp_free(t[i]);
            t[i] = nullptr;
            MDP_STAT1(stat_, MDP_TABLE_FREE);
            MDP_STAT(stat_, MDP_TABLE_DEL_BYTES, bytes);
        } else {
            empty = false;
        }
    }
    return empty;
}

/* dfs_destroy：脏页写回 + 非脏丢弃 + 释放全部表（析构，无并发） */
void mdata_piece_caches_mgr::slice_radix_tree::dfs_destroy(void *table, uint8_t level) {
    if (level == tree_levels_ - 1) {
        mdata_piece_ctrl_t *leaf = (mdata_piece_ctrl_t *)table;
        const uint64_t n = 1ull << plan_.level_bits[plan_.total_levels - 1];
        for (uint64_t i = 0; i < n; i++) {
            mdata_piece_ctrl_t *c = &leaf[i];
            const uint64_t w = c->ctrl_word.load(std::memory_order_acquire);
            if ((w & CTRL_STATE_MASK) == working && c->cache_data) {
                if ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY)
                    rt_.write_block(rt_.io_ctx, c->blk, 2, c->cache_data);
                mdp_free(c->cache_data);
            }
            /* io_ing/evicting：无并发析构下不可能残留页（cache_data 应为空），防御跳过 */
        }
        mdp_free(leaf);
    } else {
        void **t = (void **)table;
        const uint64_t w = 1ull << plan_.level_bits[1 + level];
        for (uint64_t i = 0; i < w; i++)
            if (t[i]) dfs_destroy(t[i], level + 1);
        mdp_free(t);
    }
}
