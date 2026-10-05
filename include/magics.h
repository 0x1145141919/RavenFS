#pragma once

/* ═══════════════════════════════════════════════════════
 * RavenFS 磁盘扫描魔数签名
 *
 * 用于全盘扫描时低误报识别 B+tree 节点。
 * 每个节点写入 256-bit (K=4) 固定值，
 * 假阳性率 < 2^-256，完全可靠。
 * ═══════════════════════════════════════════════════════ */

/* ─── 文件区段 B+tree (Bptree_node_t) ───
 * entry[3..6].words[3] = magic1..magic4 */
#define BPTREE_MAGIC1  0xE2C21AFE1C35CFF8ULL
#define BPTREE_MAGIC2  0xBF483E2462E9EA83ULL
#define BPTREE_MAGIC3  0x38F45A47363D9F3DULL
#define BPTREE_MAGIC4  0x66EC3C500307DA82ULL

/* ─── 目录项 B+tree (Dentry_Bptree_node_t) ───
 * head.magic1..magic4 = magic1..magic4 */
#define DENTRY_MAGIC1  0x1407B23B4E3119EFULL
#define DENTRY_MAGIC2  0x09429E79C450AF5DULL
#define DENTRY_MAGIC3  0xD9C792AE76308B5BULL
#define DENTRY_MAGIC4  0x0586CE7C1EDF3B9CULL

/* ─── 目录名字数据面 · 映射树 (Bptree_dir_name_data_extent_*) ───
 * meta.magics[0..3]；数据面必须 8K 对齐、无空洞（v1.3） */
#define NAMEDATA_EXTENT_MAGIC1  0xC372D1FC567D65CFULL
#define NAMEDATA_EXTENT_MAGIC2  0x6E702B0D21436D8EULL
#define NAMEDATA_EXTENT_MAGIC3  0x0CCD38A459D96B13ULL
#define NAMEDATA_EXTENT_MAGIC4  0xD5F8B50F3ED12C4DULL

/* ─── 目录名字数据面 · 空闲区间树 (Bptree_dir_name_data_free_ranges_*) ───
 * meta.magics[0..3]；inline(≤9) / B+树 二态；记「已映射但内容已释放」区间 */
#define NAMEDATA_FREERANGES_MAGIC1  0xFFD7291C71147B5EULL
#define NAMEDATA_FREERANGES_MAGIC2  0xDC68716E675F2409ULL
#define NAMEDATA_FREERANGES_MAGIC3  0x3994B6520C33B94FULL
#define NAMEDATA_FREERANGES_MAGIC4  0xC49BE2A5D4F3C1A1ULL

/* ─── 全局 inode 数据面 · 映射树 (Bptree_inode_data_extent_*) ───
 * meta.magics[0..3]；逻辑 inode 空间 [0, pieces×8K) → 物理 FS_block。
 * 机械与名字数据面映射树同构（4K 逻辑块粒度、fblkbase/length 均 2 对齐、物理 8K
 * 对齐、稠密无洞）；魔数独立，供全盘扫描区分「文件树 / 名字数据面 / inode 数据面」。
 * 盘上形态 = **快照**：每 30s 由内存映射 RBT 整棵重建（bulk build，非增量补丁），
 * 挂载时 bulk-load 回 RBT。 */
#define INODE_DATA_EXTENT_MAGIC1  0xCBF11C1152260768ULL
#define INODE_DATA_EXTENT_MAGIC2  0x402E275272ECE079ULL
#define INODE_DATA_EXTENT_MAGIC3  0xE9FE8A9551304699ULL
#define INODE_DATA_EXTENT_MAGIC4  0xA051B9AFF19DBF8AULL

/* ─── 全局 inode 数据面 · 空闲面（扁 fr 快照，**非 B+树**）──
 * inode_data_fr_snapshot_hdr.magic[0..3]（快照区头 256-bit 签名，供全盘扫描识别
 * 该快照区；**不是** B+树节点魔数——该区无 Bptree_meta_t）。快照 = 扁平区间数组，
 * 30s 整段重写、挂载 bulk-load 回内存 RBT。
 * 条目用**数据面专属坐标类型** inode_dataplane_coordinate_free_range_t：
 * base/len 单位 = **1 inode = 256B**，字节量 = <<8（用户 2026-10-05 定）；
 * 与名字数据面 free_range_t（字节单位）坐标系不同，禁止混用。 */
#define INODE_DATA_FR_SNAPSHOT_MAGIC1  0x6C591CA8002381E5ULL
#define INODE_DATA_FR_SNAPSHOT_MAGIC2  0x7C4D62EC982323E4ULL
#define INODE_DATA_FR_SNAPSHOT_MAGIC3  0x064D499E99983279ULL
#define INODE_DATA_FR_SNAPSHOT_MAGIC4  0xB335E5A62E4F1586ULL

/* ─── Superblock (Primary & Backup) ───
 * v1.x：单字 magic = "RAVN" = SUPERBLOCK_MAGIC
 * v2.0：256-bit magic[4]；magic[0] 固定前缀 = 字节[0:7]="RAVENFS\0"（小端 u64），
 *       magic[1..3] 每卷随机 → 识别只比对 magic[0]。 */
#define SUPERBLOCK_MAGIC      0x5241564EULL              /* v1.x "RAVN"（单字） */
#define SUPERBLOCK_MAGIC_V20_W0 0x0053464E45564152ULL    /* v2.0 magic[0]："RAVENFS\0" 前缀（小端）*/
