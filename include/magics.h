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

/* ─── Superblock (Primary & Backup) ───
 * RavenSuperblock.magic = "RAVN" = SUPERBLOCK_MAGIC */
#define SUPERBLOCK_MAGIC  0x5241564EULL
