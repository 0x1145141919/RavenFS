/* ═══════════════════════════════════════════════════════════════════════════
 * audit_image.cpp — 坐标层实现（open + SB + 五道 fail-fast 硬门 + mmap）
 *
 * 纪律（audit/DISCIPLINE.md）：
 *   · 本文件是审计的「接地层」，**只依赖 disk_struct_v2.0.h + libc**；
 *   · 五道门全部在引导期；过了门就 mmap 整盘，之后一切访存走 fsblk() 坐标；
 *   · 每道门的判据都**独立重推**，不复制 god/ko 的实现（对照 mkfs/mkfs_v20.cpp、
 *     linux/super.c 逐式核对，但代码自持）。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "audit/audit_image.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

#ifndef BLKSSZGET
#  define BLKSSZGET _IO(0x12, 104)
#endif

namespace rfs_audit {

namespace {

constexpr uint64_t BITS_PER_BLK = 32768;   /* 4096B × 8：每块位图块描述 32768 个块 */

/* CRC32（poly 0xEDB88320 反射，初/末取反）——与 mkfs.cpp / god 同源，独立实现。 */
uint32_t crc32_compute(const void *data, size_t len)
{
    static uint32_t tbl[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ (crc & 1u ? 0xEDB88320u : 0u);
            tbl[i] = crc;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *b = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i++)
        crc = tbl[(crc ^ b[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

} /* namespace */

const char *gate_str(gate_t g)
{
    switch (g) {
    case gate_t::OK:              return "OK";
    case gate_t::IO:              return "IO";
    case gate_t::NOT_V2:          return "F0_NOT_V2";
    case gate_t::WORLD_BASE:      return "F1_WORLD_BASE";
    case gate_t::GEOM_DEGENERATE: return "F2_GEOM_DEGENERATE";
    case gate_t::NOT_BACKED:      return "F3_NOT_BACKED";
    case gate_t::INODE_PLANE:     return "F4_INODE_PLANE";
    }
    return "?";
}

audit_image::~audit_image() { close_all(); }

void audit_image::close_all()
{
    if (map_) {
        munmap(const_cast<uint8_t *>(map_), map_len_);
        map_ = nullptr;
        map_len_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

gate_t audit_image::open_bootstrap(const char *path, std::string &err)
{
    /* ── 1. open(O_RDONLY)（审计永不写）── */
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        err = std::string("open(O_RDONLY) failed: ") + std::strerror(errno);
        rep_.gate = gate_t::IO;
        return rep_.gate;
    }

    /* ── 2. 字节源尺寸 + 逻辑块大小（F1/F3 的外部锚）── */
    struct stat st {};
    if (fstat(fd_, &st) != 0) {
        err = std::string("fstat failed: ") + std::strerror(errno);
        rep_.gate = gate_t::IO;
        return rep_.gate;
    }
    if (S_ISBLK(st.st_mode)) {
        off_t sz = lseek(fd_, 0, SEEK_END);
        dev_bytes_ = sz > 0 ? static_cast<uint64_t>(sz) : 0;
        int lba = 0;
        if (ioctl(fd_, BLKSSZGET, &lba) == 0 && lba > 0)
            rep_.lba_log2 = static_cast<uint8_t>(__builtin_ctz(static_cast<unsigned>(lba)));
        else
            rep_.lba_log2 = 12;
    } else {
        dev_bytes_ = static_cast<uint64_t>(st.st_size);
        rep_.lba_log2 = 12;   /* 常规文件模型：LBA 恒 4K */
    }

    /* ── 3. 读 Primary SB（设备 byte[0,4096)）── */
    if (dev_bytes_ < AUDIT_BLOCK) {
        err = "byte source smaller than one SB block (<4096B)";
        rep_.gate = gate_t::NOT_BACKED;
        return rep_.gate;
    }
    alignas(4096) uint8_t raw[4096];
    if (pread(fd_, raw, 4096, 0) != 4096) {
        err = std::string("pread(Primary SB) failed: ") + std::strerror(errno);
        rep_.gate = gate_t::IO;
        return rep_.gate;
    }
    std::memcpy(&sb_, raw, sizeof(RavenSuperblock));

    /* ══ F0 身份门：magic[0] 前缀 + version ══
     * 「一眼不是 v2.0 盘」= 无坐标系可言。crc 坏**不在此门**（见下）。 */
    rep_.magic_ok   = (sb_.magic[0] == RFS_SB_MAGIC_W0);
    rep_.version_ok = (sb_.version == RAVENFS_VERSION);
    if (!rep_.magic_ok || !rep_.version_ok) {
        char b[160];
        std::snprintf(b, sizeof b,
                      "not a v2.0 image: magic0=%#llx (want %#llx) version=%u (want %u)",
                      (unsigned long long)sb_.magic[0], (unsigned long long)RFS_SB_MAGIC_W0,
                      sb_.version, (unsigned)RAVENFS_VERSION);
        err = b;
        rep_.gate = gate_t::NOT_V2;
        return rep_.gate;
    }
    /* crc 是 FATAL 之外的一致性项：坏 = BAD，但几何门仍会兜住不可寻址的情形。 */
    {
        RavenSuperblock c = sb_;
        c.crc32 = 0;
        rep_.crc_ok = (sb_.crc32 == crc32_compute(&c, sizeof(c)));
    }

    /* ══ F1 原点门：blk0_off = world_base_lba × lba 必须 == 4096 ══ */
    {
        const uint64_t lba = 1ULL << rep_.lba_log2;
        const uint64_t wbl = sb_.world_base_lba;
        uint64_t off = wbl * lba;
        const bool of = (wbl != 0 && off / wbl != lba);
        rep_.world_base_ok = (!of && off == AUDIT_BLK0);
        if (!rep_.world_base_ok) {
            char b[160];
            std::snprintf(b, sizeof b,
                          "world_base_lba=%llu × lba=%llu != %llu (coordinate origin broken)",
                          (unsigned long long)wbl, (unsigned long long)lba,
                          (unsigned long long)AUDIT_BLK0);
            err = b;
            rep_.gate = gate_t::WORLD_BASE;
            return rep_.gate;
        }
    }

    /* ══ F2 退化/溢出门 ══ */
    if (sb_.total_blocks == 0) {
        err = "total_blocks == 0 (degenerate geometry)";
        rep_.gate = gate_t::GEOM_DEGENERATE;
        return rep_.gate;
    }
    if (sb_.total_blocks > (UINT64_MAX - AUDIT_BLK0) / AUDIT_BLOCK) {
        err = "total_blocks×4096 overflows u64 (degenerate geometry)";
        rep_.gate = gate_t::GEOM_DEGENERATE;
        return rep_.gate;
    }
    geo_.total_blocks  = sb_.total_blocks;
    geo_.fs_byte_end   = AUDIT_BLK0 + sb_.total_blocks * AUDIT_BLOCK;
    geo_.bitmap_blocks = (sb_.total_blocks + BITS_PER_BLK - 1) / BITS_PER_BLK;
    geo_.bitmap_start  = sb_.total_blocks - geo_.bitmap_blocks;
    if (geo_.bitmap_start <= 2) {   /* 对齐 linux/super.c 的 geometry underflow 门 */
        char b[160];
        std::snprintf(b, sizeof b, "bitmap_start=%llu <= 2 (metadata underflow)",
                      (unsigned long long)geo_.bitmap_start);
        err = b;
        rep_.gate = gate_t::GEOM_DEGENERATE;
        return rep_.gate;
    }

    /* ══ F3 落底门：字节源必须覆盖声称的块世界 ══ */
    if (dev_bytes_ < geo_.fs_byte_end) {
        char b[200];
        std::snprintf(b, sizeof b,
                      "byte source %lluB < required %lluB (total_blocks=%llu not fully backed)",
                      (unsigned long long)dev_bytes_, (unsigned long long)geo_.fs_byte_end,
                      (unsigned long long)sb_.total_blocks);
        err = b;
        rep_.gate = gate_t::NOT_BACKED;
        return rep_.gate;
    }
    geo_.device_bigger = (dev_bytes_ > geo_.fs_byte_end);   /* 合法，observe 非错 */

    /* ══ F4 inode 面根门：无 inode 空间 / ctl 不可解 → DFS 无起点 ══ */
    geo_.inode_pieces = RFS_IDP_PIECE_COUNT(sb_.inode_data_plane_pieces_count);
    if (geo_.inode_pieces == 0) {
        err = "inode_data_plane_pieces_count == 0 (no inode space, no root)";
        rep_.gate = gate_t::INODE_PLANE;
        return rep_.gate;
    }
    geo_.inode_count = geo_.inode_pieces * RFS_IDP_INODES_PER_PIECE;
    {
        const uint64_t ctl = sb_.inode_data_ctl;
        const uint64_t ptr = RFS_IDP_CTL_PTR(ctl);
        const bool direct  = RFS_IDP_CTL_IS_DIRECT(ctl) != 0;
        const bool ptr_bad = (ptr == RFS_IDP_CTL_PTR_INVALID) || (ptr >= sb_.total_blocks) ||
                             ((ptr & 1) != 0);   /* inode 片 8K = 2 块，须 2 对齐 */
        if (ptr_bad) {
            char b[200];
            std::snprintf(b, sizeof b,
                          "inode_data_ctl ptr=%llu direct=%d unaddressable (no DFS start)",
                          (unsigned long long)ptr, (int)direct);
            err = b;
            rep_.gate = gate_t::INODE_PLANE;
            return rep_.gate;
        }
        if (direct) {
            /* direct 段 = inode_pieces×2 个连续物理块，须整体落界 */
            if (ptr + geo_.inode_pieces * 2ULL > sb_.total_blocks) {
                char b[200];
                std::snprintf(b, sizeof b,
                              "direct inode plane [%llu,+%llu) exceeds total_blocks=%llu",
                              (unsigned long long)ptr,
                              (unsigned long long)(geo_.inode_pieces * 2ULL),
                              (unsigned long long)sb_.total_blocks);
                err = b;
                rep_.gate = gate_t::INODE_PLANE;
                return rep_.gate;
            }
        }
        /* tree 态：只校验根块可解；树**内部**结构错留给 DFS 报 inodeplane_bad。 */
    }

    /* ── 4. 五道门全过 → 整盘 mmap（PROT_READ + MAP_PRIVATE + MAP_NORESERVE）── */
    void *m = mmap(nullptr, dev_bytes_, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd_, 0);
    if (m == MAP_FAILED) {
        err = std::string("mmap failed: ") + std::strerror(errno);
        rep_.gate = gate_t::IO;
        return rep_.gate;
    }
    map_     = static_cast<const uint8_t *>(m);
    map_len_ = dev_bytes_;
    blk0_off_ = AUDIT_BLK0;

    /* ── 5. 非门 BAD 观察：位图自述段 [bitmap_start,total_blocks) 须全 1 ── */
    {
        const uint8_t *bm = fsblk(geo_.bitmap_start);
        rep_.bitmap_tail_ok = true;
        for (uint64_t i = geo_.bitmap_start; i < geo_.total_blocks; i++) {
            if (!((bm[i >> 3] >> (i & 7)) & 1u)) { rep_.bitmap_tail_ok = false; break; }
        }
    }

    rep_.gate = gate_t::OK;
    return rep_.gate;
}

bool audit_image::block_allocated(uint64_t blk) const
{
    if (!map_ || blk >= sb_.total_blocks) return false;
    const uint8_t *bm = fsblk(geo_.bitmap_start);
    return ((bm[blk >> 3] >> (blk & 7)) & 1u) != 0;
}

void audit_image::dump(FILE *out) const
{
    const geometry_t &g = geo_;
    std::fprintf(out, "[AUDIT][BOOT] gate=%s\n", gate_str(rep_.gate));
    std::fprintf(out, "[AUDIT][SB] magic=%s version=%s crc=%s world_base=%s\n",
                 rep_.magic_ok ? "ok" : "BAD",
                 rep_.version_ok ? "ok" : "BAD",
                 rep_.crc_ok ? "ok" : "BAD",
                 rep_.world_base_ok ? "ok" : "BAD");
    std::fprintf(out,
                 "[AUDIT][GEO] total_blocks=%llu bitmap_start=%llu bitmap_blocks=%llu "
                 "fs_byte_end=%llu dev_bytes=%llu device_bigger=%d\n",
                 (unsigned long long)g.total_blocks, (unsigned long long)g.bitmap_start,
                 (unsigned long long)g.bitmap_blocks, (unsigned long long)g.fs_byte_end,
                 (unsigned long long)dev_bytes_, (int)g.device_bigger);
    std::fprintf(out,
                 "[AUDIT][IDP] pieces=%llu inode_count=%llu ctl=%#llx ctl_ptr=%llu direct=%d\n",
                 (unsigned long long)g.inode_pieces, (unsigned long long)g.inode_count,
                 (unsigned long long)sb_.inode_data_ctl,
                 (unsigned long long)RFS_IDP_CTL_PTR(sb_.inode_data_ctl),
                 (int)(RFS_IDP_CTL_IS_DIRECT(sb_.inode_data_ctl) != 0));
    std::fprintf(out, "[AUDIT][BM] bitmap_tail_ok=%d (自述段须全 1；0=BAD)\n",
                 (int)rep_.bitmap_tail_ok);
    std::fprintf(out, "[AUDIT][SBINFO] uuid=%02x%02x%02x%02x world_base_lba=%llu\n",
                 sb_.uuid[0], sb_.uuid[1], sb_.uuid[2], sb_.uuid[3],
                 (unsigned long long)sb_.world_base_lba);
}

} /* namespace rfs_audit */
