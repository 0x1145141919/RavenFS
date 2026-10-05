#pragma once
#include "rc.hpp"   /* result_code_t + RC_* 全局共享结果码（轻量头，零盘结构依赖） */
#include "cache_mgr_mode.hpp"     /* cache_work_mode_t：三后端枚举（no_cache / self_reliat / offload） */
#include "cache_wheel.hpp"        /* 共用轮子：ctrl_word 位域常量 + cache_ctrl_state_t + bitfield 分片解码 */
#include "util/tsc.h"             /* x86_64 Invariant TSC：self_reliat TTL 换算（原经 inode_cache_radix.hpp 传递，2026-10-05 显式化） */

#include <atomic>
#include <shared_mutex>   /* slice_radix_tree::lock_ — 可睡眠读写锁（允许持锁等 I/O，不烧核） */
#include <cstdint>
#include <cstdlib>   /* aligned_alloc/free：no_cache 会话缓冲（8K 对齐，O_DIRECT 可直读） */
#if defined(__linux__) || defined(__unix__)
#include <sys/mman.h>   /* msync：offload 后端 flush_all 的内核页缓存同步 syscall */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * mdata_piece_caches.hpp — 元数据负载片缓存（mdata_piece_caches_mgr）
 *
 * 设计对齐 inode_cache_radix.hpp（2026-08-29 v2 迁移同一范式）：
 *   - v2 配置唯一化：mdata_piece_caches_cfg_t 为宿主侧唯一配置面
 *   - 三后端枚举复用 cache_mgr_mode.hpp，会话侧（mdata_piece_session）
 *     按 backed_mode 三路分派，接口面与 InodeSession 对齐
 *   - no_cache + offload_cache 全内联（不建 .cpp）；self_reliat 实装在
 *     juvenile_god/mdata_piece_cache.cpp（2026-08-30 落地：双档）
 *
 * ── 专用负载形状（本缓存唯一的服务对象） ──
 * mdata piece = 元数据负载片：基址偶数 + 长度 2 个连续 FS_block（8192B）。
 *   - 来源：B+tree 节点（Bptree_*_node_t）恒 8192B = 2 连续 FS_block
 *     （disk_struct_v1.1.hpp static_assert(sizeof==8192)），分配一律
 *     hard_alloc(2, 1)（2 块连续 + 2^1 对齐 → 起始块号恒偶数）
 *   - 本模块不做通用块缓存：只缓存「基址偶数、长度 2」的精确负载，
 *     形状不符的 blkidx 一律判引索错误（见 mdata_piece_session::index_error）
 *
 * ── self_reliat 双档（2026-08-30 决策，行 = 1 负载恒 8K） ──
 *   按 bits_len（= ceil(log2(Piece_count))）分档：
 *     bits ≤ 13  → CACHE_MODE_CTRL_ARRAY  全量控制器数组：负载页懒加载不淘汰
 *     bits > 13  → CACHE_MODE_RADIX_SHARD 多级静态基数树分片（statey 表 14~52）
 *   ⚠ 与 inode 缓存的关键差异：本模块缓存行恒 = 1 个负载（8192B，blk_len 恒 2），
 *     没有 inode 版「行 = 2^row_bits 单元 + 末行 row_blk_len 收窄」的复杂度 ——
 *     session 构造时 blkidx_valid 已阻断尾部不足（保证 blkidx+2 在覆盖区间内），
 *     self_reliat 加载时直接 read_block(blkidx, 2) 即可。
 *     （row_bits=0：statey 表每档 sum(level_bits)=bits，无行内索引位）
 *
 * 模式语义（对应 ravenfs_v1.1.hpp ctrl_words_bits 旧掩码意图）：
 *   - no_cache      ：session 自管 8192B 缓冲 + read_block 直读盘（硬读，
 *                     旧 BLOCK_CACHE_ENABLE_MASK=0 且无 mmap 后端），
 *                     析构时脏则 write_block 写回，不脏直接释放
 *   - offload_cache ：内核页缓存直映 —— 整盘 mmap 窗口切出的元数据区切片
 *                     （旧 MMAP_BACKEND_CONTROL_MASK=1 时强制使能位翻 1），
 *                     数据不落用户态，一致性由内核页缓存管；session 无
 *                     缓冲无脏标记，flush_all 走 msync
 *   - self_reliat   ：手动用户态缓存（本文件声明 + mdata_piece_cache.cpp 实现）
 *
 * 几何（负载为单位）：缓存覆盖 Piece_count 个负载，首负载起始 FS_block
 * = Piece_base_FS_block_idx（必须偶数）；FS_block 覆盖区间
 * [base, base + count×2)。宿主通常传 base=0（数据区 = FS_block[0,
 * ia_bm_base)，元数据节点也在其内，block_model.md §3.2）。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * self_reliat 内部档位（对齐 inode_cache_mode_t 的全局枚举风格；仅
 * self_reliat 后端有意义，其余后端返回占位值勿依赖）
 *   MDP_MODE_CTRL_ARRAY   bits ≤ 13 —— 全量控制器数组：负载页懒加载不淘汰
 *   MDP_MODE_RADIX_SHARD  bits > 13 —— statey 表（14~52）+ 多级静态基数树分片
 * ═══════════════════════════════════════════════════════════════════ */
enum mdata_piece_mode_t : uint8_t {
    MDP_MODE_CTRL_ARRAY   = 0,   /* bits ≤ 13：全量控制器数组 */
    MDP_MODE_RADIX_SHARD  = 1,   /* bits > 13：多级静态基数树分片 */
};

/* ── v2 配置：宿主侧唯一配置面（老 Block_cache_cfg_t 若存在即整体淘汰，
 *    本结构为唯一直通面；per-mode 特化与 Inode_cache_cfg_t_v2 同构） ── */
struct mdata_piece_caches_cfg_t{
    cache_work_mode_t mode;                          /* 后端显式指定：no_cache / offload_cache / self_reliat_cache */
    result_code_t (*read_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
    result_code_t (*write_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);
    void *io_ctx;                                    /* 宿主上下文（通常传 fs this；回调内自行换算设备偏移） */
    uint64_t Piece_count;                            /* 负载片数（≥1；每片 = 2 连续 FS_block = 8192B） */
    uint64_t Piece_base_FS_block_idx;                /* 首负载起始 FS_block（必须偶数，对应 hard_alloc(2,1) 对齐） */
    struct {
        uint64_t virtual_canvas_base;                /* offload：Piece_base 对应 FS_block 的 mmap 窗口切片地址 */
    } offload_specific;
    struct {
        uint64_t ttl_bare_seconds = 0;               /* self_reliat：TTL 裸秒（0 = 默认 2s），对齐 inode 版 self_reliat_specific */
    } self_reliat_specific;
};

/* ═══════════════════════════════════════════════════════════════════
 * mdata_piece_caches_mgr — 元数据负载片缓存管理器
 *
 * 构造按 cfg.mode 分派（单真相源）：
 *   - no_cache      ：仅记几何（session 自管缓冲直读盘）
 *   - offload_cache ：仅记 canvas 地址 + 几何（数据不落用户态）
 *   - self_reliat   ：未实装 —— ctor 置 RC_ERR_STATE + ready_=false
 *                     （接口面已留，见下；实装前宿主不应传该模式）
 * 退化配置（Piece_count==0 / Piece_base 非偶数）→ RC_ERR_STATE。
 *
 * flush_all — 同步点（挂载/卸载）：
 *   no_cache 丢弃（无缓存可同步）/ offload msync 整片 canvas /
 *   self_reliat 返回 RC_ERR_STATE（不可达）。
 * ═══════════════════════════════════════════════════════════════════ */
class mdata_piece_caches_mgr{
public:
    const cache_work_mode_t backed_mode;             /* session 第一件事情是获得这个 */
    void* const io_ctx;
    /* 回调成员不用 const 修饰：const 放在返回类型前会把函数指针变成
     * 「指向 const 返回函数」的不兼容类型（与普通回调互不转换）；
     * 成员由 ctor 一次性赋值，运行期只读。blk_len 恒 2（负载 = 2 连续块）。 */
    result_code_t (*read_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
    result_code_t (*write_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);

    /* 构造：v2 大结构体整体传入（不再逐参）。rc_out 非空时收构造结果
     * （RC_OK / RC_ERR_STATE：退化配置等）。self_reliat 分支实装见
     * juvenile_god/mdata_piece_cache.cpp（双档：CTRL_ARRAY ≤13 / RADIX >13）。 */
    mdata_piece_caches_mgr(const mdata_piece_caches_cfg_t &cfg, result_code_t *rc_out = nullptr)
        : backed_mode(cfg.mode), io_ctx(cfg.io_ctx),
          read_block(cfg.read_block), write_block(cfg.write_block) {
        result_code_t rc = RC_OK;
        switch (backed_mode) {
        case cache_work_mode_t::no_cache:
        case cache_work_mode_t::offload_cache:
            /* 两模式均为「仅记几何」：数据不在用户态缓存（no_cache 由 session
             * 自管缓冲直读；offload 由内核页缓存管）。 */
            piece_base_  = cfg.Piece_base_FS_block_idx;
            piece_count_ = cfg.Piece_count;
            if (backed_mode == cache_work_mode_t::offload_cache)
                virtual_canvas_base_ = cfg.offload_specific.virtual_canvas_base;
            if (!piece_count_ || (piece_base_ & 1)) rc = RC_ERR_STATE;   /* 防御：退化配置直接拒（基址必须偶数） */
            break;
        case cache_work_mode_t::self_reliat_cache: {
            /* TTL：裸秒 → tick（0 = 用默认 2s，交给 ctor_self_reliat 换算） */
            uint64_t ttl = 0;
            if (cfg.self_reliat_specific.ttl_bare_seconds) {
                tsc_info_t tsc;
                if (tsc_probe(&tsc))
                    ttl = tsc_ticks_of_seconds(cfg.self_reliat_specific.ttl_bare_seconds, tsc);
            }
            piece_base_  = cfg.Piece_base_FS_block_idx;
            piece_count_ = cfg.Piece_count;
            rc = ctor_self_reliat(ttl);   /* 内部按 bits 推导双档（见 cpp） */
            break;
        }
        }
        ready_ = (rc == RC_OK);
        if (rc_out) *rc_out = rc;
    }

    /* 析构：no_cache/offload 无内部结构；self_reliat 脏页写回 + 释放（见 cpp） */
    ~mdata_piece_caches_mgr() { teardown_self_reliat(); }

    /* flush_all — 同步点（挂载/卸载）：
     *   no_cache      ：无缓存可同步，丢弃（RC_OK）
     *   offload_cache ：msync 整片 canvas（内核页缓存同步 syscall）
     *   self_reliat   ：脏页写回（驻留）+ 非脏页丢弃（双档统一，见 cpp） */
    result_code_t flush_all() {
        switch (backed_mode) {
        case cache_work_mode_t::no_cache:
            return RC_OK;   /* 无缓存可同步 */
        case cache_work_mode_t::offload_cache: {
#if defined(__linux__) || defined(__unix__)
            const uint64_t bytes = piece_count_ * 8192;   /* 每片 2×FS_block（4096B，block_model.md §3） */
            if (msync((void *)virtual_canvas_base_, bytes, MS_SYNC) != 0) return RC_ERR_IO;
#else
            /* 非 POSIX 端口：宿主自管同步（预留） */
#endif
            return RC_OK;
        }
        case cache_work_mode_t::self_reliat_cache:
            flush_self_reliat();
            return RC_OK;
        }
        return RC_OK;   /* 不可达 */
    }

    bool is_ready() const { return ready_; }
    /* no_cache 直读用：缓存覆盖几何（负载为单位；FS_block 覆盖 [base, base+count×2)） */
    uint64_t piece_base()  const { return piece_base_; }
    uint64_t piece_count() const { return piece_count_; }
    /* 引索合法性：blkidx 必须偶数（2 对齐，hard_alloc(2,1) 契约）且落在覆盖区间内。
     * session 构造用；后续任何以 blkidx 为参的接口都以此为先决条件。 */
    bool blkidx_valid(uint64_t blkidx) const {
        return (blkidx & 1) == 0 &&
               blkidx >= piece_base_ &&
               blkidx < piece_base_ + piece_count_ * 2;
    }

    /* ── offload 会话接口（内核页缓存直映，无缓冲/无脏标记/无 token） ──
     * offload_get：指针直算 canvas + ((blkidx-base)>>1)×8192（负载序数 × 8K）；
     * blkidx 非偶 / 区间外返回 nullptr。put = 无操作（一致性由内核管，
     * 脏页由 flush_all 的 msync 落盘）。 */
    uint8_t *offload_get(uint64_t blkidx) {
        if (backed_mode != cache_work_mode_t::offload_cache) return nullptr;
        if (!blkidx_valid(blkidx)) return nullptr;   /* 防御：引索不合法（非偶/越界） */
        return reinterpret_cast<uint8_t *>(virtual_canvas_base_)
               + ((blkidx - piece_base_) >> 1) * 8192;   /* 负载序数 × 8K */
    }
    void offload_put(uint64_t blkidx) {
        (void)blkidx;   /* 内核页缓存管一致性，无会话资源可释放 */
    }

    /* ── self_reliat 会话接口（实装见 mdata_piece_cache.cpp） ──
     * get/put 无需 token：session 携带 blkidx，put 时重新 decode 定位分片
     * （与 inode 版 token 等价，接口更简）；mark_dirty 页粒度脏标记（I/O 恒整页回写）。
     * 加载前提：调用方（session）已用 blkidx_valid 校验 —— 负载 2 块恒完整，
     * 直接 read_block(blkidx, 2)，无 inode 版末行收窄逻辑。 */
    uint8_t *self_reliat_get(uint64_t blkidx);
    void     self_reliat_put(uint64_t blkidx);
    void     self_reliat_mark_dirty(uint64_t blkidx);
    /* TTL 淘汰（绑死 x86_64 + Invariant TSC）：
     * set_ttl_ticks 外部调整（默认 2s，ctor 按 TSC 频率换算），0 = 禁用；
     * reap_expired 专用淘汰线程周期调用 —— 回收「超时 && 无访问者」的负载行
     * （CTRL_ARRAY 全扫 / RADIX 逐片扫；脏写回 + 释放，不摘表）。 */
    void     self_reliat_set_ttl_ticks(uint64_t ticks);
    void     self_reliat_reap_expired();
    mdata_piece_mode_t self_reliat_mode() const;   /* 仅 self_reliat 后端有意义 */

private:
    /* 几何（v2 ctor 一次性写定，运行期只读；负载为单位） */
    uint64_t piece_count_ = 0;   /* 负载片数（= cfg.Piece_count） */
    uint64_t piece_base_  = 0;   /* 首负载起始 FS_block（= cfg.Piece_base_FS_block_idx，恒偶数） */
    bool ready_ = false;
    uint64_t virtual_canvas_base_ = 0;   /* offload 后端：Piece_base 对应 FS_block 的 mmap 切片地址 */

    /* ═══════════════════════════════════════════════════════════════
     * self_reliat 内部（双档，见 mdata_piece_cache.cpp）：
     *   MDP_MODE_CTRL_ARRAY   bits ≤ 13 —— 控制器全量常驻，负载页懒加载不淘汰
     *   MDP_MODE_RADIX_SHARD  bits > 13 —— statey 表（14~52）+ 多级静态基数树分片
     * 行 = 1 负载恒 8192B：blk 即 blkidx（偶），blk_len 恒 2，无末行收窄。
     * ═══════════════════════════════════════════════════════════════ */
    /* 缓存行控制器（行 = 1 负载）：ctrl_word 位域常量/状态机与 inode 版共用
     * （CTRL_STATE_MASK / CTRL_ACCESSOR_INC / CTRL_FLAG_DIRTY / idle..evicting）。
     * 无 page_blk_len：负载恒 2 块，写回/统计恒 8192B，不需要收窄记录。 */
    struct mdata_piece_ctrl_t{
        uint8_t *cache_data;          /* 8K 负载基址；working 发布/观察（ctrl_word release/acquire） */
        std::atomic<uint64_t> accss_stamp;  /* 最近访问 TSC 戳（尽力而为，relaxed 读写） */
        std::atomic<uint64_t> ctrl_word;    /* state/flags/accessor 单原子字（位域见 inode_cache_radix.hpp） */
        uint64_t blk;                       /* 负载起始块号（偶数；加载者 CAS io_ing 成功后写定，reap/flush 读） */
    };
    /* v2 派生快照（self_reliat 消费；RADIX 分片树持值拷贝，无共享指针/悬垂） */
    struct mdata_piece_rt_t{
        result_code_t (*read_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
        result_code_t (*write_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);
        void *io_ctx;
        uint64_t pieces_count;   /* = cfg.Piece_count */
        uint64_t pieces_base;    /* = cfg.Piece_base_FS_block_idx（恒偶数） */
    };

    uint8_t  bits_len_ = 0;   /* ceil(log2(piece_count_))，self_reliat 档位判定唯一真相源 */
    mdata_piece_mode_t mode_ = MDP_MODE_CTRL_ARRAY;   /* self_reliat ctor 按 bits 覆写 */
    mdata_piece_rt_t rt_ = {};                        /* v2 派生快照（self_reliat 消费） */
    std::atomic<uint64_t> ttl_ticks_ = 0;             /* 缓存行 TTL（TSC tick）：默认 2s（ctor 换算），
                                                         set_ttl_ticks 调整；slice 持 &ttl_ticks_ 共享 */

    /* ── CTRL_ARRAY 分支（bits ≤ 13） ── 控制器全量常驻，数据页懒分配：
     *    get 的 io_ing 态内 alloc+read 原子完成，析构 write-back+free */
    mdata_piece_ctrl_t *ctrl_array_ = nullptr;   /* [piece_count_]，位置即身份：line = piece_idx = (blkidx-base)>>1 */

    /* ── RADIX 分支（bits > 13） ── 分片树（statey 表 14~52，row_bits 恒 0）：
     *    decode(piece_idx) → chunks[0] 选片，树内逐层 → 叶表 ctrl_t → cache_data。
     *    行 = 1 负载：line = piece_idx，blk = base + piece_idx*2 = blkidx 本身。 */
    struct slice_radix_tree_cfg_t{
        mdata_piece_rt_t         rt;
        bitfield_decode_plan_t   plan;
        const std::atomic<uint64_t> *ttl_ticks;   /* 指向 mgr 的 ttl_ticks_（单一真相源） */
    };
    class slice_radix_tree{
        /* ── rt 快照 + 拓扑（构造后只读） ── */
        mdata_piece_rt_t   rt_ = {};
        bitfield_decode_plan_t plan_ = {};           /* 树拓扑真相源（ctor 用，保留备析构/调试） */
        uint8_t  tree_levels_ = 0;                   /* 树层数 = plan_.total_levels - 1（level_bits[0] 是 mgr 层） */
        const std::atomic<uint64_t> *ttl_ticks_ = nullptr;
        uint64_t table_size_all_accumulate = 0;      /* 全部表结构内存字节（元数据域，写锁内累加） */
        std::atomic<uint64_t> real_cache_accumulate{0};  /* 已驻留缓存行字节 = 行数 × 8192（原子累加） */
        std::shared_mutex lock_;                     /* 可睡眠读写锁；读锁仅在 get 内建链窗口持有（不跨会话），
                                                         * 嵌套 session 无自死锁（2026-08-30 修复；树只增不删，
                                                         * 会话内 locate/mark_dirty 无锁安全，页由 accessor 保护） */
        void *root_ = nullptr;                       /* 根表：tree_levels_==1 → 叶表（ctrl_t 数组）；否则指针槽数组 */
    public:
        explicit slice_radix_tree(const slice_radix_tree_cfg_t &cfg);
        ~slice_radix_tree();                         /* 全部缓存：脏页写回 + 非脏丢弃 + 释放全部表 */
        /* get：吃解析好的位域结果（树不解码）。读锁假定命中 → miss 写锁建链（无 I/O）
         * 回头读锁；返回前读锁随 guard 析构释放（不跨会话——树只增不删，会话内
         * locate 无锁安全；嵌套 session 内层 miss 建链不再自死锁）。
         * 行 = 1 负载：blk = pieces_base + (raw<<1)（raw = piece_idx），blk_len 恒 2。 */
        uint8_t *get(bitfield_decoded_result_t *decoded);
        void put(bitfield_decoded_result_t *decoded);   /* accessor-1（无读锁可释放，get 已释） */
        void mark_dirty(bitfield_decoded_result_t *decoded);
        void flush_all();
        void reap_expired();
        slice_radix_tree(const slice_radix_tree &) = delete;
        slice_radix_tree &operator=(const slice_radix_tree &) = delete;
        slice_radix_tree(slice_radix_tree &&) = delete;
        slice_radix_tree &operator=(slice_radix_tree &&) = delete;
    private:
        void *alloc_leaf();
        void *alloc_internal(uint8_t level);
        void ensure_path(bitfield_decoded_result_t *d);
        mdata_piece_ctrl_t *locate(bitfield_decoded_result_t *d);
        void dfs_flush(void *table, uint8_t level);
        void dfs_reap(void *table, uint8_t level, uint64_t now, uint64_t ttl);
        void dfs_destroy(void *table, uint8_t level);
    };
    slice_radix_tree **slices_ = nullptr;   /* [2^level_bits[0]] 分片树（placement new，析构逐个释放） */
    uint64_t n_slices_ = 0;                 /* = 2^level_bits[0]（RADIX 分支 ctor 派生） */
    bitfield_decode_plan_t global_slice_plan;   /* RADIX 位域规划（statey 表解析，见 cpp） */

    /* ── self_reliat 装配/收口 ── */
    result_code_t ctor_self_reliat(uint64_t ttl_ticks_cfg);   /* 几何/回调成员已由 v2 ctor 写定 */
    result_code_t ctor_ctrl_array();                          /* 控制器全量建好（状态全 idle），数据页懒加载 */
    void flush_self_reliat();                                 /* 脏写回驻留 + 非脏丢弃（双档统一） */
    void teardown_self_reliat();                              /* 析构：脏页写回 + 释放全部（非 self_reliat 后端空操作） */
    static uint8_t *statey_levels_of(uint8_t bits, uint8_t *out_levels);   /* RADIX：按 bits 查策略表（14~52） */

    /* ── 缓存行级公共原语（CTRL_ARRAY 与 RADIX 共用同一状态机） ──
     * mdp_ctrl_acquire ：懒加载状态机入口。返回 working 且 accessor 已 +1 的 8K 页基址；
     *   blk_len 恒 2（负载 = 2 连续块，blkidx_valid 已保证完整）→ 无末行收窄。
     * mdp_ctrl_reap_one：单行 TTL 淘汰尝试（working && accessor==0 && 超时）。
     * mdp_ctrl_flush_one：flush_all 单行语义（脏写回驻留 / 非脏丢弃）。
     * mdp_alloc/mdp_free：8K 对齐分配适配器（host 用户态 libc；移植时改这两处）。 */
    static uint8_t *mdp_ctrl_acquire(mdata_piece_ctrl_t *c, uint64_t blk,
                                     const mdata_piece_rt_t &rt, bool *out_loaded = nullptr);
    static bool     mdp_ctrl_reap_one(mdata_piece_ctrl_t *c, const mdata_piece_rt_t &rt,
                                      uint64_t now, uint64_t ttl);
    static bool     mdp_ctrl_flush_one(mdata_piece_ctrl_t *c, const mdata_piece_rt_t &rt);
    static void    *mdp_alloc(uint64_t size);
    static void     mdp_free(void *ptr);
};

/* ═══════════════════════════════════════════════════════════════════
 * mdata_piece_session — RAII 元数据负载片会话（接口对齐 InodeSession）
 *
 * 引索错误标志位：blkidx 必须至少 2 对齐（偶数）—— 这是本缓存负载形状
 * 的先决条件（基址偶数 + 长度 2，hard_alloc(2,1) 契约）。构造时 blkidx
 * 非偶 / 区间外 → index_error 置位 + data=nullptr，后续任何工作无法展开
 * （调用方必须先查 index_error()）。I/O 失败不置引索错误位（那是盘的问题，
 * 不是引索的问题），仅 data=nullptr。
 *
 * 模式语义：
 *   - no_cache    ：自管 8192B 缓冲（8K 对齐，O_DIRECT 可直读），构造
 *                    read_block(blkidx, 2) 直读盘；dirty() 记会话级脏，
 *                    析构脏则 write_block(blkidx, 2) 写回、不脏直接释放
 *   - offload     ：指针直算 canvas（data = 内核页缓存切片），无缓冲无脏，
 *                    一致性由内核管（flush_all 的 msync 落盘）
 *   - self_reliat ：用户态缓存（双档：CTRL_ARRAY ≤13 / RADIX >13），
 *                   构造懒加载 + accessor 占坑，dirty() 页粒度脏标记，
 *                   析构 self_reliat_put 释放（blkidx 无需 token，put 重解位域）
 *
 * 允许同 blkidx 嵌套（业务大量存在：victim_sess+new_sess、位图面多负载片
 * sess+sess2；2026-08-30 修复旧「读锁泄漏到 put」导致的内层建链自死锁）：
 *   · 分片读锁只在 slice::get 内（返回即释，不跨会话）；
 *   · 会话期只持 accessor 计数（+1/-1 可重入），put/mark_dirty 无锁走
 *     「只增不删」的表（本会话路径已由 get 物化，locate 必命中）。
 * 嵌套可见性按后端：
 *   · self_reliat（CTRL_ARRAY/RADIX）/ offload：同一 8K 页，读写互通；
 *   · no_cache：每会话自管独立 8K 缓冲，同 blkidx 嵌套无一致性（各自 dirty
 *     会后写覆盖），仅只读嵌套安全。
 * ═══════════════════════════════════════════════════════════════════ */
class mdata_piece_session {
    mdata_piece_caches_mgr *mgr_ = nullptr;
    uint64_t blkidx_ = 0;                               /* 会话目标负载起始 FS_block（必须偶数） */
    cache_work_mode_t mgr_backend_mode = cache_work_mode_t::no_cache;
    uint8_t *nocache_buff = nullptr;                    /* no_cache：自管 8192B（aligned_alloc 8K 对齐） */
    bool nocache_dirty = false;                         /* no_cache：会话级脏标记，析构时脏则 write_block 写回 */
    bool index_error_ = false;                          /* 引索错误标志位：blkidx 非 2 对齐 / 区间外 → 置位 */
public:
    uint8_t *data = nullptr;                            /* 指向负载数据（8192B）；失败保持 nullptr（上层按失败处理） */

    mdata_piece_session(mdata_piece_caches_mgr *mgr, uint64_t blkidx)
        : mgr_(mgr), blkidx_(blkidx) {
        if (!mgr_) return;   /* 防御：无 mgr 不做事（data 保持 nullptr） */
        mgr_backend_mode = mgr_->backed_mode;
        if (!mgr_->blkidx_valid(blkidx_)) {
            index_error_ = true;   /* 引索错误：非偶 / 区间外，后续工作无法展开（不碰盘） */
            return;
        }
        switch (mgr_backend_mode) {
        case cache_work_mode_t::no_cache: {
            /* no_cache：session 自管缓冲 + read_block 直读盘（硬读，不经过缓存）。
             * 负载恒 2 FS_block（8192B）→ blk_len 恒 2。
             * 读失败 → 释放缓冲，data 保持 nullptr（I/O 错误，非引索错误）。 */
            nocache_buff = (uint8_t *)std::aligned_alloc(8192, 8192);   /* 8K 对齐，O_DIRECT 可直读 */
            if (!nocache_buff) return;
            if (mgr_->read_block(mgr_->io_ctx, blkidx_, 2, nocache_buff) != RC_OK) {
                std::free(nocache_buff);
                nocache_buff = nullptr;
                return;
            }
            data = nocache_buff;
            break;
        }
        case cache_work_mode_t::offload_cache:
            data = mgr_->offload_get(blkidx_);   /* 指针直算，无缓冲无 token */
            break;
        case cache_work_mode_t::self_reliat_cache:
            data = mgr_->self_reliat_get(blkidx_);   /* 用户态缓存：懒加载 + 占坑（accessor） */
            break;
        }
    }
    ~mdata_piece_session() { release(); }
    explicit operator bool() const { return data != nullptr; }

    /* 引索错误标志位查询：true = 传入 blkidx 不合法（非 2 对齐 / 区间外），
     * 本会话 data 恒 nullptr，调用方应据此走错误路径（不要尝试读 data）。 */
    bool index_error() const { return index_error_; }

    void dirty() {
        if (!data) return;
        switch (mgr_backend_mode) {
        case cache_work_mode_t::no_cache:
            nocache_dirty = true;   /* 记会话级脏：析构时写回 */
            break;
        case cache_work_mode_t::offload_cache:
            break;   /* 内核页缓存管一致性，无需脏标记 */
        case cache_work_mode_t::self_reliat_cache:
            mgr_->self_reliat_mark_dirty(blkidx_);   /* 页粒度脏标记（整页回写） */
            break;
        }
    }

    /* move-only，禁拷贝：no_cache 的缓冲与脏标记随搬，源对象置空；
     * offload 无状态，data 指针随搬即可；index_error_ 随搬。 */
    mdata_piece_session(mdata_piece_session &&o) noexcept
        : mgr_(o.mgr_), blkidx_(o.blkidx_), mgr_backend_mode(o.mgr_backend_mode),
          nocache_buff(o.nocache_buff), nocache_dirty(o.nocache_dirty),
          index_error_(o.index_error_), data(o.data) {
        o.nocache_buff = nullptr;
        o.data = nullptr;
    }
    mdata_piece_session &operator=(mdata_piece_session &&o) noexcept {
        if (this != &o) {
            release();   /* 先释放自身持有的资源 */
            mgr_ = o.mgr_; blkidx_ = o.blkidx_; mgr_backend_mode = o.mgr_backend_mode;
            nocache_buff = o.nocache_buff; nocache_dirty = o.nocache_dirty;
            index_error_ = o.index_error_;
            data = o.data;
            o.nocache_buff = nullptr; o.data = nullptr;
        }
        return *this;
    }
    mdata_piece_session(const mdata_piece_session &) = delete;
    mdata_piece_session &operator=(const mdata_piece_session &) = delete;

private:
    /* 按后端模式释放会话持有：no_cache 脏写回+释放缓冲 / offload 无操作 /
     * self_reliat 无操作。析构与 move 赋值共用，三模式语义只此一份。 */
    void release() {
        if (!mgr_) {
            if (nocache_buff) { std::free(nocache_buff); nocache_buff = nullptr; }
            data = nullptr;
            return;
        }
        switch (mgr_backend_mode) {
        case cache_work_mode_t::no_cache:
            if (data && nocache_dirty)
                mgr_->write_block(mgr_->io_ctx, blkidx_, 2, nocache_buff);   /* 负载 2 块整写回 */
            std::free(nocache_buff);
            nocache_buff = nullptr;
            break;
        case cache_work_mode_t::offload_cache:
            break;   /* 内核页缓存管一致性，无需释放 */
        case cache_work_mode_t::self_reliat_cache:
            if (data) mgr_->self_reliat_put(blkidx_);   /* accessor-1（无锁；读锁已随 get 释放） */
            break;
        }
        data = nullptr;
    }
};
