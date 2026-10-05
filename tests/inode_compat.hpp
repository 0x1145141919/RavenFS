#pragma once
/* ═══════════════════════════════════════════════════════════════════
 * inode_compat.hpp — 测试侧 inode 读写兼容壳（v2.0）
 *
 * v2.0 上帝类**废除** inode_read/inode_write：inode 访问统一走 InodeSession
 * （数据面 inode_locate + mdata_piece 8K 片缓存）。本头给测试提供等价的小 helper，
 * 免去逐处展开会话样板。
 *
 * 依赖：调用方以 `#define private public` 引入 ravenfs_v1.1.hpp（使 fs.inode_io /
 * fs.mdata_piece_mgr 可达）。包含顺序：ravenfs 头之后 include 本头即可。
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"
#include <cstring>

/* 读一个 inode 到 out（失败置零）。 */
static inline void juv_inode_read(RavenFS_juvenile &fs, uint64_t inum, raven_Inode_t *out) {
    InodeSession s(fs.inode_io, inum);
    if (s) *out = *s.inode;
    else   std::memset(out, 0, sizeof(*out));
}

/* 写一个 inode（脏标记；回写由 mdata_piece 层管）。 */
static inline void juv_inode_write(RavenFS_juvenile &fs, uint64_t inum, const raven_Inode_t *in) {
    InodeSession s(fs.inode_io, inum);
    if (s) { *s.inode = *in; s.dirty(); }
}

/* 同步点：把 mdata_piece 8K 片缓存的脏页刷回（测试校验前调用）。 */
static inline void juv_flush(RavenFS_juvenile &fs) {
    if (fs.mdata_piece_mgr) fs.mdata_piece_mgr->flush_all();
}
