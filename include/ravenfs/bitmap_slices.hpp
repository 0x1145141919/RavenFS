#pragma once
#include "rc.hpp"                    /* result_code_t / RC_* / PTR_INVALID（轻量头，零盘结构依赖） */
#include "../util/lock.h"            /* rw_try_spinlock_t（2026-09-03 收编：原嵌 God 类，随位图模块顶出） */
#include <cstdint>
#include <cstddef>

/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slices.hpp — 双位图分片分配器独立模块（2026-09-03 从上帝类顶出）
 *
 * 原形态：fs_bitmap_descriptor_t / slice_config_t / bitmap_slice_t /
 * rw_try_spinlock_t 全部嵌套在 RavenFS_juvenile 内，路由函数（block_alloc_*
 * / block_occupy_soft / block_free / inode_alloc...）是上帝类私有方法——
 * 测试被迫挂整个上帝类 + 镜像才能测路由（抽象泄露）。
 *
 * 现形态（对齐 inode_cache / mdata_piece 的"cfg 大结构体无状态初始化"范式）：
 *   bitmap_slices_mgr_t = 双组（block + inode）分片分配器，纯内存、零 I/O、
 *   零设备知识（比 inode_cache 更纯：连 read/write 回调都不需要）。
 *   - cfg 只喂"已加载/已映射的位图原始内存 + 位数 + CPU 数"，生命周期归调用方
 *   - ctor：2^n 上界 ∩ 满片数约束推导 slice_count → 双组切片 enable →
 *     per-CPU preferred 初始化（inode 片 0 hint=1 跳过 root）
 *   - dtor：delete[] 自有 slices/preferred 数组（位图内存不碰）
 *   - 调用方责任：位图原始内存的加载（pread）/窗口映射（mmap）/校验/写回，
 *     及平台身份 get_processor_id()（外部合约，见 cpu_id_stub.cpp 头注释）
 *
 * 设计文档：Docs/runtime_tech/02_bitmap_slices.md（含锁纪律/不变量契约）；
 * 三端移植注记：换锁实现与 cpu 身份即可，纯算法零平台依赖。
 *
 * v2（CHUNK 懒加载窗口版：mgr 自持存储 + 赋能式 I/O）已顶出 → 见 bitmap_slices_v2.hpp。
 * ═══════════════════════════════════════════════════════════════════ */

/* ── 位图全局描述符（bit i ↔ block[i] / inode[i]，FS_blk_base=0）── */
struct fs_bitmap_descriptor_t {
    uint64_t FS_blk_base;
    uint64_t *bitmap_raw;   /* 4K 对齐（posix_memalign / mmap 窗口切片） */
    uint64_t bits_count;
};

/* 切片配置（enable_slice 入参；坐标 = parent_descriptor 全局位号） */
struct slice_config_t {
    uint8_t *slice_base;    /* 片位图基址（4K 对齐，enable_slice 校验） */
    uint64_t slice_bitscount;
};

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slice_t — 分片位图自治单元（原 God 嵌套类，原样顶出）
 *
 * 自治纪律（详细见 Docs/runtime_tech/02_bitmap_slices.md §3/§8）：
 *   1. 片与片零共享：无跨片状态、无跨片锁 → 并行度 = 片数
 *   2. 本类所有方法一律不加锁：临界区由外部用 slice_lock 编排
 *      （try_lock_write → 操作 → unlock_write），方法是纯位操作
 *   3. 刻意不搞构造/析构函数：往 64B 缓存行对齐设计（数组化分配）
 *   4. 坐标双视角：对外全部用全局位号（parent_descriptor 视角），
 *      内部换算 片内偏移 = 全局位号 − base_idx
 * ═══════════════════════════════════════════════════════════════ */
class bitmap_slice_t {
private:
    uint8_t *slice_base;            /* 4K 对齐 */
    uint64_t base_idx;              /* 片首全局位号（=(slice_base−parent->bitmap_raw)×8） */
    uint64_t slice_bitscount;
    fs_bitmap_descriptor_t *parent_descriptor;
    uint64_t free_count;            /* enable 时 O(片) 扫出，增量维护 */
    uint64_t last_finish_hint;      /* 分配游标（片内偏移，分配连续性来源） */
public:
    rw_try_spinlock_t slice_lock;
    uint64_t get_free_count();
    uint64_t get_hint();
    void set_hint(uint64_t new_hint);
    /* 下面均为全局位图区间视角：传入/传出均为全局位号，内部路由换算 */
    result_code_t enable_slice(fs_bitmap_descriptor_t *global_descriptor, slice_config_t config);
    /* 硬分配：片内找 bitscount 连续空闲位、起始片内偏移 2^align_log2 对齐；
     * 从 last_finish_hint 起扫，段尾绕回 [0, hint) 再扫一次；不部分分配。 */
    result_code_t alloc_hard(uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx);
    /* 软分配：第一个空闲段取对齐候选，实际 = min(段长, bitscount)；
     * 缺口回传 bits_left_to_be_allocated。 */
    result_code_t alloc_soft(uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx,
                             uint64_t &bits_left_to_be_allocated);
    /* 区间测试：[base, base+len) 全同 val → RC_OK / 有异位 → RC_ERR_NOT_FOUND */
    result_code_t test_interval(bool val, uint64_t base, uint64_t len);
    /* 区间释放：清位，free_count 按实际清掉的 1→0 位计数（重复释放不虚高） */
    result_code_t free_interval(uint64_t base, uint64_t len);
    /* 锚定软占领（定点对·对偶 free_interval）：从 base 起贪心认领连续空闲
     * 前缀，撞占用/额度上限即停。real_occupy=len−*rest_bits(入=已满足量,
     * 链式记账) 为上限；出 = len−claimed 总缺口；撞片尾如实回传 rest（跨界
     * 续圈归路由层）；claimed==0 → RC_ERR_NO_BLOCK；cap 片内夹断永不越片；
     * hint 只进不退。 */
    result_code_t soft_occupy(uint64_t base, uint64_t len, uint64_t *rest_bits);
    void disable_slice();
};

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slices_cfg_t — 无状态初始化配置（调用方组装，mgr 只消费不持有）
 * ═══════════════════════════════════════════════════════════════ */
struct bitmap_slices_cfg_t {
    /* 位图原始内存（已加载/已映射），生命周期归调用方；4K 对齐 */
    uint8_t  *block_bits_raw  = nullptr;
    uint64_t  block_bits_count = 0;    /* block 组位数（= bitmap_blocks×4096×8） */
    uint8_t  *inode_bits_raw  = nullptr;
    uint64_t  inode_bits_count = 0;    /* inode 组位数（= inode_bm_blocks×4096×8） */
    /* preferred 槽数 + 2^n 上界推导输入；0 → 退化单槽 */
    uint32_t  logical_processor_count = 1;
    /* 片下限（位数）：1 个位图块 = 32768 bit，4K 对齐切片约束，勿动 */
    uint64_t  min_slice_bits = 32768;
};

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slices_mgr_t — 双位图分片分配器（block 组 + inode 组）
 *
 * slice_count 推导是两组**共享**的（2^n 上界 ∩ 两组满片数最小值）——
 * 故合并单 mgr 而非双实例（双实例会重复推导且几何可能分叉）。
 *
 * 锁纪律（继承自旧上帝类路由，Docs §5/§8）：
 *   - alloc_*：try 锁，忙则换片（preferred 亲和扫描，仿 FPA::alloc）
 *   - free / occupy_soft（定点对）：自旋强制抢锁（定点不可换片/释放不可丢）
 *   - block_occupy_soft 片尾跨界续圈在内部消化（逐片单锁前缀链 = 真实
 *     连续，永不跨片双持）；可能产出跨片 run → block_free 跨片循环归还
 *
 * 移植注记：get_processor_id() 为外部合约（本模块 cpp extern 声明），
 * 内核端必须提供强定义；用户态由 weak 桩/测试强定义覆盖（cpu_id_stub.cpp）。
 * ═══════════════════════════════════════════════════════════════ */
class bitmap_slices_mgr_t {
public:
    /* ctor 即装配：推导 slice_count → 双组 enable_slice → preferred 初始化。
     * 失败时 *res_code 给出错误码且对象已自清理（成员为空/未分配态）。
     * 位图原始内存不取所有权（cfg 指针只读消费）。 */
    bitmap_slices_mgr_t(const bitmap_slices_cfg_t &cfg, uint64_t *res_code);
    ~bitmap_slices_mgr_t();
    bitmap_slices_mgr_t(const bitmap_slices_mgr_t &) = delete;
    bitmap_slices_mgr_t &operator=(const bitmap_slices_mgr_t &) = delete;

    /* ── block 组（FS_block 分配器）── */
    /* 硬分配：require_count 连续 + 起始块号 2^align_log2 对齐；单片内完成
     * （不跨片拼接）；全片失败 → PTR_INVALID（原子，不部分分配）。 */
    uint64_t block_alloc_hard(uint64_t require_count, uint8_t align_log2);
    /* 软分配：一次只从单片取（preferred 优先），实际 = min(区间长, require)；
     * *remain_count 出 = 缺口；一块没有 → PTR_INVALID。 */
    uint64_t block_alloc_soft(uint64_t require_count, uint64_t *remain_count);
    /* 软锚定占领（定点对·对偶 block_free）——尾部延展法轮子：blkbase 起
     * 认领 ≤ require_count 连续空闲块。锚点片唯一 → 自旋强制抢锁；片尾
     * 跨界续圈内部消化（下一片 base 0 继续，逐片单锁 = 真实连续）。
     * 返回：占领 ≥1 块 → blkbase（原址）；一块未占 → PTR_INVALID。
     * *rest_count 出 = 缺口。⚠ 可能产出跨片 run → block_free 跨片循环归还。 */
    uint64_t block_occupy_soft(uint64_t blkbase, uint64_t require_count, uint64_t *rest_count);
    /* 区间释放（跨片循环）：物理连续 run 可能跨片——逐段定位所属片释放；
     * 释放无连续要求，无需跨片双持。 */
    void     block_free(uint64_t blkbase, uint64_t count);
    /* 测试：直读位图内存（无锁；调用方保证并发安全） */
    bool     block_test(uint64_t blk) const;

    /* ── inode 组（inode 槽分配器；inode 0 = root，片 0 hint 从 1 起）── */
    uint64_t inode_alloc();
    void     inode_free(uint64_t inum);
    bool     inode_test(uint64_t inum) const;

    /* ── 几何/审计 ── */
    uint64_t slice_count() const        { return slice_count_; }
    uint32_t cpu_slots() const          { return cpu_slots_; }
    uint64_t block_per_slice_bits() const { return blk_.per_slice_bits; }
    uint64_t inode_per_slice_bits() const { return ino_.per_slice_bits; }
    uint64_t block_free_total() const;   /* 各片 free_count 求和（空闲块总数） */
    uint64_t inode_free_total() const;
    uint32_t block_preferred_of(uint32_t cpu) const;  /* dump_info 用 */
    uint32_t inode_preferred_of(uint32_t cpu) const;

private:
    /* 一组位图的全套状态（block / inode 各一） */
    struct group_t {
        fs_bitmap_descriptor_t desc{};
        bitmap_slice_t *slices = nullptr;
        uint64_t per_slice_bits = 0;
        uint32_t *preferred = nullptr;   /* [cpu_slots_]：每 CPU 亲和片游标 */
    };
    group_t blk_, ino_;
    uint32_t slice_count_ = 0;
    uint32_t cpu_slots_ = 0;

    void teardown();                     /* delete[] 自有数组（幂等） */
    /* 装配一组：按 nbits 与 slice_count_ 计算 per_slice_bits 并逐片 enable；
     * 组内 preferred 由 ctor 统一初始化。失败 → 非 RC_OK（已清理本组 slices）。 */
    result_code_t setup_group(group_t &g, uint8_t *raw, uint64_t nbits);
};
