#pragma once

#include <cstdint>
#include <type_traits>

/* ═══════════════════════════════════════════════════════
 * 高位标志位统一定义
 *
 * words[0..2] 的高 12 bits [63:52] 是位域
 * 低 52 bits [51:0]  存数值 (key, len, block addr)
 * 52 bits × 4K block = 16EB 地址空间，绰绰有余
 * ═══════════════════════════════════════════════════════ */

#define MASK_LOW52  0x000FFFFFFFFFFFFFULL
#define MASK_HIGH12 0xFFF0000000000000ULL

/* words[0] 位域 */
#define KEY_VALID   (1ULL << 63)          /* bit63: entry 有效 */
#define KEY_TYPE    (0x7FFULL << 52)       /* bits[62:52]: 类型标志 */

/* words[1] 位域 */
#define LEN_FLAGS   (0xFFFULL << 52)       /* bits[63:52]: len 标志位 */

/* words[2] 位域 */
#define ADDR_FLAGS  (0xFFFULL << 52)       /* bits[63:52]: 地址标志位 */
#define ADDR_BTREE  (1ULL << 63)           /* bit63: 块类型 (1=btree node, 0=data) */

/* ═══════════════════════════════════════════════════════
 * 文件块区间：闭区间 [fblkbase, fblkbase+len]
 * len = 末端偏移，块数 = len + 1；len=0 表示单块，无空集状态
 * ═══════════════════════════════════════════════════════ */
struct f_blkitv {
    uint64_t fblkbase;
    uint64_t len;
};

/* 区间序：I < J  ⇔  I.R + 1 ≤ J.L */
inline int fblk_cmp(f_blkitv left, f_blkitv right) {
    if (left.fblkbase + left.len < right.fblkbase) return 1;
    if (right.fblkbase + right.len < left.fblkbase) return -1;
    return 0;
}

/* 区间右端点 */
inline uint64_t f_blkitv_end(f_blkitv itv) {
    return itv.fblkbase + itv.len;
}

/* ═══════════════════════════════════════════════════════
 * 节点元数据 — entry[0].words[3] 编码
 * ═══════════════════════════════════════════════════════ */
union node_meta_t {
    struct {
        uint64_t entry_count_minus1 : 8;
        uint64_t is_internal_node   : 1;
        uint64_t reserved           : 3;
        uint64_t parent_ptr         : 52;
    } fields;
    uint64_t raw;
};
static_assert(sizeof(node_meta_t) == 8, "");

/* ═══════════════════════════════════════════════════════
 * B+tree entry (32B = 4 × uint64_t)
 * ═══════════════════════════════════════════════════════ */
struct btree_node_entry_t {
    uint64_t words[4];

    bool is_valid() const {
        return (words[0] & KEY_VALID) != 0;
    }
    void set_valid(bool v) {
        if (v) words[0] |=  KEY_VALID;
        else   words[0] &= ~KEY_VALID;
    }

    uint64_t get_key() const {
        return words[0] & MASK_LOW52;
    }
    void set_key(uint64_t k) {
        words[0] = (words[0] & MASK_HIGH12) | (k & MASK_LOW52);
    }

    uint64_t get_key_flags() const {
        return words[0] & KEY_TYPE;
    }
    void set_key_flags(uint64_t f) {
        words[0] = (words[0] & ~KEY_TYPE) | (f & KEY_TYPE);
    }

    /* ─── 叶子语义 ─── */
    f_blkitv get_interval() const {
        return { get_key(), words[1] & MASK_LOW52 };
    }
    void set_interval(f_blkitv itv) {
        set_key(itv.fblkbase);
        words[1] = (words[1] & MASK_HIGH12) | (itv.len & MASK_LOW52);
    }

    uint64_t get_len() const {
        return words[1] & MASK_LOW52;
    }
    void set_len(uint64_t l) {
        words[1] = (words[1] & MASK_HIGH12) | (l & MASK_LOW52);
    }

    uint64_t get_pbase() const {
        return words[2] & MASK_LOW52;
    }
    void set_pbase(uint64_t p) {
        words[2] = (words[2] & MASK_HIGH12) | (p & MASK_LOW52);
    }

    /* ─── 内部语义 ─── */
    uint64_t get_subptr() const {
        return words[2] & MASK_LOW52;
    }
    void set_subptr(uint64_t s) {
        words[2] = (words[2] & MASK_HIGH12) | (s & MASK_LOW52);
    }

    uint64_t get_addr_flags() const {
        return words[2] & ADDR_FLAGS;
    }
    void set_addr_flags(uint64_t f) {
        words[2] = (words[2] & ~ADDR_FLAGS) | (f & ADDR_FLAGS);
    }

    node_meta_t get_meta() const {
        node_meta_t m;
        m.raw = words[3];
        return m;
    }
    void set_meta(node_meta_t m) {
        words[3] = m.raw;
    }
};
static_assert(sizeof(btree_node_entry_t) == 32, "");

/* ═══════════════════════════════════════════════════════
 * B+tree 节点 (8K = 256 entries × 32B)
 * ═══════════════════════════════════════════════════════ */
struct Bptree_node_t {
    btree_node_entry_t entries[256];

    uint8_t get_entry_count() const {
        return entries[0].get_meta().fields.entry_count_minus1 + 1;
    }
    void set_entry_count(uint8_t c) {
        node_meta_t m = entries[0].get_meta();
        m.fields.entry_count_minus1 = (c > 0 ? c - 1 : 0);
        entries[0].set_meta(m);
    }

    bool is_internal() const {
        return entries[0].get_meta().fields.is_internal_node != 0;
    }
    void set_internal(bool v) {
        node_meta_t m = entries[0].get_meta();
        m.fields.is_internal_node = v ? 1 : 0;
        entries[0].set_meta(m);
    }

    uint64_t get_parent_ptr() const {
        return entries[0].get_meta().fields.parent_ptr;
    }
    void set_parent_ptr(uint64_t p) {
        node_meta_t m = entries[0].get_meta();
        m.fields.parent_ptr = p & MASK_LOW52;
        entries[0].set_meta(m);
    }

    uint64_t get_prev_sibling() const {
        return entries[1].words[3] & MASK_LOW52;
    }
    void set_prev_sibling(uint64_t p) {
        entries[1].words[3] = (entries[1].words[3] & MASK_HIGH12) | (p & MASK_LOW52);
    }

    uint64_t get_next_sibling() const {
        return entries[2].words[3] & MASK_LOW52;
    }
    void set_next_sibling(uint64_t n) {
        entries[2].words[3] = (entries[2].words[3] & MASK_HIGH12) | (n & MASK_LOW52);
    }
};
static_assert(sizeof(Bptree_node_t) == 8192, "");

/* ═══════════════════════════════════════════════════════
 * Inode (256B)
 * ═══════════════════════════════════════════════════════ */
struct raven_Inode_t {
    uint64_t  flags;
    uint32_t  link_count;
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  m_ns_stamp;
    uint32_t  c_ns_stamp;
    uint32_t  a_ns_stamp;
    uint32_t  user_permission_bits;
    uint32_t  group_permission_bits;
    uint32_t  other_permission_bits;
    uint64_t  size;
    uint64_t  mtime;
    uint64_t  ctime;
    uint64_t  atime;
    uint64_t  data_root;       // 0 = inline
    uint32_t  parent_inode;
    uint32_t  generation;
    uint8_t   inline_data[160];
};
static_assert(sizeof(raven_Inode_t) == 256, "Inode size must be 256B");

/* ═══════════════════════════════════════════════════════
 * Superblock (4K)
 * ═══════════════════════════════════════════════════════ */
struct RavenSuperblock {
    uint64_t  magic;                // "RAVN" = 0x5241564E
    uint32_t  version;
    uint32_t  block_size;           // 固定 4096

    uint64_t  total_blocks;
    uint64_t  bitmap_start;
    uint64_t  bitmap_blocks;

    uint64_t  inode_array_base;
    uint64_t  inode_array_count;
    uint16_t  inode_size;           // 256
    uint16_t  reserved0;

    uint32_t  root_inode;
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;
    uint8_t   pad[4096 - 22 * 8 - 4 - 48 - 16 - 4]; // 凑满 4096
};
static_assert(sizeof(RavenSuperblock) == 4096, "Superblock must be 4K");

/* ═══════════════════════════════════════════════════════
 * Dentry (目录项，变长)
 * ═══════════════════════════════════════════════════════ */
struct RavenDentry {
    uint64_t  hash;              // key = filename hash
    uint64_t  inode;             // inode 编号
    uint32_t  group;             // 块组 (保留)
    uint8_t   name_len;
    uint8_t   file_type;
    uint16_t  _pad;
    char      name[];            // 变长
};
