#pragma once

#include "stdint.h"

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

/* ═══════════════════════════════════════════════════════════════════
 * 节点元数据 — entry[0].words[3] 编码
 * ═══════════════════════════════════════════════════════════════════
 *
 * ※ In-memory 元数据缓存借用槽位 (v1 未实现，注释留底) ※
 *
 * 私有节点缓存 (类似 XFS xfs_buf / btrfs extent_buffer)
 * 可以在加载到内存时借用以下字段缓存线性地址指针，
 * 使遍历全走直接解引用而不需映射表。
 *
 * 内部节点:
 *   words[1]  磁盘未使用 → 借用为 child 线性地址
 *             编码: bits[63:13] = 8K 对齐虚地址, bits[12:0] = 自由标志
 *             刷回前清零
 *
 * 叶子节点:
 *   entry[3].words[3] 磁盘自由 → 借用为父节点线性地址
 *   entry[4].words[3] 磁盘自由 → 借用为左邻居线性地址
 *   entry[5].words[3] 磁盘自由 → 借用为右邻居线性地址
 *   刷回前清零
 *
 * 注意: entry[1/2].words[3] (sibling block#) 保留磁盘语义不变，
 * 不借用作内存指针。
 *
 * 落地难点 (2026-06-27):
 *   1. 缓存生命周期 — 加载/驱逐钩子、分裂/合并时的双字段同步
 *      (words[1] 线性地址 + words[2] 磁盘块号)
 *   2. 写回竞赛 — 刷回前清零后、IO 完成前，节点数据对并发访问损坏
 *   3. 多线程 B+tree — 借用字段一致性与树操作并发的交互
 *   4. 本设计 + Linux 移植需要私有 buffer cache (不走 page cache)
 * ═══════════════════════════════════════════════════════════════════ */
union node_meta_t {
    struct {
        uint64_t entry_count_minus1 : 8;   /* 有效 entry 数 - 1 */
        uint64_t is_internal_node   : 1;   /* 0=leaf, 1=internal */
        uint64_t reserved           : 3;
        uint64_t parent_ptr         : 52;  /* 父节点物理块号 */
    } fields;
    uint64_t raw;
};
static_assert(sizeof(node_meta_t) == 8, "");

/* ═══════════════════════════════════════════════════════
 * 文件块区间：闭区间 [fblkbase, fblkbase+len]
 * len = 末端偏移，块数 = len + 1；len=0 表示单块，无空集状态
 *
 * 区间序（严格比较器具 fblk_cmp）：
 *   I < J  ⇔  I.R + 1 ≤ J.L
 *   fblk_cmp(a,b) = 1  →  a 完全在 b 之前
 *   fblk_cmp(a,b) = -1 →  b 完全在 a 之前
 *   fblk_cmp(a,b) = 0  →  重叠 (invariant 违反)
 *
 * 叶子 invariant: 同一节点内相邻 extent 满足
 *   fblk_cmp(entries[i], entries[i+1]) == 1
 * ═══════════════════════════════════════════════════════ */
struct f_blkitv {
    uint64_t fblkbase;
    uint64_t len;
};
int inline fblk_cmp(f_blkitv left, f_blkitv right) {
    if (left.fblkbase + left.len < right.fblkbase) return 1;
    if (right.fblkbase + right.len < left.fblkbase) return -1;
    return 0;
}

/* 区间右端点：闭区间覆盖的最后一个块号 */
uint64_t inline f_blkitv_end(f_blkitv itv) {
    return itv.fblkbase + itv.len;
}
/* ═══════════════════════════════════════════════════════
 * B+tree entry (32B = 4 × uint64_t)
 *
 * 统一布局:
 *   words[0] = { V:1 | type:11 | key:52 }
 *   words[1] = { flags:12 | len:52 }
 *   words[2] = { flags:12 | addr:52 }
 *   words[3] = entry[0] → node_meta_t
 *            = 其他 entry → 自由使用 (sibling ptr, flags...)
 *
 * ─── 叶子语义 ───
 *   get_key()      = fblkbase (区间左端)
 *   get_interval() = { fblkbase, len }
 *   get_pbase()    = pblkbase (物理块号)
 *   invariant: fblk_cmp(entries[i], entries[i+1]) == 1
 *
 * ─── 内部语义 (方案 A) ───
 *   所有 entry 统一:
 *     get_key()    = max_R_subtree (该子树最大右端点 fblkbase+len)
 *     get_subptr() = child_i
 *   entry_count = children 数，最大 256
 *   查找: target ≤ entry[i].get_key() → child_i
 * ═══════════════════════════════════════════════════════ */
struct btree_node_entry_t {
    uint64_t words[4];

    /* ─── 通用 ─── */
    bool is_valid() const {
        return (words[0] & KEY_VALID) != 0;
    }
    void set_valid(bool v) {
        if (v) words[0] |=  KEY_VALID;
        else   words[0] &= ~KEY_VALID;
    }

    /* 低 52-bit 的裸 key */
    uint64_t get_key() const {
        return words[0] & MASK_LOW52;
    }
    void set_key(uint64_t k) {
        words[0] = (words[0] & MASK_HIGH12) | (k & MASK_LOW52);
    }

    /* words[0] 高 12 位的类型标志 */
    uint64_t get_key_flags() const {
        return words[0] & KEY_TYPE;
    }
    void set_key_flags(uint64_t f) {
        words[0] = (words[0] & ~KEY_TYPE) | (f & KEY_TYPE);
    }

    /* ─── 叶子语义: 文件 extent ─── */
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

    /* ─── 内部语义: 子树指针 ─── */
    uint64_t get_subptr() const {
        return words[2] & MASK_LOW52;
    }
    void set_subptr(uint64_t s) {
        words[2] = (words[2] & MASK_HIGH12) | (s & MASK_LOW52);
    }

    /* words[2] 高 12 位的地址标志 */
    uint64_t get_addr_flags() const {
        return words[2] & ADDR_FLAGS;
    }
    void set_addr_flags(uint64_t f) {
        words[2] = (words[2] & ~ADDR_FLAGS) | (f & ADDR_FLAGS);
    }

    /* ─── 元数据访问 (entry[0] 专用) ─── */
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
 * B+tree 节点 (8K = 256 entries × 32B = 2 × 4K pages)
 *
 * entry[0].words[3]  → node_meta_t
 * 叶子：所有 entry 的 words[0..2] 存 extent (key,len,pblkbase)
 * 内部：所有 entry 的 words[0] 存 key=max_R(子树 fblkbase+len), words[2] 存 subptr
 * 
 * ═══════════════════════════════════════════════════════ */
struct Bptree_node_t {
    btree_node_entry_t entries[256];

    /* ─── 元数据快捷访问 ─── */
    uint8_t  get_entry_count() const {
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
    void set_parent_ptr(uint64_t p) {//parent_ptr为0则表示根节点
        node_meta_t m = entries[0].get_meta();
        m.fields.parent_ptr = p & MASK_LOW52;
        entries[0].set_meta(m);
    }

    /* ─── 兄弟指针 (同级链表: entry[1/2].words[3]) ─── */
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
struct raven_Inode_t{                                                                                                     
    uint64_t  flags;                  // type + other flags                                                               
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
    uint64_t  data_root;            // 0 = inline                                                                        
    uint32_t  parent_inode;                                                                                               
    uint32_t  generation;   
    uint8_t inline_data[160];
};
static_assert(sizeof(raven_Inode_t)==256,"Inode size must met 256B");
