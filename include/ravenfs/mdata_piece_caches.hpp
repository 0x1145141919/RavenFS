#pragma once
#include "disk_struct_v1.1.hpp"   /* result_code_t + RC_* 全局共享结果码 */
#include "cache_mgr_mode.hpp"     /* cache_work_mode_t：三后端枚举（no_cache / self_reliat / offload） */

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
 *   - 本文件先行实装 no_cache + offload_cache（最简两模式，逻辑全内联，
 *     不建 .cpp）；self_reliat（手动用户态缓存）仅保留接口面占位，
 *     ctor 对 self_reliat_cache 直接拒绝（RC_ERR_STATE + ready_=false）
 *
 * ── 专用负载形状（本缓存唯一的服务对象） ──
 * mdata piece = 元数据负载片：基址偶数 + 长度 2 个连续 FS_block（8192B）。
 *   - 来源：B+tree 节点（Bptree_*_node_t）恒 8192B = 2 连续 FS_block
 *     （disk_struct_v1.1.hpp static_assert(sizeof==8192)），分配一律
 *     hard_alloc(2, 1)（2 块连续 + 2^1 对齐 → 起始块号恒偶数）
 *   - 本模块不做通用块缓存：只缓存「基址偶数、长度 2」的精确负载，
 *     形状不符的 blkidx 一律判引索错误（见 mdata_piece_session::index_error）
 *
 * 模式语义（对应 ravenfs_v1.1.hpp ctrl_words_bits 旧掩码意图）：
 *   - no_cache      ：session 自管 8192B 缓冲 + read_block 直读盘（硬读，
 *                     旧 BLOCK_CACHE_ENABLE_MASK=0 且无 mmap 后端），
 *                     析构时脏则 write_block 写回，不脏直接释放
 *   - offload_cache ：内核页缓存直映 —— 整盘 mmap 窗口切出的元数据区切片
 *                     （旧 MMAP_BACKEND_CONTROL_MASK=1 时强制使能位翻 1），
 *                     数据不落用户态，一致性由内核页缓存管；session 无
 *                     缓冲无脏标记，flush_all 走 msync
 *   - self_reliat   ：手动用户态缓存（后续按 inode RADIX 分片范式补）
 *
 * 几何（负载为单位）：缓存覆盖 Piece_count 个负载，首负载起始 FS_block
 * = Piece_base_FS_block_idx（必须偶数）；FS_block 覆盖区间
 * [base, base + count×2)。宿主通常传 base=0（数据区 = FS_block[0,
 * ia_bm_base)，元数据节点也在其内，block_model.md §3.2）。
 * ═══════════════════════════════════════════════════════════════════ */

/* ── v2 配置：宿主侧唯一配置面（老 Block_cache_cfg_t 若存在即整体淘汰，
 *    本结构为唯一直通面；per-mode 特化与 Inode_cache_cfg_t_v2 同构） ── */
struct mdata_piece_caches_cfg_t{
    cache_work_mode_t mode;                          /* 后端显式指定：no_cache / offload_cache（self_reliat 未实装，ctor 拒绝） */
    result_code_t (*read_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
    result_code_t (*write_block)(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);
    void *io_ctx;                                    /* 宿主上下文（通常传 fs this；回调内自行换算设备偏移） */
    uint64_t Piece_count;                            /* 负载片数（≥1；每片 = 2 连续 FS_block = 8192B） */
    uint64_t Piece_base_FS_block_idx;                /* 首负载起始 FS_block（必须偶数，对应 hard_alloc(2,1) 对齐） */
    struct {
        uint64_t virtual_canvas_base;                /* offload：Piece_base 对应 FS_block 的 mmap 窗口切片地址 */
    } offload_specific;
    struct {
        /* 预留：self_reliat 后续补（TTL tick 等，对齐 inode 版 self_reliat_specific） */
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
     * （RC_OK / RC_ERR_STATE：self_reliat 未实装、退化配置等）。 */
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
        case cache_work_mode_t::self_reliat_cache:
            rc = RC_ERR_STATE;   /* 未实装：仅 no_cache / offload_cache 可用（见文件头） */
            break;
        }
        ready_ = (rc == RC_OK);
        if (rc_out) *rc_out = rc;
    }

    /* 析构：两模式均无内部结构（no_cache 缓冲在 session、offload 数据在内核
     * 页缓存），无需收口；self_reliat 未实装。 */
    ~mdata_piece_caches_mgr() = default;

    /* flush_all — 同步点（挂载/卸载）：
     *   no_cache      ：无缓存可同步，丢弃（RC_OK）
     *   offload_cache ：msync 整片 canvas（内核页缓存同步 syscall）
     *   self_reliat   ：未实装（宿主不会走到） */
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
            return RC_ERR_STATE;   /* 未实装，不可达 */
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

    /* ── self_reliat 会话接口（占位：未实装，ctor 已拒绝该模式，不可达） ──
     * 对齐 inode 版接口面（get/put/mark_dirty/reap/set_ttl 等），实装后
     * 由会话三路分派接入；实装前这些函数恒返回空/空操作，勿调用。 */
    uint8_t *self_reliat_get(uint64_t blkidx) {
        (void)blkidx;   /* 未实装 */
        return nullptr;
    }
    void self_reliat_put(uint64_t blkidx) {
        (void)blkidx;   /* 未实装 */
    }

private:
    /* 几何（v2 ctor 一次性写定，运行期只读；负载为单位） */
    uint64_t piece_count_ = 0;   /* 负载片数（= cfg.Piece_count） */
    uint64_t piece_base_  = 0;   /* 首负载起始 FS_block（= cfg.Piece_base_FS_block_idx，恒偶数） */
    bool ready_ = false;
    uint64_t virtual_canvas_base_ = 0;   /* offload 后端：Piece_base 对应 FS_block 的 mmap 切片地址 */
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
 *   - self_reliat ：未实装（mgr 未 ready，防御走 data=nullptr）
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
            break;   /* 未实装：mgr ctor 已拒绝该模式，防御走 data=nullptr */
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
            break;   /* 未实装 */
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
            break;   /* 未实装 */
        }
        data = nullptr;
    }
};
