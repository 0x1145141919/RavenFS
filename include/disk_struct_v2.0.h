#pragma once
/*
 * ═══════════════════════════════════════════════════════════════════════════
 * RavenFS on-disk 结构 —— v1.2（**C / C++ 双兼容**）
 *
 * 来源真值：
 *   · include/disk_struct_v1.1.hpp          （全量字段基座）
 *   · Docs/disk_layout_v1.2_inode.md        （v1.2 inode 语义重定义，权威详版）
 *   · Docs/linux/ravenfs-linux-ko-requirements-spec.md  §4.5.1 / §6-E2（摘要）
 *
 * 本文件相对 v1.1 的变更有两条主线：
 *
 * 【主线一 · 内容】v1.2 inode 语义重定义（**字节 offset / 尺寸不变**，仍 256B）
 *   · 第一字（word1）：  type:2 | direct:1 | other:61   →   type:6 | direct:1 | i_flags:57
 *   · permission_bits：  只写 12 位权限（全仓 0 处读）    →   mode:12 | xattr_ptr:52
 *   · inline_data：      内容                          →   内容 / 无数据类型(CHR/BLK) 存 rdev@[0:4]
 *   · type 值域：        REG=0 DIR=1 SYM=2             →   {FIFO1,CHR2,DIR4,BLK6,REG8,LNK10,SOCK12}
 *   · 新增：             xattr 块（单块自描述，一个 52 位块指针，超限 -E2BIG）
 *   · 版本：             RAVENFS_VERSION 2             →   3（v1.1 盘不兼容 → reformat）
 *
 * 【主线二 · 形式】**用「掩码 + 宏」替代位域（bit-field）**
 *   理由（为什么全盘换掉位域）：
 *     1. C 与 C++ 的位域布局是实现定义的，跨语言（ko=C / god=C++）二进制对接不可靠；
 *     2. 位域无法安全地对「非对齐/跨字节」的落盘字直接取址读写（bio/bh 场景按字节拷）；
 *     3. 掩码+宏的表达是显式整数运算 → 字节序、偏移、取值域全程可静态断言、可单元测试。
 *   形式：每个落盘字声明为裸 `uintN_t`；字段用 `RFS_<域>_<字段>_{SHIFT,MASK}`
 *         + `RFS_<域>_<字段>_GET(word)` + `RFS_<域>_PACK(...)` / `_SET_xxx(word,val)` 访问。
 *   每个结构后 `RFS_STATIC_ASSERT(sizeof(...) == N)` 锁死布局。
 *
 * ⚠️ 与 v1.1 代码的落差：god / mkfs / audit 现有代码访问 `.type` / `.permission_bits` /
 *    `.ptr` 等位域成员，本头文件不再提供这些成员 → 迁移动作见 disk_layout_v1.2_inode.md §8 / §8.1。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifdef __KERNEL__
#  include <linux/types.h>   /* 内核：提供 u8..u64，并在 linux/types.h 里 typedef 到 uintN_t */
#else
#  include <stdint.h>        /* 用户态 / god / C++ */
#  include <stddef.h>        /* NULL */
#endif

/* ─────────────────────────── 语言 / 编译器适配 ─────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
#  define RFS_PACKED __attribute__((packed))
#else
#  define RFS_PACKED
#endif

#if defined(__cplusplus)
#  define RFS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#  define RFS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#  define RFS_SA_CAT_(a, b) a##b
#  define RFS_SA_CAT(a, b) RFS_SA_CAT_(a, b)
#  define RFS_STATIC_ASSERT(cond, msg) \
       typedef char RFS_SA_CAT(rfs_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

/* ─────────────────────────── 通用位宽掩码 ─────────────────────────── */
#define RFS_MASK6   ((1ULL <<  6) - 1)   /* 0x0000_0000_0000_003F */
#define RFS_MASK12  ((1ULL << 12) - 1)   /* 0x0000_0000_0000_0FFF */
#define RFS_MASK13  ((1ULL << 13) - 1)   /* 0x0000_0000_0000_1FFF */
#define RFS_MASK51  ((1ULL << 51) - 1)   /* 0x0007_FFFF_FFFF_FFFF */
#define RFS_MASK52  ((1ULL << 52) - 1)   /* 0x000F_FFFF_FFFF_FFFF */
#define RFS_MASK57  ((1ULL << 57) - 1)   /* 0x01FF_FFFF_FFFF_FFFF */

/* ═══════════════════════════════════════════════════════════════════════════
 * 结果码（全局共享；2026-10-05 实体迁移至 ravenfs/rc.hpp，本盘头仅转引）
 *   结果码是纯运行时常量，与落盘字节布局无关：独立成轻量头后，缓存/分配器
 *   等运行时模块可只拿 RC_*，不必连整份 on-disk 结构契约一起拖进来。
 *   本盘头仍经此 include 保持 RC_* 对全部盘侧代码可见（对外符号集不变）。
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "ravenfs/rc.hpp"

/* ═══════════════════════════════════════════════════════════════════════════
 * 文件类型常量（v1.2：镜像 S_IFMT，值 = S_IFMT >> 12；其余值含 0 非法）
 * ═══════════════════════════════════════════════════════════════════════════ */
enum rfs_inode_type {
    INODE_TYPE_FIFO      = 1,    /* S_IFIFO  0o010000 */
    INODE_TYPE_CHR       = 2,    /* S_IFCHR  0o020000 */
    INODE_TYPE_DIR       = 4,    /* S_IFDIR  0o040000 */
    INODE_TYPE_BLK       = 6,    /* S_IFBLK  0o060000 */
    INODE_TYPE_REGULAR   = 8,    /* S_IFREG  0o100000 */
    INODE_TYPE_SOFT_SYM  = 10,   /* S_IFLNK  0o120000 */
    INODE_TYPE_SOCK      = 12    /* S_IFSOCK 0o140000 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 常量 / 哨兵
 * ═══════════════════════════════════════════════════════════════════════════ */
#define RAVENFS_VERSION          0u          /* v2.0 = 0（v1.2 = 3，不兼容） */
#define DENTRY_INLINE_NAME_MAX   40u         /* name 内联上限：≤40 内联，>40 外置 */
#define RAVENFS_MAX_NAME_LEN     (DENTRY_INLINE_NAME_MAX + 512u)  /* 552B */
/* PTR_INVALID —— 通用无效指针哨兵，实体已随结果码移至 ravenfs/rc.hpp */
#define NAME_HEAP_INACTIVE       RFS_MASK52  /* 名字堆未激活（52 位全 1；禁用 0） */
#define RFS_XATTR_NONE           RFS_MASK52  /* xattr_ptr 无 xattr（52 位全 1；禁用 0） */

/* ── v2.0 超级块 magic（256-bit；见 RavenSuperblock.magic[4]）──
 * magic[0] = 固定前缀：字节[0:7] = "RAVENFS\0"（下方为小端序 u64 表达）；
 * magic[1..3] = 每卷随机（卷身份）。识别只比对 magic[0]（前缀匹配），随机部分由
 * crc32 兜底。与 v1.x 单字 "RAVN"（magics.h 的 SUPERBLOCK_MAGIC）**不兼容**。 */
#define RFS_SB_MAGIC_W0          0x0053464E45564152ULL  /* bytes[0:7] = 'R','A','V','E','N','F','S',0（小端）*/

/* ═══════════════════════════════════════════════════════════════════════════
 * ★ FS 契约级硬编码（ABI）：根目录 inode 索引 ≡ 0
 *
 *   root_dir 的 inode 恒为**逻辑索引 0** ⇒ inode 数据面逻辑空间第 0 槽
 *   （piece 0 的第 0 个 256B）。配合 v1.2 §7.A 的 `st_ino = idx + 1`：根 st_ino ≡ 1。
 *
 *   · 本值是**格式契约 / 不变量**，**不是**「mkfs 局部实现细节」——v1.2 曾把 root
 *     定义为 mkfs 局部值（消费者读 SB.root_inode），**v2.0 正式推翻**：SB 已无 root
 *     字段，全部端**硬编码 0**（见 RavenSuperblock 内注解）。
 *   · 后果：idx 0 **永久归 root、分配器永不外发**；mkfs 初始 fr 必从 idx 1 起。
 *   · 纪律：mkfs / god / ko / 工具**只准引用本常量**，禁止散落字面量 0。
 * ═══════════════════════════════════════════════════════════════════════════ */
#define RFS_ROOT_INODE_IDX       0ULL

/* ═══════════════════════════════════════════════════════════════════════════
 * ptr_in_idx —— 通用指针：free:12 | ptr:52
 *   free_to_use 语义由上下文定义（ddentry_t: 存 name_len）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct ptr_in_idx {
    uint64_t v;                     /* [0:12)=free_to_use  [12:64)=ptr(52) */
};
RFS_STATIC_ASSERT(sizeof(struct ptr_in_idx) == 8, "ptr_in_idx must be 8B");

#define RFS_PTR_FREE_SHIFT   0
#define RFS_PTR_FREE_MASK    RFS_MASK12
#define RFS_PTR_ADDR_SHIFT   12
#define RFS_PTR_ADDR_MASK    RFS_MASK52

#define RFS_PTR_FREE(x)      ((uint32_t)(((x).v >> RFS_PTR_FREE_SHIFT) & RFS_PTR_FREE_MASK))
#define RFS_PTR_ADDR(x)      ((uint64_t)(((x).v >> RFS_PTR_ADDR_SHIFT) & RFS_PTR_ADDR_MASK))
#define RFS_PTR_PACK(free, addr) \
    ((uint64_t)((((uint64_t)(free) & RFS_PTR_FREE_MASK) << RFS_PTR_FREE_SHIFT) | \
                (((uint64_t)(addr) & RFS_PTR_ADDR_MASK) << RFS_PTR_ADDR_SHIFT)))
#define RFS_PTR_SET_FREE(x, free) \
    do { (x).v = (((x).v) & ~(RFS_PTR_FREE_MASK << RFS_PTR_FREE_SHIFT)) | \
                 ((((uint64_t)(free)) & RFS_PTR_FREE_MASK) << RFS_PTR_FREE_SHIFT); } while (0)
#define RFS_PTR_SET_ADDR(x, addr) \
    do { (x).v = (((x).v) & ~(RFS_PTR_ADDR_MASK << RFS_PTR_ADDR_SHIFT)) | \
                 ((((uint64_t)(addr)) & RFS_PTR_ADDR_MASK) << RFS_PTR_ADDR_SHIFT); } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * parent_ptr_t —— 父指针（双位解码四态）
 *   entry_count:10 | is_inner_node:1 | is_root_node:1 | ptr:52
 *   根节点: ptr = 自身 inode 编号（即 "."）；非根: ptr = 父节点块号
 * ═══════════════════════════════════════════════════════════════════════════ */
struct parent_ptr_t {
    uint64_t v;                     /* [0:10)=entry_count [10]=inner [11]=root [12:64)=ptr(52) */
};
RFS_STATIC_ASSERT(sizeof(struct parent_ptr_t) == 8, "parent_ptr_t must be 8B");

#define RFS_PP_COUNT_SHIFT   0
#define RFS_PP_COUNT_MASK    ((1ULL << 10) - 1)
#define RFS_PP_INNER_SHIFT   10
#define RFS_PP_INNER_MASK    0x1ULL
#define RFS_PP_ROOT_SHIFT    11
#define RFS_PP_ROOT_MASK     0x1ULL
#define RFS_PP_PTR_SHIFT     12
#define RFS_PP_PTR_MASK      RFS_MASK52

#define RFS_PP_COUNT(x)      ((uint32_t)(((x).v >> RFS_PP_COUNT_SHIFT) & RFS_PP_COUNT_MASK))
#define RFS_PP_IS_INNER(x)   ((uint32_t)(((x).v >> RFS_PP_INNER_SHIFT) & RFS_PP_INNER_MASK))
#define RFS_PP_IS_ROOT(x)    ((uint32_t)(((x).v >> RFS_PP_ROOT_SHIFT)  & RFS_PP_ROOT_MASK))
#define RFS_PP_PTR(x)        ((uint64_t)(((x).v >> RFS_PP_PTR_SHIFT)   & RFS_PP_PTR_MASK))
#define RFS_PP_PACK(count, inner, root, ptr) \
    ((uint64_t)((((uint64_t)(count) & RFS_PP_COUNT_MASK) << RFS_PP_COUNT_SHIFT) | \
                (((uint64_t)(inner) & RFS_PP_INNER_MASK) << RFS_PP_INNER_SHIFT) | \
                (((uint64_t)(root)  & RFS_PP_ROOT_MASK)  << RFS_PP_ROOT_SHIFT)  | \
                (((uint64_t)(ptr)   & RFS_PP_PTR_MASK)   << RFS_PP_PTR_SHIFT)))

/* 逐字段写（掩码法；god 迁移用，与 RFS_PTR_SET_XXX / RFS_INO1_SET_XXX 同风格） */
#define RFS_PP_SET_COUNT(x, count) \
    do { (x).v = (((x).v) & ~(RFS_PP_COUNT_MASK << RFS_PP_COUNT_SHIFT)) | \
                 ((((uint64_t)(count)) & RFS_PP_COUNT_MASK) << RFS_PP_COUNT_SHIFT); } while (0)
#define RFS_PP_SET_IS_INNER(x, inner) \
    do { (x).v = (((x).v) & ~(RFS_PP_INNER_MASK << RFS_PP_INNER_SHIFT)) | \
                 ((((uint64_t)(inner)) & RFS_PP_INNER_MASK) << RFS_PP_INNER_SHIFT); } while (0)
#define RFS_PP_SET_IS_ROOT(x, root) \
    do { (x).v = (((x).v) & ~(RFS_PP_ROOT_MASK << RFS_PP_ROOT_SHIFT)) | \
                 ((((uint64_t)(root)) & RFS_PP_ROOT_MASK) << RFS_PP_ROOT_SHIFT); } while (0)
#define RFS_PP_SET_PTR(x, ptr) \
    do { (x).v = (((x).v) & ~(RFS_PP_PTR_MASK << RFS_PP_PTR_SHIFT)) | \
                 ((((uint64_t)(ptr)) & RFS_PP_PTR_MASK) << RFS_PP_PTR_SHIFT); } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * name_heap_controller_t —— 名字堆控制器（每目录 10 个）
 *   word0: bytes_per_bit_log2:6 | total_bits_count_log2:6 | bitmap_base_block_ptr:52
 *   word1: reserved:12          | data_base_block_ptr:52
 *   （v1.1 §7 不变；本文件仅换形式）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct name_heap_controller_t {
    uint64_t word0;                 /* [0:6)=gran_log2 [6:12)=bits_log2 [12:64)=bitmap_base(52) */
    uint64_t word1;                 /* [0:12)=reserved [12:64)=data_base(52) */
};
RFS_STATIC_ASSERT(sizeof(struct name_heap_controller_t) == 16, "name_heap_controller_t must be 16B");

#define RFS_NHC_GRAN_SHIFT     0
#define RFS_NHC_GRAN_MASK      RFS_MASK6
#define RFS_NHC_BITS_SHIFT     6
#define RFS_NHC_BITS_MASK      RFS_MASK6
#define RFS_NHC_BITMAP_SHIFT   12
#define RFS_NHC_BITMAP_MASK    RFS_MASK52
#define RFS_NHC_RESV_SHIFT     0
#define RFS_NHC_RESV_MASK      RFS_MASK12
#define RFS_NHC_DATA_SHIFT     12
#define RFS_NHC_DATA_MASK      RFS_MASK52

#define RFS_NHC_GRAN(x)        ((uint32_t)(((x).word0 >> RFS_NHC_GRAN_SHIFT)   & RFS_NHC_GRAN_MASK))
#define RFS_NHC_BITS(x)        ((uint32_t)(((x).word0 >> RFS_NHC_BITS_SHIFT)   & RFS_NHC_BITS_MASK))
#define RFS_NHC_BITMAP(x)      ((uint64_t)(((x).word0 >> RFS_NHC_BITMAP_SHIFT) & RFS_NHC_BITMAP_MASK))
#define RFS_NHC_DATA(x)        ((uint64_t)(((x).word1 >> RFS_NHC_DATA_SHIFT)   & RFS_NHC_DATA_MASK))
#define RFS_NHC_SET(x, gran, bits, bitmap, resv, data) do { \
    (x).word0 = ((((uint64_t)(gran)   & RFS_NHC_GRAN_MASK)   << RFS_NHC_GRAN_SHIFT)   | \
                (((uint64_t)(bits)   & RFS_NHC_BITS_MASK)   << RFS_NHC_BITS_SHIFT)   | \
                (((uint64_t)(bitmap) & RFS_NHC_BITMAP_MASK) << RFS_NHC_BITMAP_SHIFT)); \
    (x).word1 = ((((uint64_t)(resv)   & RFS_NHC_RESV_MASK)   << RFS_NHC_RESV_SHIFT)   | \
                (((uint64_t)(data)   & RFS_NHC_DATA_MASK)   << RFS_NHC_DATA_SHIFT)); } while (0)

/* 逐字段写（掩码法；保留未触及字段；god 迁移用） */
#define RFS_NHC_SET_GRAN(x, gran) \
    do { (x).word0 = (((x).word0) & ~(RFS_NHC_GRAN_MASK << RFS_NHC_GRAN_SHIFT)) | \
                     ((((uint64_t)(gran)) & RFS_NHC_GRAN_MASK) << RFS_NHC_GRAN_SHIFT); } while (0)
#define RFS_NHC_SET_BITS(x, bits) \
    do { (x).word0 = (((x).word0) & ~(RFS_NHC_BITS_MASK << RFS_NHC_BITS_SHIFT)) | \
                     ((((uint64_t)(bits)) & RFS_NHC_BITS_MASK) << RFS_NHC_BITS_SHIFT); } while (0)
#define RFS_NHC_SET_BITMAP(x, bmp) \
    do { (x).word0 = (((x).word0) & ~(RFS_NHC_BITMAP_MASK << RFS_NHC_BITMAP_SHIFT)) | \
                     ((((uint64_t)(bmp)) & RFS_NHC_BITMAP_MASK) << RFS_NHC_BITMAP_SHIFT); } while (0)
#define RFS_NHC_SET_DATA(x, data) \
    do { (x).word1 = (((x).word1) & ~(RFS_NHC_DATA_MASK << RFS_NHC_DATA_SHIFT)) | \
                     ((((uint64_t)(data)) & RFS_NHC_DATA_MASK) << RFS_NHC_DATA_SHIFT); } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * inode 第一字（word1）—— type:6 | is_data_root_direct_bit:1 | i_flags:57
 *   仅 REG/LNK：is_data_root_direct_bit 有意义（1=直指 0=B+tree）
 *   DIR 与无数据类型：忽略（DIR 里该位恒 0，两结构第一字位布局同构）
 *   i_flags：镜像 Linux S_*（存值不移位），v1.2 恒 0（chattr 降级）
 * ═══════════════════════════════════════════════════════════════════════════ */
#define RFS_INO1_TYPE_SHIFT    0
#define RFS_INO1_TYPE_MASK     RFS_MASK6
#define RFS_INO1_DIRECT_SHIFT  6
#define RFS_INO1_DIRECT_MASK   0x1ULL
#define RFS_INO1_FLAGS_SHIFT   7
#define RFS_INO1_FLAGS_MASK    RFS_MASK57

#define RFS_INO1_GET_TYPE(w)   ((uint32_t)(((uint64_t)(w) >> RFS_INO1_TYPE_SHIFT)   & RFS_INO1_TYPE_MASK))
#define RFS_INO1_GET_DIRECT(w) ((uint32_t)(((uint64_t)(w) >> RFS_INO1_DIRECT_SHIFT) & RFS_INO1_DIRECT_MASK))
#define RFS_INO1_GET_FLAGS(w)  ((uint64_t)(((uint64_t)(w) >> RFS_INO1_FLAGS_SHIFT)  & RFS_INO1_FLAGS_MASK))
#define RFS_INO1_PACK(type, direct, flags) \
    ((uint64_t)((((uint64_t)(type)   & RFS_INO1_TYPE_MASK)   << RFS_INO1_TYPE_SHIFT)   | \
                (((uint64_t)(direct) & RFS_INO1_DIRECT_MASK) << RFS_INO1_DIRECT_SHIFT) | \
                (((uint64_t)(flags)  & RFS_INO1_FLAGS_MASK)  << RFS_INO1_FLAGS_SHIFT)))
#define RFS_INO1_SET_TYPE(ino, type) \
    do { (ino).word1 = (((ino).word1) & ~(RFS_INO1_TYPE_MASK << RFS_INO1_TYPE_SHIFT)) | \
                       ((((uint64_t)(type)) & RFS_INO1_TYPE_MASK) << RFS_INO1_TYPE_SHIFT); } while (0)
#define RFS_INO1_SET_DIRECT(ino, direct) \
    do { (ino).word1 = (((ino).word1) & ~(RFS_INO1_DIRECT_MASK << RFS_INO1_DIRECT_SHIFT)) | \
                       ((((uint64_t)(direct)) & RFS_INO1_DIRECT_MASK) << RFS_INO1_DIRECT_SHIFT); } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * mode_xattr（offsets [32:40)）—— mode:12 | xattr_ptr:52
 *   原名 permission_bits（v1.1 只写不读）。**已定名 `mode_xattr`（2026-09-26）**。
 *   mode = i_mode & 0o7777（rwxrwxrwx + suid/sgid/sticky）
 *   xattr_ptr = 52 位块指针 → 单块自描述 xattr 块；无 xattr = RFS_XATTR_NONE（全 1，禁用 0）
 *   恒等式：i_mode == (type << 12) | mode（两切片不相交，零冗余）
 * ═══════════════════════════════════════════════════════════════════════════ */
#define RFS_MODE_SHIFT        0
#define RFS_MODE_MASK         RFS_MASK12
#define RFS_XATTRPTR_SHIFT    12
#define RFS_XATTRPTR_MASK     RFS_MASK52

#define RFS_MODE_GET(mx)       ((uint32_t)(((uint64_t)(mx) >> RFS_MODE_SHIFT)     & RFS_MODE_MASK))
#define RFS_XATTRPTR_GET(mx)   ((uint64_t)(((uint64_t)(mx) >> RFS_XATTRPTR_SHIFT) & RFS_XATTRPTR_MASK))
#define RFS_MODE_XATTR_PACK(mode, xptr) \
    ((uint64_t)((((uint64_t)(mode) & RFS_MODE_MASK)     << RFS_MODE_SHIFT) | \
                (((uint64_t)(xptr) & RFS_XATTRPTR_MASK) << RFS_XATTRPTR_SHIFT)))
#define RFS_MODE_XATTR_SET(mx, mode, xptr) \
    do { (mx) = RFS_MODE_XATTR_PACK((mode), (xptr)); } while (0)

/* i_mode 组装 / 拆解（恒等式 RFS_INO1_TYPE(w)<<12 | RFS_MODE_GET(mx) == i_mode） */
#define RFS_IMODE_BUILD(type, mode)  ((uint16_t)((((uint32_t)(type) << 12) | ((uint32_t)(mode) & 0x0FFF)) & 0xFFFF))
#define RFS_IMODE_TYPE(imode)        ((uint32_t)((((uint32_t)(imode)) >> 12) & 0x0F))
#define RFS_IMODE_PERM(imode)        ((uint32_t)(((uint32_t)(imode)) & 0x0FFF))

/* ═══════════════════════════════════════════════════════════════════════════
 * raven_Inode_t —— REG / LNK / CHR / BLK / FIFO / SOCK 共用（256B）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct raven_Inode_t {
    uint64_t  word1;            /* [0:8)   type:6 | direct:1 | i_flags:57          */
    uint32_t  uid;              /* [8:12)                                           */
    uint32_t  gid;              /* [12:16)                                          */
    uint32_t  m_ns_stamp;       /* [16:20)  mtime 纳秒余数                          */
    uint32_t  c_ns_stamp;       /* [20:24)  ctime 纳秒余数                          */
    uint32_t  a_ns_stamp;       /* [24:28)  atime 纳秒余数                          */
    uint32_t  link_count;       /* [28:32)  REG/LNK=指向本 inode 的 dentry 数；DIR=2+直属 DIR 子 inode 数（D-12 POSIX） */
    uint64_t  mode_xattr;       /* [32:40)  mode:12 | xattr_ptr:52（原 permission_bits） */
    uint64_t  size;             /* [40:48)  字节数 / 目录条目数                      */
    uint64_t  mtime;            /* [48:56)  秒                                      */
    uint64_t  ctime;            /* [56:64)  秒                                      */
    uint64_t  atime;            /* [64:72)  秒                                      */
    uint64_t  data_root;        /* [72:80)  PTR_INVALID = inline                     */
    uint64_t  padding[2];       /* [80:96)  未来预留（v1.2 未用）                    */
    uint8_t   inline_data[160]; /* [96:256) 内容 / 无数据类型(CHR/BLK): rdev@[0:4]   */
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct raven_Inode_t) == 256, "raven_Inode_t must be 256B");

/* rdev（仅 CHR/BLK）—— 统一存 `inline_data` 起始 4B：**小端序 u32**，编码 = Linux `new_encode_dev()`
 *   （12 位 major + 20 位 minor 恰打包进 32 位 → u32 足够；读写用 le32_to_cpu()/cpu_to_le32()）
 *   若将来设备号上限需更大空间 → 升级为 `inline_data[0:8]` **小端 u64**（同起点；向后不兼容只波及这 8B）。
 *   非设备文件 / FIFO / SOCK：该区置 0。 */
#define RFS_INODE_RDEV_OFF       0
#define RFS_INODE_RDEV_SIZE      4    /* u32 小端（现行） */
#define RFS_INODE_RDEV_SIZE_MAX  8    /* u64 小端（备用上限方案） */
/* ═══════════════════════════════════════════════════════════════════════════
 * free_range_t —— 空闲区间（名字数据面**内部**语义）
 *
 * 含义（2026-10-05 再定，字节粒度）：目录名字数据面（name data plane）逻辑
 * 空间 [0, pieces×8K) 内**空闲的字节区间**；半开 [base, base+len)。
 *   · 组成 = 删长名释放的区间 ∪ **尚未分配的前沿尾部**（每新增 8K 片，其
 *     未用尾部即成为空闲区间）——故空闲区间之并 == 「已映射 ∧ 未分配」，
 *     分配"用尽"即 fr 中无 len ≥ 需求者。
 *   · 映射树（Bptree_dir_name_data_extent_*）描述「逻辑→物理的**稠密无洞**
 *     映射」；本类型描述「映射之内、内容空闲」的区间——两者不冲突。
 *   · **fr 树 leaf 按 (len 主序, base 副序) 升序**（2026-10-05 定）：分配是
 *     best-fit（找最小 len ≥ 需求），len 主序天然给出该序；base 仅同 len 内定序。
 *   inline 表与 B+树叶**复用同一 free_range_t**（用户裁定：64 位时代一切皆
 *   可能，不搞 8B 打包特例）。
 *
 * ⚠️ v2.0 全局 inode 数据面 fr **不复用本类型**：它有专属坐标类型
 *   `inode_dataplane_coordinate_free_range_t`（base/len 单位 = inode = 256B，
 *   字节量 <<8）——见文件下方「寻址坐标语言」§3。本 free_range_t **只属于名字
 *   数据面**（字节单位）；两者尺寸同为 16B 但坐标系不同，禁止互转 / 混用。
 * ═══════════════════════════════════════════════════════════════════════════ */
struct free_range_t{
    uint64_t base;   /* 区间起点（名字数据面内字节偏移） */
    uint64_t len;    /* 区间长度（字节；0 = 空区间） */
};
RFS_STATIC_ASSERT(sizeof(struct free_range_t) == 16, "free_range_t must be 16B");

/* inline 态空闲区间表上限：区间数 > 此值 ⇒ 必须升格为 B+树（与
 * possible_inline_free_ranges[] 容量同源；单一真相源）。*/
#define RFS_ND_FREE_INLINE_MAX  9u
/* ═══════════════════════════════════════════════════════════════════════════
 * raven_dir_Inode_t —— DIR 专用（256B）
 *   [0:80)   前段与 raven_Inode_t 同构（word1 的 direct 位恒 0）；
 *            [72:80) 在通用结构名为 data_root，此处名为 ddentry_Bptree_root
 *            （dentry 树根；PTR_INVALID = inline）。
 *   [80:112) 名字数据面控制字（name_data_ctl / _piece_count /
 *            name_data_free_ranges_ctl / _count）。
 *   [112:256) possible_inline_free_ranges[9]（inline 态空闲区间表；9×16B=144B）。
 *   DIR 不存在 rdev。
 * ═══════════════════════════════════════════════════════════════════════════ */
struct raven_dir_Inode_t {
    uint64_t  word1;            /* [0:8)   type:6 | (bit6=0) | i_flags:57 */
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  m_ns_stamp;
    uint32_t  c_ns_stamp;
    uint32_t  a_ns_stamp;
    uint32_t  link_count;       /* POSIX：2 + 直属 DIR 型子 inode 数（空目录=2；与 size 解耦，D-12；. / .. 由 ptrs[0]/ptrs[1] 合成） */
    uint64_t  mode_xattr;       /* mode:12 | xattr_ptr:52 */
    uint64_t  size;
    uint64_t  mtime;
    uint64_t  ctime;
    uint64_t  atime;
    uint64_t  ddentry_Bptree_root;        /* dentry 树根块号；dir 恒有效（PTR_INVALID=内部契约违反，不变量 11） */
    uint64_t  name_data_ctl;      /* [0]=direct(1)/tree(0)；[1:52)=数据面映射树根块号（8K 对齐，bit0 借位）；[52:64)=保留 */
    uint64_t  name_data_piece_count;      /* [0:51)=数据片数原值（片=8K）；[51:64)=保留 */
    uint64_t  name_data_free_ranges_ctl;  /* [0]=inline(1)/tree(0)；[1:52)=空闲区间树根块号（8K 对齐，bit0 借位）；[52:64)=保留 */
    uint64_t  name_data_free_ranges_count;/* 全域=空闲区间总数；> RFS_ND_FREE_INLINE_MAX(9) ⇒ 必须 tree 态 */
    struct free_range_t  possible_inline_free_ranges[RFS_ND_FREE_INLINE_MAX];/* inline 态空闲区间表（≤9 条；与 B+树叶 free_range_t 复用同型） */
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct raven_dir_Inode_t) == 256, "raven_dir_Inode_t must be 256B");

/* ═══════════════════════════════════════════════════════════════════════════
 * 名字数据面控制字访问器（v1.3 新增；与文件其它落盘字段同风格）
 *
 * 两棵 per-目录数据面 B+ 树的 inode 侧描述：
 *   映射树     name_data_ctl             （direct / B+树二态 + 根块指针）
 *   空闲区间树 name_data_free_ranges_ctl （inline / B+树二态 + 根块指针）
 *
 * ★「8K 对齐偷 bit0」约定（2026-10-05 确认）：
 *   树根块号恒 2 FS_block（8K）对齐 ⇒ 块号偶数 ⇒ 其 bit0 恒 0；故 bit0 空出来
 *   当「direct(1)/tree(0)」二态标志，块指针实际落在 [1:52)（51 位）。
 *   写指针 = ptr << 1；读指针 = (word >> 1) & RFS_MASK51；[52:64) 12 位保留。
 *
 * name_data_ctl 的 bit0 语义（2026-10-05 定）：
 *   direct(1) = 数据面为**单连续物理段**，ptr 域 = 该段物理基块号（8K 对齐）；
 *              非空数据面的 bootstrap 态，无映射树。
 *   tree(0)   = 数据面有映射树，ptr 域 = 映射树根块号。
 *   空态（未分配）由 **ptr 域全 1**（RFS_ND_CTL_PTR_INVALID）表示（整字全 0xF），
 *   此时 bit0 亦为 1，但 ptr==INVALID 优先判空。
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── name_data_ctl ── */
#define RFS_ND_CTL_DIRECT_SHIFT  0
#define RFS_ND_CTL_DIRECT_MASK   0x1ULL
#define RFS_ND_CTL_PTR_SHIFT     1
#define RFS_ND_CTL_PTR_MASK      RFS_MASK51
#define RFS_ND_CTL_RSV_SHIFT     52
#define RFS_ND_CTL_RSV_MASK      RFS_MASK12

#define RFS_ND_CTL_IS_DIRECT(x)  ((uint32_t)(((uint64_t)(x) >> RFS_ND_CTL_DIRECT_SHIFT) & RFS_ND_CTL_DIRECT_MASK))
#define RFS_ND_CTL_PTR(x)        ((uint64_t)(((uint64_t)(x) >> RFS_ND_CTL_PTR_SHIFT)   & RFS_ND_CTL_PTR_MASK))
#define RFS_ND_CTL_RSV(x)        ((uint32_t)(((uint64_t)(x) >> RFS_ND_CTL_RSV_SHIFT)   & RFS_ND_CTL_RSV_MASK))
#define RFS_ND_CTL_PACK(direct_, ptr) \
    ((uint64_t)((((uint64_t)(direct_) & RFS_ND_CTL_DIRECT_MASK) << RFS_ND_CTL_DIRECT_SHIFT) | \
                (((uint64_t)(ptr)     & RFS_ND_CTL_PTR_MASK)    << RFS_ND_CTL_PTR_SHIFT)))
#define RFS_ND_CTL_SET_DIRECT(x, v) \
    do { (x) = (((x) & ~(RFS_ND_CTL_DIRECT_MASK << RFS_ND_CTL_DIRECT_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_ND_CTL_DIRECT_MASK) << RFS_ND_CTL_DIRECT_SHIFT)); } while (0)
#define RFS_ND_CTL_SET_PTR(x, v) \
    do { (x) = (((x) & ~(RFS_ND_CTL_PTR_MASK << RFS_ND_CTL_PTR_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_ND_CTL_PTR_MASK) << RFS_ND_CTL_PTR_SHIFT)); } while (0)

/* 无效指针 / 空态哨兵（2026-10-05 定）：指针域 [1:52) 全 1 = **无效指针**
 *   （对齐 NAME_HEAP_INACTIVE / PTR_INVALID 的「全 1 = 无效」惯例）。
 * 空（未分配）态 = direct=? ∧ ptr 全 1 ∧ rsv 全 1 ⇒ **整字全 0xF**。*/
#define RFS_ND_CTL_PTR_INVALID  RFS_MASK51   /* [1:52) 全 1 = 无效指针 */
#define RFS_ND_CTL_INACTIVE     (~0ULL)      /* 空态整字：ptr=全1 ∧ rsv=全1 */

/* ── name_data_piece_count ──
 * 存储 = **实际数据片数原值**（2026-10-05 定：8K 数据片数目，直接存，不压缩）。
 *   [0:51) 51 位足够容纳任何现实片数；[51:64) 保留。
 *   （早前草案的「存 count/2、读 <<1」编码无法表达奇数片数——单片 8K 即奇数，
 *    已废弃，改为存原值。） */
#define RFS_ND_PIECE_RAW_SHIFT   0
#define RFS_ND_PIECE_RAW_MASK    RFS_MASK51
#define RFS_ND_PIECE_RSV_SHIFT   51
#define RFS_ND_PIECE_RSV_MASK    RFS_MASK13

#define RFS_ND_PIECE_RAW(x)      ((uint64_t)(((uint64_t)(x) >> RFS_ND_PIECE_RAW_SHIFT) & RFS_ND_PIECE_RAW_MASK))
#define RFS_ND_PIECE_COUNT(x)    RFS_ND_PIECE_RAW(x)   /* 实际数据片数（片 = 8K） */
#define RFS_ND_PIECE_RSV(x)      ((uint32_t)(((uint64_t)(x) >> RFS_ND_PIECE_RSV_SHIFT) & RFS_ND_PIECE_RSV_MASK))
#define RFS_ND_PIECE_PACK(count) ((uint64_t)(((uint64_t)(count) & RFS_ND_PIECE_RAW_MASK) << RFS_ND_PIECE_RAW_SHIFT))
#define RFS_ND_PIECE_SET_RAW(x, v) \
    do { (x) = (((x) & ~(RFS_ND_PIECE_RAW_MASK << RFS_ND_PIECE_RAW_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_ND_PIECE_RAW_MASK) << RFS_ND_PIECE_RAW_SHIFT)); } while (0)
#define RFS_ND_PIECE_SET_COUNT(x, c) RFS_ND_PIECE_SET_RAW((x), (uint64_t)(c))

/* ── name_data_free_ranges_ctl（位布局同 name_data_ctl）── */
#define RFS_ND_FR_CTL_INLINE_SHIFT  0
#define RFS_ND_FR_CTL_INLINE_MASK   0x1ULL
#define RFS_ND_FR_CTL_PTR_SHIFT     1
#define RFS_ND_FR_CTL_PTR_MASK      RFS_MASK51
#define RFS_ND_FR_CTL_RSV_SHIFT     52
#define RFS_ND_FR_CTL_RSV_MASK      RFS_MASK12

#define RFS_ND_FR_CTL_IS_INLINE(x) ((uint32_t)(((uint64_t)(x) >> RFS_ND_FR_CTL_INLINE_SHIFT) & RFS_ND_FR_CTL_INLINE_MASK))
#define RFS_ND_FR_CTL_PTR(x)       ((uint64_t)(((uint64_t)(x) >> RFS_ND_FR_CTL_PTR_SHIFT)   & RFS_ND_FR_CTL_PTR_MASK))
#define RFS_ND_FR_CTL_RSV(x)       ((uint32_t)(((uint64_t)(x) >> RFS_ND_FR_CTL_RSV_SHIFT)   & RFS_ND_FR_CTL_RSV_MASK))
#define RFS_ND_FR_CTL_PACK(inline_, ptr) \
    ((uint64_t)((((uint64_t)(inline_) & RFS_ND_FR_CTL_INLINE_MASK) << RFS_ND_FR_CTL_INLINE_SHIFT) | \
                (((uint64_t)(ptr)     & RFS_ND_FR_CTL_PTR_MASK)    << RFS_ND_FR_CTL_PTR_SHIFT)))
#define RFS_ND_FR_CTL_SET_INLINE(x, v) \
    do { (x) = (((x) & ~(RFS_ND_FR_CTL_INLINE_MASK << RFS_ND_FR_CTL_INLINE_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_ND_FR_CTL_INLINE_MASK) << RFS_ND_FR_CTL_INLINE_SHIFT)); } while (0)
#define RFS_ND_FR_CTL_SET_PTR(x, v) \
    do { (x) = (((x) & ~(RFS_ND_FR_CTL_PTR_MASK << RFS_ND_FR_CTL_PTR_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_ND_FR_CTL_PTR_MASK) << RFS_ND_FR_CTL_PTR_SHIFT)); } while (0)

/* 无效指针 / 空态哨兵（同 name_data_ctl）*/
#define RFS_ND_FR_CTL_PTR_INVALID  RFS_MASK51   /* [1:52) 全 1 = 无效指针 */
#define RFS_ND_FR_CTL_INACTIVE     (~0ULL)      /* 空态整字：inline=1 ∧ ptr=全1 ∧ rsv=全1 */

/* ── name_data_free_ranges_count（全域；无位域）── */
#define RFS_ND_FR_COUNT(x)        ((uint64_t)(x))
#define RFS_ND_FR_IS_TREE(x)      ((uint64_t)(x) > (uint64_t)RFS_ND_FREE_INLINE_MAX)
#define RFS_ND_FR_SET_COUNT(x, c) do { (x) = (uint64_t)(c); } while (0)

/* ── v1.3 迁移注记（2026-10-05）──────────────────────────────────────────
 * v1.2 的「10 × name_heap_controller_t + 位图档」及其有序索引访问器
 * rfs_dir_heap()/rfs_dir_heap_c()/RFS_DIR_HEAP_COUNT **在 v1.3 整体删除**：
 * 长名 tail 改由「目录名字数据面」承载（映射见 name_data_ctl，空闲区间见
 * name_data_free_ranges_ctl + possible_inline_free_ranges[]）。
 * name_heap_controller_t 结构与 NAME_HEAP_INACTIVE 哨兵对本结构不再使用。
 * ─────────────────────────────────────────────────────────────────────── */

/* ═══════════════════════════════════════════════════════════════════════════
 * Bptree_meta_t —— 节点元数据（独立 64B 头；ptrs[3] 按角色解码，见 v1.1 §4）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct Bptree_meta_t {
    uint64_t magics[4];          /* 256-bit 扫描签名（全盘恢复识别节点类型） */
    struct parent_ptr_t parent_ptr;
    struct ptr_in_idx   ptrs[3];
};
RFS_STATIC_ASSERT(sizeof(struct Bptree_meta_t) == 64, "Bptree_meta_t must be 64B");

/* ═══════════════════════════════════════════════════════════════════════════
 * 区段（文件块区间；左闭右开 [fblkbase, fblkbase+length)）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct extent_t {
    uint64_t length;             /* 区间长度 = 块数 */
    uint64_t fblkbase;           /* 文件逻辑块号起点 */
    struct ptr_in_idx block_base;/* 物理 FS_block 起点 */
};

/* 区段树内部节点路由 hint（上界语义：key = 子树最大开边界 fblkbase+length） */
struct extent_hint_t {
    uint64_t key;
    struct ptr_in_idx sub_node_ptr;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 目录项 ddentry_t / 路由 hint（hash 路由，上界 hash_max）
 * ═══════════════════════════════════════════════════════════════════════════ */
/* 命名约定（2026-09-27）：**盘上目录项条目 = ddentry_t**（on-disk dentry entry）；
 * VFS 的 struct dentry（dcache）是另一回事，勿混。dentry 树节点/魔数沿用旧名。 */
struct ddentry_t {
    struct ptr_in_idx inode_ptr;        /* free_to_use = name_len（全名长度） */
    uint64_t hash;                      /* 名字 64 位 hash（查找主键；算法 = XXH64(seed=0)，见 disk_layout_v1.1.md §9） */
    uint64_t full_char_remain_offset;      /* name_len>40: 外置名在目录名字数据面内的**逻辑字节偏移**；否则 PTR_INVALID */
    char     name[DENTRY_INLINE_NAME_MAX];
};

struct ddentry_hint_t {
    struct ptr_in_idx sub_ptr;
    uint64_t hash_max;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * B+tree 节点（恒 8192B = 2 个 FS_block；数组 = 阶数 + 1，[阶数] 为插入瞬时槽）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct Bptree_extent_inner_node_t {
    struct Bptree_meta_t meta;
    struct extent_hint_t extents[338];
    uint64_t padding[340];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_extent_inner_node_t) == 8192, "extent inner node must be 8192B");

struct Bptree_extent_leaf_node_t {
    struct Bptree_meta_t meta;
    struct extent_t extents[338];
    uint64_t padding[2];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_extent_leaf_node_t) == 8192, "extent leaf node must be 8192B");
/**
 * Bptree_dir_name_data_extent_*新增的这两个B+树节点在B+树操作上，节点分配纪律上和Bptree_extent_leaf_node_t完全一致，
 * 但是魔数不一样，并且有新的不变量
 * Bptree_dir_name_data_extent_的数据面必须符合mdata_piece的8k对齐+8k尺寸，这对外部映射接口提出了要求
 * 普通文件有size是逻辑上上线大小，可以允许最后一个块尾巴有剩余，可以有空洞，
 * 但是Bptree_dir_name_data_extent_不允许有空洞，并且由于特殊性数据面大小必须对齐8k,所以说存储在inode上时为了0冗余原则，故意如此设计
 */
struct Bptree_dir_name_data_extent_inner_node_t {
    struct Bptree_meta_t meta;
    struct extent_hint_t extents[338];
    uint64_t padding[340];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_dir_name_data_extent_inner_node_t) == 8192, "name_data extent inner node must be 8192B");
struct Bptree_dir_name_data_extent_leaf_node_t {
    struct Bptree_meta_t meta;
    struct extent_t extents[338];
    uint64_t padding[2];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_dir_name_data_extent_leaf_node_t) == 8192, "name_data extent leaf node must be 8192B");
/* 空闲区间树路由 hint：key = 子树内空闲区间的**最大复合键 (max_len, max_base)**
 * （len 主序 / base 副序；复合键不可只留 len——同 len 跨叶时仅凭 max_len 会落错叶、
 * 破坏连续分区）。
 * 路由 = 找第一个 (max_len,max_base) ≥ target 的子树（无则最右击穿）：
 *   · 分配 best-fit：target = (need, 0)（等价 max_len ≥ need）；
 *   · 插入定位   ：target = 新键 (len, base)。
 * 叶内按 (len,base) 升序 lower_bound。24B hint ⇒ 337 階（+1 瞬时槽）+ padding[2]。 */
struct free_ranges_hint_t{
    uint64_t max_len;
    uint64_t max_base;
    struct ptr_in_idx sub_node_ptr;
};
RFS_STATIC_ASSERT(sizeof(struct free_ranges_hint_t) == 24, "free_ranges_hint_t must be 24B");
struct Bptree_dir_name_data_free_ranges_inner_node_t{
    struct Bptree_meta_t meta;
    struct free_ranges_hint_t hints[338];   /* 337 阶 + 1 瞬时槽 */
    uint64_t padding[2];
};
RFS_STATIC_ASSERT(sizeof(struct Bptree_dir_name_data_free_ranges_inner_node_t) == 8192, "name_data free_ranges inner node must be 8192B");
struct Bptree_dir_name_data_free_ranges_leaf_node_t{
    struct Bptree_meta_t meta;
    struct free_range_t free_ranges[338];   /* 337 阶 + 1 瞬时槽 */
    uint64_t padding[340];
};
RFS_STATIC_ASSERT(sizeof(struct Bptree_dir_name_data_free_ranges_leaf_node_t) == 8192, "name_data free_ranges leaf node must be 8192B");

/* ═══════════════════════════════════════════════════════════════════════════
 * v2.0 全局 inode 数据面（inode data plane）—— 寻址坐标语言
 *                        ★★ EXPLICIT CONTRACT，先读这里 ★★
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 【0. 本体】
 *   数据面 = 一段**弹性（可增长）的、逻辑上紧密堆叠的 256B inode 数组**。
 *   逻辑空间 [0, total_inodes)：第 i 项就是第 i 个 inode 记录本身（不是树节点）。
 *   物理落盘以 **8K 片（piece）** 为增长单位向块分配器取块、经映射树 逻辑→物理；
 *   片内 32 个 inode 物理连续；片与片之间物理可不连续（仅 direct 态整体连续）。
 *   「弹性」= 逻辑数组长度随分配增长（= pieces×32），不预留固定容量。
 *   定位：v1.2 的「固定 inode 数组 [inode_array_base, bitmap_start) + inode 位图」
 *   被整体切除，改由超级块 `inode_data_*` 四字段为根统辖（与 v1.3「每目录名字
 *   数据面」同范式，抬到全局）。
 *
 * 【1. 四坐标（同一空间的不同代数表示；互转恒等，代码只准走 §5 的宏）】
 *
 *     inode 索引  inum / idx    单位 1 inode = 256B   值域 [0, total_inodes)
 *        │ 主坐标：dentry.inode_ptr、st_ino(=idx+1)、fr 区间、root=0 全用它
 *        │ idx << 8
 *        ▼
 *     逻辑字节    loff           单位 1B               值域 [0, pieces<<13)
 *        │ loff >> 12
 *        ▼
 *     逻辑 4K 块  lblk = idx>>4   单位 4096B            值域 [0, pieces<<1)
 *        │ ★ 映射树 extent_t.fblkbase 坐标（extent_t 单位恒 4K；2 对齐纪律见 §6）
 *        └（另一等价切法：逻辑片 pidx = idx>>5，单位 8K，值域 [0, pieces)）
 *
 *   恒等换算：
 *       loff = idx << 8      pidx = idx >> 5      lblk = idx >> 4
 *       idx  = loff >> 8     idx  = pidx << 5     idx  = lblk << 4
 *
 * 【2. 几何常量（8K 片 = 2 个 4K 块 = 32 个 256B inode）】
 *       INODE_BYTES = 256        INODES_PER_BLOCK = 16
 *       PIECE_BYTES = 8192       INODES_PER_PIECE = 32
 *       BLOCKS_PER_PIECE = 2
 *       SHIFT inode→byte = 8     inode→block = 4     inode→piece = 5
 *
 * 【3. 两个面各持哪个坐标（数字不同，禁止串用）】
 *   · 映射面（盘上 B+树 Bptree_inode_data_extent_*）：键 = **逻辑 4K 块 lblk**；
 *     记录 [fblkbase, fblkbase+length) 块 → 物理 FS_block。★extent_t 单位**恒 4K**；
 *     数据面纪律：fblkbase 与 length **均 2 对齐**（base 落 8K 片边界、跨整数个片），
 *     故每片 length=2、且段尾 (fblkbase+length) 亦 2 对齐。
 *   · 空闲面（内存 RBT / 盘上扁快照）：键 = **inode 索引 inum**；
 *     base/len 单位 = inode（256B），区间 [base, base+len) 个 inode，字节量 = <<8。
 *     （专用类型 inode_dataplane_coordinate_free_range_t；盘上为扁平数组，非 B+树）
 *   ⇒ 同一逻辑量在两坐标里差一个 SHIFT（块坐标 = inum >> 4）。
 *
 * 【4. 为什么两棵树用不同坐标】
 *   分配/回收最小粒度 = **1 个 inode**（不可能只分配半片）⇒ fr 必须 inode 粒度；
 *   映射树机械继承文件区段树（extent_t / walk / split 全按 FS_block 计）⇒ 保持
 *   4K 块粒度。两棵树各用最自然坐标，只在 resolve 处换算一次。
 *
 * 【5. resolve(idx) 规范路径（唯一换算入口；散落 <<8 / >>5 视为违规）】
 *       lblk     = idx >> 4
 *       ext      = mapping_tree_lookup(lblk)        // 必须 lblk ∈ ext 覆盖域
 *       phys_blk = ext.block_base + (lblk - ext.fblkbase)
 *       phys_off = (idx & 15) * 256
 *       dst      = fblock_to_byte(phys_blk) + phys_off
 *
 * 【6. 边界 / 不变量】
 *   · 逻辑空间稠密无洞 [0, pieces×32)（映射树覆盖；fr ⊆ 其中）；
 *   · fr 区间互不重叠、单位 = inode、落 [0, pieces×32)；
 *   · 物理片 8K 对齐；映射树 extent_t 单位恒 4K，纪律 fblkbase/length 均 2 对齐。
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── 坐标常量 + 换算宏（上面 §1/§2 的机器可校验版；单位/位移的唯一真相源）── */
#define RFS_IDP_INODE_BYTES        256u
#define RFS_IDP_PIECE_BYTES        8192u
#define RFS_IDP_BLOCKS_PER_PIECE   2u
#define RFS_IDP_INODES_PER_BLOCK   16u
#define RFS_IDP_INODES_PER_PIECE   32u
#define RFS_IDP_SHIFT_INODE_BYTE   8u    /* inode → 逻辑字节   = ×256 */
#define RFS_IDP_SHIFT_INODE_BLOCK  4u    /* inode → 逻辑 4K 块 = ÷16（映射树坐标）*/
#define RFS_IDP_SHIFT_INODE_PIECE  5u    /* inode → 逻辑 8K 片 = ÷32 */

#define RFS_IDP_INODE_TO_BYTE(idx)    ((uint64_t)(idx)  << RFS_IDP_SHIFT_INODE_BYTE)
#define RFS_IDP_INODE_TO_BLOCK(idx)   ((uint64_t)(idx)  >> RFS_IDP_SHIFT_INODE_BLOCK)
#define RFS_IDP_INODE_TO_PIECE(idx)   ((uint64_t)(idx)  >> RFS_IDP_SHIFT_INODE_PIECE)
#define RFS_IDP_BYTE_TO_INODE(off)    ((uint64_t)(off)  >> RFS_IDP_SHIFT_INODE_BYTE)
#define RFS_IDP_BLOCK_TO_INODE(blk)   ((uint64_t)(blk)  << RFS_IDP_SHIFT_INODE_BLOCK)
#define RFS_IDP_PIECE_TO_INODE(pidx)  ((uint64_t)(pidx) << RFS_IDP_SHIFT_INODE_PIECE)

/* ═══════════════════════════════════════════════════════════════════════════
 * v2.0 全局 inode 数据面 —— 盘上快照格式（**A/B 混合**，2026-10-05 定）
 *
 * 运行时（内存）：两棵红黑树 + 两把互斥锁，全量常驻（热路径零盘上节点 I/O）：
 *   · 映射 RBT —— 全局 inode 数据面最需容忍**碎片**（每 create/unlink/alloc 都过）；
 *   · 空闲 RBT —— best-fit，配合分配器**水位线**（运行时 `fs_allocator.hpp` 的
 *     `mgr_slice_t.last_finish_hint_`；**盘面不存**，见下方 fr 快照注）。
 * 定时刷回（30s）→ 两种不同形态的盘上快照：
 *   · 映射面 = 下方 2 个 B+ 树节点：**整棵重建为新 B+树**（bulk build，非增量补丁），
 *     走「激进重建」机械（同 extent_rebuild_tree）；节点布局与文件区段树 / 名字数据面
 *     **逐字节同构**（同 Bptree_meta_t / extent_t / extent_hint_t、337 阶），仅魔数独立
 *     （INODE_DATA_EXTENT_MAGIC*）。
 *   · 空闲面 = **扁平区间数组**（下文 fr 快照格式，无 B+树），挂载时 bulk-load 回 RBT。
 * 两指针（inode_data_ctl / inode_data_fr_ctl）同处超级块 ⇒ 单块写即**原子换新**，
 * 保证「映射 / 空闲」成对一致（否则掉电错配 → 挂载后重复分配）。
 *
 * fr 条目用专属坐标类型 inode_dataplane_coordinate_free_range_t（inode 单位，
 * 字节量 <<8），与名字数据面 free_range_t（字节单位）互不通用——见坐标语言 §3。
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── 映射树 · 内部节点（上界路由：key = 子树最大开边界 fblkbase+length）── */
struct Bptree_inode_data_extent_inner_node_t {
    struct Bptree_meta_t meta;
    struct extent_hint_t extents[338];   /* 337 阶 + 1 瞬时槽 */
    uint64_t padding[340];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_inode_data_extent_inner_node_t) == 8192,
                  "inode_data extent inner node must be 8192B");

/* ── 映射树 · 叶子（升序、互不重叠、稠密无洞；extent_t 单位 4K，fblkbase/length 2 对齐）── */
struct Bptree_inode_data_extent_leaf_node_t {
    struct Bptree_meta_t meta;
    struct extent_t extents[338];        /* 337 阶 + 1 瞬时槽 */
    uint64_t padding[2];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_inode_data_extent_leaf_node_t) == 8192,
                  "inode_data extent leaf node must be 8192B");
/* ── fr 空闲区间（inode 数据面专属坐标；**不是** free_range_t）──
 * 半开区间 [base, base+len)：base = 空闲段的**起始 inode 索引**，len = **inode 个数**。
 * 单位恒为 inode（256B）：逻辑字节区间 = [base<<8, (base+len)<<8)。
 * 与名字数据面 free_range_t 结构同尺寸（16B）但**语义坐标系不同**，禁止互转/混用。 */
struct inode_dataplane_coordinate_free_range_t{
    uint64_t base;   /* 空闲段起始 inode 索引（单位：1 inode = 256B） */
    uint64_t len;    /* 连续空闲 inode 个数（单位：1 inode = 256B） */
};
RFS_STATIC_ASSERT(sizeof(struct inode_dataplane_coordinate_free_range_t) == 16,
                  "inode_dataplane coordinate free_range must be 16B");

/* ── 扁 fr 快照（空闲面；**非 B+树**，整段重写 / bulk-load）──
 * 30s 定时把内存 fr RBT **压平**成扁平区间数组整段写入；挂载时整体 bulk-load 回
 * RBT（无 B+树搜索 / 分裂 / hint）。选扁平而非 B+树：全局 fr 条目通常不多（碎片
 * 段），整刷简单、与「全量内存 + 定时刷回」同构；B+树增量写优势在此用不上。
 *
 * 区布局（连续 FS_block，首块**1 块（4K）对齐**即可——2026-10-06 定，不再要求 8K）：
 *   [0:64)   struct inode_data_fr_snapshot_hdr
 *   [64:)    entry_count × inode_dataplane_coordinate_free_range_t（16B，inode 坐标）
 * 区块数 = ceil((64 + count×16) / 4096)。
 * SB：inode_data_fr_ctl.ptr = 区首块（[1:52) 指针域，可奇/偶；bit0 为**纯保留位**恒 0，
 *     与对齐无关）；inode_data_fr_count = entry_count（与 hdr 交叉校验）。
 * 换新：先写新快照区 → **单块写 SB**（映射 / 空闲两指针 + 代次一并可见）→ 释放旧区。
 *   掉电安全：新指针未落盘 = 旧态完整；落盘 = 新区完整，旧区成孤儿可由 fsck 回收。
 * 魔数 INODE_DATA_FR_SNAPSHOT_MAGIC1..4 供全盘扫描识别该区（**不是** B+树节点魔数，
 *   故本区**不含** Bptree_meta_t）。
 *
 * 注：**分配器水位线不在此盘面结构里**。水位的唯一归属是运行时的
 *   `include/ravenfs/fs_allocator.hpp`（`mgr_slice_t.last_finish_hint_`，只进不退的
 *   分配游标）；盘面只存「空闲区间事实」，水位由运行时分配器自行维护/推导。 */
struct inode_data_fr_snapshot_hdr {
    uint64_t magic[4];      /* 256-bit 扫描签名 */
    uint64_t entry_count;   /* 有效条目数（= SB.inode_data_fr_count，交叉校验） */
    uint64_t capacity;      /* 本区条目容量（释放旧区时算块数用） */
    uint32_t crc32;         /* 覆盖 hdr（除 crc 自身）+ 全部 entries */
    uint32_t rsv0;
    uint64_t rsv1[1];       /* 补齐 64B（分配器水位线不落盘，见上注） */
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct inode_data_fr_snapshot_hdr) == 64,
                  "inode_data fr snapshot hdr must be 64B");

struct Bptree_ddentry_leaf_node_t {
    struct Bptree_meta_t meta;
    struct ddentry_t entries[127];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_ddentry_leaf_node_t) == 8192, "dentry leaf node must be 8192B");

struct Bptree_ddentry_inner_node_t {
    struct Bptree_meta_t meta;
    struct ddentry_hint_t hints[127];
    uint64_t padding[762];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct Bptree_ddentry_inner_node_t) == 8192, "dentry inner node must be 8192B");

/* ═════════════════════════════════════════════════════════════════════════════
 * xattr 块（v1.2 新增）—— 单块自描述
 *   块头 32B；整套 ≤ **1 个 FS_block(4096B)**（硬上限，放不下 → -E2BIG）
 *   命名空间索引抄 ext4（省存前缀）；完整键 = <前缀> <e_name>
 *
 *   【布局 2026-09-26 定：**抄 ext4**】ext4 用**显式 `e_value_offs`** 定位值 →
 *   条目本身**不含 value**；值从**块尾向下**堆叠（4B 对齐）；条目区从块头后向高地址
 *   增长，以**首个 u32 == 0** 终止（ext4 `IS_LAST_ENTRY`）→ 消除“推进/终止”歧义。
 *   相对 ext4 的删减：无 `e_value_inum`（我们无「值外置 inode」）、无 `e_hash`（无共享）。
 * ═════════════════════════════════════════════════════════════════════════════ */
#define RFS_XATTR_BLOCK_SIZE   4096u
#define RFS_XATTR_HDR_SIZE     32u
#define RFS_XATTR_ENTRY_HDR_SIZE 8u      /* = sizeof(struct rfs_xattr_entry) */
#define RFS_XATTR_PAD          4u
#define RFS_XATTR_ROUND        (RFS_XATTR_PAD - 1u)   /* = 3 */
#define RFS_XATTR_ALIGN(n)     (((n) + RFS_XATTR_ROUND) & ~(uint64_t)RFS_XATTR_ROUND)
#define RFS_XATTR_ALIGN4(n)    RFS_XATTR_ALIGN(n)     /* 别名（兼容旧引用） */
/* 条目占位（含名字 + 尾对齐）—— 抄 ext4 EXT4_XATTR_LEN */
#define RFS_XATTR_LEN(nl)      (((nl) + RFS_XATTR_ROUND + RFS_XATTR_ENTRY_HDR_SIZE) & ~(uint64_t)RFS_XATTR_ROUND)
#define RFS_XATTR_NEXT(e)      ((struct rfs_xattr_entry *)((char *)(e) + RFS_XATTR_LEN((e)->e_name_len)))
#define RFS_XATTR_IS_LAST(e)   (*(const uint32_t *)(e) == 0u)   /* 终止：首个 u32==0（抄 ext4） */
/* 块内净可用（块 − 块头 − 终止 4B 零；值区从块尾向下，不额外占位） */
#define RFS_XATTR_USABLE       (RFS_XATTR_BLOCK_SIZE - RFS_XATTR_HDR_SIZE - 4u)

/* xattr 块识别魔数：仅“意思意思”的 u32 token（**2026-09-26 定**）。
 * 对比：B+tree 节点用 256-bit 强魔数，是为**容灾重建**（全盘扫描低误报识别节点类型）；
 * 而灾难场景关心的是**文件组织 + 目录树**，不会去重建 xattr → xattr 块无扫描级强度需求。
 * 仅作指针跟随后的轻识别，**不是校验和**（完整性靠 audit 结构自洽 + bitmap 认领）。 */
#define RFS_XATTR_H_MAGIC      0x52465358u   /* ASCII "RFSX" */

/* 命名空间索引（抄 ext4 fs/ext4/xattr.h） */
#define RFS_XATTR_INDEX_USER                 1
#define RFS_XATTR_INDEX_POSIX_ACL_ACCESS     2
#define RFS_XATTR_INDEX_POSIX_ACL_DEFAULT    3
#define RFS_XATTR_INDEX_TRUSTED              4
#define RFS_XATTR_INDEX_SECURITY             6
#define RFS_XATTR_INDEX_SYSTEM               7

struct rfs_xattr_block_hdr {        /* 32B */
    uint32_t h_magic;               /* 识别魔数（RFS_XATTR_H_MAGIC = "RFSX"，非校验和） */
    uint32_t h_refcount;            /* v1.2 恒 1（共享留将来） */
    uint32_t h_blocks;              /* 恒 1（沿用 ext 死字段约定，防未来） */
    uint32_t h_hash;                /* 全集 hash（v1.2 置 0；共享用） */
    uint64_t h_reserved[2];         /* 保留 */
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct rfs_xattr_block_hdr) == 32, "xattr block hdr must be 32B");

struct rfs_xattr_entry {            /* 8B 定长头；名字紧随其后（4B 对齐）；**值在 e_value_offs 处** */
    uint8_t  e_name_len;            /* 名字后缀长（不含命名空间前缀） */
    uint8_t  e_name_index;          /* 命名空间索引（RFS_XATTR_INDEX_*） */
    uint16_t e_value_offs;          /* 值在块内偏移（u16；块 ≤4K 足够）；e_value_size==0 时忽略 */
    uint32_t e_value_size;          /* 值长（字节） */
    /* char e_name[e_name_len]; —— 名字后**不内联 value**（value 在 e_value_offs 处） */
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct rfs_xattr_entry) == 8, "xattr entry must be 8B");

/* ═══════════════════════════════════════════════════════════════════════════
 * RavenSuperblock（Primary & Backup，4096B）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct RavenSuperblock {
    uint64_t  magic[4];             /* 256-bit 高强度魔数：magic[0]=RFS_SB_MAGIC_W0（"RAVENFS\0"前缀），magic[1..3] 每卷随机 */
    uint32_t  version;              /* = RAVENFS_VERSION（v2.0） */
    uint32_t  pad0;
    uint64_t  world_base_lba;       /* FS_block[0] 在设备 LBA 空间的起始 LBA */
    uint64_t  total_blocks;         /* FS_block 世界总块数 */

    uint64_t  inode_data_ctl;                     /* 映射树控制：RFS_IDP_CTL_*（direct/tree + 根块） */
    uint64_t  inode_data_plane_pieces_count;      /* 8K 数据面片数原值：RFS_IDP_PIECE_* */
    uint64_t  inode_data_fr_ctl;                  /* 扁 fr 快照区首块：RFS_IDP_FR_CTL_*（非 B+树，[1:52) 指针） */
    uint64_t  inode_data_fr_count;                /* 扁 fr 快照条目数：RFS_IDP_FR_*（条目坐标 = inode，字节量 <<8） */
    /* root_dir inode 索引 ≡ RFS_ROOT_INODE_IDX(=0)：FS 契约级硬编码，SB 无 root 字段 */
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;

    uint64_t  backup_prev;          /* Primary: PTR_INVALID | Backup: 前驱 FS_block 号 */
    uint64_t  backup_next;          /* Primary: 链头     | Backup: 后继 FS_block 号 */
    uint32_t  backup_count;

    uint64_t  born_time;
    uint64_t  latest_mount_time;
    uint64_t  latest_syn_time;      //以后挂载之后每隔30s一次刷回一次超级块+inode_data_plane的元数据面+空闲管理面，并且更新这个时间戳，以后会根据这个latest_syn_time和latest_umount_time比较作为判据断言发生了断电
    uint64_t  latest_umount_time;
    uint64_t  system_mount_time_accumulate;
    uint64_t  system_mount_counter;
    uint64_t  tools_mount_time_accumulate;
    uint64_t  tools_mount_counter;
    uint64_t  abnormal_shutdown_counts;
    uint8_t   pad[3848];            /* 248(实字段) + 3848 = 4096B；新增字段后必须重算 */
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct RavenSuperblock) == 4096, "RavenSuperblock must be 4096B");

/* ═══════════════════════════════════════════════════════════════════════════
 * 全局 inode 数据面 —— 超级块控制字访问器（v2.0 新增）
 *
 * 四个 SB 字段与 raven_dir_Inode_t 的名字数据面四控制字**同源同构**（把
 * 「每目录一张」抬成「全局一张」）：
 *
 *   name_data_ctl                ↔ inode_data_ctl
 *   name_data_piece_count        ↔ inode_data_plane_pieces_count
 *   name_data_free_ranges_ctl    ↔ inode_data_fr_ctl
 *   name_data_free_ranges_count  ↔ inode_data_fr_count
 *
 * ★「8K 对齐偷 bit0」：映射 B+树根 / 扁 fr 快照区首块均恒 2 FS_block（8K）对齐 ⇒
 *   块号偶 ⇒ bit0 恒 0；映射 ctl 借 bit0 当 direct(1)/tree(0) 二态，指针落 [1:52)
 *   （51 位），[52:64) 12 位保留。空态哨兵 = 指针域全 1（RFS_IDP_CTL_PTR_INVALID）。
 *
 * ★★ 唯一语义分叉：inode_data_fr_ctl 指向**扁 fr 快照区**（非 B+树、无 inline 态）。
 *   名字数据面 fr 之所以有 inline：内联表躺在**目录 inode 的 256B 内联区**（免费嵌入、
 *   不额外占块）——小目录真省一块。全局 inode 数据面的空闲面「载体」是超级块 + 30s 整刷
 *   的扁快照：既无「省块」收益、又把 SB pad 吃掉没意义，故直接扁数组。
 *   ⇒ 位布局仍与 name_data_free_ranges_ctl 对位（bit0 借用 + [1:52) 指针），bit0 降为
 *      **保留位、恒 0**（RFS_IDP_FR_CTL_RSV0_*）。
 *
 * ⚠️ fr 条目自身的坐标（单位 = inode = 256B、字节量 <<8）见
 *   inode_dataplane_coordinate_free_range_t；盘上格式见 inode_data_fr_snapshot_hdr，
 *   与本 ctl 位域无关。
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── inode_data_ctl（映射树控制：direct(1) / tree(0) + 根块）── */
#define RFS_IDP_CTL_DIRECT_SHIFT  0
#define RFS_IDP_CTL_DIRECT_MASK   0x1ULL
#define RFS_IDP_CTL_PTR_SHIFT     1
#define RFS_IDP_CTL_PTR_MASK      RFS_MASK51
#define RFS_IDP_CTL_RSV_SHIFT     52
#define RFS_IDP_CTL_RSV_MASK      RFS_MASK12

#define RFS_IDP_CTL_IS_DIRECT(x)  ((uint32_t)(((uint64_t)(x) >> RFS_IDP_CTL_DIRECT_SHIFT) & RFS_IDP_CTL_DIRECT_MASK))
#define RFS_IDP_CTL_PTR(x)        ((uint64_t)(((uint64_t)(x) >> RFS_IDP_CTL_PTR_SHIFT)   & RFS_IDP_CTL_PTR_MASK))
#define RFS_IDP_CTL_RSV(x)        ((uint32_t)(((uint64_t)(x) >> RFS_IDP_CTL_RSV_SHIFT)   & RFS_IDP_CTL_RSV_MASK))
#define RFS_IDP_CTL_PACK(direct_, ptr) \
    ((uint64_t)((((uint64_t)(direct_) & RFS_IDP_CTL_DIRECT_MASK) << RFS_IDP_CTL_DIRECT_SHIFT) | \
                (((uint64_t)(ptr)     & RFS_IDP_CTL_PTR_MASK)    << RFS_IDP_CTL_PTR_SHIFT)))
#define RFS_IDP_CTL_SET_DIRECT(x, v) \
    do { (x) = (((x) & ~(RFS_IDP_CTL_DIRECT_MASK << RFS_IDP_CTL_DIRECT_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_IDP_CTL_DIRECT_MASK) << RFS_IDP_CTL_DIRECT_SHIFT)); } while (0)
#define RFS_IDP_CTL_SET_PTR(x, v) \
    do { (x) = (((x) & ~(RFS_IDP_CTL_PTR_MASK << RFS_IDP_CTL_PTR_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_IDP_CTL_PTR_MASK) << RFS_IDP_CTL_PTR_SHIFT)); } while (0)

#define RFS_IDP_CTL_PTR_INVALID  RFS_MASK51   /* [1:52) 全 1 = 无效指针 */
#define RFS_IDP_CTL_INACTIVE     (~0ULL)      /* 空态整字：direct=? ∧ ptr=全1 ∧ rsv=全1 */

/* ── inode_data_plane_pieces_count（8K 数据面片数原值；[0:51) 存原值，[51:64) 保留）── */
#define RFS_IDP_PIECE_RAW_SHIFT   0
#define RFS_IDP_PIECE_RAW_MASK    RFS_MASK51
#define RFS_IDP_PIECE_RSV_SHIFT   51
#define RFS_IDP_PIECE_RSV_MASK    RFS_MASK13

#define RFS_IDP_PIECE_RAW(x)      ((uint64_t)(((uint64_t)(x) >> RFS_IDP_PIECE_RAW_SHIFT) & RFS_IDP_PIECE_RAW_MASK))
#define RFS_IDP_PIECE_COUNT(x)    RFS_IDP_PIECE_RAW(x)   /* 实际 8K 数据面片数 */
#define RFS_IDP_PIECE_RSV(x)      ((uint32_t)(((uint64_t)(x) >> RFS_IDP_PIECE_RSV_SHIFT) & RFS_IDP_PIECE_RSV_MASK))
#define RFS_IDP_PIECE_PACK(count) ((uint64_t)(((uint64_t)(count) & RFS_IDP_PIECE_RAW_MASK) << RFS_IDP_PIECE_RAW_SHIFT))
#define RFS_IDP_PIECE_SET_RAW(x, v) \
    do { (x) = (((x) & ~(RFS_IDP_PIECE_RAW_MASK << RFS_IDP_PIECE_RAW_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_IDP_PIECE_RAW_MASK) << RFS_IDP_PIECE_RAW_SHIFT)); } while (0)
#define RFS_IDP_PIECE_SET_COUNT(x, c) RFS_IDP_PIECE_SET_RAW((x), (uint64_t)(c))

/* ── inode_data_fr_ctl（指向**扁 fr 快照区**首块；无 B+树 / 无 inline）──
 * 位布局与 name_data_free_ranges_ctl 对位，但 bit0 降为保留位、恒 0（见上方 ★★）。 */
#define RFS_IDP_FR_CTL_RSV0_SHIFT   0
#define RFS_IDP_FR_CTL_RSV0_MASK    0x1ULL
#define RFS_IDP_FR_CTL_PTR_SHIFT    1
#define RFS_IDP_FR_CTL_PTR_MASK     RFS_MASK51
#define RFS_IDP_FR_CTL_RSV_SHIFT    52
#define RFS_IDP_FR_CTL_RSV_MASK     RFS_MASK12

#define RFS_IDP_FR_CTL_RSV0(x)      ((uint32_t)(((uint64_t)(x) >> RFS_IDP_FR_CTL_RSV0_SHIFT) & RFS_IDP_FR_CTL_RSV0_MASK))
#define RFS_IDP_FR_CTL_PTR(x)       ((uint64_t)(((uint64_t)(x) >> RFS_IDP_FR_CTL_PTR_SHIFT)   & RFS_IDP_FR_CTL_PTR_MASK))
#define RFS_IDP_FR_CTL_RSV(x)       ((uint32_t)(((uint64_t)(x) >> RFS_IDP_FR_CTL_RSV_SHIFT)   & RFS_IDP_FR_CTL_RSV_MASK))
/* 保留位恒 0（合法 ctl 恒真）；无二态位 */
#define RFS_IDP_FR_CTL_RSV0_OK(x)   (RFS_IDP_FR_CTL_RSV0(x) == 0)
#define RFS_IDP_FR_CTL_PACK(ptr) \
    ((uint64_t)(((uint64_t)(ptr) & RFS_IDP_FR_CTL_PTR_MASK) << RFS_IDP_FR_CTL_PTR_SHIFT))
#define RFS_IDP_FR_CTL_SET_PTR(x, v) \
    do { (x) = (((x) & ~(RFS_IDP_FR_CTL_PTR_MASK << RFS_IDP_FR_CTL_PTR_SHIFT)) | \
               ((((uint64_t)(v)) & RFS_IDP_FR_CTL_PTR_MASK) << RFS_IDP_FR_CTL_PTR_SHIFT)); } while (0)

/* 有效 FS 恒有 fr 快照区（mkfs 即建），故 INACTIVE 仅作「未初始化」防御哨兵：
 * 指针域全 1（PTR_INVALID），保留位 rsv0 保持 0。 */
#define RFS_IDP_FR_CTL_PTR_INVALID  RFS_MASK51   /* [1:52) 全 1 = 无效指针 */
#define RFS_IDP_FR_CTL_INACTIVE     ((uint64_t)RFS_MASK51 << RFS_IDP_FR_CTL_PTR_SHIFT)

/* ── inode_data_fr_count（扁 fr 快照条目数；无位域）──
 * = inode_data_fr_snapshot_hdr.entry_count（交叉校验）；分配是内存 RBT 的事，
 * 盘上仅作挂账 / 审计。 */
#define RFS_IDP_FR_COUNT(x)        ((uint64_t)(x))
#define RFS_IDP_FR_SET_COUNT(x, c) do { (x) = (uint64_t)(c); } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * （C++ only）8K 节点便捷 I/O 宏 —— 供 god（参考实现）沿用；ko（C）自行实现读写。
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifdef __cplusplus
template <class T> static inline T &ravenfs_fs_ref(T &t) { return t; }
template <class T> static inline T &ravenfs_fs_ref(T *t) { return *t; }

#define RAVENFS_READ_8K_NODE(fs, blk, node) \
    (ravenfs_fs_ref(fs).read_buff((blk), 2, (node)))
#define RAVENFS_WRITE_8K_NODE(fs, blk, node) \
    (ravenfs_fs_ref(fs).write_buff((blk), 2, (node)))
#endif /* __cplusplus */
