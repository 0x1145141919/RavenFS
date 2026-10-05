#pragma once
#include "rc.hpp"                 /* result_code_t / RC_* / PTR_INVALID（轻量头） */
#include "cache_mgr_mode.hpp"     /* cache_work_mode_t（mdata_piece 后端枚举） */
#include "fs_allocator.hpp"       /* fs_allocator_t / inode_location_t：inum→物理翻译 */
#include "mdata_piece_caches.hpp" /* mdata_piece_session：8K 负载片缓存（内嵌会话） */
#include <utility>                /* std::move */

/* ═══════════════════════════════════════════════════════════════════
 * inode_cache_radix.hpp — inode 会话（v2.0）
 *
 * v2.0 改造（2026-10-06）：
 *   · **彻底删除** Inode_cache_mgr_t / Inode_cache_cfg_t_v2 / 三后端里的
 *     FULL_RAM / CTRL_ARRAY / RADIX 自管缓存机械。inode 存储是全局弹性数据面
 *     （8K 片 + 映射树），缓存统一交给 mdata_piece_caches_mgr（8K 片粒度，
 *     键 = 物理 FS_block，三后端 no_cache / offload / self_reliat 复用）。
 *   · InodeSession 变成薄壳：**翻译**（inode_io_t::plane->inode_locate）取物理
 *     8K 片基 → 开一个 mdata_piece_session → inode = &data[(inum & 31)]。
 *   · 引用计数（accessor）由 mdata_piece_session 承担，本层不另计。
 *
 * ★ inode_io_t 会话绑定（宿主组装）：解析器（数据面）+ 8K 片缓存。
 *   生命周期归宿主；InodeSession 只借指针，不持有。
 *
 * ★ 磁盘粒度：v2.0 每 8K 片 = 32 个 256B inode；片内槽 = inum & 31。
 *   片内取址需 raven_Inode_t 完整定义 → inode_slot_of 下沉 inode_cache.cpp。
 * ═══════════════════════════════════════════════════════════════════ */
struct raven_Inode_t;

struct inode_io_t {
    fs_allocator_t         *plane = nullptr;   /* inode_locate：inum → 物理 8K 片基 */
    mdata_piece_caches_mgr *cache = nullptr;   /* 8K 片缓存（inode 与 B+tree 节点共用同一 mgr）*/
};

/* ═══════════════════════════════════════════════════════════════════
 * InodeSession — inode 访问会话（RAII；move-only）
 *
 * 构造：翻译取物理片 → mdata_piece_session 占坑（accessor+1 / offload 直映 /
 *       no_cache 自管缓冲）；inode 指向片内槽；失败保持 inode=nullptr。
 * dirty：片粒度脏标记（no_cache 会话级 / self_reliat 页粒度 / offload 无操作）。
 * 析构：mdata_piece_session 归还（脏写回 + accessor-1）。
 *
 * 允许同 inum 嵌套（业务大量存在）：每次会话各自 resolve + 开片，互不持锁；
 * self_reliat/offload 同片读写互通；no_cache 各自缓冲（仅只读嵌套安全）。
 * ═══════════════════════════════════════════════════════════════════ */
class InodeSession {
    mdata_piece_session piece_;     /* 内嵌 8K 片会话（含 accessor / 缓冲 / 脏标记） */
    uint64_t inum_ = 0;
public:
    raven_Inode_t *inode = nullptr;

    InodeSession(const inode_io_t *io, uint64_t inum)
        : piece_(io ? io->cache : nullptr, piece_blk_of(io, inum)), inum_(inum) {
        if (piece_.data)
            inode = inode_slot_of(piece_.data, inum & 31ULL);
    }
    ~InodeSession() = default;

    explicit operator bool() const { return inode != nullptr; }

    void dirty() { if (inode) piece_.dirty(); }

    InodeSession(InodeSession &&o) noexcept
        : piece_(std::move(o.piece_)), inum_(o.inum_), inode(o.inode) {
        o.inode = nullptr;
    }
    InodeSession &operator=(InodeSession &&o) noexcept {
        if (this != &o) {
            piece_ = std::move(o.piece_);   /* mdata_piece_session 的 move-assign 会先 release 自身 */
            inum_  = o.inum_;
            inode  = o.inode;
            o.inode = nullptr;
        }
        return *this;
    }
    InodeSession(const InodeSession &) = delete;
    InodeSession &operator=(const InodeSession &) = delete;
private:
    /* inum → 物理 8K 片基（偶）；非法/失败 → ~0（mdata_piece 判 index_error）。impl in cpp。 */
    static uint64_t piece_blk_of(const inode_io_t *io, uint64_t inum);
    /* 8K 片缓冲取第 idx 槽（idx = inum & 31）。impl in cpp（需 raven_Inode_t 完整定义）。 */
    static raven_Inode_t *inode_slot_of(void *piece_data, uint64_t idx);
};
