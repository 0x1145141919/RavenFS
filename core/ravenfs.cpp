#include "ravenfs/ravenfs.h"

#include <cstdlib>
#include <cstring>

RavenFS::~RavenFS() {
    umount();
}

bool RavenFS::mount(RavenIOBackend* io) {
    io_ = io;
    total_blocks_ = io->total_blocks();

    /* 读 superblock */
    sb_ = reinterpret_cast<RavenSuperblock*>(std::aligned_alloc(4096, 4096));
    if (!sb_) return false;
    if (!io_->read_blocks(sb_, 0, 1)) {
        std::free(sb_); sb_ = nullptr;
        return false;
    }

    if (sb_->magic != 0x5241564E) {
        /* 未格式化 */
        std::free(sb_); sb_ = nullptr;
        return false;
    }

    mounted_ = true;
    return true;
}

bool RavenFS::mkfs(RavenIOBackend* io, uint64_t inode_count) {
    /* TODO: 格式化 */
    (void)io; (void)inode_count;
    return false;
}

void RavenFS::umount() {
    if (sb_) { std::free(sb_); sb_ = nullptr; }
    // 刷回所有脏块
    io_ = nullptr;
    mounted_ = false;
}

/* ─── 块 I/O ─── */

bool RavenFS::read_block(uint64_t block, void* buf) {
    return io_->read_blocks(buf, block, 1);
}

bool RavenFS::write_block(uint64_t block, const void* buf) {
    return io_->write_blocks(buf, block, 1);
}

/* ─── 文件操作桩 ─── */

bool RavenFS::create_file(Visitor who, const char* path) {
    (void)who; (void)path; return false;
}

bool RavenFS::mkdir(Visitor who, const char* path) {
    (void)who; (void)path; return false;
}

bool RavenFS::unlink(Visitor who, const char* path) {
    (void)who; (void)path; return false;
}

bool RavenFS::rmdir(Visitor who, const char* path) {
    (void)who; (void)path; return false;
}

FileID RavenFS::open(Visitor who, const char* path) {
    (void)who; (void)path; return INVALID_FILE_ID;
}

bool RavenFS::close(FileID id) {
    (void)id; return false;
}

bool RavenFS::read(FileID id, void* buf, uint64_t size, uint64_t offset) {
    (void)id; (void)buf; (void)size; (void)offset; return false;
}

bool RavenFS::write(FileID id, const void* buf, uint64_t size, uint64_t offset) {
    (void)id; (void)buf; (void)size; (void)offset; return false;
}

bool RavenFS::truncate(FileID id, uint64_t new_size) {
    (void)id; (void)new_size; return false;
}

int RavenFS::readdir(const char* path, DirEntry* entries, int max_count) {
    (void)path; (void)entries; (void)max_count; return -1;
}
