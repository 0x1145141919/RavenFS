#pragma once
#include "disk_struct_v1.1.hpp"
class Inode_cache_mgr_t;
class RavenFS_juvenile;
enum cache_ctrl_state_t{
    idle,
    io_ing,
    working
};
struct Inode_cache_ctrl_t{
    raven_Inode_t*cache_data;
    uint64_t accss_stamp;
    uint32_t numof_accessor;
    uint16_t flags;
    uint8_t state;//需要以cas状态切换的
};
/* flags 位域（草稿）：state 枚举已接管 V/T 语义（有效/传输中），flags 留修饰位 */
static constexpr uint16_t CTRL_FLAG_DIRTY = 1u << 0;   /* 页被修改，需回写 */
struct cut_result_t{
    uint64_t main;
    uint64_t cut_out_section;
};
extern cut_result_t cut_high(uint64_t raw_metarial,uint8_t bits_to_cut);//从 raw_metarial的最高非0有效位开始向下切bits_to_cut个bit,切下来的放入cut_out_section，原主干放入main
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
     * FULL_RAM 判定已在 bits 层完成；预算主要约束 RADIX 分片树的
     * 可驻留页数（准入/淘汰水位），CTRL_ARRAY 数据面理论上全量 */
    uint64_t memory_budget;

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

    /* ── 统一会话接口（三模式表面一致） ── */
    raven_Inode_t *get(uint64_t inum);  /* 返回行内原位指针；FULL_RAM 恒命中 */ //很明显，get的时候有必要拿走slice_cache_acces_token_t这个
    void           put(uint64_t inum);  /* accessor 计数 -1；FULL_RAM 无计数，no-op */ //slice_cache_acces_token_t也得作为参数，缓存占用减一，相应分片的锁也要解开读锁
    void           mark_dirty(uint64_t inum);
    result_code_t  flush_all();         /* 全量脏页回写（挂载/卸载同步点用） */
    struct slice_cache_acces_token_t
    {
        uint16_t slice_idx;
    };
    bool is_ready() const { return ready_; }
    inode_cache_mode_t mode() const { return mode_; }
    uint8_t bits_len() const { return cfg_.bits_len_of_inodes_count; }

private:
    static constexpr uint8_t  ROW_BITS = 4;            /* 行内引索 4bit = 16 inode/页 */ //要不得，我的juvenile_god/inode_cache.cpp里面设计了有些情况是5bit的，当然全量缓存控制器数组是每个缓存行4k,16entries
    static constexpr uint64_t INODES_PER_PAGE = 16;

    Inode_cache_cfg_t  cfg_;
    inode_cache_mode_t mode_;
    bool ready_ = false;

    /* ── FULL_RAM 分支 [1,12] ── */
    raven_Inode_t *full_ram_array_ = nullptr;   /* [inodes_count] 全量常驻 */
    uint8_t       *full_ram_dirty_ = nullptr;   /* 脏页位图 [(pages+7)/8] */  //实际上不需要，可以考虑libnvme的cmp技术，大不了一个page一个page地比较逐个写入，若是怕过度写造成不必要的写入量，但是既然都小到这个全量上内存那就直接全部一口气写完

    /* ── CTRL_ARRAY 分支 [13,18] ── */
    Inode_cache_ctrl_t *ctrl_array_ = nullptr;  /* [ctrl_count]，位置即身份：line = inum>>4 */
    raven_Inode_t      *data_pages_ = nullptr;  /* [ctrl_count][16]，按需载入 */  //反对，linux内核和用户空间就直接state在io_ing的时候不但<读取+分配虚拟内存>地原子性加载即可，且逐出时也是<写回+释放>原子性更新，所以这个成员没必要

    /* ── RADIX 分支 [19,52] ── 草稿：策略表已布置（inode_cache.cpp 内），
     *    global_slice_plan 解析 + 分片树/片 RW 锁/准入淘汰待实装 */
    struct bits_slice_plan_t{//词不达意，我想表达的是根据bits_len_of_inodes_count的后如何规划位域对应到对应层级的表的引索，这种意思        
        /**
         * total_levels表示这个bits_len_of_inodes_count从最高位开始向下每个多少层，也就是
         * per_level_width数组从0~total_levels-1的条目是使用的
         * 而解析位域则是使用对应的cut_high函数，从原始位域递归从0迭代向上切，每切出来一份放入解析成员，最后剩下的就是缓存行内引索
         */
        /**
         * 不过先硬性定一手，per_level_width[0]是Inode_cache_mgr_t层的引索，后面的从1开始的层级是分片基数树从上到下的位域宽度
         */
        uint8_t total_levels;
        uint8_t*per_level_width;
    };
    bits_slice_plan_t global_slice_plan;
    class slice_radix_tree{
        uint64_t decode_base;//负责解码的Inode_base_idx
        uint64_t decode_limit;
        spinrwlock_cpp_t lock;//实际上应该是互斥读写锁
        void* root_table;
        public:
        slice_radix_tree();//显然，构造函数需要分享Inode_cache_cfg_t指针以及额外的decode_base，decode_limit以及最重要的global_slice_plan信息
        ~slice_radix_tree();//全部缓存，D的写回，非D的丢弃
        raven_Inode_t *get(uint64_t inum);//先读锁假定会一次命中，中间有任何问题就是用写锁RAII把链路打通，而后回头用读锁。写锁用RAII刻意限制临界区，但是读锁刻意泄露
        void put(uint64_t inum);//这个地方会释放读锁以及对应缓存行访客数目-1,前面泄露的读锁在这里释放
        //刻意泄露读锁，而自动释放放到InodeSession的RAII层
    };
    /* ── 几何换算（三模式共用） ── */
    uint64_t ctrl_count() const { return (cfg_.inodes_count + INODES_PER_PAGE - 1) >> ROW_BITS; }
    uint64_t line_of(uint64_t inum) const { return inum >> ROW_BITS; }
    uint64_t row_of(uint64_t inum) const { return inum & (INODES_PER_PAGE - 1); }
    uint64_t blk_of(uint64_t line) const { return cfg_.inode_array_base + line; }

    result_code_t ctor_full_ram();
    result_code_t ctor_ctrl_array();
    void flush_full_ram();
    void flush_ctrl_array();
};

/* ═══════════════════════════════════════════════════════════════════
 * InodeSession — RAII 会话（接口统一，决策见 209433a）
 * 锁只维护状态原子性，绝不持锁做 I/O；numof_accessor > 0 保证会话
 * 期间行不被淘汰（RADIX 模式下语义，FULL_RAM/CTRL_ARRAY 天然安全）
 * ═══════════════════════════════════════════════════════════════════ */
class InodeSession {
    Inode_cache_mgr_t *mgr_ = nullptr;
    uint64_t inum_ = 0;
    Inode_cache_mgr_t::slice_cache_acces_token_t token;
public:
    raven_Inode_t *inode = nullptr;

    InodeSession(Inode_cache_mgr_t *mgr, uint64_t inum)
        : mgr_(mgr), inum_(inum) { inode = mgr_ ? mgr_->get(inum) : nullptr; }
    ~InodeSession() { if (inode) { mgr_->put(inum_); inode = nullptr; } }
    explicit operator bool() const { return inode != nullptr; }
    void dirty() { if (inode) mgr_->mark_dirty(inum_); }

    /* move-only，禁拷贝 */
    InodeSession(InodeSession &&o) noexcept : mgr_(o.mgr_), inum_(o.inum_), inode(o.inode) {
        o.inode = nullptr;
    }
    InodeSession &operator=(InodeSession &&o) noexcept {
        if (this != &o) {
            if (inode) mgr_->put(inum_);
            mgr_ = o.mgr_; inum_ = o.inum_; inode = o.inode;
            o.inode = nullptr;
        }
        return *this;
    }
    InodeSession(const InodeSession &) = delete;
    InodeSession &operator=(const InodeSession &) = delete;
};
