#pragma once
#include "disk_struct_v1.1.hpp"
#include <atomic>
#include <shared_mutex>   /* slice_radix_tree::lock — 可睡眠读写锁（允许持锁等 I/O，不烧核） */
#include "util/tsc.h"    /* x86_64 Invariant TSC：TTL 换算（绑死 x86_64） */
#include "cache_mgr_mode.hpp"
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
    uint64_t page_blk_len;              /* 本行有效 FS_block 数（加载者写定 = cache_data 实际字节/4096）。
                                           非末行 = 2^(row_bits-4)（满行）；末行按有效 inode 收窄 ≥1。
                                           写回（reap/flush/destroy）与统计一律读它，不重算。 */
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
 * ── 2026-08-29 增量：v2 配置唯一化 + 三后端自适应（老 cfg 全面淘汰） ──
 *
 * Inode_cache_cfg_t_v2：宿主侧唯一配置面（v1.1 时代的 Inode_cache_cfg_t
 * 已整体删除，内部不再有 v1.1 适配层）。设计：
 *   - mode 显式指定后端（no_cache / self_reliat / offload），不再由 bits
 *     隐式推导；self_reliat 内部才按 bits_len 推导 FULL_RAM/CTRL_ARRAY/RADIX
 *   - 回调 read_line/write_line（行 = 磁盘粒度 16 inode/块，blk_len 单位
 *     FS_block）；分片树/行原语消费 v2 派生的 inode_cache_rt_t 快照
 *   - 无 alloc/free 字段：self_reliat 内部统一走 libc 分配器（host 用户态），
 *     移植（sparrowos/linux 内核）时改 inode_cache.cpp 顶部的两个适配器即可
 *   - per-mode 特化：self_reliat 的 TTL（裸秒，0 = 默认 2s）/ offload 的
 *     mmap canvas 基址（inode array 起始 FS_block 的内核页缓存切片）
 *
 * 会话侧（InodeSession）按 backed_mode 三路分派：no_cache 自管缓冲直读 /
 * self_reliat 占坑 token（accessor）/ offload 内核页缓存直映（无 token）。
 * 旧 get/put/mark_dirty/set_ttl_ticks/reap_expired/mode 已随老 cfg 一并删除。
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

struct Inode_cache_cfg_t_v2{
    cache_work_mode_t mode;
    result_code_t (*read_line)(void *io_ctx, uint64_t blk,uint64_t blk_len, void *buf);
    result_code_t (*write_line)(void *io_ctx, uint64_t blk,uint64_t blk_len, const void *buf);
    void *io_ctx;                       /* 宿主上下文（通常传 fs this） */
    uint64_t Inode_count;//mkfs保证16对齐，这个也应该落到disk_layout里面
    uint64_t Inode_base_FS_block_idx;
    struct {
        uint64_t ttl_bare_seconds = 0;//构造时分填0则会使用默认值2s
    }self_reliat_specific;
    struct {
        uint64_t virtual_canvas_base;//很明显是FS_block引索为Inode_base_FS_block_idx的地方开始的从mmap切出来的地址切片
    }offload_specific;
};

/* ── 内部运行时快照（inode_cache.cpp 消费；宿主无需关心） ──
 * 老 Inode_cache_cfg_t 淘汰后，分片树/行原语直接吃这份 v2 派生快照：
 * v2 的 I/O 回调 + io_ctx + 几何，分配器固定 libc（host_alloc/host_free，
 * 见 cpp 顶部适配器）。slice_radix_tree 每片持一份值拷贝（40B，无共享
 * 指针/悬垂），mgr 自身持一份 rt_ 供 CTRL_ARRAY 分支与原语调用。 */
struct inode_cache_rt_t {
    result_code_t (*read_line)(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
    result_code_t (*write_line)(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);
    void *io_ctx;
    uint64_t inodes_count;      /* = cfg.Inode_count（mkfs 16 对齐） */
    uint64_t inode_array_base;  /* = cfg.Inode_base_FS_block_idx */
};
class Inode_cache_mgr_t{
public:
    const cache_work_mode_t backed_mode;//session第一件事情是获得这个
    void* const io_ctx;
    /* 回调成员不用 const 修饰：const 放在返回类型前会把函数指针变成
     * 「指向 const 返回函数」的不兼容类型（与普通回调互不转换）；
     * 成员由 ctor 一次性赋值，运行期只读。 */
    result_code_t (*read_line)(void *io_ctx, uint64_t blk,uint64_t blk_len, void *buf);
    result_code_t (*write_line)(void *io_ctx, uint64_t blk,uint64_t blk_len, const void *buf);
    /* 构造：v2 大结构体整体传入（不再逐参 / 摸宿主）。
     * 后端模式 = cfg.mode（单真相源）：
     *   no_cache      —— 无内部结构，session 自管缓冲直读盘
     *   self_reliat   —— 用户态缓存，内部按 bits 再推导 FULL_RAM/CTRL_ARRAY/RADIX
     *   offload_cache —— 内核页缓存直映（mmap canvas），仅记几何 + canvas 地址
     * rc_out 非空时收构造结果（RC_OK / RC_ERR_IO / RC_ERR_NO_BLOCK / RC_ERR_STATE） */
    Inode_cache_mgr_t(const Inode_cache_cfg_t_v2 &cfg, result_code_t *rc_out = nullptr);
    ~Inode_cache_mgr_t();//每个片进行折构（脏页回写 + 释放）

    /* ── 统一会话接口（三模式表面一致） ──
     * get 输出会话 token（RADIX 定位分片/释放读锁用），put 必须带回该 token；
     * out_token 可空（调用方不关心时），FULL_RAM/CTRL_ARRAY 填 slice_idx=0 */
    struct slice_cache_acces_token_t
    {
        uint16_t slice_idx;   /* RADIX：分片序号（per_level_width[0] 高位切分）；其余模式恒 0 */
    };
    /* flush_all — 同步点（挂载/卸载用），不淘汰接口；内部按后端模式自适应：
     *   no_cache      ：无缓存可同步，丢弃（RC_OK）
     *   self_reliat   ：用户态丢弃非脏页 + 脏页写回（写回后驻留，卸载由 dtor 统一释放）
     *   offload_cache ：相关 syscall（msync 整片 canvas 回写内核页缓存） */
    result_code_t  flush_all();
    bool is_ready() const { return ready_; }
    uint8_t bits_len() const { return bits_len_; }
    /* no_cache 直读用：inum 所在 FS_block = inode array 基址 + (inum>>4)（磁盘粒度 16 inode/块） */
    uint64_t inode_array_base() const { return inode_array_base_; }

    /* ── self_reliat 会话接口（FULL_RAM/CTRL_ARRAY/RADIX 三内部模式表面一致） ──
     * get 输出会话 token（RADIX 定位分片/释放读锁用），put 必须带回该 token；
     * out_token 可空（调用方不关心时），FULL_RAM/CTRL_ARRAY 填 slice_idx=0。 */
    raven_Inode_t *self_reliat_get(uint64_t inum, slice_cache_acces_token_t *out_token = nullptr);
    void           self_reliat_put(uint64_t inum, const slice_cache_acces_token_t &token);
    void           self_reliat_mark_dirty(uint64_t inum);
    /* TTL 淘汰（绑死 x86_64 + Invariant TSC）：
     * self_reliat_set_ttl_ticks：外部调整缓存行 TTL（默认 2s，ctor 按 TSC 频率换算），
     *                            调整即时生效（slice 持指针共享同一真相源），0 = 禁用
     * self_reliat_reap_expired：专用淘汰线程周期调用 —— 回收全部「超时 && 无访问者」
     *                            的行（RADIX 逐片扫 + CTRL_ARRAY 全扫；写回脏页 + 释放） */
    void           self_reliat_set_ttl_ticks(uint64_t ticks);
    void           self_reliat_reap_expired();
    inode_cache_mode_t self_reliat_mode();

    /* ── offload 会话接口（内核页缓存直映，无 token/accessor） ── */
    raven_Inode_t *offload_get(uint64_t inum);
    void           offload_put(uint64_t inum);
private:
    static constexpr uint8_t  ROW_BITS = 4;   /* 仅 FULL_RAM/CTRL_ARRAY 可用：行 = 4K 页 = 16 entries（磁盘粒度 16 inode/块，恒 4bit）。
                                                 RADIX 行宽由 plan 派生（bits=25/33 档 5bit/32 entries=2 块），禁止用此常量（见 global_slice_plan 注记） */
    static constexpr uint64_t INODES_PER_PAGE = 16;

    /* 几何/回调直存成员（老 cfg 适配层已拆，v2 ctor 一次性写定，运行期只读） */
    uint64_t inodes_count_     = 0;   /* = cfg.Inode_count（mkfs 16 对齐） */
    uint64_t inode_array_base_ = 0;   /* = cfg.Inode_base_FS_block_idx */
    uint8_t  bits_len_         = 0;   /* ceil(log2(inodes_count_))，模式判定唯一真相源 */
    inode_cache_rt_t rt_ = {};        /* v2 派生快照（self_reliat 消费；RADIX 分片树持值拷贝） */
    inode_cache_mode_t mode_ = CACHE_MODE_FULL_RAM;   /* 占位默认；self_reliat ctor 按 bits 覆写 */
    bool ready_ = false;
    uint64_t virtual_canvas_base_ = 0;   /* offload 后端：inode array 起始 FS_block 的 mmap 切片地址 */
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
     * 不复制宿主、不反向持有 mgr：树只依赖 rt 快照（值拷贝），可脱离 mgr 单测
     *   rt          ：v2 派生快照（I/O 回调 + io_ctx + 几何），每片一份值拷贝
     *   plan        ：位域解码规划。仅 ctor 推树拓扑用（tree_levels = total_levels-1，
     *                 根表宽 = 2^level_bits[1]）；运行期 get/put 吃解析好的
     *                 bitfield_decoded_result_t，不再摸 plan
     *   row_bits    ：行内位宽（= bits_len - sum(level_bits)）；
     *                 blk = base + (raw>>row_bits) << (row_bits-4)
     *   ttl_ticks   ：指向 mgr 的 ttl_ticks_（单一真相源，外部可运行期调整），
     *                 reap 扫描时读最新值（原子 load） */
    struct slice_radix_tree_cfg_t{
        inode_cache_rt_t         rt;
        bitfield_decode_plan_t   plan;
        uint8_t  row_bits;
        const std::atomic<uint64_t> *ttl_ticks;
    };

    class slice_radix_tree{
        /* ── rt 快照 + 拓扑（构造后只读） ── */
        inode_cache_rt_t   rt_ = {};               /* v2 派生快照值拷贝：I/O 回调 + io_ctx + 几何 */
        bitfield_decode_plan_t plan_ = {};           /* 树拓扑真相源（ctor 用，保留备析构/调试） */
        uint8_t  row_bits_;                 /* 行内位宽：blk = base + (raw>>row_bits_)<<(row_bits_-4) */
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
                                                          * 读锁持有跨越 get→put 整个会话：get 内 std::shared_lock
                                                          * 持有，成功路径 release() 交所有权（非泄露路径析构收口），
                                                          * put 手动 unlock_shared 释放；会话内建链写锁被阻塞 →
                                                          * 树只增不删，会话内 mark_dirty 可无锁走路径 */
        void *root_ = nullptr;                         /* 根表：tree_levels_==1 → 叶表（ctrl_t 数组）；
                                                          * 否则指针槽数组（槽 = 子表指针，nullptr = 未建） */
    public:
        explicit slice_radix_tree(const slice_radix_tree_cfg_t &cfg);
        ~slice_radix_tree();                           /* 全部缓存：脏页写回 + 非脏丢弃 + 释放全部表 */
        /* get：吃解析好的位域结果（树不解码）。先读锁假定一次命中；miss → 写锁打通链路
         * （建中间表/叶表，无 I/O）后回头读锁。RAII：非泄露路径 guard 析构收口；
         * 成功返回前 release() 交出读锁所有权 —— 刻意泄露到 put 释放 */
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
        uint64_t row_blk_len(uint64_t line) const;     /* 行有效 FS_block 数（末行按有效 inode 收窄 ≥1） */
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
    static raven_Inode_t *ctrl_acquire(Inode_cache_ctrl_t *c, uint64_t blk, uint64_t blk_len,
                                       const inode_cache_rt_t &rt, bool *out_loaded = nullptr);
    static bool ctrl_reap_one(Inode_cache_ctrl_t *c, const inode_cache_rt_t &rt,
                              uint64_t now, uint64_t ttl);
    /* ── 几何换算（三模式共用） ── */
    uint64_t ctrl_count() const { return (inodes_count_ + INODES_PER_PAGE - 1) >> ROW_BITS; }
    uint64_t line_of(uint64_t inum) const { return inum >> ROW_BITS; }
    uint64_t row_of(uint64_t inum) const { return inum & (INODES_PER_PAGE - 1); }
    uint64_t blk_of(uint64_t line) const { return inode_array_base_ + line; }

    result_code_t ctor_full_ram();
    result_code_t ctor_ctrl_array();
    /* self_reliat 内部装配（几何/回调成员已由 v2 ctor 写定）：组装 rt_ + TTL 换算 + 模式分支 */
    result_code_t ctor_self_reliat(uint64_t ttl_ticks_cfg);
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
    cache_work_mode_t mgr_backend_mode = cache_work_mode_t::self_reliat_cache;
    void*   nocache_buff = nullptr;   /* no_cache：session 自管一块（4096B = 16 inode），构造分配、析构释放 */
    bool    nocache_dirty = false;    /* no_cache：session 级脏标记，析构时脏则 write_line 写回，不脏直接释放 */
public:
    raven_Inode_t *inode = nullptr;

    InodeSession(Inode_cache_mgr_t *mgr, uint64_t inum)
        : mgr_(mgr), inum_(inum) {
        if (!mgr_) return;   /* 防御：无 mgr 不做事（inode 保持 nullptr） */
        mgr_backend_mode = mgr_->backed_mode;
        switch (mgr_backend_mode) {
        case cache_work_mode_t::no_cache: {
            /* no_cache：session 自管缓冲 + read_line 直读盘（不经过缓存）。
             * 磁盘天然粒度 16 inode/块（mkfs 16 对齐）→ blk_len 恒 1。
             * 读失败 → 释放缓冲，inode 保持 nullptr（上层按失败处理）。 */
            nocache_buff = new uint8_t[4096];   /* 移植时按各自分配轮子量体裁衣（O_DIRECT 需 4K 对齐） */
            if (!nocache_buff) return;
            const uint64_t blk = mgr_->inode_array_base() + (inum_ >> 4);
            if (mgr_->read_line(mgr_->io_ctx, blk, 1, nocache_buff) != RC_OK) {
                delete[] (uint8_t*)nocache_buff;
                nocache_buff = nullptr;
                return;
            }
            inode = &((raven_Inode_t*)nocache_buff)[inum_ & 15];
            break;
        }
        case cache_work_mode_t::self_reliat_cache:
            inode = mgr_->self_reliat_get(inum, &token);
            break;
        case cache_work_mode_t::offload_cache:
            inode = mgr_->offload_get(inum);   /* 无 token：内核页缓存后端，无分片锁/accessor */
            break;
        }
    }
    ~InodeSession() { release(); }
    explicit operator bool() const { return inode != nullptr; }
    void dirty() {
        if (!inode) return;
        switch (mgr_backend_mode) {
        case cache_work_mode_t::no_cache:
            nocache_dirty = true;   /* 记 session 级脏：析构时写回 */
            break;
        case cache_work_mode_t::self_reliat_cache:
            mgr_->self_reliat_mark_dirty(inum_);
            break;
        case cache_work_mode_t::offload_cache:
            break;   /* 内核页缓存管一致性，无需脏标记 */
        }
    }

    /* move-only，禁拷贝；token 随会话搬移（RADIX 读锁归属跟随会话）；
     * no_cache 的缓冲与脏标记随搬，源对象置空。 */
    InodeSession(InodeSession &&o) noexcept
        : mgr_(o.mgr_), inum_(o.inum_), token(o.token),
          mgr_backend_mode(o.mgr_backend_mode), nocache_buff(o.nocache_buff),
          nocache_dirty(o.nocache_dirty), inode(o.inode) {
        o.nocache_buff = nullptr;
        o.inode = nullptr;
    }
    InodeSession &operator=(InodeSession &&o) noexcept {
        if (this != &o) {
            release();   /* 先释放自身持有的后端资源 */
            mgr_ = o.mgr_; inum_ = o.inum_; token = o.token;
            mgr_backend_mode = o.mgr_backend_mode;
            nocache_buff = o.nocache_buff; nocache_dirty = o.nocache_dirty;
            inode = o.inode;
            o.nocache_buff = nullptr; o.inode = nullptr;
        }
        return *this;
    }
    InodeSession(const InodeSession &) = delete;
    InodeSession &operator=(const InodeSession &) = delete;
private:
    /* 按后端模式释放会话持有：no_cache 脏写回+释放缓冲 / self_reliat_put /
     * offload_put。析构与 move 赋值共用，三模式语义只此一份。 */
    void release() {
        if (!mgr_) {
            if (nocache_buff) { delete[] (uint8_t*)nocache_buff; nocache_buff = nullptr; }
            inode = nullptr;
            return;
        }
        switch (mgr_backend_mode) {
        case cache_work_mode_t::no_cache:
            if (inode && nocache_dirty)
                mgr_->write_line(mgr_->io_ctx, mgr_->inode_array_base() + (inum_ >> 4), 1, nocache_buff);
            delete[] (uint8_t*)nocache_buff;
            nocache_buff = nullptr;
            break;
        case cache_work_mode_t::self_reliat_cache:
            if (inode) mgr_->self_reliat_put(inum_, token);
            break;
        case cache_work_mode_t::offload_cache:
            if (inode) mgr_->offload_put(inum_);
            break;
        }
        inode = nullptr;
    }
};
