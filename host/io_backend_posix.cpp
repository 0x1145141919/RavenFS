#include "ravenfs/io_backend.h"

#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

class PosixIOBackend : public RavenIOBackend {
public:
    PosixIOBackend(const char* path)
        : fd_(::open(path, O_RDWR | O_CREAT, 0644))
    {
        if (fd_ < 0) {
            perror("open");
            return;
        }
        struct stat st;
        if (fstat(fd_, &st) == 0 && st.st_size > 0) {
            total_blocks_ = (uint64_t)st.st_size / BLOCK_SIZE;
        }
    }

    ~PosixIOBackend() override {
        if (fd_ >= 0) ::close(fd_);
    }

    bool read_blocks(void* buf, uint64_t block_start, uint64_t count) override {
        if (fd_ < 0) return false;
        return ::pread(fd_, buf, count * BLOCK_SIZE,
                       (off_t)(block_start * BLOCK_SIZE)) == (ssize_t)(count * BLOCK_SIZE);
    }

    bool write_blocks(const void* buf, uint64_t block_start, uint64_t count) override {
        if (fd_ < 0) return false;
        return ::pwrite(fd_, buf, count * BLOCK_SIZE,
                        (off_t)(block_start * BLOCK_SIZE)) == (ssize_t)(count * BLOCK_SIZE);
    }

    uint64_t total_blocks() const override {
        return total_blocks_;
    }

    void flush() override {
        if (fd_ >= 0) ::fsync(fd_);
    }

    /* 扩展文件到指定块数（mkfs 用） */
    bool resize(uint64_t blocks) {
        if (fd_ < 0) return false;
        off_t sz = (off_t)(blocks * BLOCK_SIZE);
        if (::ftruncate(fd_, sz) < 0) return false;
        total_blocks_ = blocks;
        return true;
    }

private:
    int      fd_ = -1;
    uint64_t total_blocks_ = 0;
};
