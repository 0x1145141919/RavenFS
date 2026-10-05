/*
 * sbinfo —— v2.0 盘面（弹性 inode 数据面；独立 TU，见 sbinfo_common.hpp）。
 * 由 sbinfo.cpp 按 SB magic[0]（"RAVENFS\0" 前缀）分派进来。
 *
 * 除 SB 字段/时间戳/crc/backup 链外，额外解读 v2.0 独有的两块：
 *   · inode 数据面：inode_data_ctl（direct/tree + 段/树根）、片数；解析 root inode(idx0)。
 *   · 空闲面：扁 fr 快照（hdr + 条目），验 crc / 列空闲区间。
 */
#include "sbinfo_common.hpp"
#include <disk_struct_v2.0.h>
#include <magics.h>

#include <cstddef>
#include <cstring>

/* SB crc32：v2.0 crc 字段 offset = offsetof(RavenSuperblock, crc32)（=152） */
static uint32_t sb_crc32_v20(const uint8_t *raw) {
    std::vector<uint8_t> tmp(raw, raw + FS_BLOCK_BYTES);
    memset(tmp.data() + offsetof(RavenSuperblock, crc32), 0, 4);
    return crc32_calc(tmp.data(), tmp.size());
}

/* ═══════════════════ udev/blkid 探针（v2.0） ═══════════════════ */
int sbinfo_v20_probe(const uint8_t *raw) {
    RavenSuperblock sb;
    memcpy(&sb, raw, sizeof sb);
    if (sb.magic[0] != RFS_SB_MAGIC_W0) return 0;
    std::string uuid  = fmt_uuid(sb.uuid);
    std::string label = fmt_label(sb.volume_name, sizeof sb.volume_name);
    printf("ID_FS_TYPE=ravenfs\n");
    printf("ID_FS_USAGE=filesystem\n");
    printf("ID_FS_VERSION=2.0\n");
    printf("ID_FS_UUID=%s\n", uuid.c_str());
    printf("ID_FS_UUID_ENC=%s\n", uuid.c_str());
    if (!label.empty()) {
        printf("ID_FS_LABEL=%s\n", label.c_str());
        probe_enc(label);
        printf("ID_FS_LABEL_ENC=%s\n", label.c_str());
    }
    return 0;
}

/* ═══════════════════ Primary + Backup SB 详查（v2.0） ═══════════════════ */
int sbinfo_v20_dump(int fd, const char *path, uint64_t dev_bytes, uint64_t lba_size,
                    const uint8_t *raw) {
    (void)path;
    RavenSuperblock sb;
    memcpy(&sb, raw, sizeof sb);
    const uint64_t world_byte_base = sb.world_base_lba * lba_size;

    ok("magic[0] = \"RAVENFS\\0\" (%#018llx)", (unsigned long long)sb.magic[0]);
    printf("       magic[1..3] = %#018llx %#018llx %#018llx  (每卷随机)\n",
           (unsigned long long)sb.magic[1], (unsigned long long)sb.magic[2],
           (unsigned long long)sb.magic[3]);

    if (sb.version == RAVENFS_VERSION)
        ok("version = %u (v2.0 = RAVENFS_VERSION)", sb.version);
    else
        bad("version = %u，≠ RAVENFS_VERSION(%u)", sb.version, (unsigned)RAVENFS_VERSION);

    {
        uint32_t stored = sb.crc32;
        uint32_t calc   = sb_crc32_v20(raw);
        if (stored == calc) ok("crc32 = %#010x（重算一致）", stored);
        else                bad("crc32 = %#010x，重算 = %#010x（不一致）", stored, calc);
    }

    printf("\n-- 字段 --\n");
    printf("volume label     : \"%s\"\n", fmt_label(sb.volume_name, sizeof sb.volume_name).c_str());
    printf("uuid             : %s\n", fmt_uuid(sb.uuid).c_str());
    printf("world_base_lba   : %llu  (lba=%llu → FS 世界字节基址 %llu)",
           (unsigned long long)sb.world_base_lba, (unsigned long long)lba_size,
           (unsigned long long)world_byte_base);
    if (world_byte_base == FS_BLOCK_BYTES) printf("  [mkfs 契约: 4096]\n");
    else printf("  [偏离 mkfs 契约 4096]\n");
    printf("total_blocks     : %llu  (≈ %s)\n",
           (unsigned long long)sb.total_blocks,
           fmt_bytes(sb.total_blocks * FS_BLOCK_BYTES).c_str());
    printf("root inode idx   : %llu  [FS 契约硬编码 RFS_ROOT_INODE_IDX]\n",
           (unsigned long long)RFS_ROOT_INODE_IDX);

    /* ── inode 数据面控制字 ── */
    printf("\n-- inode 数据面（超级块统辖） --\n");
    {
        const bool is_direct = RFS_IDP_CTL_IS_DIRECT(sb.inode_data_ctl) != 0;
        const uint64_t plane_base = RFS_IDP_CTL_PTR(sb.inode_data_ctl);
        const uint64_t pieces = RFS_IDP_PIECE_COUNT(sb.inode_data_plane_pieces_count);
        printf("inode_data_ctl              : %#018llx  %s  base/root=%llu\n",
               (unsigned long long)sb.inode_data_ctl,
               is_direct ? "direct（单连续段）" : "tree（映射树）",
               (unsigned long long)plane_base);
        printf("inode_data_plane_pieces_count: %llu  (= %llu 个 8K 片 = %llu inode 逻辑空间)\n",
               (unsigned long long)pieces,
               (unsigned long long)pieces, (unsigned long long)(pieces * 32));
        printf("inode_data_fr_ctl           : %#018llx  fr 快照区首块=%llu\n",
               (unsigned long long)sb.inode_data_fr_ctl,
               (unsigned long long)RFS_IDP_FR_CTL_PTR(sb.inode_data_fr_ctl));
        printf("inode_data_fr_count         : %llu\n", (unsigned long long)sb.inode_data_fr_count);

        /* root inode（v2.0 恒 idx 0）：direct 态可直算物理块 */
        if (is_direct && pieces > 0) {
            const uint64_t lpiece = RFS_ROOT_INODE_IDX >> 5;      /* idx/32 */
            const uint64_t blk = plane_base + lpiece * 2;         /* 每片 2 块 */
            const off_t off = (off_t)(world_byte_base + blk * FS_BLOCK_BYTES +
                                      (RFS_ROOT_INODE_IDX & 31) * 256);
            raven_Inode_t ri;
            if (pread_full(fd, &ri, sizeof ri, off)) {
                const uint32_t type = RFS_INO1_GET_TYPE(ri.word1);
                printf("root inode       : blk=%llu off=%llu  type=%u %s  mode=%04o  size=%llu lc=%u dentry_root=%llu\n",
                       (unsigned long long)blk, (unsigned long long)off, type,
                       type == INODE_TYPE_DIR ? "(DIR)" : "(?)",
                       RFS_MODE_GET(ri.mode_xattr), (unsigned long long)ri.size,
                       ri.link_count, (unsigned long long)ri.data_root);
                if (type == INODE_TYPE_DIR) ok("root inode 结构可读且为 DIR");
                else                        bad("root inode type=%u ≠ DIR(%u)", type, (unsigned)INODE_TYPE_DIR);
            } else {
                bad("root inode 读取失败（off=%llu）", (unsigned long long)off);
            }
        } else if (!is_direct) {
            printf("root inode       : （映射树态，需解析映射树；本工具暂跳过）\n");
        }
    }

    /* ── 扁 fr 快照（空闲面）── */
    printf("\n-- 空闲面（扁 fr 快照，非 B+树） --\n");
    {
        const uint64_t fr_base = RFS_IDP_FR_CTL_PTR(sb.inode_data_fr_ctl);
        const off_t hoff = (off_t)(world_byte_base + fr_base * FS_BLOCK_BYTES);
        inode_data_fr_snapshot_hdr h;
        if (!pread_full(fd, &h, sizeof h, hoff)) {
            bad("fr 快照头读取失败（base=%llu）", (unsigned long long)fr_base);
        } else {
            const bool m_ok = (h.magic[0] == INODE_DATA_FR_SNAPSHOT_MAGIC1 &&
                               h.magic[1] == INODE_DATA_FR_SNAPSHOT_MAGIC2 &&
                               h.magic[2] == INODE_DATA_FR_SNAPSHOT_MAGIC3 &&
                               h.magic[3] == INODE_DATA_FR_SNAPSHOT_MAGIC4);
            if (m_ok) ok("fr magic ✓ (INODE_DATA_FR_SNAPSHOT_MAGIC1..4)");
            else      bad("fr magic ✗（magic[0]=%#llx）", (unsigned long long)h.magic[0]);

            printf("fr base=%llu  entry_count=%llu  capacity=%llu\n",
                   (unsigned long long)fr_base,
                   (unsigned long long)h.entry_count, (unsigned long long)h.capacity);
            if (h.entry_count == sb.inode_data_fr_count)
                ok("fr entry_count == SB.inode_data_fr_count = %llu", (unsigned long long)h.entry_count);
            else
                warn("fr entry_count = %llu ≠ SB.inode_data_fr_count = %llu",
                     (unsigned long long)h.entry_count, (unsigned long long)sb.inode_data_fr_count);

            if (h.entry_count > 0 && h.entry_count <= (1u << 24)) {
                const size_t bytes = sizeof h + (size_t)h.entry_count * 16;
                std::vector<uint8_t> reg(bytes);
                if (!pread_full(fd, reg.data(), bytes, hoff)) {
                    bad("fr 快照区读取失败（%zu B）", bytes);
                } else {
                    /* crc：置零 hdr.crc32 字段后全长重算 */
                    uint32_t stored = h.crc32;
                    memset(reg.data() + offsetof(inode_data_fr_snapshot_hdr, crc32), 0, 4);
                    uint32_t calc = crc32_calc(reg.data(), reg.size());
                    if (stored == calc) ok("fr crc32 = %#010x（重算一致）", stored);
                    else                bad("fr crc32 = %#010x，重算 = %#010x（不一致）", stored, calc);

                    /* 列前若干条空闲区间（inode 单位；字节量 <<8） */
                    const uint64_t show = h.entry_count < 8 ? h.entry_count : 8;
                    for (uint64_t i = 0; i < show; i++) {
                        inode_dataplane_coordinate_free_range_t e;
                        memcpy(&e, reg.data() + sizeof h + i * 16, sizeof e);
                        printf("  fr[%llu] base=%llu len=%llu  (字节 [%llu, %llu))\n",
                               (unsigned long long)i,
                               (unsigned long long)e.base, (unsigned long long)e.len,
                               (unsigned long long)(e.base << 8),
                               (unsigned long long)((e.base + e.len) << 8));
                    }
                    if (h.entry_count > show)
                        printf("  ...（共 %llu 条，仅列前 %llu）\n",
                               (unsigned long long)h.entry_count, (unsigned long long)show);
                }
            }
        }
    }

    /* ── 卷生命周期时间戳（v2.0） ── */
    printf("\n-- 卷生命周期时间戳 --\n");
    printf("born           : %s\n", fmt_time(sb.born_time).c_str());
    printf("latest_mount   : %s\n", fmt_time(sb.latest_mount_time).c_str());
    printf("latest_syn     : %s\n", fmt_time(sb.latest_syn_time).c_str());
    printf("latest_umount  : %s\n", fmt_time(sb.latest_umount_time).c_str());
    printf("abnormal_shutdown_counts: %llu\n", (unsigned long long)sb.abnormal_shutdown_counts);
    printf("system_mount   : %llu 次 / %llu 秒\n",
           (unsigned long long)sb.system_mount_counter,
           (unsigned long long)sb.system_mount_time_accumulate);
    printf("tools_mount    : %llu 次 / %llu 秒\n",
           (unsigned long long)sb.tools_mount_counter,
           (unsigned long long)sb.tools_mount_time_accumulate);
    if (sb.latest_mount_time == 0 && sb.latest_umount_time == 0 && sb.latest_syn_time == 0)
        printf("state          : 从未挂载\n");
    else if (sb.latest_syn_time > sb.latest_umount_time)
        warn("state          : latest_syn > latest_umount → 上次非正常关机（或仍挂载中）");
    else
        printf("state          : 干净卸载历史（latest_umount ≥ latest_syn）\n");

    /* ── 派生几何（v2.0：只有 block bitmap 尾巴） ── */
    printf("\n-- 派生几何（v2.0：仅 block bitmap 尾巴） --\n");
    if (sb.total_blocks == 0) {
        bad("total_blocks 为 0，几何不可信");
    } else {
        const uint64_t total = sb.total_blocks;
        const uint64_t bb = (total + 32767) / 32768;
        const uint64_t bitmap_start = total - bb;
        printf("block bitmap     : [%llu, %llu)  (+%llu blocks)\n",
               (unsigned long long)bitmap_start, (unsigned long long)total,
               (unsigned long long)bb);
        printf("(无 inode array / 无 inode bitmap；inode 数据面见上)\n");
        const uint64_t expect = FS_BLOCK_BYTES * (total + 1);
        if (dev_bytes == expect)
            ok("device bytes = 4096*(total_blocks+1) = %llu", (unsigned long long)expect);
        else
            warn("device bytes %llu ≠ 4096*(total_blocks+1) = %llu（SB total_blocks 与盘尺寸不符）",
                 (unsigned long long)dev_bytes, (unsigned long long)expect);
    }

    /* ── Backup SB 链 ── */
    printf("\n-- Backup SB 链（count=%u, head=backup_next） --\n", sb.backup_count);
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

            bool magic_ok = (bsb.magic[0] == RFS_SB_MAGIC_W0);
            uint32_t bcalc = sb_crc32_v20(braw.data());
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
                bad("backup[%u] blk=%llu  magic%s crc%s prev链%s（prev=%llu 期望=%llu）",
                    guard, (unsigned long long)blk,
                    magic_ok ? "✓" : "✗", crc_ok ? "✓" : "✗", link_ok ? "✓" : "✗",
                    (unsigned long long)bsb.backup_prev, (unsigned long long)prev_blk);

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
