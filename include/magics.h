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

/* ─── Superblock (Primary & Backup) ───
 * RavenSuperblock.magic = "RAVN" = SUPERBLOCK_MAGIC */
#define SUPERBLOCK_MAGIC  0x5241564EULL
