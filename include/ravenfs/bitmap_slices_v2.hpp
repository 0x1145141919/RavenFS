#pragma once
#include "rc.hpp"                    /* result_code_t / RC_* / PTR_INVALID（轻量头，零盘结构依赖） */
#include "../util/lock.h"            /* 锁原语（供上层路由/后续实现取用；本头当前用 std::mutex） */
#include <cstdint>
#include <cstddef>
#include <mutex>                     /* std::mutex：分片唯一锁 slice_lock（可睡，允许持锁等盘 I/O） */

/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slices_v2.hpp — 双位图分片分配器「窗口版」（2026-09-30 从
 * bitmap_slices.hpp 尾段顶出为独立头）
 *
 * 定位：bitmap_slices_mgr_t（v1，外部备内存、纯内存零 I/O）的演进形态。
 * v2 由 mgr 自持存储、自装载/自扫描——调用方只给「两段位图落在盘上何处」
 * +「I/O 手段」（赋能式回调 + io_ctx），位数由块数推导。**v2 只做 CHUNK
 * 懒加载窗口**——要全量常驻内存请走 v1（bitmap_slices_mgr_t）。配置契约见下
 * bitmap_slices_v2cfg_t（含取舍注记）；
 * 底层分片思想见 Docs/runtime_tech/02_bitmap_slices.md，窗口化规格见
 * Docs/runtime_tech/03_chunk_backed_slices.md。
 *
 * ★ 核心决策（v2 vs v1）—— slice 坐标系统一为「片内局部，从 0 起」，
 *   不吃外部全局坐标系
 *
 * v1（bitmap_slice_t，位图模块 v1 形态）是**双坐标视角**：
 *   · 片持 parent_descriptor 回指 + base_idx（片首全局位号）；
 *   · enable_slice(global_descriptor, config) 吃外部全局描述符；
 *   · 对外出入全是**全局位号**（alloc 出 out_base_idx = base_idx + hit），
 *     内部才换算 片内偏移 = 全局位号 − base_idx。
 *   —— 片因此"知道自己站在全图哪一段"，与全局位图强耦合。
 *
 * v2 **刻意改为单一局部视角**（本版第一核心决策）：
 *   · 片内一切坐标/参数均为**片内偏移，恒从 0 起**；片不持全局描述符回指；
 *     全局基址外化为独立字段 in_global_base_idx（见 chunk_slice_mgr_t），
 *     片内算法绝不消费它；入参出参均局部 —— 片完全不知道自己在全图何处。
 *   · 全局 ↔ 局部换算**一律由路由层（mgr）承担**：
 *        全局位号 = 片 base_idx + 片内偏移
 *        片内偏移 = 全局位号 − 片 base_idx（释放定位 = 全局位号 / per_slice_bits）
 *   · 片退化为自包含、可独立构造/搬运/单测的纯局部单元 ——「片与片零共享」
 *     从"运行期不共享状态"进一步收紧为"编译期不引用全局"。
 *
 * 动机（为何值得偏离 v1）：
 *   1. 去掉 parent_descriptor 回指 → 片无外部依赖，单测无需合成全局位图；
 *   2. 与窗口化（CHUNK）天然正交：片只管"局部第 i 位"，窗口换页/lazy 的
 *      地址翻译全部落 backing 层，不再与全局坐标纠缠（v1 靠 slice_base
 *      重指技巧在全局坐标下打补丁，v2 把这个技巧整块消解）；
 *   3. 坐标系只剩一个（局部），换算点收在 mgr 一处，杜绝"某函数漏换
 *      全局/局部"的整类 bug。
 *
 * 后果：mgr 对外接口仍吃/吐**全局位号**（签名不变），换算全收在路由内；
 *       slice 侧接口一律改吃/吐**局部偏移**（v2 slice 类按此重写）。
 * 施工注记：slice 侧已按局部坐标落地（chunk_slice_mgr_t：片内 0 起 +
 *       外化 in_global_base_idx）；mgr 路由侧的 global↔local 换算待补。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slices_v2cfg_t — v2「窗口版」初始化配置（调用方组装，mgr 消费）
 *
 * 与 v1（bitmap_slices_cfg_t）的根本差异：
 *   v1 —— 外部把位图**原始内存**备好（block_bits_raw / inode_bits_raw +
 *         位数），mgr 纯内存、零 I/O、零设备知识、不持所有权。
 *   v2 —— 外部只给「两段位图落在盘上何处」+「I/O 手段」，mgr 自持存储、
 *         自装载/自扫描（**只做 CHUNK 懒加载窗口**；全量常驻内存用 v1）。
 *         I/O 上下文
 *         范式照抄 Inode_cache_cfg_t_v2 / mdata_piece_caches_cfg_t（赋能式
 *         回调 + io_ctx + 几何）；位图**位数不传**，由块数推导。
 *
 * 取舍（2026-09-30）：
 *   · 不设后端开关：位图分配器必须「可读可写」（no_cache/offload 无对应物），
 *     且**全量常驻已由 v1 承担** ⇒ v2 专做 CHUNK 懒加载，不引入 FULL/CHUNK
 *     二选一（等价于 ko bitmap_backing 只用其 CHUNK 档）。
 *   · 回调取**绝对 FS_block 号**（同 Inode_cache_cfg_t_v2::read_line，不用
 *     ko §2 的 rel+is_inode 形式）——两组共用一对回调，基准换算收在 mgr 内，
 *     宿主回调零 is_inode 分支。blk_len 单位 = FS_block(4K)，buf 4K 对齐。
 *   · 自描述校验下放：v1 在宿主 bitmaps_load 内做，v2 装载收进 mgr，故
 *     校验区间/root 位成为 cfg 字段。
 * ═══════════════════════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════════════════════
 * slice_soul_t — 分片静态属性（enable 时拷入；运行期只读）
 *
 * 一个分片的"不变面"：I/O 手段 + chunk 几何 + 本片位图区在盘上落点。
 * 每片一份（各片位图区基址不同）⇒ 按值持有，不跨片共享。
 * ═══════════════════════════════════════════════════════════════ */
struct slice_soul_t {
    result_code_t (*read_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
    result_code_t (*write_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);
    void    *io_ctx;             /* 宿主上下文（god 传 fs this；ko 传 sb/sbi） */
    uint64_t chunk_base_blk;     /* 本片位图区起始 FS_block（绝对） */
    uint64_t chunk_blk_length;   /* 本片位图区总块数（chunk 区长度） */
    uint8_t  chunk_order;        /* chunk = 4096<<order 字节 = (1<<order) 个 FS_block */
};

/* ═══════════════════════════════════════════════════════════════
 * 索引合成 / 尾块边界（数学，先钉死；见 03_chunk_backed_slices.md §3）
 *
 *   CHUNK_SHIFT = 15 + chunk_order          chunk_bits = 1<<CHUNK_SHIFT（位）
 *   ctl_idx     = slice_off >> CHUNK_SHIFT  （片内偏移 → chunk 序数）
 *   win_off     = slice_off & (chunk_bits-1)（片内偏移 → 本 chunk 内偏移）
 *   盘块        = chunk_base_blk + (ctl_idx << chunk_order)
 *   tail_bits   = 32768 * (k mod 2^order)   （k = slice_bits/32768）
 *   ⇒ tail ∈ {0（不存在）, chunk_bits, 或 32768 的倍数} —— **无 sub-page 尾块**
 *     （slice_bits 恒 32768 倍数）⇒ 位移/掩码全链路安全，尾块差异只在
 *     limit_of(ctl) 一处夹断，零特判分支。
 * ═══════════════════════════════════════════════════════════════ */
static constexpr uint32_t SLICE_CHUNK_NONE = 0xFFFFFFFFu;  /* online_idx_ 哨兵 */

/* compact_word 位域（chunk_mgr 单原子状态字；仿 inode_cache ctrl_word 打包）：
 *   [0,2) summary（ALL_USED/ALL_FREE/MIXED）· [2,3) dirty · [3,4) has_recs
 * 打包动机：快路径/免载入摘要裁决一次 load 拿全判定位，免多字段撕读。 */
static constexpr uint64_t CHUNK_SUMMARY_MASK  = 0x3ull;
static constexpr uint64_t CHUNK_SUM_ALL_USED  = 0ull;      /* 盘态全 1（无空闲） */
static constexpr uint64_t CHUNK_SUM_ALL_FREE  = 1ull;      /* 整窗可判空闲 */
static constexpr uint64_t CHUNK_SUM_MIXED     = 2ull;      /* 须上线查 */
static constexpr uint64_t CHUNK_FLAG_DIRTY    = 1ull << 2; /* 在线且被改（未回写） */
static constexpr uint64_t CHUNK_FLAG_HAS_RECS = 1ull << 3; /* 本 chunk 归还记录链非空（rec_head≠null） */

/* 归还记录（free 挂账单位，无论在线与否）：片内偏移视角的连续区间 [off, off+len)，单链表节点。
 * 不变式：一记录 ⊆ 单 chunk（free 在插入时按 chunk 边界切开）⇒ drain 退化为
 * "逐节点清位 + 释放节点"，无需窗口求交/收缩/劈分（对比 ko drain_window）。
 * 不搞共享池 / 定长容量：**单链表 + 堆节点（容量无限）**——free 只前插、drain 遍历
 * 释放，无需随机访问；仍满足"片与片零共享"。
 * 链操作**全在 slice_lock 内**（asyn 也走锁全量）⇒ 无需额外锁。【2026-09-30 用户裁。
 * 可选改 std::forward_list；本头保持裸单链零 STL 依赖。】 */
struct rec_seg_t {
    uint64_t   off;    /* 片内偏移 */
    uint64_t   len;
    rec_seg_t *next;
};

/* 单 chunk 控制器（片内每人一个，位置即身份 ctl_idx）。
 * rec_head = 本 chunk 归还记录链头（堆节点；该窗被使用前合并进位图并释放）。
 * 不存 disk_block_rel / tail_bits：均可由 ctl_idx + soul 推导，少一处不一致源。
 * online 与否是*片级*事实（一片至多一个在线 chunk）⇒ 权威放 chunk_slice_mgr_t，
 * 不在此冗余。 */
struct chunk_mgr {
    uint64_t   compact_word = CHUNK_SUM_MIXED;  /* 位域见 CHUNK_* 常量 */
    rec_seg_t *rec_head = nullptr;              /* 归还记录链头（单链，堆节点，容量无限） */
};

/* 分片总状态机（整体工作态门闸）——只有 WORKABLE 能接待：
 *   SLICE_UNWORKABLE —— 不接待：构造后未 enable / shutdown 完成后（资源已收）。
 *   SLICE_WORKABLE   —— 可接待：enable 成功之后、shutdown 之前。
 * 生命周期：ctor→UNWORKABLE → enable 建好 mgrs/buffer→WORKABLE →
 *          shutdown 锁内跑完一轮工作（回写+还账+释放）→UNWORKABLE。
 * 门闸纪律：alloc/free/test/occupy/chunks_asyn **仅 WORKABLE 可调**；非 WORKABLE
 *          一律拒（RC_ERR_STATE），不得碰 mgrs/buffer。状态迁移在 slice_lock 内。 */
enum slice_state_t : uint8_t {
    SLICE_UNWORKABLE = 0,   /* 不接待 */
    SLICE_WORKABLE   = 1,   /* 可接待 */
};

/* free_count 校准回报（chunks_asyn 出参；见 ★ 记账）：
 * slice 原语层**允许重复 free**（整 FS 不允许），故运行中 free_count 可能与真实位态
 * 漂移 → asyn 整片重扫时把 [sync 前, sync 后] 两个计数一并回报，由外部判定是否
 * 不同步（old 应等于外部记账预期）。指针可空（不关心时不传）。 */
struct free_count_calib_t {
    uint64_t old_free_count;   /* 校准前（本片累计记账值） */
    uint64_t new_free_count;   /* 校准后（整片重扫实测值） */
};

/* ═══════════════════════════════════════════════════════════════════
 * chunk_slice_mgr_t — 窗口化分片自治单元（v2 slice）
 *
 * ★ 坐标契约（本版第一核心决策，见文件顶）：片内算法/状态一律**局部（0 起）**；
 *   入参出参均为**片内偏移**（参数名 slice_base / out_slice_base_idx 即此），
 *   不吃外部全局坐标；全局基址 in_global_base_idx 外化为独立字段（enable 初始
 *   化），片内逻辑不消费，供上层换算（全局 = in_global_base_idx + 片内偏移）。
 *
 * ★ 状态机（整体工作态门闸）：**UNWORKABLE ↔ WORKABLE**。
 *   ctor=UNWORKABLE → enable 建好资源=WORKABLE → shutdown 锁内跑完一轮工作=UNWORKABLE。
 *   门闸纪律：**仅 WORKABLE 接待服务**（alloc/free/test/occupy/chunks_asyn）；非
 *   WORKABLE 一律拒 RC_ERR_STATE（不碰 mgrs/buffer）。状态迁移在 slice_lock 内。
 *
 * ★ 窗口模型：一片有 chunks_per_slice 个 chunk 控制器（mgrs[]，位置即身份），
 *   任一时刻**至多一个 chunk 上线**（唯一 active_chunk_buffer）——故 online 是片级
 *   事实（online_idx_）。命中在线窗 = 纯内存快路径；命中离线窗 = 换片慢路径
 *   （evict → load → drain → publish）。
 *
 * ★ 返还（统一挂链）：**free 一律前插本 chunk 的归还记录链，无论该 chunk 是否在线**
 *   （零 I/O，O(1)）。合并点 = 该窗被使用之前（新上线：load 后 publish 前；已在线窗：
 *   本次操作开启时）——先合并、后 publish/扫描，否则 alloc 会把盘上仍为 1 的位判成
 *   已用 → 永久泄漏。挂账即记账（free_count 立刻更新，alloc gate 保真）。
 *   slice_lock 互斥整个接口 ⇒ 一次操作扫描期间不会有新记录插入 ⇒ 一遍扫描即终态；
 *   失败（如 alloc 无满足段）**不要重试期待新记录救场**（不会有）。
 *
 * ★ 记账（free_count）：**初始化实测**（enable 全量读一趟计数）→ alloc 成功 **扣减** →
 *   free 挂账即 **记账（+len）** → chunks_asyn 全量合并后 **校准**（整片重扫，纠偏），
 *   并把 [旧, 新] 计数经 free_count_calib_t 回报外部。
 *   与驻留无关，恒反映逻辑空闲位数（statfs / alloc gate 保真）。
 *
 * ★ 锁（**一把**）：
 *   slice_lock（可睡互斥锁）——覆盖分片全部临界：位运算 / online_idx_ / compact_word
 *   / 归还记录链挂账 / 换片与盘 I/O（持锁等 I/O；可睡故允许）。
 *   分配走 try_lock：拿不到 = 本片忙 → 上层按 preferred 换下一片（分片设计本就是
 *   拿并行度换掉竞争，故单片一把锁足够；避免两把锁带来的腐坏面）。free/occupy
 *   走阻塞 lock（定点/释放不可丢）。
 * ═══════════════════════════════════════════════════════════════════ */
class chunk_slice_mgr_t {
    /* ── 静态属性 & 生命周期 ── */
    slice_soul_t   static_properties{};       /* enable 拷入（本片不变面） */
    slice_state_t  slice_state = SLICE_UNWORKABLE;   /* 门闸：仅 WORKABLE 接待（见类顶 ★ 状态机） */

    /* ── 几何（enable 派生，运行期只读） ── */
    uint64_t slice_bits_ = 0;                 /* 本片位数（32768 倍数） */
    uint32_t chunks_per_slice_ = 0;           /* = ceil(slice_bits / chunk_bits) */
    uint64_t tail_bits_ = 0;                  /* 尾 chunk 有效位（32768 倍数，≥1 页） */

    /* ── 唯一在线窗 ── */
    uint32_t online_idx_ = SLICE_CHUNK_NONE;  /* 当前上线 chunk；NONE 哨兵。变更持 slice_lock */
    uint32_t gen_ = 0;                        /* 换片代号：发布/下线递增，快路径复检 */
    uint8_t *active_chunk_buffer = nullptr;   /* 尺寸 = 4096<<order；唯一在线窗 */

    /* ── 控制器数组 ── */
    chunk_mgr  *mgrs = nullptr;               /* [chunks_per_slice_]：位置即身份 */

    /* ── 记账 ── */
    uint64_t free_count = 0;                  /* 本片逻辑空闲位数：init 实测 / alloc 扣减 / free 记账 / asyn 校准 */
    uint64_t in_slice_hint = 0;               /* 片内分配游标（局部；只进不退 + 绕回） */

    /* ── 唯一分片锁（见类顶 ★ 锁）──
     * 一把可睡互斥锁覆盖整个分片（位运算 + 在线窗 + 归还记录链 + 盘 I/O）。
     * 分配走 try_lock（忙 → 上层换片）；free/occupy 走阻塞 lock（不可丢）。 */
    std::mutex     slice_lock;

public:
    /* 外化基址：本片在 group 全局位号系的基址（enable 初始化）。
     * 片内算法不消费；供上层换算（全局 = in_global_base_idx + 片内偏移）/审计。 */
    uint64_t in_global_base_idx = 0;

    /* 构造/析构 = 空操作（资源由 enable 建、shutdown 收）。 */
    uint64_t get_free_count();
    uint64_t get_hint();
    void     set_hint(uint64_t new_hint);

    /* ── 分配（入参/出参均片内偏移；片与片零共享，try-换片路由在 mgr 层）──
     * **仅 WORKABLE**；否则 RC_ERR_STATE。
     * 内部 **try_lock** 分片锁：拿不到 = 本片忙 → RC_ERR_BUSY（mgr 按 preferred 换片）。
     * alloc_hard：需 bitscount 连续空闲位、起始位 2^align_log2 对齐；不部分分配；
     *             片内全窗迭代（hint 窗起 → 片尾 → 绕回 [0,hint)），全片无 → RC_ERR_NO_BLOCK。
     * alloc_soft：首空闲段取对齐候选，实际 = min(段长, bitscount)；缺口回传。
     * ⚠ **单窗语义**：一次分配命中恒落在**单个 chunk（窗口）内**——请求 > 单窗容量
     *   chunk_bits(=32768<<chunk_order) 时，alloc_hard 返 RC_ERR_NO_BLOCK、alloc_soft
     *   只在单窗内取（缺口回传）。**大跨度连续分配请走 soft_occupy / mgr 的
     *   block_occupy_soft（跨窗、跨片续圈）。**【2026-09-30 最高规格压测固化】
     * 对齐：**align_log2 仅支持 0..15**（片恒 32768=2^15 位对齐，窗起点又 ≥ 2^15 ⇒
     *       ≤15 的任意对齐点都落在片内整数位，无跨窗歧义）；>15 → RC_ERR_STATE（防御，勿截断）。 */
    result_code_t alloc_hard(uint8_t align_log2, uint64_t bitscount, uint64_t &out_slice_base_idx);
    result_code_t alloc_soft(uint8_t align_log2, uint64_t bitscount, uint64_t &out_slice_base_idx,
                             uint64_t &bits_left_to_be_allocated);
    /* ── 区间测试/释放（片内坐标；区间须整落本片，跨界由 mgr 切段）──
     * **仅 WORKABLE**；否则 RC_ERR_STATE。
     * free/soft_occupy 走**阻塞** lock（定点/释放不可丢）；test 可视需要 try。 */
    result_code_t test_interval(bool val, uint64_t slice_base, uint64_t len);
    result_code_t free_interval(uint64_t slice_base, uint64_t len);
    /* ── 锚定占领（定点对·对偶 free_interval；片内贪心前缀，顶片尾即停）──
     * **仅 WORKABLE**；否则 RC_ERR_STATE。 */
    result_code_t soft_occupy(uint64_t slice_base, uint64_t len, uint64_t *rest_bits);

    /* ── 生命周期 ──
     * enable：UNWORKABLE→WORKABLE。拷 soul → 派生几何 → 建 mgrs[]（链头空）/buffer →
     *         **一次整片全量读入（临时缓冲）实测 free_count + 填每 chunk 2bit 摘要**
     *         （与 chunks_asyn 同机制；mount 期一次性 I/O）。
     * shutdown：**须 WORKABLE**；锁内跑完一轮工作（全体在线脏窗回写 + 全体 chunk 归还
     *         链合并落盘 + 释放 buffer/mgrs/残留链节点）→ 置 UNWORKABLE。 */
    result_code_t enable(slice_soul_t *soul, uint64_t in_global_base_idx);
    result_code_t shutdown();
    /* ── TTL/周期同步：**仅 WORKABLE**；持 slice_lock 一整轮全量（2026-09-30 用户裁）──
     * 流程：一次 I/O 读入本片全量位图 → 合并本片全部 chunk 的归还记录链（逐节点清位
     *       + 释放节点）→ 一次 I/O 整片写回（在线窗以 active_chunk_buffer 为准覆盖暂存；
     *       写回后其 dirty 清零）。
     * 取舍：相对"逐 chunk 换片 + 绕锁/自旋 asyn"——本方案 **I/O 次数少（1 读 1 写）、
     *       复杂度低**（无需 per-chunk 三态、无需链自旋锁），代价是 **持锁时间长**（性能
     *       抖动）。也因为全走锁，链操作无需额外锁。
     * 校准回报：out_calib 非空时回填 {sync 前 free_count, sync 后整片重扫值}，
     *         供外部判定不同步（见 free_count_calib_t；slice 层允许重复 free 致漂移）。
     * 实现注记：整片读入用**堆上临时缓冲**（slice_bits/8 字节）——分配→读→合并→写回→释放，
     *         一次性不常驻；合并后**校准本片 free_count**（整片重扫）。ko 移植入乡随俗
     *         （vmalloc/kvmalloc + 按 ko 锁纪律，勿照搬 god 分配器）。 */
    result_code_t chunks_asyn(free_count_calib_t *out_calib = nullptr);

private:
    /* ── 门闸/几何/编码 ── */
    bool is_workable() const { return slice_state == SLICE_WORKABLE; }
    uint64_t chunk_bits() const { return 32768ULL << static_properties.chunk_order; }
    uint32_t ctl_of(uint64_t slice_off) const {
        return (uint32_t)(slice_off >> (15 + static_properties.chunk_order));
    }
    uint64_t limit_of(uint32_t ctl) const {
        return (ctl + 1u == chunks_per_slice_) ? tail_bits_ : chunk_bits();
    }
    uint64_t chunk_disk_blk(uint32_t ctl) const {
        return static_properties.chunk_base_blk
             + ((uint64_t)ctl << static_properties.chunk_order);
    }

    /* ── 换片状态机（全程持 slice_lock；可睡故允许持锁等盘 I/O）──
     * ensure_online：窗不在线时的慢路径全序列：evict 旧窗 → load 目标窗 →★ drain
     *               归还记录 → publish(online_idx_=ctl) → 按 drained 置 dirty。
     *               （摘要在窗迭代时先裁决：ALL_USED 直接跳窗，不触发换片。）
     * drain_chunk ：合并本 chunk 归还记录链（须持 slice_lock；逐节点清位 + 释放节点）。
     * evict_online：下线当前在线窗（快照+清 dirty → 写盘 → 重算摘要）。 */
    int  ensure_online(uint32_t ctl);
    void drain_chunk(uint32_t ctl);
    void evict_online();
    void recompute_summary(uint32_t ctl, const void *src);

    /* ── 单窗口原语（须持 slice_lock 且窗在线；from/to/out 均本 chunk 内偏移）── */
    bool win_alloc_hard(uint64_t from, uint64_t to, uint8_t align_log2, uint64_t count, uint64_t *out);
    bool win_alloc_soft(uint64_t from, uint64_t to, uint8_t align_log2, uint64_t count,
                        uint64_t *out, uint64_t *left);
    int  win_soft_occupy(uint64_t wlo, uint64_t whi, uint64_t base, uint64_t len, uint64_t *rest);
};
struct bitmap_slices_v2cfg_t {
    /* ── I/O 手段（赋能式；两组共用）── */
    result_code_t (*read_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
    result_code_t (*write_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);
    void *io_ctx;                                 /* 宿主上下文（god 传 fs this；ko 传 sb/sbi） */

    /* ── 两段位图在盘上的落点（mount 由 superblock/disk_layout 提供；
     *    位数由 块数 × 4096 × 8 推导，不传）── */
    uint64_t block_bitmap_base_FS_block;          /* block 组位图起始 FS_block */
    uint64_t block_bitmap_blocks;                 /* block 组位图占块数（bit i ↔ FS_block[i]） */
    uint64_t inode_bitmap_base_FS_block;          /* inode 组位图起始 FS_block */
    uint64_t inode_bitmap_blocks;                 /* inode 组位图占块数（bit i ↔ inode[i]） */
    uint8_t inobm_chunk_order;
    uint8_t blkbm_chunk_order;
    uint32_t processor_count;
};
class bitmap_slices_mgr_t_v2 {//窗口版本
public:
    /* ctor 即装配（CHUNK-only）：推导 slice_count → 逐片构造 soul（I/O + 本片盘位图
     * 区落点）并 enable（建 mgrs/buffer）→ preferred 初始化。
     * 失败时 *res_code 给出错误码且对象已自清理（成员为空/未分配态）。
     * cfg 只读消费；buffer/mgrs 归各 slice 自持（enable 建 / shutdown 收）。 */
    bitmap_slices_mgr_t_v2(const bitmap_slices_v2cfg_t &cfg, uint64_t *res_code);
    ~bitmap_slices_mgr_t_v2();
    bitmap_slices_mgr_t_v2(const bitmap_slices_mgr_t_v2 &) = delete;
    bitmap_slices_mgr_t_v2 &operator=(const bitmap_slices_mgr_t_v2 &) = delete;

    /* ── 分配（**单窗语义**：命中恒在单 chunk 内；require_count 超单窗容量 → hard 返
     *    PTR_INVALID / soft 部分返。**大跨度连续分配用 block_occupy_soft**，可跨窗跨片）── */
    uint64_t block_alloc_hard(uint64_t require_count, uint8_t align_log2);
    uint64_t block_alloc_soft(uint64_t require_count, uint64_t *remain_count);
    uint64_t block_occupy_soft(uint64_t blkbase, uint64_t require_count, uint64_t *rest_count);
    void     block_free(uint64_t blkbase, uint64_t count);
    bool     block_test(uint64_t blk) const;

    /* ── inode 组（inode 槽分配器；inode 0 = root，片 0 hint 从 1 起）── */
    uint64_t inode_alloc();
    void     inode_free(uint64_t inum);
    bool     inode_test(uint64_t inum) const;

    /* ── 几何/审计 ── */
    uint64_t slice_count() const        { return slice_count_; }
    uint64_t block_free_total() const;   /* 各片 free_count 求和（空闲块总数） */
    uint64_t inode_free_total() const;
    uint32_t block_preferred_of(uint32_t cpu) const;  /* dump_info 用 */
    uint32_t inode_preferred_of(uint32_t cpu) const;

private:
    /* 一组位图的全套状态（block / inode 各一） */
    struct group_t {
        chunk_slice_mgr_t *slices = nullptr;
        uint32_t *preferred = nullptr;   /* [cpu_slots_]：每 CPU 亲和片游标 */
    };
    group_t blk_, ino_;
    uint32_t slice_count_ = 0;
};
