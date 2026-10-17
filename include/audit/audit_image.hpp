#pragma once
/* ═══════════════════════════════════════════════════════════════════════════
 * audit_image.hpp — audit 顶层独立项目的「坐标层」
 *
 * 定位：与 juvenile_god/、linux/ 平级的独立只读校验器的**唯一访存入口**。
 * 纪律见 audit/DISCIPLINE.md。三句话：
 *   1. 只信「裸盘字节 + disk_struct_v2.0.h 盘面契约」；
 *   2. 唯一地址语言 = base + blk×4096（坐标偏移量，禁止走任何 god 设施）；
 *   3. 不链 libravenfs_juvenile.a，不碰 fs_allocator / mdata_piece_cache /
 *      InodeSession / read_buff。
 *
 * 本层职责 = 引导（bootstrap）：
 *   open(O_RDONLY) → 读 Primary SB → 五道 fail-fast 硬门（F0–F4）→ 整盘 mmap。
 *   **五道门全部在引导期；DFS 一旦开始，几何问题一律降级为 BAD（报而不断）。**
 *   依据：几何错的唯一后果是「算出的地址没有可读字节」——那种状态下 DFS 结论是
 *   噪声，故 fail-fast；坐标空间完整可寻址后，其余都是内容发现，正是审计的意义。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "disk_struct_v2.0.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace rfs_audit {

constexpr uint64_t AUDIT_BLOCK = 4096;   /* FS_block 字节尺寸（契约恒 4K） */
constexpr uint64_t AUDIT_BLK0  = 4096;   /* FS_block[0] 的设备字节偏移（world_base 契约） */

/* ── 引导期五道 fail-fast 硬门（+ IO）。任何非 OK = 坐标空间无指称，拒绝审计。 ── */
enum class gate_t : int {
    OK = 0,
    IO,               /* open / pread / mmap 失败（非盘面问题，环境问题） */
    NOT_V2,           /* F0 身份门：magic[0] 前缀 / version 非 v2.0 */
    WORLD_BASE,       /* F1 原点门：world_base_lba×lba != 4096（整个坐标原点错） */
    GEOM_DEGENERATE,  /* F2 退化/溢出门：total_blocks==0 / 字节跨度溢出 / bitmap_start<=2 */
    NOT_BACKED,       /* F3 落底门：字节源 < 4096+total_blocks×4096（声称的块读不到） */
    INODE_PLANE,      /* F4 inode 面根门：无 inode 空间 / ctl 指针不可解（DFS 无起点） */
};

const char *gate_str(gate_t g);

/* ── 重推几何（v2.0：SB 不存位图几何，只有 block bitmap 尾巴）──
 * bitmap_blocks = ceil(total_blocks/32768)，bitmap_start = total_blocks - bitmap_blocks。*/
struct geometry_t {
    uint64_t total_blocks   = 0;
    uint64_t bitmap_blocks  = 0;
    uint64_t bitmap_start   = 0;
    uint64_t inode_pieces   = 0;
    uint64_t inode_count    = 0;   /* inode_pieces × 32 */
    uint64_t fs_byte_end    = 0;   /* AUDIT_BLK0 + total_blocks×AUDIT_BLOCK */
    bool     device_bigger  = false; /* 字节源 > fs_byte_end：分区/扩容合法，observe 非错 */
};

/* ── 引导报告：五道门的判决 + 非门观察位（BAD 类，报而不断）── */
struct bootstrap_report_t {
    gate_t      gate = gate_t::IO;
    bool        magic_ok      = false;
    bool        version_ok    = false;
    bool        crc_ok        = false;  /* 非门：crc 坏 = BAD，不中止（几何门仍兜底） */
    bool        world_base_ok = false;
    bool        bitmap_tail_ok = false; /* 非门：位图自述段非全 1 = BAD */
    uint8_t     lba_log2      = 12;
};

class audit_image {
public:
    audit_image() = default;
    ~audit_image();
    audit_image(const audit_image &) = delete;
    audit_image &operator=(const audit_image &) = delete;

    /* 引导：open + 读 SB + 五道门 + mmap。返回 OK 才可进入 DFS。
     * 失败时 err 填人类可读原因；IO 类返回 gate_t::IO。 */
    gate_t open_bootstrap(const char *path, std::string &err);

    bool ok() const { return map_ != nullptr && rep_.gate == gate_t::OK; }

    /* ── 坐标语言（审计代码此后唯一允许的访存途径）── */
    const uint8_t *fsblk(uint64_t blk) const {
        return map_ + blk0_off_ + blk * AUDIT_BLOCK;
    }
    /* 盘面 block 位图直接解码（不经 allocator：位 i ↔ FS_block i；位图存储起于
     * FS_block[bitmap_start]，但位序从块 0 起）。 */
    bool block_allocated(uint64_t blk) const;

    const RavenSuperblock   &sb()      const { return sb_; }
    const geometry_t        &geo()     const { return geo_; }
    const bootstrap_report_t &report() const { return rep_; }
    uint64_t device_bytes()            const { return dev_bytes_; }

    void dump(FILE *out) const;

private:
    void close_all();

    int             fd_       = -1;
    const uint8_t  *map_      = nullptr;
    uint64_t        map_len_  = 0;
    uint64_t        dev_bytes_ = 0;
    uint64_t        blk0_off_ = AUDIT_BLK0;
    RavenSuperblock sb_{};
    geometry_t      geo_{};
    bootstrap_report_t rep_{};
};

} /* namespace rfs_audit */
