/*
 * sbinfo —— v1.x 盘面（v1.1 / v1.2 / v1.3；三版 SB 字节布局全等，仅语义分叉）。
 * 由 sbinfo.cpp 按 SB magic（"RAVN"）分派进来。
 */
#include "sbinfo_common.hpp"
#include <disk_struct_v1.2.h>
#include <magics.h>

#include <cstddef>
#include <cstring>

/* SB crc32 校验口径（同 audit）：原始 4096B 拷贝 → 置零 crc32 字段 → 全长重算 */
static uint32_t sb_crc32_v1(const uint8_t *raw) {
    std::vector<uint8_t> tmp(raw, raw + FS_BLOCK_BYTES);
    memset(tmp.data() + offsetof(RavenSuperblock, crc32), 0, 4);
    return crc32_calc(tmp.data(), tmp.size());
}

/* ═══════════════════ udev/blkid 探针（v1.x） ═══════════════════ */
int sbinfo_v1_probe(const uint8_t *raw) {
    RavenSuperblock sb;
    memcpy(&sb, raw, sizeof sb);
    if (sb.magic != SUPERBLOCK_MAGIC) return 0;
    if (sb.version != 2 && sb.version != 3 && sb.version != 4) return 0;
    std::string uuid  = fmt_uuid(sb.uuid);
    std::string label = fmt_label(sb.volume_name, sizeof sb.volume_name);
    const char *vstr = sb.version == 4 ? "1.3" : (sb.version == 3 ? "1.2" : "1.1");
    printf("ID_FS_TYPE=ravenfs\n");
    printf("ID_FS_USAGE=filesystem\n");
    printf("ID_FS_VERSION=%s\n", vstr);
    printf("ID_FS_UUID=%s\n", uuid.c_str());
    printf("ID_FS_UUID_ENC=%s\n", uuid.c_str());
    if (!label.empty()) {
        printf("ID_FS_LABEL=%s\n", label.c_str());
        probe_enc(label);
        printf("ID_FS_LABEL_ENC=%s\n", label.c_str());
    }
    return 0;
}

/* ═══════════════════ Primary + Backup SB 详查（v1.x） ═══════════════════ */
int sbinfo_v1_dump(int fd, const char *path, uint64_t dev_bytes, uint64_t lba_size,
                   const uint8_t *raw) {
    (void)path; (void)dev_bytes;
    RavenSuperblock sb;
    memcpy(&sb, raw, sizeof sb);

    ok("magic = \"RAVN\" (%#llx)", (unsigned long long)sb.magic);

    /* version：2/3/4 均为合法盘面（SB 布局全等），标注即可 */
    const char *vname =
        sb.version == 2 ? "v1.1（god 盘面）" :
        sb.version == 3 ? "v1.2" :
        sb.version == 4 ? "v1.3（名字数据面双树）" : "未知版本";
    if (sb.version == 2 || sb.version == 3 || sb.version == 4)
        ok("version = %u (%s)", sb.version, vname);
    else
        bad("version = %u，既非 2/3/4（v1.1/v1.2/v1.3）", sb.version);

    /* crc32 */
    {
        uint32_t stored = sb.crc32;
        uint32_t calc   = sb_crc32_v1(raw);
        if (stored == calc) ok("crc32 = %#010x（重算一致）", stored);
        else                bad("crc32 = %#010x，重算 = %#010x（不一致）", stored, calc);
    }

    /* 核心字段 */
    printf("\n-- 字段 --\n");
    printf("volume label     : \"%s\"\n", fmt_label(sb.volume_name, sizeof sb.volume_name).c_str());
    printf("uuid             : %s\n", fmt_uuid(sb.uuid).c_str());
    printf("world_base_lba   : %llu  (lba=%llu → FS 世界字节基址 %llu)",
           (unsigned long long)sb.world_base_lba, (unsigned long long)lba_size,
           (unsigned long long)(sb.world_base_lba * lba_size));
    if (sb.world_base_lba * lba_size == FS_BLOCK_BYTES) printf("  [mkfs 契约: 4096]\n");
    else printf("  [偏离 mkfs 契约 4096]\n");
    printf("total_blocks     : %llu  (≈ %s)\n",
           (unsigned long long)sb.total_blocks,
           fmt_bytes(sb.total_blocks * FS_BLOCK_BYTES).c_str());
    printf("inode_array_count: %llu\n", (unsigned long long)sb.inode_array_count);
    printf("root_inode       : %llu%s\n", (unsigned long long)sb.root_inode,
           sb.root_inode == 16 ? "  [v1.2/v1.3]" : (sb.root_inode == 0 ? "  [v1.1]" : "  [?]"));

    printf("\n-- 卷生命周期时间戳 --\n");
    printf("born   : %s\n", fmt_time(sb.born_time).c_str());
    printf("mount  : %s\n", fmt_time(sb.mount_time).c_str());
    printf("umount : %s\n", fmt_time(sb.umount_time).c_str());
    printf("modify : %s\n", fmt_time(sb.modify_time).c_str());
    if (sb.mount_time == 0 && sb.umount_time == 0)
        printf("state  : 从未挂载（或挂载路径未回写时间戳）\n");
    else if (sb.umount_time == 0 && sb.mount_time != 0)
        warn("state  : 有 mount 无 umount → 仍挂载中，或上次**非正常关机**（umount 0 哨兵）");
    else if (sb.mount_time > sb.umount_time)
        warn("state  : mount(%llu) > umount(%llu) → 上次非正常关机",
             (unsigned long long)sb.mount_time, (unsigned long long)sb.umount_time);
    else
        printf("state  : 干净卸载历史（umount ≥ mount）\n");

    /* 派生几何（与 mkfs calc_geometry 逐式一致） */
    printf("\n-- 派生几何（mkfs calc_geometry 同式） --\n");
    if (sb.total_blocks == 0 || sb.inode_array_count == 0) {
        bad("total_blocks/inode_array_count 为 0，几何不可信");
    } else {
        uint64_t total = sb.total_blocks, ic = sb.inode_array_count;
        uint64_t bb = (total + 32767) / 32768;
        uint64_t bitmap_start = total - bb;
        uint64_t ib  = (ic * 256 + 4095) / 4096;
        uint64_t iab = bitmap_start - ib;
        uint64_t ibb = (ic + 32767) / 32768;
        uint64_t ibm = iab - ibb;
        if (ibm <= 1) bad("inode_bitmap_base = %llu ≤ 1，几何非法（设备太小？）",
                          (unsigned long long)ibm);
        else {
            printf("block bitmap     : [%llu, %llu)  (+%llu blocks)\n",
                   (unsigned long long)bitmap_start, (unsigned long long)total,
                   (unsigned long long)bb);
            printf("inode array      : [%llu, %llu)  (+%llu blocks)\n",
                   (unsigned long long)iab, (unsigned long long)bitmap_start,
                   (unsigned long long)ib);
            printf("inode bitmap     : [%llu, %llu)  (+%llu blocks)\n",
                   (unsigned long long)ibm, (unsigned long long)iab,
                   (unsigned long long)ibb);
        }
        uint64_t expect = FS_BLOCK_BYTES * (total + 1);
        if (dev_bytes == expect)
            ok("device bytes = 4096*(total_blocks+1) = %llu", (unsigned long long)expect);
        else
            warn("device bytes %llu ≠ 4096*(total_blocks+1) = %llu（SB total_blocks 与盘尺寸不符）",
                 (unsigned long long)dev_bytes, (unsigned long long)expect);
    }

    /* Backup SB 链 */
    printf("\n-- Backup SB 链（count=%u, head=backup_next） --\n", sb.backup_count);
    const uint64_t world_byte_base = sb.world_base_lba * lba_size;
    if (sb.backup_next == PTR_INVALID) {
        if (sb.backup_count == 0) ok("无 backup SB（backup_count=0）");
        else bad("backup_next = PTR_INVALID 但 backup_count = %u", sb.backup_count);
    } else {
        uint64_t blk = sb.backup_next, prev_blk = PTR_INVALID;
        uint32_t guard = 0;
        uint32_t max_iter = sb.backup_count + 8;
        bool saw_dup = false;
        while (blk != PTR_INVALID && blk < sb.total_blocks && guard < max_iter) {
            off_t off = (off_t)(world_byte_base + blk * FS_BLOCK_BYTES);
            std::vector<uint8_t> braw(FS_BLOCK_BYTES);
            if (!pread_full(fd, braw.data(), FS_BLOCK_BYTES, off)) {
                bad("backup[%u] blk=%llu 读取失败（off=%llu）",
                    guard, (unsigned long long)blk, (unsigned long long)off);
                break;
            }
            RavenSuperblock bsb;
            memcpy(&bsb, braw.data(), sizeof bsb);

            bool magic_ok = (bsb.magic == SUPERBLOCK_MAGIC);
            uint32_t bcalc = sb_crc32_v1(braw.data());
            bool crc_ok   = (bsb.crc32 == bcalc);
            bool link_ok  = (bsb.backup_prev == prev_blk);

            if (magic_ok && crc_ok && link_ok)
                ok("backup[%u] blk=%llu off=%llu  magic✓ crc✓ prev链✓ (prev=%s next=%s)",
                   guard, (unsigned long long)blk, (unsigned long long)off,
                   bsb.backup_prev == PTR_INVALID ? "-" :
                       std::to_string(bsb.backup_prev).c_str(),
                   bsb.backup_next == PTR_INVALID ? "-" :
                       std::to_string(bsb.backup_next).c_str());
            else
                bad("backup[%u] blk=%llu  magic%s crc%s prev链%s（存=%s 重算=%s prev=%llu 期望=%llu）",
                    guard, (unsigned long long)blk,
                    magic_ok ? "✓" : "✗", crc_ok ? "✓" : "✗", link_ok ? "✓" : "✗",
                    magic_ok ? "" : "≠RAVN ",
                    magic_ok ? (crc_ok ? "" : "不一致") : "",
                    (unsigned long long)bsb.backup_prev,
                    (unsigned long long)prev_blk);

            if (bsb.backup_next == blk) { saw_dup = true; break; }
            prev_blk = blk;
            blk = bsb.backup_next;
            guard++;
        }
        if (saw_dup) bad("backup 链自环（next == self）");
        else if (blk != PTR_INVALID && guard >= max_iter)
            bad("backup 链超长（> %u 节点）或成环，已截断", max_iter);

        uint32_t nodes = 0;
        {
            uint64_t b = sb.backup_next;
            uint32_t g = 0;
            while (b != PTR_INVALID && b < sb.total_blocks && g < max_iter) {
                std::vector<uint8_t> tmp(FS_BLOCK_BYTES);
                if (!pread_full(fd, tmp.data(), FS_BLOCK_BYTES,
                                (off_t)(world_byte_base + b * FS_BLOCK_BYTES))) break;
                RavenSuperblock t;
                memcpy(&t, tmp.data(), sizeof t);
                nodes++; g++;
                if (t.backup_next == b) break;
                b = t.backup_next;
            }
        }
        if (nodes == sb.backup_count)
            ok("backup 节点数 = backup_count = %u", nodes);
        else
            warn("backup 节点数 = %u ≠ backup_count = %u", nodes, sb.backup_count);
    }
    return 0;
}
