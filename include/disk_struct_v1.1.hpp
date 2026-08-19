#pragma once
#include "stdint.h"

/* ═══════════════════════════════════════════════════════════════════
 * RavenFS on-disk 结构 v1.1
 *
 * v1 → v1.1 变更主线（对齐 2026-08-19）：
 *
 * 1. 元数据独立：Bptree_meta_t 独立 64B 头（magic 32B + parent_ptr + ptrs[3]），
 *    不再借用 entry 槽位。v1 把 meta/兄弟指针/magic 塞进 entry 槽的 words[3]，
 *    是 bug_bplus_separator_corruption.md 中 entry_count 语义混乱的温床。
 *
 * 2. 指针类型化：ptr_in_idx / parent_ptr_t 位域结构体，替代裸 u64 + MASK 宏。
 *
 * 3. Dentry 树改 hash 路由，上界（hash_max）：
 *    - 内部节点 = 340 个 (hash_max, child) 对，无"最左子树"（上界路由的
 *      好处：每个 hint 自带 child，无孤儿子树；查找单规则
 *      target ≤ hash_max[i] → child[i]）
 *    - 分裂：左半最大 hash 提升为父 separator
 *    - 删除：删叶子最大项 → 必刷父 separator（上界路由对称代价，写死路径；
 *      下界路由删最小项要刷的坑见 v1 bug 3.3）
 *    - 叶子内 (hash, name) 复合排序：hash 定位后碰撞区间内字典序，
 *      名字可二分
 *
 * 4. 长名外置：name 内联 40B；>40 由每目录名字堆（name heap）分配，
 *    full_char_remain_ptr 物理定位（块号[12:63] | 块内偏移[0:11]，12 位 = 4K 块内），
 *    堆只是分配器，解析不依赖堆上下文；文件名硬上限 2047B
 *
 * 5. 容量 + 1 瞬时槽：所有节点数组 = 阶数 + 1，[阶数] 槽是插入瞬时
 *    产物（插入先占槽，检查 count > 阶数 再分裂；单趟处理、内外统一）。
 *    瞬时槽仅存内存，禁止落盘——误落盘会破坏 count 语义（v1 教训）
 *
 * 6. readdir 按 hash 乱序输出（路径解析高频、ls 低频，接受；. / .. 由
 *    readdir 路径合成，不在树内）
 * ═══════════════════════════════════════════════════════════════════ */

constexpr uint8_t INODE_TYPE_REGULAR=0;
constexpr uint8_t INODE_TYPE_DIR=1;
constexpr uint8_t INODE_TYPE_SOFT_SYM=2;

/* 通用指针：52 位地址（块号或 inode 索引）+ 12 位上下文自由位
 * free_to_use 语义由上下文定义（Dentry_t: 存 name_len） */
struct ptr_in_idx{
    uint64_t free_to_use:12;
    uint64_t ptr:52;//块地址或者说是Inode引索
};
/* ── 常量 / 哨兵 ── */
constexpr uint8_t  DENTRY_INLINE_NAME_MAX = 40;   /* name 内联上限：≤40 内联，>40 外置 */
constexpr uint64_t PTR_INVALID            = (~0ULL); /* 通用无效指针哨兵（v1 惯例沿用） */
/* 名字堆控制器（每目录至多 11 个，见 inode.dir_inode_name_heap_level）
 * 异构堆：各堆粒度(bytes_per_bit_log2)与位图容量(total_bits_count_log2)独立，
 * 按需创建；名字按长度分流到粒度匹配的堆，目标是各堆位图 bit 占用率尽量高。
 * 位图 bit[i] 管辖数据区 [i×2^bytes_per_bit_log2, (i+1)×2^bytes_per_bit_log2)。
 * ⚠ 名字数据必须连续：分配 = 找 N 个连续空闲 bit 并置位（非单 bit）。
 * 文件名硬上限 2047B → 外置部分 ≤2007B < 4096B，永不跨块。 */
struct name_heap_controller_t{
    uint64_t bytes_per_bit_log2:6;      // 位图 1 bit 管辖的字节数 log2（实际取值 3~11，8B~2KB）
    uint64_t total_bits_count_log2:6;   // 位图总位数 log2（位图占 FS_block 数 = 2^log2 / 32768 向上取整）
    uint64_t bitmap_base_block_ptr:52;  // 位图所在 FS_block（全局 bitmap 亮灯）
    uint64_t reserved:12;
    uint64_t data_base_block_ptr:52;    // 数据区所在 FS_block（全局 bitmap 亮灯）
};
struct raven_Inode_t{                                                                                                     
    uint64_t  type:2;                  // INODE_TYPE_REGULAR/DIR/SOFT_LINK
    uint64_t  is_data_root_direct_bit:1;
    uint64_t  dir_inode_name_heap_level:2;// 仅 dir 型 inode 有效：名字堆级别
    // level 0: 目录所有名字 ≤40B 全内联（Dentry_t.name），无堆，零开销
    // level 1: inline_data[160] 充当位图；padding[1] 复用为 ptr_in_idx
    //          (ptr=数据区块基址, free_to_use=一个 bit 管辖字节数 log2)；
    //          位图留升级余量（不装满），升 level 2 时新建控制器承接，数据不搬
    // level 2: padding[2](16B) + inline_data[160] = 11 个 name_heap_controller_t
    //          位图/数据全外置，异构堆按需扩容
    // level 3: 预留，未设计
    // 注意：inline_data 语义按 type 分档 —— REGULAR=文件内联数据；
    //       DIR level 1=位图；DIR level 2=控制器数组
    uint64_t  other:59;                                                                                                                                                             
    uint32_t  uid;                                                                                                        
    uint32_t  gid; 
    uint32_t  m_ns_stamp;
    uint32_t  c_ns_stamp;   
    uint32_t  a_ns_stamp;
    uint32_t  link_count;  
    uint64_t  permission_bits;                                                                                                  
    uint64_t  size;                                                                                                       
    uint64_t  mtime;                                                                                                      
    uint64_t  ctime;   
    uint64_t  atime;                                                                                                    
    uint64_t  data_root;            // PTR_INVALID = inline       
    uint64_t padding[2];                                                                                                                                                              
    uint8_t inline_data[160];
}__attribute__((packed));
static_assert(sizeof(raven_Inode_t)==256,"Inode size must met 256B");


/* 父指针：双位解码四态（is_inner_node × is_root_node）
 * 根节点: ptr = 自身 inode 编号（即 "."），非根: ptr = 父节点块号 */
struct parent_ptr_t{
    uint64_t entry_count:10;
    uint64_t is_inner_node:1;
    uint64_t is_root_node:1;//1表示为真，0表示为假
    uint64_t ptr:52;//根节点指向Inode引索，非根节点指向父节点的块引索基址
};

/* ── 节点元数据（独立 64B 头，v1.1 起不再借用 entry 槽位） ──
 *
 * ptrs[3] 按角色解码：
 *   非根叶子: ptrs[0]=预留  ptrs[1]=前驱叶子块号  ptrs[2]=后继叶子块号
 *   非根内部: ptrs[0]=预留  ptrs[1]=预留          ptrs[2]=预留
 *   根叶子  : ptrs[0]=预留  ptrs[1]=".." inode    ptrs[2]=预留
 *   根内部  : ptrs[0]=预留  ptrs[1]=".." inode    ptrs[2]=预留
 *
 * 内部节点无最左子树（上界路由：N hints = N children，无孤儿子树）。
 * 根叶子/根内部不做同级链表，ptrs[2] 不参与。
 */
struct Bptree_meta_t{
    uint64_t magics[4];          // 256-bit 扫描签名（全盘恢复识别节点类型）
    parent_ptr_t parent_ptr;     // 根: 自身 inode(.)；非根: 父节点块号
    ptr_in_idx ptrs[3];          // 角色解码见上
};

/* 文件块区间：闭区间 [fblkbase, fblkbase+len]，len=末端偏移，块数=len+1 */
struct extent_t{
    uint64_t length;             // 区间长度（末端偏移，闭区间语义继承 v1）
    uint64_t fblkbase;           // 文件逻辑块号起点
    ptr_in_idx block_base;       // 物理 FS_block 起点
};

/* 目录项：name 内联 40B，>40 走外置
 * inode_ptr.free_to_use = name_len（全名长度，0~4095）
 * hash = 名字 64 位 hash（算法待定；叶子内 (hash, name) 复合排序，
 *        路由只看 hash，碰撞区间内按名字字典序比较） */
struct Dentry_t{
    ptr_in_idx inode_ptr;            // free_to_use = name_len
    uint64_t hash;                   // 名字 hash（查找主键）
    uint64_t full_char_remain_ptr;   // name_len>40: 名字堆分配器产生的物理定位
                                     // 块号[12:63]|块内偏移[0:11] 指向 name[40..]；否则 PTR_INVALID
    char name[DENTRY_INLINE_NAME_MAX];
};

/* 内部节点路由 hint：上界语义 —— hash_max = 子树最大 hash
 * 查找: target ≤ hash_max[i] → sub_ptr[i]
 * 分裂: 左半最大 hash 提升为父 separator
 * 删除: 叶子最大项被删 → 刷父 hash_max（必走路径） */
struct Dentry_hint_t{
    ptr_in_idx sub_ptr;
    uint64_t hash_max;
};

/* 区段树内部节点路由 hint：上界语义 —— key = 子树最大右端点 (fblkbase+len)
 * （v1 max_R 语义继承，查找 target ≤ key → sub_node） */
struct extent_hint_t{
    uint64_t key;
    ptr_in_idx sub_node_ptr;
};

/* 区段树 B+树，337 阶；inner hints = 338 槽，显然[337]是插入瞬时存在的产物，无论内外 */
struct Bptree_extent_inner_node_t{
    Bptree_meta_t meta;
    extent_hint_t extents[338];
    uint64_t padding[340];
}__attribute__((packed));
static_assert(sizeof(Bptree_extent_inner_node_t)==8192,"" );
/* 区段树叶子，337 阶；extents = 338 槽，[337] 为插入瞬时槽（同 inner） */
struct Bptree_extent_leaf_node_t{
    Bptree_meta_t meta;
    extent_t extents[338];
    uint64_t padding[2];
}__attribute__((packed));
static_assert(sizeof(Bptree_extent_leaf_node_t)==8192,"" );

/* Dentry B+树，126 阶；显然[126]是插入瞬时存在的产物，无论内外 */
struct Bptree_dentry_leaf_node_t{
    Bptree_meta_t meta;
    Dentry_t entries[127];
}__attribute__((packed));
static_assert(sizeof(Bptree_dentry_leaf_node_t)==8192,"" );
/* Dentry B+树内部节点，126 阶；hints = 127 槽，[126] 为插入瞬时槽 */
struct Bptree_dentry_inner_node_t{
    Bptree_meta_t meta;
    Dentry_hint_t hints[127];
    uint64_t padding[762];
}__attribute__((packed));
static_assert(sizeof(Bptree_dentry_inner_node_t)==8192,"" );

/* ═══════════════════════════════════════════════════════
 * RavenSuperblock — 统一结构体
 *
 * Primary Superblock（上岗）：
 *   位置：裸盘 LBA[0] / GPT 分区 offset=0
 *   不在 FS_block 世界内，bitmap 不管理
 *   角色：FS_block 世界的定义者和入口
 *   backup_prev = PTR_INVALID（哨兵）
 *   backup_next = 链表头（第一个 Backup 的 FS_block 号）
 *   backup_count = 备份总数
 *
 * Backup Superblock（备胎）：
 *   位置：随机散落在 FS_block 世界内，bitmap 标记 allocated
 *   内容：与 Primary 全同（magic ~ crc32 所有字段一致）
 *   backup_prev = 前驱 Backup 的 FS_block 号（PTR_INVALID=链头）
 *   backup_next = 后继 Backup 的 FS_block 号（PTR_INVALID=链尾）
 *   backup_count = 备份总数（与 Primary 交叉校验）
 * ═══════════════════════════════════════════════════════ */
struct RavenSuperblock {
    uint64_t  magic;                // "RAVN" = 0x5241564E
    uint32_t  version;              // 格式版本号（v1.1 起 bump）
    uint32_t  pad0;                 // 对齐，替代 LBA_size（NVMe Identify 已知，不落盘）
    uint64_t  world_base_lba;       // FS_block[0] 在设备 LBA 空间的起始 LBA
    uint64_t  total_blocks;         // FS_block 世界总块数

    uint64_t  inode_array_count;         // Inode 上限（推导出 inode_blocks, inode_bitmap_blocks）
    uint64_t  root_inode;                // 根目录 inode 索引
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;

    /* ── 备份链表（角色双解码） ── */
    uint64_t  backup_prev;          // Primary: PTR_INVALID（哨兵）| Backup: 前驱 FS_block 号
    uint64_t  backup_next;          // Primary: 链头 | Backup: 后继 FS_block 号
    uint32_t  backup_count;         // 两者均存，交叉校验

    uint8_t   pad[3960];
}__attribute__((packed));
static_assert(sizeof(RavenSuperblock)==4096,"RavenSuperblock size must be 4096B");
