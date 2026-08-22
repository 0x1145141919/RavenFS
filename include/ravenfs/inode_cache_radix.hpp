#pragma once
#include "disk_struct_v1.1.hpp"
#include <atomic>
#include <shared_mutex>   /* slice_radix_tree::lock — 可睡眠读写锁（允许持锁等 I/O，不烧核） */
#include "util/tsc.h"    /* x86_64 Invariant TSC：TTL 换算（绑死 x86_64） */
class Inode_cache_mgr_t;
class RavenFS_juvenile;
enum cache_ctrl_state_t{
    idle,      /* 未载入（CAS 状态机：idle → io_ing → working） */
    io_ing,    /* 他人加载中 / 本线程持有加载权 */
    working,   /* 已就绪，可被占坑 */
    evicting   /* TTL 淘汰接管中：写回+释放进行时，get 忙等，完成后回 idle */
};
/* ═══════════════════════════════════════════════════════════════════
 * ctrl_word 单原子字位域布局（state + flags + accessor 打包）：
 *   [0,8)   state    —— CAS 状态机（idle/io_ing/working/evicting）
 *   [8,24)  flags    —— 修饰位（CTRL_FLAG_DIRTY）
 *   [24,56) accessor —— 会话占坑计数（32 位）
 *
 * 为什么打包：reap「accessor==0 才能接管淘汰」与 get「占坑 +accessor」
 * 必须落在同一个原子操作上，否则存在 TOCTOU：
 *   旧两字段方案：get 复核 state==working 通过后返回指针，reap 才 CAS
 *   working→evicting 成功（其 accessor 检查读在 get 占坑之前）→ 页被 free，
 *   已返回的指针悬垂。
 * 打包后：占坑 = CAS(word, word + INC)（成功即占坑，无需复核）；
 *         接管 = CAS(word, evicting)（CAS 前提含 working && accessor==0）
 *         两个窗口同时关闭。
 * ═══════════════════════════════════════════════════════════════════ */
static constexpr uint64_t CTRL_STATE_MASK     = 0xFFull;
static constexpr uint64_t CTRL_FLAGS_SHIFT    = 8;
static constexpr uint64_t CTRL_ACCESSOR_SHIFT = 24;
static constexpr uint64_t CTRL_ACCESSOR_INC   = 1ull << CTRL_ACCESSOR_SHIFT;
static constexpr uint64_t CTRL_FLAG_DIRTY     = 1u << 0;   /* flags 域内位：页被修改，需回写 */
struct Inode_cache_ctrl_t{
    raven_Inode_t *cache_data;          /* 页基址；working 发布/观察（ctrl_word release/acquire） */
    std::atomic<uint64_t> accss_stamp;  /* 最近访问 TSC 戳（尽力而为，relaxed 读写） */
    std::atomic<uint64_t> ctrl_word;    /* 位域见上：state/flags/accessor 单原子字 */
    uint64_t blk;                       /* 本行块号（加载者 CAS io_ing 成功后写定，reap/flush 读） */
};
struct cut_result_t{
    uint64_t main;
    uint64_t cut_out_section;
};
extern cut_result_t cut_high(uint64_t raw_metarial,uint8_t bits_to_cut);
/* cut_high — 位域切割原语（左对齐方案，通用轮子，块缓存等场景可复用）：
 * raw 须已左对齐（位域最高位 = bit 63）；从位域顶端向下切 bits_to_cut 个 bit：
 * cut_out_section = 切下的高位段（右对齐），main = 剩余位域（保持左对齐，供递归）。
 * 实现见 juvenile_god/inode_cache.cpp */
/**
 * 我们主要根据bits_len_of_inodes_count在1～52的枚举值动态枚举规划，[5,12]是全量上内存，[13,18]是全量缓存行控制器数组(每个都是4k页面16entries)，大于等于19才是需要引入静态基数树分片
 * （statey_19~52 策略表定义已随实现移入 juvenile_god/inode_cache.cpp，此处仅留设计注记）
 */

/* ═══════════════════════════════════════════════════════════════════
 * ── 2026-08-22 增量：三模式判定 + 配置大结构体（Inode_cache_cfg_t） ──
 *
 * 生命周期整体设计：
 *   - 构造：cfg 整体传入，模式由 bits_len_of_inodes_count 推导
 *     （单真相源，不显式指定模式，避免两处对不上）；
 *     FULL_RAM 在构造时全量载入（构造即常驻）
 *   - 析构：脏页全量回写 + 释放；RADIX 逐片析构
 *   - 会话：get/put/mark_dirty 三模式表面一致（InodeSession 收口，
 *     决策见 209433a），I/O 永不持锁
 *
 * 为什么用大结构体而非逐参 / 摸宿主：
 *   1. 避免隐式状态依赖 —— mgr 不反向持有 RavenFS_juvenile，
 *      几何（inode_array_base）与 I/O（read_page/write_page 回调）
 *      全部显式注入；换宿主只改 cfg 组装，mgr 核心零改动
 *   2. 可移植可测试 —— 测试端注入内存缓冲 + 假回调即可跑全生命周期，
 *      不挂真盘、不依赖上帝类
 *
 * 实现位置：juvenile_god/inode_cache.cpp（本头文件仅声明）
 * ═══════════════════════════════════════════════════════════════════ */
enum inode_cache_mode_t : uint8_t {
    CACHE_MODE_FULL_RAM    = 0,  /* [1,12]  全量上内存：数据常驻，构造即载入 */
    CACHE_MODE_CTRL_ARRAY  = 1,  /* [13,18] 全量控制器数组：页按需加载，加载后常驻不淘汰 */
    CACHE_MODE_RADIX_SHARD = 2,  /* [19,52] 静态基数树分片：控制器按需建，有容量上限要淘汰 */
};

static inline inode_cache_mode_t inode_cache_mode_of(uint8_t bits) {
    if (bits <= 12) return CACHE_MODE_FULL_RAM;      /* [1,4] 一并归入：更小，无悬念 */
    if (bits <= 18) return CACHE_MODE_CTRL_ARRAY;
    return CACHE_MODE_RADIX_SHARD;
}

struct Inode_cache_cfg_t {
    /* ── 几何（disk_layout_v1.1 继承 v1，见 block_model.md） ── */
    uint64_t inodes_count;              /* inode array 总 inode 数（含根 inode 0） */
    uint8_t  bits_len_of_inodes_count;  /* ceil(log2(inodes_count))，1~52；
                                           模式判定唯一真相源 */
    uint64_t inode_array_base;          /* inode array 起始 FS_block 号；
                                           页地址 = base + line（line = inum >> 4） */

    /* ── 数据面内存预算（字节） ──
     * 淘汰已改为 TTL 超时（见 ttl_ticks），预算不再参与准入/淘汰水位，
     * 保留作内存分布统计与后续调优参考 */
    uint64_t memory_budget;

    /* ── 缓存行 TTL（TSC tick） ──
     * 0 = 默认 2s（ctor 按 Invariant TSC 频率换算）；
     * 运行期可用 Inode_cache_mgr_t::set_ttl_ticks 调整 */
    uint64_t ttl_ticks = 0;

    /* ── I/O 回调（页粒度：1 页 = 4096B = 16 × raven_Inode_t） ──
     * 回调而非直接调 fs->read_buff：mgr 不依赖宿主类，
     * host 用户态 / sparrowos kmod / linux 模块三端各适配一次 */
    result_code_t (*read_page)(void *io_ctx, uint64_t blk, void *buf);
    result_code_t (*write_page)(void *io_ctx, uint64_t blk, const void *buf);
    void *io_ctx;                       /* 宿主上下文（通常传 fs this） */

    /* ── 内存分配回调 ──
     * 三端分配器不同（host malloc / sparrowos FreePagesAllocator /
     * linux kmalloc），mgr 不直接 new */
    void *(*alloc)(void *io_ctx, uint64_t size);
    void  (*free)(void *io_ctx, void *ptr);
};

class Inode_cache_mgr_t{
public:
    /* 构造：cfg 大结构体整体传入（不再逐参 / 摸宿主）。
     * 模式由 bits 推导；FULL_RAM 在此全量载入。
     * rc_out 非空时收构造结果（RC_OK / RC_ERR_IO / RC_ERR_NO_BLOCK） */
    Inode_cache_mgr_t(const Inode_cache_cfg_t &cfg, result_code_t *rc_out = nullptr);
    ~Inode_cache_mgr_t();//每个片进行折构（脏页回写 + 释放）

    /* ── 统一会话接口（三模式表面一致） ──
     * get 输出会话 token（RADIX 定位分片/释放读锁用），put 必须带回该 token；
     * out_token 可空（调用方不关心时），FULL_RAM/CTRL_ARRAY 填 slice_idx=0 */
    struct slice_cache_acces_token_t
    {
        uint16_t slice_idx;   /* RADIX：分片序号（per_level_width[0] 高位切分）；其余模式恒 0 */
    };
    raven_Inode_t *get(uint64_t inum, slice_cache_acces_token_t *out_token = nullptr);
    void           put(uint64_t inum, const slice_cache_acces_token_t &token);
    void           mark_dirty(uint64_t inum);
    result_code_t  flush_all();         /* 全量脏页回写（挂载/卸载同步点用） */
    /* ── TTL 淘汰（绑死 x86_64 + Invariant TSC） ──
     * set_ttl_ticks：外部调整缓存行 TTL（默认 2s，ctor 按 TSC 频率换算）；
     *                调整即时生效（slice 持指针共享同一真相源）
     * reap_expired：专用淘汰线程周期调用 —— 回收全部「超时 && 无访问者」的行
     *                （RADIX 逐片扫 + CTRL_ARRAY 全扫；写回脏页 + 释放 + 摘除） */
    void set_ttl_ticks(uint64_t ticks);
    void reap_expired();
    bool is_ready() const { return ready_; }
    inode_cache_mode_t mode() const { return mode_; }
    uint8_t bits_len() const { return cfg_.bits_len_of_inodes_count; }

private:
    static constexpr uint8_t  ROW_BITS = 4;            /* 行内引索 4bit = 16 inode/页 */ //要不得，我的juvenile_god/inode_cache.cpp里面设计了有些情况是5bit的，当然全量缓存控制器数组是每个缓存行4k,16entries
    static constexpr uint64_t INODES_PER_PAGE = 16;

    Inode_cache_cfg_t  cfg_;
    inode_cache_mode_t mode_;
    bool ready_ = false;
    std::atomic<uint64_t> ttl_ticks_ = 0;   /* 缓存行 TTL（TSC tick）：默认 2s（ctor 换算），
                                               set_ttl_ticks 调整；slice 持 &ttl_ticks_ 共享，
                                               reap 扫描时读最新值（原子，防数据竞争） */

    /* ── FULL_RAM 分支 [1,12] ── 全量常驻，写回全量一口气（≤1MB，无脏页位图） */
    raven_Inode_t *full_ram_array_ = nullptr;   /* [inodes_count] 全量常驻 */

    /* ── CTRL_ARRAY 分支 [13,18] ── 控制器全量常驻，数据页懒分配：
     *    get 的 io_ing 态内 alloc+read 原子完成，析构 write-back+free */
    Inode_cache_ctrl_t *ctrl_array_ = nullptr;  /* [ctrl_count]，位置即身份：line = inum>>4 */

    /* ── RADIX 分支 [19,52] ── 分片树已实装：
     *    global_slice_plan 按 statey 策略表解析（inode_cache.cpp 内），
     *    slices_ = [2^level_bits[0]] 棵 slice_radix_tree
     *
     * 位域规划（bitfield_decode_plan_t）：按 bits_len_of_inodes_count 从最高位向下
     * 逐层切分（cut_high 递归），level_bits[0] = mgr 层分片选择宽度（slice 数 = 2^w0），
     * level_bits[1..] = 基数树逐层宽度，剩余低位 = 缓存行内引索（row_bits）。
     * row_bits = bits - sum(level_bits)：绝大多数档 4bit(16 entries=4KB=1块)，
     * 但 bits=25 / bits=33 两档为 5bit(32 entries=8KB=2块) —— 行内宽度由 decode 派生，
     * 不做独立常量；ROW_BITS 常量仅 FULL_RAM/CTRL_ARRAY 临时可用（RADIX 实装时改成员变量）。
     * decode 结果 chunks[0..total_levels-1] = 各层索引（[0]=slice 选择），
     * chunks[total_levels] = 行内索引；plan 为唯一真相源，decode 不依赖 mgr 状态。 */
    struct bitfield_decode_plan_t{
        /**
         * bits_len：inode 号总位宽（= cfg.bits_len_of_inodes_count 的镜像，plan 必须自包含）：
         *           decode 左对齐 + row_bits 派生的唯一真相源
         * total_levels：使用的层数（含 mgr 层），level_bits[0..total_levels-1] 有效
         * level_bits[0]：mgr 层分片选择位宽（slice 数 = 2^level_bits[0]）
         * level_bits[1..]：分片基数树从上到下的逐层位宽
         * 剩余低位 = 行内引索：row_bits = bits_len - sum(level_bits)，多数 4，25/33 档 5
         */
        uint8_t bits_len;
        uint8_t total_levels;
        uint8_t*level_bits;
    };
    bitfield_decode_plan_t global_slice_plan;
    struct bitfield_decoded_result_t{
        uint64_t raw;//原始数据（inum）
        uint8_t total_levels;//从plan处继承：0～total_levels-1 引索中间位域解析结果（层级索引），引索 total_levels 的是缓存行解码引索（行内索引）
        uint16_t chunks[7];//层级索引容量：现表最大 5 层 + 行内 = 6，留 1 冗余（支撑未来 6 层）；每项位宽 ≤ 11bit ≤ uint16_t
    };
    static bitfield_decoded_result_t decode(uint64_t raw,bitfield_decode_plan_t plan);//内部递归调用cut_high；raw 左对齐 + 累积推导 row_bits，plan 为唯一真相源，不依赖 mgr 状态（static：slice 树可直接复用）
    /* slice_radix_tree_cfg_t — 分片树构造大结构体（组装方：mgr ctor 的 RADIX 分支，按 chunks[0] 逐片组装）
     * 哲学对齐 Inode_cache_cfg_t：不复制回调、不反向持有宿主；树只依赖 cfg，可脱离 mgr 单测
     *   host_cfg    ：mgr 的 cfg 直传（I/O 回调 + alloc/free + io_ctx + inode_array_base + 总预算）
     *   plan        ：位域解码规划。仅 ctor 推树拓扑用（tree_levels = total_levels-1，
     *                 根表宽 = 2^level_bits[1]）；运行期 get/put 吃解析好的
     *                 bitfield_decoded_result_t，不再摸 plan
     *   row_bits    ：行内位宽（= bits_len - sum(level_bits)）；
     *                 blk = base + (raw>>row_bits) << (row_bits-4)
     *   budget_bytes：保留作统计；淘汰已改为 TTL 超时，不再做压力水位
     *   ttl_ticks   ：指向 mgr 的 ttl_ticks_（单一真相源，外部可运行期调整），
     *                 reap 扫描时读最新值（原子 load） */
    struct slice_radix_tree_cfg_t{
        const Inode_cache_cfg_t *host_cfg;
        bitfield_decode_plan_t   plan;
        uint8_t  row_bits;
        const std::atomic<uint64_t> *ttl_ticks;
    };

    class slice_radix_tree{
        /* ── cfg 快照（构造后只读） ── */
        const Inode_cache_cfg_t *host_cfg_ = nullptr;  /* 共享 mgr cfg：I/O/alloc/free/几何/预算 */
        bitfield_decode_plan_t   plan_ = {};           /* 树拓扑真相源（ctor 用，保留备析构/调试） */
        uint8_t  row_bits_        = 4;                 /* 行内位宽：blk = base + (raw>>row_bits_)<<(row_bits_-4) */
        uint8_t  tree_levels_     = 0;                 /* 树层数 = plan_.total_levels - 1（level_bits[0] 是 mgr 层） */
        const std::atomic<uint64_t> *ttl_ticks_ = nullptr; /* 指向 mgr ttl_ticks_（共享真相源，reap 时原子读） */
        /* 统计记账：
         * table_size_all_accumulate：根表→中间表→叶子表 全部表结构内存字节（元数据域）。
         *                             建表在建链写锁内进行 → 写锁内累加，普通成员即可。
         * real_cache_accumulate   ：已驻留缓存行字节 = 行数 × (4096 << (row_bits_-4))（数据域）。
         *                             页加载/淘汰发生在片读锁内（I/O 不持写锁）→ 原子累加，
         *                             尽力而为统计，relaxed。
         * 用途：统计性能、数据 vs 元数据占比，为后续时空权衡调优提供依据
         * （元数据域过大时可缩小表项宽度 —— 换来额外 load/store，届时权衡） */
        uint64_t table_size_all_accumulate = 0;
        std::atomic<uint64_t> real_cache_accumulate{0};
        /* ── 并发与结构 ── */
        std::shared_mutex lock_;                       /* 可睡眠读写锁：读共享/写独占，持锁可等 I/O（不烧核）
                                                          * 读锁持有跨越 get→put 整个会话（get 手动 lock_shared，
                                                          * put 手动 unlock_shared；会话内建链写锁被阻塞 →
                                                          * 树只增不删，会话内 mark_dirty 可无锁走路径） */
        void *root_ = nullptr;                         /* 根表：tree_levels_==1 → 叶表（ctrl_t 数组）；
                                                          * 否则指针槽数组（槽 = 子表指针，nullptr = 未建） */
    public:
        explicit slice_radix_tree(const slice_radix_tree_cfg_t &cfg);
        ~slice_radix_tree();                           /* 全部缓存：脏页写回 + 非脏丢弃 + 释放全部表 */
        /* get：吃解析好的位域结果（树不解码）。先读锁假定一次命中；miss → 写锁打通链路
         * （建中间表/叶表，无 I/O）后回头读锁。读锁刻意泄露到 put 释放 */
        raven_Inode_t *get(bitfield_decoded_result_t *decoded);
        /* put：释放 get 泄露的读锁 + 对应缓存行 accessor-1（行不被淘汰由 accessor>0 保证） */
        void put(bitfield_decoded_result_t *decoded);
        /* mark_dirty：会话内调用（调用方已持本片读锁）——无锁走路径置 DIRTY。
         * 安全前提：树只增不删 + 建链写锁被会话读锁阻塞 */
        void mark_dirty(bitfield_decoded_result_t *decoded);
        /* flush_all：读锁内 DFS 全量脏页写回（挂载/卸载同步点；宿主应保证与 reap 不并发） */
        void flush_all();
        /* reap_expired：专用淘汰线程经 mgr::reap_expired 调用 —— 读锁内 DFS 回收本片
         * 超时（now - accss_stamp > ttl）且无访问者的行：脏写回 → 释放 → 回 idle（不摘表）。
         * 为何不摘表：摘表需写锁，而 get 持读锁忙等 evicting → 写锁等读锁释放 → 死锁。 */
        void reap_expired();

        /* 禁拷贝/移动：shared_mutex 不可搬移，树持锁语义唯一 */
        slice_radix_tree(const slice_radix_tree &) = delete;
        slice_radix_tree &operator=(const slice_radix_tree &) = delete;
        slice_radix_tree(slice_radix_tree &&) = delete;
        slice_radix_tree &operator=(slice_radix_tree &&) = delete;
    private:
        void *alloc_leaf();                            /* 叶表：ctrl_t 数组全 idle（写锁内） */
        void *alloc_internal(uint8_t level);           /* 中间表：指针槽数组清零（写锁内） */
        void ensure_path(bitfield_decoded_result_t *d);/* 写锁内：沿 decoded 路径补建缺失表 */
        Inode_cache_ctrl_t *locate(bitfield_decoded_result_t *d); /* 读路径：无锁/读锁内皆可 */
        void dfs_flush(void *table, uint8_t level);
        void dfs_reap(void *table, uint8_t level, uint64_t now, uint64_t ttl);
        void dfs_destroy(void *table, uint8_t level);
    };
    slice_radix_tree **slices_ = nullptr;   /* [2^level_bits[0]] 分片树（placement new，析构逐个释放） */
    uint64_t n_slices_ = 0;                 /* = 2^level_bits[0]（RADIX 分支 ctor 派生） */

    /* ── 缓存行级公共原语（CTRL_ARRAY 与 RADIX 共用同一状态机） ──
     * ctrl_acquire ：懒加载状态机入口。返回 working 且 accessor 已 +1 的页基址；
     *                I/O/alloc 失败回 idle 并返回 nullptr。out_loaded 置位 = 本调用
     *                完成页加载（统计用）。忙等 io_ing/evicting（罕见路径）。
     * ctrl_reap_one：单行 TTL 淘汰尝试。仅 working && accessor==0 && 超时的行可被
     *                CAS 接管（原子前提）；成功后写回脏页 → 释放 → 回 idle。
     *                返回 true = 已淘汰（调用方记账）。 */
    static raven_Inode_t *ctrl_acquire(Inode_cache_ctrl_t *c, uint64_t blk, uint64_t page_bytes,
                                       const Inode_cache_cfg_t &cfg, bool *out_loaded = nullptr);
    static bool ctrl_reap_one(Inode_cache_ctrl_t *c, const Inode_cache_cfg_t &cfg,
                              uint64_t now, uint64_t ttl);
    /* ── 几何换算（三模式共用） ── */
    uint64_t ctrl_count() const { return (cfg_.inodes_count + INODES_PER_PAGE - 1) >> ROW_BITS; }
    uint64_t line_of(uint64_t inum) const { return inum >> ROW_BITS; }
    uint64_t row_of(uint64_t inum) const { return inum & (INODES_PER_PAGE - 1); }
    uint64_t blk_of(uint64_t line) const { return cfg_.inode_array_base + line; }

    result_code_t ctor_full_ram();
    result_code_t ctor_ctrl_array();
    void flush_full_ram();
    void flush_ctrl_array();

    /* RADIX：按 bits 查 statey 策略表（inode_cache.cpp 内实现，19~52 档） */
    static uint8_t *statey_levels_of(uint8_t bits, uint8_t *out_levels);
};

/* ═══════════════════════════════════════════════════════════════════
 * InodeSession — RAII 会话（接口统一，决策见 209433a）
 * 锁只维护状态原子性，绝不持锁做 I/O；accessor > 0 保证会话
 * 期间行不被淘汰（RADIX 模式下语义，FULL_RAM/CTRL_ARRAY 天然安全）
 * ═══════════════════════════════════════════════════════════════════ */
class InodeSession {
    Inode_cache_mgr_t *mgr_ = nullptr;
    uint64_t inum_ = 0;
    Inode_cache_mgr_t::slice_cache_acces_token_t token{};
public:
    raven_Inode_t *inode = nullptr;

    InodeSession(Inode_cache_mgr_t *mgr, uint64_t inum)
        : mgr_(mgr), inum_(inum) {
        inode = mgr_ ? mgr_->get(inum, &token) : nullptr;
    }
    ~InodeSession() { if (inode) { mgr_->put(inum_, token); inode = nullptr; } }
    explicit operator bool() const { return inode != nullptr; }
    void dirty() { if (inode) mgr_->mark_dirty(inum_); }

    /* move-only，禁拷贝；token 随会话搬移（RADIX 读锁归属跟随会话） */
    InodeSession(InodeSession &&o) noexcept
        : mgr_(o.mgr_), inum_(o.inum_), token(o.token), inode(o.inode) {
        o.inode = nullptr;
    }
    InodeSession &operator=(InodeSession &&o) noexcept {
        if (this != &o) {
            if (inode) mgr_->put(inum_, token);
            mgr_ = o.mgr_; inum_ = o.inum_; token = o.token; inode = o.inode;
            o.inode = nullptr;
        }
        return *this;
    }
    InodeSession(const InodeSession &) = delete;
    InodeSession &operator=(const InodeSession &) = delete;
};
