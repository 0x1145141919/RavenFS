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
#define RFS_MASK52  ((1ULL << 52) - 1)   /* 0x000F_FFFF_FFFF_FFFF */
#define RFS_MASK57  ((1ULL << 57) - 1)   /* 0x01FF_FFFF_FFFF_FFFF */

/* ═══════════════════════════════════════════════════════════════════════════
 * 结果码（全局共享；原居 disk_struct_v1.1.hpp，沿用）
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef uint64_t result_code_t;

enum rfs_rc {
    RC_OK = 0,
    RC_ERR_OPEN,            /* 设备打开失败 */
    RC_ERR_MAGIC,           /* Primary SB magic 不符 */
    RC_ERR_VERSION,         /* 版本 != RAVENFS_VERSION(v1.2=3) */
    RC_ERR_CRC,             /* Primary SB crc32 校验失败 */
    RC_ERR_BITMAP_SELF,     /* bitmap 自描述校验失败（尾巴三段非全 1） */
    RC_ERR_EXIST,           /* 同名 dentry 已存在 */
    RC_ERR_NOT_FOUND,       /* dentry 未找到 */
    RC_ERR_NO_BLOCK,        /* FS_block 无空闲 */
    RC_ERR_NO_INODE,        /* inode 槽无空闲 */
    RC_ERR_NAME_LEN,        /* 名字 > RAVENFS_MAX_NAME_LEN */
    RC_ERR_IO,              /* 块 I/O 失败（v1.2 起：bio 后端，不再是 pread/pwrite） */
    RC_ERR_STATE,           /* 内部状态异常（根节点角色不符等） */
    RC_ERR_NOT_EMPTY,       /* 目录非空：dir 型目标删除前置校验失败（rmdir 语义） */
    RC_ERR_NOT_SUPPORTED,   /* 功能暂不支持：归零全量空间回收未实现哨兵（inode 暂挂孤儿，fsck 可回收） */
    RC_ERR_OVERLAP,         /* extent 区间与既有映射重叠（防御；正常流程 = 契约违反） */
    RC_ERR_IS_DIR,          /* dir 型目标：文件数据操作接口不受理（EISDIR 对齐） */
    RC_ERR_XATTR,           /* v1.2 新增：xattr 块读/校验/超限（>1 FS_block → -E2BIG）失败 */

    RC_NEED_SPLIT = 0x1000  /* 内部使用：本层分裂，需父层处理 */
};

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
#define RAVENFS_VERSION          3u          /* v1.2 = 3（v1.1 = 2，不兼容） */
#define DENTRY_INLINE_NAME_MAX   40u         /* name 内联上限：≤40 内联，>40 外置 */
#define RAVENFS_MAX_NAME_LEN     (DENTRY_INLINE_NAME_MAX + 512u)  /* 552B */
#define PTR_INVALID              (~0ULL)     /* 通用无效指针哨兵 */
#define NAME_HEAP_INACTIVE       RFS_MASK52  /* 名字堆未激活（52 位全 1；禁用 0） */
#define RFS_XATTR_NONE           RFS_MASK52  /* xattr_ptr 无 xattr（52 位全 1；禁用 0） */
#define RFS_SB_MAGIC             0x5241564EULL  /* "RAVN" */

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
    uint32_t  link_count;       /* [28:32)  REG/LNK=指向本 inode 的 dentry 数；DIR=size+2 */
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
 * raven_dir_Inode_t —— DIR 专用（256B）
 *   前 96B 与 raven_Inode_t 同构（word1 的 direct 位恒 0），[96:256) 换成 10 个名字堆描述符。
 *   DIR 不存在 rdev。
 * ═══════════════════════════════════════════════════════════════════════════ */
struct raven_dir_Inode_t {
    uint64_t  word1;            /* [0:8)   type:6 | (bit6=0) | i_flags:57 */
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  m_ns_stamp;
    uint32_t  c_ns_stamp;
    uint32_t  a_ns_stamp;
    uint32_t  link_count;       /* = size + 2（2 = . .. 合成引用约定，随 size 同步写） */
    uint64_t  mode_xattr;       /* mode:12 | xattr_ptr:52 */
    uint64_t  size;
    uint64_t  mtime;
    uint64_t  ctime;
    uint64_t  atime;
    uint64_t  data_root;        /* PTR_INVALID = inline */
    uint64_t  padding[2];       /* 未来预留 */
    /* ── 名字堆：10 个描述符 **刻意展开命名**（与 v1.1 同构；**不用数组**，杜绝衰减/越界腐坏）。
     * 粒度 × 位图档：8B{8k,16k,32k} · 64B{8k,16k} · 128B{8k,16k} · 256B{8k,16k} · 512B{8k}
     * 尾段长度档（name_len-40）：≤40→8B堆；41–64→64B；65–128→128B；129–256→256B；257–512→512B。
     * 懒激活：激活 = 位图面 + 数据面各一次连续分配（mdata_piece 硬 8k 对齐）。
     * 按序访问用 rfs_dir_heap()/rfs_dir_heap_c()（有序索引 + **越界返回 NULL**）。 */
    struct name_heap_controller_t heaps_8B_8k;   /* [96:112)  */
    struct name_heap_controller_t heaps_8B_16k;  /* [112:128) */
    struct name_heap_controller_t heaps_8B_32k;  /* [128:144) */
    struct name_heap_controller_t heaps_64B_8k;  /* [144:160) */
    struct name_heap_controller_t heaps_64B_16k; /* [160:176) */
    struct name_heap_controller_t heaps_128B_8k; /* [176:192) */
    struct name_heap_controller_t heaps_128B_16k;/* [192:208) */
    struct name_heap_controller_t heaps_256B_8k; /* [208:224) */
    struct name_heap_controller_t heaps_256B_16k;/* [224:240) */
    struct name_heap_controller_t heaps_512B_8k; /* [240:256) */
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct raven_dir_Inode_t) == 256, "raven_dir_Inode_t must be 256B");

/* 名字堆描述符个数（访问器越界保护用） */
#define RFS_DIR_HEAP_COUNT 10

/*
 * 有序索引访问（0..9）—— 刻意用 switch 而**不是**首地址+偏移：
 *   · 越界 i ≥ 10 → 返回 NULL（调用方必检）；
 *   · 不依赖“10 个字段连续”的实现细节；
 *   · 用 offsetof 取址，避开 packed 成员的取址告警（-Waddress-of-packed-member）。
 *   · C / C++ 双兼容。
 */
static inline const struct name_heap_controller_t *
rfs_dir_heap_c(const struct raven_dir_Inode_t *d, unsigned int i)
{
    static const unsigned int off[RFS_DIR_HEAP_COUNT] = {
        offsetof(struct raven_dir_Inode_t, heaps_8B_8k),
        offsetof(struct raven_dir_Inode_t, heaps_8B_16k),
        offsetof(struct raven_dir_Inode_t, heaps_8B_32k),
        offsetof(struct raven_dir_Inode_t, heaps_64B_8k),
        offsetof(struct raven_dir_Inode_t, heaps_64B_16k),
        offsetof(struct raven_dir_Inode_t, heaps_128B_8k),
        offsetof(struct raven_dir_Inode_t, heaps_128B_16k),
        offsetof(struct raven_dir_Inode_t, heaps_256B_8k),
        offsetof(struct raven_dir_Inode_t, heaps_256B_16k),
        offsetof(struct raven_dir_Inode_t, heaps_512B_8k),
    };

    if (i >= RFS_DIR_HEAP_COUNT)
        return NULL;
    return (const struct name_heap_controller_t *)(const void *)((const char *)d + off[i]);
}
static inline struct name_heap_controller_t *
rfs_dir_heap(struct raven_dir_Inode_t *d, unsigned int i)
{
    return (struct name_heap_controller_t *)rfs_dir_heap_c(d, i);
}

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
    uint64_t full_char_remain_ptr;      /* name_len>40: 外置名物理定位；否则 PTR_INVALID */
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
    uint64_t  magic;                /* RFS_SB_MAGIC = "RAVN" */
    uint32_t  version;              /* = RAVENFS_VERSION (v1.2 = 3) */
    uint32_t  pad0;
    uint64_t  world_base_lba;       /* FS_block[0] 在设备 LBA 空间的起始 LBA */
    uint64_t  total_blocks;         /* FS_block 世界总块数 */

    uint64_t  inode_array_count;    /* Inode 上限 */
    uint64_t  root_inode;           /* 根目录 inode 索引 */
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;

    uint64_t  backup_prev;          /* Primary: PTR_INVALID | Backup: 前驱 FS_block 号 */
    uint64_t  backup_next;          /* Primary: 链头     | Backup: 后继 FS_block 号 */
    uint32_t  backup_count;

    uint64_t  born_time;
    uint64_t  mount_time;
    uint64_t  umount_time;
    uint64_t  modify_time;

    uint8_t   pad[3928];
} RFS_PACKED;
RFS_STATIC_ASSERT(sizeof(struct RavenSuperblock) == 4096, "RavenSuperblock must be 4096B");

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
