#pragma once
#include "../disk_struct_v1.1.hpp"
#include "../magics.h"

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>

/* ═══════════════════════════════════════════════════════════════════
 * RavenFS v1.1 — 幼年期上帝类（RavenFS_juvenile）
 *
 * 范围：仅对 root_dir 进行操纵，专用于构建 dentry B+tree 操作原型。
 * 本头文件是接口契约草案，实现待写；设计约束如下：
 *
 * 1. 零缓存：块 / inode 访问全部直写盘（pwrite/pread），不建任何缓存层。
 *    inode_read/inode_write 每次直接落盘，不存在脏页/回写概念。
 * 2. 双位图全量进内存：block bitmap 与 inode bitmap 在 mount 时一次性
 *    读入内存数组（磁盘布局位序原样映射），alloc/free 只改内存态，
 *    **析构（卸载）时全量写回**（速度优先；幼年期接受掉电丢位图更新，
 *    不追求事务安全）。两组接口：block 组 / inode 组。
 * 3. 名字硬限制 ≤40B：level 0 目录全内联（Dentry_t.name），名字堆
 *    未实现，>40B 一律 RC_ERR_NAME_LEN。后续名字堆接入后解除。
 * 4. 删除 = 物理删除（v1.1 §6.5）：memmove + 刷父 hash_max（上界路由
 *    对称代价，必走路径）；欠载不合并（min occupancy 待定，幼年期跳过）。
 * 5. hash 用 XXH64（seed=0，v1.1 §9 已定；64 位熵 + 跨挂载稳定，
 *    抗 DoS 升级路径：siphash + SB 持久 key，接口签名不变）。
 * 6. backup 链容灾不做（幼年期只认 Primary SB：magic/version/crc32/
 *    bitmap 自描述四连验）。
 * ═══════════════════════════════════════════════════════════════════ */

typedef uint64_t result_code_t;

/* 磁盘格式版本：v1=1，v1.1=2（disk_layout_v1.1.md §1：v1.1 起 bump）
 * 与 mkfs/mkfs.hpp 同值；挂载校验用 */
constexpr uint32_t RAVENFS_VERSION = 2;

/* ── 结果码 ── */
enum : result_code_t {
    RC_OK = 0,
    RC_ERR_OPEN,            /* 设备打开失败 */
    RC_ERR_MAGIC,           /* Primary SB magic 不符 */
    RC_ERR_VERSION,         /* 版本 != RAVENFS_VERSION(v1.1=2) */
    RC_ERR_CRC,             /* Primary SB crc32 校验失败 */
    RC_ERR_BITMAP_SELF,     /* bitmap 自描述校验失败（尾巴三段非全 1） */
    RC_ERR_EXIST,           /* 同名 dentry 已存在 */
    RC_ERR_NOT_FOUND,       /* dentry 未找到 */
    RC_ERR_NO_BLOCK,        /* FS_block 无空闲 */
    RC_ERR_NO_INODE,        /* inode 槽无空闲 */
    RC_ERR_NAME_LEN,        /* 名字 >40B（名字堆未实现） */
    RC_ERR_IO,              /* pread/pwrite 失败 */
    RC_ERR_STATE,           /* 内部状态异常（根节点角色不符等） */

    RC_NEED_SPLIT = 0x1000, /* 内部使用：本层分裂，需父层处理 */
};

/* ── readdir 输出项 ──
 * 幼年期名字 ≤40B，character[256] 留余量（含结尾 '\0'）。 */
struct dentry_out {
    uint64_t inum;          /* 目标 inode 编号 */
    uint32_t name_len;      /* 名字长度（不含 '\0'） */
    char     character[256];
};
struct dentry_bptree_path_t {
    uint64_t blocks[64];
    uint16_t  child_idx[64];
    int      depth;
    bool  is_finally_hit;
    // bool  is_break_biggest_record;
};
class RavenFS_juvenile {
public:
    /* 构造即挂载：打开设备 → 读 Primary SB → 四连验 → 双位图全量入内存。
     * 失败时 *res_code 给出 RC_ERR_*，对象处于未挂载态（fd=-1）。 */
    RavenFS_juvenile(const char *blk_device_path, uint64_t *res_code);
    ~RavenFS_juvenile();            /* 析构即卸载：释放位图内存 + 关 fd */

    RavenFS_juvenile(const RavenFS_juvenile &) = delete;
    RavenFS_juvenile &operator=(const RavenFS_juvenile &) = delete;

    bool is_mounted() const { return fd >= 0; }

    /* ══════════════════════════════════════════════════════════
     * root_dir 操纵接口（dentry B+tree 原型构建区）
     * 全部以根目录为唯一操作对象，path 概念不存在。
     * ══════════════════════════════════════════════════════════ */

    /* 创建名为 name 的普通文件：
     *   inode_alloc → 初始化(REGULAR, 0644, link_count=1, data_root=PTR_INVALID
     *   内联空文件) → inode_write → dentry 树插入。
     * name 已存在 → RC_ERR_EXIST；>40B → RC_ERR_NAME_LEN。 */
    result_code_t root_dir_insert_dentry(const char *name);

    /* 物理删除：dentry 树移除（memmove + 刷父 hash_max）→ inode_free。
     * 未找到 → RC_ERR_NOT_FOUND。 */
    result_code_t root_dir_rm_dentry(const char *name);

    /* 查找：命中 → RC_OK 且 *out_inum 输出；未命中 → RC_ERR_NOT_FOUND。 */
    result_code_t root_dir_lookup(const char *name, uint64_t *out_inum);

    /* 遍历 root_dir：按 hash 乱序（v1.1 §6.4 接受）。
     * 返回条目数 n（≥0），失败返回负错误码。
     * 成功时 *out_buf 指向堆上分配的连续数组 dentry_out[n]，
     * 使用时直接取 (*out_buf)[i]（线性数组，非指针数组）。
     * 调用者负责释放：delete[] *out_buf。 */
    int64_t root_dir_readdir(dentry_out **out_buf);

    

    /* ── 调试 ── */
    void dump_info() const;                     /* SB/几何/位图占用率/根节点 */

private:
    result_code_t dir_insert_dentry(uint64_t inode_idx,const char *name);

    result_code_t dir_rm_dentry(uint64_t inode_idx,const char *name);

    result_code_t dir_lookup(uint64_t inode_idx,const char *name, uint64_t *out_inum);
    int64_t dir_readdir(uint64_t inode_idx,dentry_out **out_buf);
/* ══════════════════════════════════════════════════════════
     * 位图接口组 A：FS_block 分配器（全量内存 bitmap）
     *   内存：block_bits[bitmap_blocks×4096]，bit i ↔ FS_block[i]
     *   元数据尾巴三段在 mkfs 已亮灯，alloc 天然跳过，无需特判。
     *   失败哨兵：PTR_INVALID（0 是合法块号）
     *   内部游标：bm_cursor（扫到尾巴绕回 2，块 0-1 = 根 dentry 节点）
     * ══════════════════════════════════════════════════════════ */
    /* 硬分配：必须 require_count 块连续 + 起始块号 2^align_log2 对齐
     * （align_log2 ∈ [0,52]；B+tree 节点 2 块连续即 align≥1 起步）。
     * 不满足则整体失败，返回 PTR_INVALID（原子，不部分分配）。 */
    uint64_t block_alloc_hard(uint64_t require_count, uint8_t align_log2);
    /* 软分配：优先速度，预期碎片。从游标处找第一个连续空闲区间
     * （长度 ≥1 即可），实际分配 = min(区间长度, require_count)。
     * *remain_count 输出缺口数 = require_count − 实际分配（0 = 全满足，
     * 调用者可据此再调一次补足）。一块都拿不到 → PTR_INVALID
     * 且 *remain_count = require_count。 */
    uint64_t block_alloc_soft(uint64_t require_count, uint64_t *remain_count);
    /* 区间释放：blkbase 起 count 块连续清位 */
    void     block_free(uint64_t blkbase, uint64_t count);
    bool     block_test(uint64_t blk) const;

    /* ══════════════════════════════════════════════════════════
     * 位图接口组 B：inode 槽分配器（全量内存 bitmap）
     *   内存：inode_bits[inode_bitmap_blocks×4096]，bit i ↔ inode[i]
     *   失败哨兵：PTR_INVALID（inode 0 = root，alloc 从 1 起扫）
     * ══════════════════════════════════════════════════════════ */
    uint64_t inode_alloc();
    void     inode_free(uint64_t inum);
    bool     inode_test(uint64_t inum) const;
    /* ── 块 I/O（零缓存，直写盘）── */
    uint64_t fblock_to_byte(uint64_t blk) const { return 4096 + blk * 4096; }
    void read_block(uint64_t blk, void *buf);        /* pread 4096B */
    void write_block(uint64_t blk, const void *buf); /* pwrite 4096B */
    /* 批量块 I/O（零缓存）：blk 起连续 blk_len 个 FS_block 一次读写。
     * RC_OK / RC_ERR_STATE(参数非法) / RC_ERR_IO(短读短写) */
    result_code_t read_buff(uint64_t blk, uint64_t blk_len, void *buf);
    result_code_t write_buff(uint64_t blk, uint64_t blk_len, void *buf);
    /* ── inode I/O（零缓存）── */
    void inode_read(uint64_t inum, raven_Inode_t *out) const;
    void inode_write(uint64_t inum, const raven_Inode_t *in);

    /* ── 位图核心 ── */
    bool bitmaps_load();     /* mount 时：读入双位图 + 自描述校验(§6.6) */
    void bitmaps_flush();    /* 析构时：双位图全量写回（各 pwrite 一次） */
    /* 区间分配：从 start 起找 count 个连续空闲 bit，起始位满足
     * 2^align_log2 对齐；成功返回起始位号，失败 PTR_INVALID。
     * block 组 hard/soft 共用底层。 */
    uint64_t bm_alloc_range(uint8_t *bits, uint64_t nbits, uint64_t count,
                            uint8_t align_log2, uint64_t start);
    uint64_t bm_alloc_first(uint8_t *bits, uint64_t nbits,
                            uint64_t start); /* 单 bit first-fit（inode 组用） */
    void bm_set(uint8_t *bits, uint64_t bit, bool v);

    /* ── 名字 hash（XXH64 seed=0，v1.1 §9 已定；跨挂载稳定）── */
    uint64_t name_hash(const char *name) const;

    /* ── dentry B+tree（仅 root_dir 单树）──
     * 树根 = root_dentry_blk，角色可能为根叶子或根内部。
     * 上界路由：target_hash ≤ hints[i].hash_max → sub_ptr[i]。
     * 叶子内 (hash, name) 复合序；瞬时槽 [126] 只存内存不落盘。 */

    /* 分裂结果（喂回父层；根分裂由 root_dir_ 层处理建新根） */
    struct dentry_split_t {
        uint64_t new_blk;        /* 新兄弟块号（父层 hint 的 sub_ptr） */
        uint64_t separator_hash; /* 左半最大 hash（父层新增 hint 的 hash_max） */
        bool     root_split;     /* 根分裂标志（由最上层消费） */
    };

    /* ── 内部状态 ── */
    int  fd = -1;                       /* 设备 fd */
    RavenSuperblock sb;                 /* Primary SB（挂载时读入） */

    uint64_t total_blocks   = 0;
    uint64_t inode_count    = 0;
    /* 尾巴三段（block_model §3.2 链式推导，mkfs 同源） */
    uint64_t bitmap_start       = 0;
    uint64_t bitmap_blocks      = 0;
    uint64_t inode_array_base   = 0;
    uint64_t inode_blocks       = 0;
    uint64_t inode_bm_base      = 0;   /* inode_array_bitmap_base */
    uint64_t inode_bm_blocks    = 0;

    uint8_t *block_bits = nullptr;      /* 全量内存位图（bitmap_blocks×4096） */
    uint8_t *inode_bits = nullptr;      /* 全量内存位图（inode_bm_blocks×4096） */
    uint64_t bm_cursor = 2;             /* block 分配游标（块 0-1 = 根 dentry 节点） */

    uint64_t root_inum      = 0;        /* 恒 0（根目录 inode 索引） */
    uint64_t root_dentry_blk = 0;       /* root inode.data_root（树根块号） */

    /* ── CRC32（Castagnoli，SB 校验 + 临时 name hash）── */
    static uint32_t crc32_table[256];
    static bool     crc32_ready;
    static void     crc32_init();
    static uint32_t crc32_compute(const void *data, size_t len);
    struct name_cmlpx_t{
        char*name;
        uint64_t hash;
    };
    result_code_t dentry_Bptree_walk(uint64_t root_blk_idx,dentry_bptree_path_t*trace,name_cmlpx_t*name);
    struct leaf_node_result_t{
        uint16_t greates_lower_bound_idx;
        bool is_idx_hitted;
    };
    /* 叶子查找无下界哨兵：greates_lower_bound_idx == 此值 → 无下界，
     * 插入位置 = 0（最左插入）。有效索引范围 [0, 126]，0xFFFF 永不冲突。 */
    static constexpr uint16_t DENTRY_LEAF_GLB_NONE = 0xFFFF;
    struct split_result_t{
        result_code_t rc;
        uint64_t low_node_blkidx;
        uint64_t high_node_blkidx;
        uint64_t low_node_maxhash;
    };
    leaf_node_result_t dentry_leaf_node_lookup(Bptree_dentry_leaf_node_t*leaf,name_cmlpx_t*name);/* is_idx_hitted=精准命中 */
    uint16_t dentry_inner_node_lookup(Bptree_dentry_inner_node_t*node,uint64_t hash);
    result_code_t dentry_leaf_entry_insert(uint64_t leaf_blk_base_idx,name_cmlpx_t* name,uint16_t idx,uint64_t&result_flags);//成功时result_flags有效，bits[0]代表内部探测是否需要分裂，bits[1]表示是否要改变父节点的对应entry的最大值
    result_code_t dentry_leaf_entry_delete(uint64_t leaf_blk_base_idx,uint16_t idx,uint64_t&result_flags);//成功时result_flags有效，bits[0]代表内部探测是否需要合并，bits[1]表示是否要改变父节点的对应entry的最大值
    split_result_t dentry_leaf_split(uint64_t victim_node_blkbase);//会内部进行叶子链表的维护，不会进行父节点的修改
    split_result_t dentry_inner_split(uint64_t victim_node_blkbase);//不会进行父节点的修改
    result_code_t dentry_spilt_parent_apply(uint64_t parent_node_blkbase,uint16_t victim_in_node_id,split_result_t result,bool&is_parent_spilt_acquire);
    result_code_t dentry_node_balance(uint64_t parent_node_blkbase,uint16_t idx,bool&is_parent_need_balance);//给定的父节点以及相应引索，让其平衡则是，若节点到达平衡阈值，向前后邻居先看是否存在总entry数目小于等于126的,小于126则合并，不满足则抹匀
    /* 分裂后新建根内部节点（低/高两个子节点提升为新根的 hints[0]/[1]）：
     * 分配 2 块 → meta 初始化 → 填充 hints → 更新两子节点父指针 → 写回
     * → 更新 root inode.data_root。low_blk/low_max 为左子块号及最大 hash，
     * high_blk 为右子块号（其最大 hash 由本函数读节点现算）。 */
    result_code_t dentry_new_root_inner(uint64_t low_blk, uint64_t low_max,
                                        uint64_t high_blk, uint64_t root_inode_idx);
};
bool is_dentry_node(Bptree_meta_t*meta);
bool is_extent_node(Bptree_meta_t*meta);
/* 名字长度把关：strlen ≤ 39（名字堆未实现，全内联） */
bool tmp_name_len_limiter(const char*name);
/* 节点最大 hash：叶子末项 entry.hash / 内部末项 hint.hash_max */
uint64_t node_max_hash(const void *node_buf);
