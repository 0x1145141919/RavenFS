#pragma once

#include "disk_struct.h"
#include "io_backend.h"

#include <cstdint>
#include <cstring>

/* 文件句柄类型 */
using FileID = uint32_t;
static constexpr FileID INVALID_FILE_ID = ~0u;

/* 访问者（权限上下文） */
struct Visitor {
    uint32_t uid = 0;
    uint32_t gid = 0;
};

/* ═══════════════════════════════════════════════════════
 * RavenFS — 上帝类
 *
 * 开发阶段所有功能内聚于此，不拆分任何层。
 * 验证全部算法后再面向各端拆分 I/O 后端。
 *
 * 用法：
 *   RavenFS fs;
 *   fs.mount(io_backend);
 *   fs.mkfs();           // 格式化
 *   fs.create_file(...);
 *   FileID f = fs.open("/path");
 *   fs.read(f, buf, sz, off);
 *   fs.write(f, buf, sz, off);
 *   fs.close(f);
 * ═══════════════════════════════════════════════════════ */
class RavenFS {
public:
    RavenFS() = default;
    ~RavenFS();

    /* ─── 生命周期 ─── */

    /* 挂载已有文件系统 */
    bool mount(RavenIOBackend* io);

    /* 格式化分区（会调用 mount） */
    bool mkfs(RavenIOBackend* io, uint64_t inode_count = 65536);

    /*卸载并释放所有内部缓存*/
    void umount();

    /* ─── 文件操作（mini VFS） ─── */

    bool create_file(Visitor who, const char* path);
    bool mkdir(Visitor who, const char* path);
    bool unlink(Visitor who, const char* path);
    bool rmdir(Visitor who, const char* path);

    FileID open(Visitor who, const char* path);
    bool close(FileID id);

    bool read(FileID id, void* buf, uint64_t size, uint64_t offset);
    bool write(FileID id, const void* buf, uint64_t size, uint64_t offset);

    bool truncate(FileID id, uint64_t new_size);

    /* ─── 目录操作 ─── */

    struct DirEntry {
        uint64_t inode;
        char     name[256];
        uint8_t  file_type;  // 0=file, 1=dir
    };

    /* 枚举目录全部项，entries 填到 count 个为止。返回实际项数，-1 失败 */
    int readdir(const char* path, DirEntry* entries, int max_count);

    /* ─── 内部（暴露给测试用） ─── */
    RavenIOBackend* io() { return io_; }
    const RavenSuperblock* sb() const { return sb_; }

    /* 读取/写入物理块 */
    bool read_block(uint64_t block, void* buf);
    bool write_block(uint64_t block, const void* buf);

    /* B+tree 操作（测试直用） */
    bool btree_load_node(uint64_t block, Bptree_node_t* node);
    bool btree_store_node(uint64_t block, const Bptree_node_t* node);
    bool btree_lookup(uint64_t root_block, uint64_t target_fblk, uint64_t* pblk);

    /* Inode 操作（测试直用） */
    bool inode_load(uint64_t inum, raven_Inode_t* inode);
    bool inode_store(uint64_t inum, const raven_Inode_t* inode);
    bool inode_alloc(uint64_t* inum);

    /* Bitmap 操作（测试直用） */
    bool bitmap_test(uint64_t block);
    bool bitmap_set(uint64_t block, bool allocated);
    bool bitmap_alloc(uint64_t* block);

private:
    RavenIOBackend*  io_ = nullptr;
    RavenSuperblock* sb_ = nullptr;    // 缓存的 superblock
    uint8_t*         cache_ = nullptr; // 私有 buffer cache
    void*            opened_files_;    // Opaque 句柄表
    uint64_t         total_blocks_ = 0;
    bool             mounted_ = false;

    /* 私有 buffer cache 接口（实现细节） */
    uint8_t* cache_fetch(uint64_t block);
    void cache_mark_dirty(uint64_t block);
    void cache_flush_block(uint64_t block);
};
