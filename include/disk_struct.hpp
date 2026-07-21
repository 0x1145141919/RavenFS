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
constexpr uint8_t INODE_TYPE_REGULAR=0;
constexpr uint8_t INODE_TYPE_DIR=1;
constexpr uint8_t INODE_TYPE_SOFT_LINK=2;

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
        uint64_t is_root_node   : 1;
        uint64_t reserved           : 2;
        uint64_t parent_ptr         : 52;  /* 父节点物理块号/根节点 */
    } fields;
    uint64_t raw;
}__attribute__((packed));
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
 * entry[1].words[3]  → prev_sibling
 * entry[2].words[3]  → next_sibling
 * entry[3..6].words[3] → magic1..magic4 (256-bit 扫描签名)
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
    void set_parent_ptr(uint64_t p) {//非根→父节点块号 | 根→所属inode编号，由is_root_node区分
        node_meta_t m = entries[0].get_meta();
        m.fields.parent_ptr = p & MASK_LOW52;
        entries[0].set_meta(m);
    }

    /* ─── 扫描签名 (entry[3..6].words[3], 256-bit, K=4) ─── */
    uint64_t get_magic1() const { return entries[3].words[3]; }
    void set_magic1(uint64_t v) { entries[3].words[3] = v; }
    uint64_t get_magic2() const { return entries[4].words[3]; }
    void set_magic2(uint64_t v) { entries[4].words[3] = v; }
    uint64_t get_magic3() const { return entries[5].words[3]; }
    void set_magic3(uint64_t v) { entries[5].words[3] = v; }
    uint64_t get_magic4() const { return entries[6].words[3]; }
    void set_magic4(uint64_t v) { entries[6].words[3] = v; }

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
    uint64_t  type:2;                  // INODE_TYPE_REGULAR/DIR/SOFT_LINK
    uint64_t  is_data_root_direct_bit:1;
    uint64_t  other:61;                                                                                                                                                             
    uint32_t  uid;                                                                                                        
    uint32_t  gid; 
    uint32_t  m_ns_stamp;
    uint32_t  c_ns_stamp;   
    uint32_t  a_ns_stamp;
    uint64_t  permission_bits;                                                                                                  
    uint64_t  size;                                                                                                       
    uint64_t  mtime;                                                                                                      
    uint64_t  ctime;   
    uint64_t  atime;                                                                                                    
    uint64_t  data_root;            // ~0ull = inline       
    uint32_t  link_count;    
    uint64_t padding[2];                                                                                                                                                              
    uint8_t inline_data[160];
}__attribute__((packed));
static_assert(sizeof(raven_Inode_t)==256,"Inode size must met 256B");

/* ═══════════════════════════════════════════════════════
 * Dentry B+tree — 目录项专用节点
 *
 * 与文件区段 B+tree (Bptree_node_t) 不同 entry/node 格式。
 * 所有 inode 类型为 RAVEN_DIR 的数据内容由此树组织。
 *
 * Dentry_Bptree_node_t (8K):
 *   Dentry_head_t head (128B)
 *   Dentry_common_t entries[63] (63 × 128B = 8064B)
 * ═══════════════════════════════════════════════════════ */

union dentry_node_meta_t {
    struct {
        uint64_t entry_count    : 6;   /* 有效 entry 数（0~63，根节点最少 1） */
        uint64_t reserved0      : 2;
        uint64_t is_internal_node : 1; /* 0=leaf, 1=internal */
        uint64_t is_root_node   : 1;   /* 1=根节点 */
        uint64_t reserved       : 2;
        uint64_t parent_ptr     : 52;  /* 非根→父节点块号 | 根→所属 inode 编号 */
    } fields;
    uint64_t raw;
}__attribute__((packed));
static_assert(sizeof(dentry_node_meta_t) == 8, "");

struct Dentry_head_t {
    dentry_node_meta_t head;        // 8B: 节点元数据
    uint64_t           ptr1;        // 8B: 内部→最左子树 | 叶子→前驱
    uint64_t           ptr2;        // 8B: 内部→空闲     | 叶子→后继
    uint64_t           reserved0;   // 8B: 预留
    uint64_t           magic1;      // 8B: 256-bit 扫描签名
    uint64_t           magic2;      // 8B: 256-bit 扫描签名
    uint64_t           magic3;      // 8B: 256-bit 扫描签名
    uint64_t           magic4;      // 8B: 256-bit 扫描签名
    uint64_t           reserved2[8]; // 64B: 扩展预留
}__attribute__((packed));
static_assert(sizeof(Dentry_head_t) == 128, "");

struct Dentry_common_t {
    uint64_t ptr;        // 8B: 叶子→inode编号 | 内部→child块号
    char     name[120];  // 120B: 文件名 (字典序 key)
}__attribute__((packed));
static_assert(sizeof(Dentry_common_t) == 128, "");

struct Dentry_Bptree_node_t {
    Dentry_head_t    head;
    Dentry_common_t  entries[63];
}__attribute__((packed));
static_assert(sizeof(Dentry_Bptree_node_t) == 8192, "");

/* ═══════════════════════════════════════════════════════
 * RavenSuperblock — 统一结构体
 *
 * Primary Superblock（上岗）：
 *   位置：裸盘 LBA[0] / GPT 分区 offset=0
 *   不在 FS_block 世界内，bitmap 不管理
 *   角色：FS_block 世界的定义者和入口
 *   backup_prev = ~0ull（哨兵）
 *   backup_next = 链表头（第一个 Backup 的 FS_block 号）
 *   backup_count = 备份总数
 *
 * Backup Superblock（备胎）：
 *   位置：随机散落在 FS_block 世界内，bitmap 标记 allocated
 *   内容：与 Primary 全同（magic ~ crc32 所有字段一致）
 *   backup_prev = 前驱 Backup 的 FS_block 号（~0ull=链头）
 *   backup_next = 后继 Backup 的 FS_block 号（~0ull=链尾）
 *   backup_count = 备份总数（与 Primary 交叉校验）
 * ═══════════════════════════════════════════════════════ */
struct RavenSuperblock {
    uint64_t  magic;                // "RAVN" = 0x5241564E
    uint32_t  version;              // 格式版本号
    uint32_t  pad0;                 // 对齐，替代 LBA_size（NVMe Identify 已知，不落盘）
    uint64_t  world_base_lba;       // FS_block[0] 在设备 LBA 空间的起始 LBA
    uint64_t  total_blocks;         // FS_block 世界总块数

    uint64_t  inode_array_count;         // Inode 上限（推导出 inode_blocks, inode_bitmap_blocks）
    uint64_t  root_inode;                // 根目录 inode 索引
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;

    /* ── 备份链表（角色双解码） ── */
    uint64_t  backup_prev;          // Primary: ~0ull（哨兵）| Backup: 前驱 FS_block 号
    uint64_t  backup_next;          // Primary: 链头 | Backup: 后继 FS_block 号
    uint32_t  backup_count;         // 两者均存，交叉校验

    // pad 尺寸由你补，跑一下 static_assert 确认
    uint8_t   pad[3960];
}__attribute__((packed));
static_assert(sizeof(RavenSuperblock)==4096,"RavenSuperblock size must be 4096B");
