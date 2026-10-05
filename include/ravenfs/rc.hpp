#pragma once
/*
 * ═══════════════════════════════════════════════════════════════════════════
 * rc.hpp — 全局共享结果码 / 通用哨兵（轻量运行时头，C / C++ 双兼容）
 *
 * 2026-10-05 从 disk_struct_v1.2.h 剥离（方案 A）：
 *   result_code_t / rfs_rc(RC_*) / PTR_INVALID 是**纯运行时**常量，与落盘
 *   字节布局无关，却长期埋在全量 on-disk 结构头里 —— 任何想拿错误码的模块
 *   （inode 缓存 / 元数据负载片缓存 / 位图分片分配器 / 红黑树分配器…）都被
 *   迫连整份盘结构（raven_Inode_t / raven_dir_Inode_t / superblock / 全部
 *   RFS_* 宏与 static_assert）一起拖进来。现独立成此轻量头：
 *     - 盘侧：disk_struct_v1.2.h 反向 #include 本头，对外符号集不变；
 *     - 运行时侧：缓存/分配器只 #include "ravenfs/rc.hpp"，零盘依赖。
 *
 * 值 / 枚举顺序与剥离前逐字一致（整数值被盘代码与用例断言依赖，勿改）。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifdef __KERNEL__
#  include <linux/types.h>   /* 内核：提供 uint64_t（同 disk_struct_v1.2.h 约定） */
#else
#  include <stdint.h>        /* 用户态 / god / C++ */
#endif

/* ─────────────────────────── 结果码（全局共享） ─────────────────────────── */
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
    RC_ERR_OVERLAP,         /* 区间与既有映射/空闲区重叠（防御；正常流程 = 契约违反） */
    RC_ERR_IS_DIR,          /* dir 型目标：文件数据操作接口不受理（EISDIR 对齐） */
    RC_ERR_XATTR,           /* v1.2 新增：xattr 块读/校验/超限（>1 FS_block → -E2BIG）失败 */
    RC_ERR_BUSY,            /* 设备已被 Linux 内核以某 FS 挂载（God 防呆拒挂：防双写者 / 撕裂读） */

    RC_NEED_SPLIT = 0x1000  /* 内部使用：本层分裂，需父层处理 */
};

/* ─────────────────────────── 通用哨兵 ─────────────────────────── */
#define PTR_INVALID              (~0ULL)     /* 通用无效指针哨兵 */
