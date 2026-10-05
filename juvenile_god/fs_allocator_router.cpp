/* ═══════════════════════════════════════════════════════════════════
 * fs_allocator_router.cpp — fs_allocator_t 路由层实现（树版分片路由）
 *
 * 2026-10-03 新建。模块契约见 include/ravenfs/fs_allocator.hpp 头注释（逐字为
 * 规格）；本文件是「路由骨架」——与位图时代的 bitmap_slices_mgr_t（见
 * juvenile_god/bitmap_slices.cpp）**同一套路由思想**，后端从「位图 + 位扫」
 * 换成「树版片自治单元 mgr_slice_t」：
 *
 *   ┌───────────────────────────────────────────────────────────────┐
 *   │ fs_allocator_t（本文件）= 几何推导 + 双组装配 + 路由 + 审计      │
 *   │   └─ 下调 mgr_slice_t（juvenile_god/fs_allocator.cpp，不自锁）  │
 *   └───────────────────────────────────────────────────────────────┘
 *
 * 与位图路由的**唯一结构差异**：
 *   1. 片是 `mgr_slice_t`（含 std::mutex，**不可拷贝**）→ 用 `new mgr_slice_t[n]`
 *      逐片 enable_slice；失败要回滚已 enable 的片（shutdown）再 delete[]。
 *   2. 树版片**不持全局位图描述符**（片自持主从双红黑树，坐标即全局位号），
 *      故 group_t 里没有 desc / bits_count 字段——本路由**不存 nbits**，凡
 *      「按全局坐标切段」处一律用**名义片长 per_slice_bits** 定位（理由详下）。
 *   3. 片锁是**可睡 std::mutex**（不是位图时代的自旋 rw_try_spinlock_t）：
 *      分配类走 try_lock（忙则换片 = preferred 旋转）；定点类（occupy / free）
 *      阻塞 lock（不可换片 / 不可丢）。片内临界区含 malloc（STL 链表节点），
 *      持锁方允许被短暂挂起，故绝不自旋。
 *
 * ── 锁纪律（逐条照抄头文件 + bitmap_slices.cpp 的语义，只换锁原语）─────
 *   - 分配（block_alloc_hard / block_alloc_soft / inode_alloc）：try_lock，
 *     忙则换片（preferred 亲和扫描，仿 FPA::alloc）。
 *   - 定点对（block_occupy_soft / block_free / inode_free / *_test）：
 *     阻塞抢锁（占/放的锚点唯一，不可换片、不可丢）。
 *   - **绝不同持两片锁**：soft_alloc 逐片单锁、occupy 片尾跨界「解锁上一片 →
 *     再锁下一片」逐片推进；故无锁序问题、无跨片死锁面。
 *
 * ── 坐标口径 ──────────────────────────────────────────────────────
 *   对外/对内一律**全局位号**；mgr_slice_t 内部即全局坐标，本路由不做局部↔
 *   全局换算——只做「全局位号 → 片号 = 位号 / per_slice_bits」的**定位**。
 *
 * ── 不存 nbits 的代价与取法（重要）───────────────────────────────
 *   位图路由能从 blk_.desc.bits_count 取到全局位数；树版没有该字段（不改头）。
 *   故本路由用**名义边界** per_slice_bits 切段：片 i 名义覆盖
 *     [i*per, (i+1)*per)，末片名义也只到 slice_count_*per（可能 > 真实末片尾）。
 *   这对**合法调用**无影响：
 *     - *_free：调用方只会释放合法（≤ 真实尾）区间；按名义边界切段，段内一定
 *       落在真实片内，free_interval 自然接受。
 *     - block_occupy_soft：末片 soft_occupy 会在**真实片尾**夹断并如实回传 rest，
 *       此时 base(真实尾) ≠ 名义 slice_end → 循环 break，不会越界续圈。
 *   即：真实片尾由 mgr_slice_t 自己把关，路由只需名义边界做**循环定位**。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/fs_allocator.hpp"

#include <new>        /* std::bad_alloc */

/* 平台身份合约：per-CPU preferred 路由按此返回值索引；内核端必须提供强定义，
 * 用户态由 tests/cpu_id_stub.cpp 的 weak 桩兜底（与 bitmap_slices.cpp 同款）。 */
extern "C" uint32_t get_processor_id();

/* ═══════════════════════════════════════════════════════════════
 * 内部轮子（匿名 namespace）
 *
 * 为什么不写成 fs_allocator_t 的私有成员函数：头文件的**私有成员布局由
 * 用户主笔、正在并行编辑**，本任务约定「只新建 cpp、不动头」。故把「装配一组
 * / 拆一组」做成**自由函数**，只接收拆开的字段引用（slices / per / preferred）
 * ——这样无需在头外命名私有嵌套类型 group_t（它的类型名在类外不可见），
 * 也避免任何友元/模板绕访问控制的把戏。语义上仍是路由层的私有装配逻辑。
 * ═══════════════════════════════════════════════════════════════ */
namespace {

/* 装配一组：按 nbits 与 slice_count 计算 per_slice_bits，逐片 enable_slice。
 *
 * per_slice_bits 推导（位图版原样）：
 *   per = ceil( ceil(nbits/n) / msb ) * msb
 * 即「均分后向上取整到 msb（片下限，默认 32768 bit = 1 位图块）倍数」。
 * msb 倍数 ⇒ 每片起点 begin=i*per 恒为 32768 倍数 ⇒ begin/8 恒 4K 对齐 ⇒
 * 满足 enable_slice 的 slice_base 4K 对齐校验。
 *
 * 失败回滚：new 失败 → RC_ERR_STATE；第 i 片 enable 失败 → 先 shutdown 已
 * enable 的 [0,i) 片（对位清理），再 delete[] + 置空，返回 RC_ERR_STATE。
 * 这满足头文件「ctor 失败即自清理」的契约。 */
result_code_t setup_slices(mgr_slice_t *&slices, uint64_t &per_slice_bits,
                           uint32_t slice_count, uint8_t *raw,
                           uint64_t nbits, uint64_t msb) {
    uint64_t per = ((nbits + slice_count - 1) / slice_count + msb - 1) / msb * msb;
    if (per == 0) per = msb;
    per_slice_bits = per;
    slices = nullptr;

    try {
        slices = new mgr_slice_t[slice_count];
    } catch (const std::bad_alloc &) {
        slices = nullptr;
        return RC_ERR_STATE;
    }

    for (uint32_t i = 0; i < slice_count; i++) {
        const uint64_t begin = (uint64_t)i * per;
        /* 防御：ctor 的「末片非空迭代」已保证 begin < nbits；此处不应触发。
         * 真触发说明调用方参数与推导不一致 → 整体失败回滚。 */
        if (begin >= nbits) {
            for (uint32_t j = 0; j < i; j++) slices[j].shutdown();
            delete[] slices; slices = nullptr;
            return RC_ERR_STATE;
        }
        uint64_t cnt = nbits - begin;
        if (cnt > per) cnt = per;

        /* slice_config_t：block 组用 block_bits_raw / inode 组用 inode_bits_raw，
         * 起点 = i*per，位数 = min(per, nbits − i*per)。enable_slice 内一次性
         * 消费该内存建树，返回后不再引用（故 raw 可即刻弃）。 */
        slice_config_t cfg{};
        cfg.slice_base          = raw + begin / 8;   /* begin 恒 msb 倍数 → 4K 对齐 */
        cfg.in_global_base_idx  = begin;
        cfg.slice_bitscount     = cnt;

        if (slices[i].enable_slice(cfg) != RC_OK) {
            for (uint32_t j = 0; j < i; j++) slices[j].shutdown();  /* 回滚已使能片 */
            delete[] slices; slices = nullptr;
            return RC_ERR_STATE;
        }
    }
    return RC_OK;
}

/* 拆一组：逐片 shutdown（对位清树），再 delete[] 片数组与 preferred。
 * 幂等：slices/preferred 可为空。非使能片 shutdown 返回 RC_ERR_STATE，忽略
 * （拆解路径只求尽力清干净，不改变析构幂等性）。 */
void teardown_slices(mgr_slice_t *&slices, uint32_t slice_count, uint32_t *&preferred) {
    if (slices) {
        for (uint32_t i = 0; i < slice_count; i++) slices[i].shutdown();
        delete[] slices;
        slices = nullptr;
    }
    delete[] preferred;
    preferred = nullptr;
}

/* 统计一条 run 链的总位数（= Σ len）。block_alloc_soft 逐片累计交付量的单源。 */
uint64_t sum_runs(const fs_run_list_t &runs) {
    uint64_t total = 0;
    for (const fs_run_t &r : runs) total += r.len;
    return total;
}

}  // namespace

/* ═══════════════════════════════════════════════════════════════
 * ctor — 装配：2^n 上界 ∩ 满片数 → 末片非空迭代 → 双组 enable → preferred 初始化
 *
 * 推导（位图版 bitmap_slices_mgr_t::bitmap_slices_mgr_t 原样照抄，只把 32768
 * 换成 cfg.min_slice_bits；该字段默认 32768，注释标「勿动」）：
 *   n   = 2^ceil(log2(logical_processor_count))            （2^n 上界）
 *   cap = min(block_nbits, inode_nbits) / msb              （满片数约束）
 *   n   = min(n, cap)，n ≥ 1
 *   末片非空迭代：per 取整到 msb 倍数后，两组末片必须非空；否则 n >>= 1（保持 2 幂）
 * slice_count 是**两组共享**的（同长度片数组），故取两组满片数最小值。
 * per_slice_bits 由 setup_slices 在装配时**分别**按组计算（几何可能不同，
 * 但片数一致——这不影响各片内的全局坐标，片只是「全局空间里的一段」）。
 *
 * ⚠ inode 0 = root：位图路由靠 `ino_.slices[0].set_hint(1)` 跳过 root。树版
 *   mgr_slice_t **没有 set_hint 接口**，本路由无法、也**无需**特殊跳过——只要
 *   源位图里 root 位已置 1（占用），enable_slice 建树时它就不在任何空闲区间里，
 *   永远不会被分配。反过来说：若源位图把 root 标成空闲，树会把 0 当普通空闲位
 *   分配出去——这是「位图即真相」的正确语义，由上层保证 root 亮灯。
 *
 * 失败自清理 + *res_code（默认 RC_ERR_STATE；成功 RC_OK）。
 * ═══════════════════════════════════════════════════════════════ */
fs_allocator_t::fs_allocator_t(const slices_cfg_t &cfg, uint64_t *res_code) {
    if (res_code) *res_code = RC_ERR_STATE;   /* 默认失败态（成员保持零值/空态） */

    if (!cfg.block_bits_raw || cfg.block_bits_count == 0) {
        return;   /* block 源缺失：保持空态（inode 数据面虽独立，但 grow 需 block 组取片） */
    }

    const uint64_t msb = cfg.min_slice_bits ? cfg.min_slice_bits : 32768;

    /* 2^n 上界（仿 FPA；0 → 由 cpu_slots_ 兜底单槽） */
    uint64_t n = 1;
    while (n < cfg.logical_processor_count && n < (1ULL << 30)) n <<= 1;
    /* 满片数约束：每片 ≥ msb bit。v2.0 只按 **block 组** 推导——inode 组已成数据面，
     * 不再参与切片（旧「两组共享 slice_count」的几何来源随 inode 位图一起退役）。 */
    const uint64_t blk_cap = cfg.block_bits_count / msb;
    uint64_t cap = blk_cap ? blk_cap : 1;
    if (n > cap) n = cap;
    /* 末片非空迭代：per 按 msb 倍数上取整后，block 组末片必须非空 */
    for (;;) {
        uint64_t per_b = ((cfg.block_bits_count + n - 1) / n + msb - 1) / msb * msb;
        if (per_b == 0) per_b = msb;
        if ((n - 1) * per_b < cfg.block_bits_count) break;
        if (n == 1) break;
        n >>= 1;
    }
    slice_count_ = (uint32_t)n;
    cpu_slots_   = cfg.logical_processor_count ? cfg.logical_processor_count : 1;

    /* per-CPU preferred：每"CPU"记住上次成功的亲和片（仿 FPA 每 CPU preferred） */
    try {
        blk_.preferred = new uint32_t[cpu_slots_];
    } catch (const std::bad_alloc &) {
        teardown_slices(blk_.slices, slice_count_, blk_.preferred);
        slice_count_ = 0; cpu_slots_ = 0;
        return;
    }
    for (uint32_t i = 0; i < cpu_slots_; i++)
        blk_.preferred[i] = (uint32_t)(i % slice_count_);

    if (setup_slices(blk_.slices, blk_.per_slice_bits, slice_count_,
                     cfg.block_bits_raw, cfg.block_bits_count, msb) != RC_OK) {
        teardown_slices(blk_.slices, slice_count_, blk_.preferred);
        slice_count_ = 0; cpu_slots_ = 0;
        return;
    }

    /* inode 数据面装配（无状态；失败即整体失败自清理） */
    if (inode_plane_.init(cfg.inode, this) != RC_OK) {
        teardown_slices(blk_.slices, slice_count_, blk_.preferred);
        slice_count_ = 0; cpu_slots_ = 0;
        return;
    }

    if (res_code) *res_code = RC_OK;
}

/* dtor：逐片 shutdown 后 delete[] 自有数组（不在 dtor 里碰任何调用方内存，
 * 因为根本不留全局位图指针——建 tree 时已消费完）。inode 数据面自带 dtor。 */
fs_allocator_t::~fs_allocator_t() {
    teardown_slices(blk_.slices, slice_count_, blk_.preferred);
}

/* ═══════════════════════════════════════════════════════════════
 * block 组 — 硬分配
 *
 * 硬分配：require_count 连续 + 起始 2^align_log2 对齐；**单片内完成、不跨片
 * 拼**（原子）。路由 = 本 CPU（get_processor_id()）preferred 片优先，逐片
 * try_lock（忙则换片）→ 片 alloc_hard；命中即回写 preferred 并返回全局起点；
 * 全片失败 → PTR_INVALID（0 是合法块号，哨兵只能用 PTR_INVALID）。
 * ═══════════════════════════════════════════════════════════════ */
uint64_t fs_allocator_t::block_alloc_hard(uint64_t require_count, uint8_t align_log2) {
    if (require_count == 0 || !blk_.slices || slice_count_ == 0 ||
        cpu_slots_ == 0 || !blk_.preferred) return PTR_INVALID;

    const uint32_t cpu = get_processor_id() % cpu_slots_;
    for (uint64_t round = 0; round < slice_count_; round++) {
        const uint64_t idx = (blk_.preferred[cpu] + round) % slice_count_;
        mgr_slice_t &s = blk_.slices[idx];
        if (!s.slice_lock.try_lock()) continue;            /* 锁忙换片 */
        uint64_t out = 0;
        const result_code_t rc = s.alloc_hard(align_log2, require_count, out);
        s.slice_lock.unlock();
        if (rc == RC_OK) {
            blk_.preferred[cpu] = (uint32_t)idx;           /* 回写亲和片 */
            return out;
        }
    }
    return PTR_INVALID;
}

/* ═══════════════════════════════════════════════════════════════
 * block 组 — 软分配（跨片 · 碎片优先 · 穷尽所有分片 · all-or-nothing）
 *
 * 自 preferred 起逐片调片内 soft_alloc(align_log2, remaining, runs, sat)：
 *   - 片内**碎片优先**消费（从小碎片端起，段与段不要求连续）；
 *   - 多段 run 全部 push 进**同一条**调用方链 runs（前插构建，共用一链）；
 *   - 每片调用后按 Σ len 重算累计交付量 got；
 *   - got ≥ require_count → 提前收敛 RC_OK（runs 即多段交付链）；
 *   - 穷尽所有片仍不够 → **回滚**：block_intervals_free(runs) 把已取段逐段
 *     归还（不变量：回滚后 runs 必须空），返回 RC_ERR_NO_BLOCK。
 * 门闸：runs == nullptr / require_count == 0 / 组未装配 → RC_ERR_STATE。
 * align_log2 非法（>63）由片 soft_alloc 报 RC_ERR_STATE → 同样回滚后透传。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t fs_allocator_t::block_alloc_soft(uint64_t require_count, uint8_t align_log2,
                                               fs_run_list_t *runs) {
    if (runs == nullptr) return RC_ERR_STATE;
    if (require_count == 0) return RC_ERR_STATE;
    if (!blk_.slices || slice_count_ == 0 || cpu_slots_ == 0 || !blk_.preferred)
        return RC_ERR_STATE;

    const uint32_t cpu = get_processor_id() % cpu_slots_;
    for (uint64_t round = 0; round < slice_count_; round++) {
        if (sum_runs(*runs) >= require_count) break;       /* 已凑满：提前收敛 */
        const uint64_t idx = (blk_.preferred[cpu] + round) % slice_count_;
        mgr_slice_t &s = blk_.slices[idx];
        if (!s.slice_lock.try_lock()) continue;            /* 锁忙换片 */

        const uint64_t remaining = require_count - sum_runs(*runs);
        bool sat = false;
        const result_code_t rc = s.soft_alloc(align_log2, remaining, runs, sat);
        s.slice_lock.unlock();

        if (rc == RC_OK) {
            blk_.preferred[cpu] = (uint32_t)idx;
            /* 本片交付了 ≥1 位（soft_alloc 契约）；继续下一片补缺口 */
        } else if (rc == RC_ERR_NO_BLOCK) {
            continue;                                      /* 本片已被吃干：换下一片 */
        } else {
            /* 非空片/非非法参数类错误（如对齐非法 RC_ERR_STATE）：回滚已取段后透传 */
            block_intervals_free(runs);
            return rc;
        }
    }

    if (sum_runs(*runs) >= require_count) return RC_OK;    /* 凑满 */

    /* 穷尽所有片仍不够：回滚已取段（all-or-nothing），返回空间不足 */
    block_intervals_free(runs);
    return RC_ERR_NO_BLOCK;
}

/* ═══════════════════════════════════════════════════════════════
 * block 组 — 软锚定占领（定点对 · 对偶 block_free）
 *
 * blkbase 起认领 ≤ require_count 连续空闲块；**锚点片唯一 → 片锁阻塞抢**
 * （不可换片）。片尾跨界续圈在本函数内消化：片内 soft_occupy 顶到片尾且仍需，
 * 则 base 推进到下一片首（= 上一片名义尾）继续——**逐片单锁**（解锁上一片才
 * 锁下一片），故「前缀链」是真实物理连续，且永不跨片双持。
 *   出：认领 ≥1 块 → RC_OK（**结果即入参 blkbase 原址**）；
 *       一块未占 → RC_ERR_NO_BLOCK；*rest_count 出 = 缺口。
 * ⚠ 可能产出跨片连续 run → 由调用方用 block_intervals_free 链式归还。
 * 门闸：require_count == 0 / 组未装配 / per_slice_bits == 0 → RC_ERR_STATE。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t fs_allocator_t::block_occupy_soft(uint64_t blkbase, uint64_t require_count,
                                                uint64_t *rest_count) {
    if (rest_count) *rest_count = require_count;
    if (require_count == 0 || !blk_.slices || slice_count_ == 0 || blk_.per_slice_bits == 0)
        return RC_ERR_STATE;

    uint64_t base = blkbase, remain = require_count;
    while (remain > 0 && (base / blk_.per_slice_bits) < slice_count_) {
        const uint64_t idx = base / blk_.per_slice_bits;   /* 锚点所在片 */
        mgr_slice_t &s = blk_.slices[idx];
        s.slice_lock.lock();                               /* 定点对：阻塞抢，不换片 */
        uint64_t left = 0;                                 /* 入=0：本锚点无先验满足量 */
        const result_code_t rc = s.soft_occupy(base, remain, &left);
        s.slice_lock.unlock();
        if (rc != RC_OK) break;                            /* 锚点即占用 / 片内无可占：前缀断 */

        const uint64_t claimed = remain - left;
        base  += claimed;
        remain = left;
        if (remain == 0) break;                            /* 需求已满 */

        /* 只有「恰好顶到片尾」才值得续圈；中途撞占用（base < 片尾）→ 连续前缀断 */
        const uint64_t nominal_slice_end = (idx + 1) * blk_.per_slice_bits;
        if (base != nominal_slice_end) break;
        /* base == 下一片首 → 循环顶重定位，续圈（末片真实尾会由片内夹断，
         * 使 base < 名义尾 → 下一轮条件不满足或此处 break，绝不越界） */
    }

    if (rest_count) *rest_count = remain;
    return (require_count - remain > 0) ? RC_OK : RC_ERR_NO_BLOCK;
}

/* ═══════════════════════════════════════════════════════════════
 * block 组 — 区间释放（跨片循环）
 *
 * 物理连续 run 可能跨片（block_occupy_soft 的跨界产物）→ 逐段按**名义片长**
 * 定位所属片释放；释放无连续要求，故逐段单锁即可，无需跨片双持。定点对语义
 * → 阻塞抢锁（释放不可丢）。
 * 注：末片按名义边界切出的段仍落在真实片内（调用方只会释放合法区间），
 * free_interval 的片内上界校验是最后守卫。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t fs_allocator_t::block_free(uint64_t blkbase, uint64_t count) {
    if (count == 0) return RC_OK;                          /* 空释放：无操作 */
    if (!blk_.slices || slice_count_ == 0 || blk_.per_slice_bits == 0)
        return RC_ERR_STATE;

    while (count > 0) {
        const uint64_t idx = blkbase / blk_.per_slice_bits;
        if (idx >= slice_count_) return RC_ERR_STATE;      /* 越出片数组：防御 */
        const uint64_t slice_end = (idx + 1) * blk_.per_slice_bits;
        uint64_t n = count;
        if (blkbase + n > slice_end) n = slice_end - blkbase;   /* 本片内可释放量 */
        if (n == 0) return RC_ERR_STATE;                   /* 防御：理论上不达 */
        mgr_slice_t &s = blk_.slices[idx];
        s.slice_lock.lock();                               /* 释放不可丢：阻塞抢 */
        const result_code_t rc = s.free_interval(blkbase, n);
        s.slice_lock.unlock();
        if (rc != RC_OK) return rc;                        /* 透传（重叠 / 越界） */
        blkbase += n;
        count   -= n;
    }
    return RC_OK;
}

/* 链式释放（跨片）：消费 run 链，逐段走 block_free。
 * 契约：*runs == nullptr → RC_ERR_STATE；已消费段不回滚（释放不可逆），
 * 余段留链供调用方续查；全消费完 → RC_OK（链空）。容器归调用方，只 drain。 */
result_code_t fs_allocator_t::block_intervals_free(fs_run_list_t *runs) {
    if (runs == nullptr) return RC_ERR_STATE;
    while (!runs->empty()) {
        const fs_run_t run = runs->front();    /* 先取副本 */
        runs->pop_front();                     /* 再消费节点 */
        const result_code_t rc = block_free(run.base, run.len);
        if (rc != RC_OK) return rc;            /* 余段保留；已消费段不回滚 */
    }
    return RC_OK;
}

/* block_test：blk 是否已占用。树版无位图内存可直读，故走片内 test_interval
 * （val=true = 段内无空闲位 ⇒ 占用）。锁：短临界，阻塞抢（与 test 语义一致）。 
 * 越界 / 未装配 → false。 */
bool fs_allocator_t::block_test(uint64_t blk) const {
    if (!blk_.slices || slice_count_ == 0 || blk_.per_slice_bits == 0) return false;
    const uint64_t idx = blk / blk_.per_slice_bits;
    if (idx >= slice_count_) return false;
    mgr_slice_t &s = blk_.slices[idx];         /* 非 const 指针 ⇒ 浅 const 下可调非 const 成员 */
    s.slice_lock.lock();
    const bool occupied = (s.test_interval(true, blk, 1) == RC_OK);
    s.slice_lock.unlock();
    return occupied;
}

/* ═══════════════════════════════════════════════════════════════
 * inode 组（v2.0）：薄转发 → inode_data_plane_t
 *
 * 数据面自带两把读写锁（空间/翻译，锁序见 inode_data_plane_t 头注）；
 * 本层只做转发，不做 preferred 分片（inode 已非位粒度分片）。
 *   · inode_alloc  ：fr best-fit；用尽 → grow（空间写→翻译写）；无块 → RC_ERR_NO_BLOCK
 *   · inode_free   ：fr 插入（自动融合）；幂等；inum==0 拒绝
 *   · inode_test   ：已分配 = 在 [0,pieces*32) 且不在 fr
 *   · inode_locate ：翻译物理 8K 片基 + 片内偏移（不做引用计数）
 * ═══════════════════════════════════════════════════════════════ */
result_code_t fs_allocator_t::inode_alloc(uint64_t &out_inum) {
    return inode_plane_.alloc(out_inum);
}

void fs_allocator_t::inode_free(uint64_t inum) {
    inode_plane_.free_inode(inum);
}

bool fs_allocator_t::inode_test(uint64_t inum) const {
    return inode_plane_.test_inode(inum);
}

result_code_t fs_allocator_t::inode_locate(uint64_t inum, inode_location_t *result) {
    return inode_plane_.locate(inum, result);
}

/* ═══════════════════════════════════════════════════════════════
 * 审计
 * ═══════════════════════════════════════════════════════════════ */

/* 各片 free_count 求和（空闲块总数）。逐片短锁读取 → 各片计数自洽（不保证
 * 跨片同一时刻的全局一致快照，审计量级足够）。 */
uint64_t fs_allocator_t::block_free_total() const {
    if (!blk_.slices || slice_count_ == 0) return 0;
    uint64_t sum = 0;
    for (uint64_t i = 0; i < slice_count_; i++) {
        mgr_slice_t &s = blk_.slices[i];
        s.slice_lock.lock();
        sum += s.get_free_count();
        s.slice_lock.unlock();
    }
    return sum;
}

uint64_t fs_allocator_t::inode_free_total() const {
    return inode_plane_.free_total();   /* 数据面空闲 inode 数（fr 读锁内取） */
}

uint32_t fs_allocator_t::block_preferred_of(uint32_t cpu) const {
    if (!blk_.preferred || cpu >= cpu_slots_) return 0;
    return blk_.preferred[cpu];
}

/* 注：全量刷回 / 落盘属上帝类职责（涉及 I/O），模块不提供 all_syn——
 * 数据面提供 dump_fr / dump_extents 供上层取一致快照后自行落盘（P3）。 */
