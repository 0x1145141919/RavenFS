#pragma once
#include "../disk_struct_v1.3.h"
#include "cache_mgr_mode.hpp"
#include "../magics.h"
#include <list>
/* audit 类型（完整定义在 ravenfs_audit.hpp；此处仅前置声明，
 * 避免把 <string>/<vector> 等标准头拉进 `#define private public` 的测试 TU）
 * 2026-09-30 重写：旧 audit_ctx_t/opts/report 模型已退役，换为可达性新模型。 */
struct audit_walk_ctx_t;
struct audit_walk_report_t;
struct audit_walk_tree_t;
struct audit_name_data_tree_t;
struct audit_fr_tree_t;
/* 缓存子模块前置声明（仅成员指针用，不拉完整头）：
 * 完整类型由使用方 cpp 各自 include inode_cache_radix.hpp /
 * mdata_piece_caches.hpp —— 这两个头会拉 util/tsc.h → <chrono>，
 * 若在 ravenfs_v1.1.hpp 直接 include，任何用
 * `#define private public` 包裹本头的测试（test_root_dir_*）会把
 * 标准库模板头一并污染，gcc16 下 sstream 可访问性检查直接报错。 */
class Inode_cache_mgr_t;        /* inode 缓存管理器（inode_cache_radix.hpp） */
class mdata_piece_caches_mgr;   /* 8K 元数据负载片缓存管理器（mdata_piece_caches.hpp） */
class bitmap_slices_mgr_t;      /* 双位图分片分配器（bitmap_slices.hpp；2026-09-03 从上帝类顶出） */class mdata_piece_caches_mgr;   /* 8K 元数据负载片缓存管理器（mdata_piece_caches.hpp） */

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>
#include <atomic>

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
 * 3. 名字上限 RAVENFS_MAX_NAME_LEN=552B：≤40B 内联（Dentry_t.name），>40B 走
 *    名字堆（每目录固定 10 描述符，见 disk_layout_v1.1.md §7）。
 * 4. 删除 = 物理删除（v1.1 §6.5）：memmove + 刷父 hash_max（上界路由
 *    对称代价，必走路径）；欠载不合并（min occupancy 待定，幼年期跳过）。
 * 5. hash 用 XXH64（seed=0，v1.1 §9 已定；64 位熵 + 跨挂载稳定，
 *    抗 DoS 升级路径：siphash + SB 持久 key，接口签名不变）。
 * 6. backup 链容灾不做（幼年期只认 Primary SB：magic/version/crc32/
 *    bitmap 自描述四连验）。
 * ═══════════════════════════════════════════════════════════════════ */

/* 磁盘格式版本：由 disk_struct_v1.3.h 的 RAVENFS_VERSION 宏提供（v1.3 = 4）。
 * v1.1 起版本常量统一下沉到磁盘结构头，避免与宏重定义冲突。 */

/* ── readdir 输出项 ──
 * 名字 ≤ RAVENFS_MAX_NAME_LEN（552B），character 含结尾 '\0'。 */
struct dentry_out {
    uint64_t inum;          /* 目标 inode 编号 */
    uint32_t name_len;      /* 名字长度（不含 '\0'） */
    char     character[RAVENFS_MAX_NAME_LEN];
};
struct bptree_path_t {
    uint64_t blocks[64];
    uint16_t  child_idx[64];
    int      depth;
    bool  is_leaf_hit;//若这里为假说明leaf层右边紧邻的下一个
};
enum ravenfs_device_type_t{
    DEV_LOOP_IMG, DEV_NVME, DEV_SATA_SSD, DEV_SATA_HDD, DEV_UNKNOWN,
};
struct ravenfs_mount_device_t{
    int fd;                 /* 外部打开并负责关闭；类只借不关 */
    uint8_t lba_size_log2;  /* 设备逻辑块大小 log2（512e→9，4Kn→12） */
    ravenfs_device_type_t type;
    bool is_device_direct;  /* 外部是否以 O_DIRECT 打开 */
};
struct ravenfs_mount_param_t{
    bool if_will_use_inode_cache;//警告：若is_device_direct==false则这个bit最好开着，不开缓存的话，相应的inode_session会一次性读4k,因为nvme盘一次至少一个LBA,也就4k,多线程下UC模式两个访问访问到同一个缓存行不同Inode到时候等着写竞争吧
    bool if_will_use_mdata_piece_cache;
    bool is_readonly;       /* true=只读挂载（rfs-audit），须配 is_device_direct=false */
    bool allow_mounted_device = false; /* 防呆逃生阀：设备已被内核挂载时仍继续（默认 false=硬拒 → RC_ERR_BUSY）。仅供取证等极端场景；正常恒定 false */
};
struct linux_userspace_mount_t{
    ravenfs_mount_device_t dev;
    ravenfs_mount_param_t param;
    uint32_t logical_processor_count;//自己想办法，找地方读取,测试时可以填任意数，但真上实际机器那就想办法拿真实数据
};
namespace ravenfs_juvenile_god_ctrl_words_bits{
    constexpr uint64_t READ_ONLY_MASK=1;//由read_only_bit控制
    constexpr uint64_t MMAP_BACKEND_CONTROL_MASK=2;//由ravenfs_mount_device_t.is_device_direct取反无条件设置这个bit,以及那个bit为1时无条件整个盘从0 offset处mmap,后续各段考虑从那个窗口切就行了
    constexpr uint64_t BLOCK_CACHE_ENABLE_MASK=4;//MMAP_BACKEND_CONTROL_MASK为1时强制这个bit翻1,虽然内核会做缓存但是上层业务的预取，写回,失效是有意义的，MMAP_BACKEND_CONTROL_MASK=0时这个位为1则手动用户态缓存，为0则相应的session直接分配内存，硬读
    constexpr uint64_t INODE_CACHE_ENABLE_MASK=8;//同上
};
class RavenFS_juvenile {
public:
    /* 构造即挂载：fd 由外部打开并提供（生命周期完全归外部，类只借不关）。
     * 挂载 = 读 Primary SB → 四连验（magic/version/crc32/bitmap 自描述）
     *      → 双位图全量入内存。
     * 无状态：设备参数（fd/LBA 大小/类型/直写/readonly）全部来自调用者
     * 结构体，类不做设备几何的内部推导与校验——设备探测是外部职责。
     * 契约：param.is_readonly=true 必须搭配 dev.is_device_direct=false
     * （rfs-audit 静态 mmap 审核专用）；readonly 下析构不 flush 位图。
     * 失败时 *res_code 给出 RC_ERR_*，对象处于未挂载态（fd=-1）。 */
    RavenFS_juvenile(const linux_userspace_mount_t &mnt, uint64_t *res_code);
    ~RavenFS_juvenile();            /* 析构即卸载：释放位图内存（readonly 跳过 flush）；fd 归外部，不关 */

    RavenFS_juvenile(const RavenFS_juvenile &) = delete;
    RavenFS_juvenile &operator=(const RavenFS_juvenile &) = delete;

    bool is_mounted() const { return fd >= 0; }

    /* ══════════════════════════════════════════════════════════
     * root_dir 操纵接口（dentry B+tree 原型构建区）
     * 全部以根目录为唯一操作对象，path 概念不存在。
     * ══════════════════════════════════════════════════════════ */
    /* 正版入口：按 inode_type 初始化新 inode ——
     *   INODE_TYPE_DIR    : 新建 dir inode（0755/link_count=2/名字堆哨兵）+
     *                       急建其 dentry 树根（根叶子 2 块，magic + . / .. 指针）；
     *   REGULAR/SOFT_SYM  : 内联空 inode（0644/link_count=1/data_root=PTR_INVALID）
     * 之后统一插入 dentry 树。name 已存在 → RC_ERR_EXIST；
     * > RAVENFS_MAX_NAME_LEN → RC_ERR_NAME_LEN；inode_type 非法 → RC_ERR_STATE。 */
    result_code_t root_dir_insert_dentry(const char *name,uint8_t inode_type);

    result_code_t root_dir_direct_file_pwrite(const char *name,uint64_t offset,uint64_t len,void*buf);//左闭右开区间
    /* root_dir 文件数据接口（punch_hole/truncate/pread）：受理 = REGULAR only
     * （dir→RC_ERR_IS_DIR；SOFT_SYM→RC_ERR_STATE）；字节粒度（非 O_DIRECT）；
     * 规格：Docs/punch_truncate_pread_spec.md */
    result_code_t root_dir_direct_file_punch_hole(const char *name,uint64_t offset,uint64_t len);
    result_code_t root_dir_direct_file_trunacate(const char *name,uint64_t len_new);
    /* 左闭右开区间；*out_bytes = 实际读字节数（EOF 短读/越界读=0，Linux 短读语义） */
    result_code_t root_dir_direct_file_pread(const char *name,uint64_t offset,uint64_t len,void*buf,uint64_t*out_bytes);//左闭右开区间
    /* 物理删除：dentry 树移除（memmove + 刷父 hash_max）→ 目标 inode 全量回收。
     * destroy_data = 销毁模式：数据块 free 前清零（防恢复）；false = 仅释放（块标
     * free，数据残留盘上，同 TRIM 语义）。
     * dir 型目标：删除前置校验目录必须全空（树根叶子 entry_count==0），
     * 非空 → RC_ERR_NOT_EMPTY；空目录连带回收名字堆 + 释放树根 2 块；
     * destroy_data 对 dir 型无额外效果（名字堆块不清零，KISS）。
     * REG/SOFT_SYM 目标：link_count--，归零 → regular_file_reclaim（按形态
     * inline/direct/extent 释放数据 + inode 槽；extent 型叶链遍历释放数据块 +
     * 自底向上释放树节点，零泄露）。回收失败 → RC_ERR_NOT_SUPPORTED 哨兵
     * （树删已生效，inode 孤儿暂挂，fsck 可回收）。未找到 → RC_ERR_NOT_FOUND。 */
    result_code_t root_dir_rm_dentry(const char *name,bool destroy_data);

    /* 查找：命中 → RC_OK 且 *out_inum 输出；未命中 → RC_ERR_NOT_FOUND。 */
    result_code_t root_dir_lookup(const char *name, uint64_t *out_inum);

    /* 遍历 root_dir：按 hash 乱序（v1.1 §6.4 接受）。
     * 返回条目数 n（≥0），失败返回负错误码。
     * 成功时 *out_buf 指向堆上分配的连续数组 dentry_out[n]，
     * 使用时直接取 (*out_buf)[i]（线性数组，非指针数组）。
     * 调用者负责释放：delete[] *out_buf。 */
    int64_t root_dir_readdir(dentry_out **out_buf);

    result_code_t root_dir_readdir_interval(uint64_t base,uint64_t entry_count,dentry_out*buf);//改成这种接口外部自己准备内存

    /* ── 调试 ── */
    void dump_info() const;                     /* SB/几何/位图占用率/根节点 */
    uint64_t get_ctrl_word(){
        return God_CTRL_WORD;
    }

    /* ══════════════════════════════════════════════════════════
     * 可达性审计（新模型；2026-09-30 重写，取代旧 audit_inode 模块）
     * 从 root 先序 DFS 下钻 → 边走边重画两张影子图（块 / inode）→
     * 收口与真实位图 XOR → 分类「没有被保护 / 疑似泄露」。
     * 内部测试/调试手段：只读、不修复；全部经 out（默认 stdout）打印。
     * 复用本类位图（block_test/inode_test）+ 缓存（mdata_piece_session），
     * 但 DFS/路由独立推导（不借 dentry/extent_Bptree_walk）。
     * 前提：mmap 后端（is_device_direct=false）；否则 RC_ERR_STATE。
     * ══════════════════════════════════════════════════════════ */
    /* 主入口：全盘可达性审计。返回 RC_OK = 走完（不代表无异常）；
     * 异常靠打印 + *rep 计数。rep 可空（只要打印）。
     * verbose=false（默认）只打发现/HEAP/SUM；true 逐 inode 打 WALK（大盛盘日志巨大）。 */
    result_code_t audit_dfs_root(audit_walk_report_t *rep = nullptr, FILE *out = stdout,
                                 bool verbose = false);
    /* fsck 修复入口（D-12 骨架，下一战役实现）——本轮仅声明，无实现体：
     * 复用 audit_dfs_root 的 DFS，在发现 DIR link_count 偏差时（repair=true）
     * 拟修复并计入 rep->link_repaired；当前签名占位，勿调用。 */
    result_code_t audit_fsck_root(audit_walk_report_t *rep = nullptr, FILE *out = stdout,
                                  bool verbose = false, bool repair = false);
private:
    uint64_t God_CTRL_WORD;
    enum regular_file_type_t{
    inline_file,direct,extent
    };
    regular_file_type_t ftype(raven_Inode_t*inode);
    /* 通用 B+ 树（extent 机械）节点族：整个 extent 树算法与 magic 无关（节点
     * 布局同构），kind 仅决定"这棵树认哪套 256-bit 魔数"。文件区段树与目录
     * 名字数据面映射树各用一套，避免混树误认（上层纪律保证树内 magic 单一）。 */
    enum extent_tree_kind_t : uint8_t {
        EXTENT_TREE_FILE     = 0,   /* 文件区段树：BPTREE_MAGIC1..4 */
        EXTENT_TREE_NAMEDATA = 1,   /* 目录名字数据面映射树：NAMEDATA_EXTENT_MAGIC1..4 */
    };
    /* 按 kind 校验节点 256-bit 魔数（extent 机械内部统一走此谓词）。 */
    static bool is_extent_node_kind(const Bptree_meta_t *meta, extent_tree_kind_t kind);
    /* 核心版本：按 inode_type 初始化 + 插入。dir 型急建树根（magic + . / ..）；
     * 目标目录 inode_idx 的 data_root 恒有效（v1.1 §10 不变量 11），
     * 遇 PTR_INVALID = 严重内部磁盘契约违反 → RC_ERR_STATE。 */
    result_code_t dir_insert_dentry(uint64_t inode_idx,const char *name,uint8_t inode_type);
    result_code_t dir_rm_dentry(uint64_t inode_idx,const char *name,bool destroy_data);
    result_code_t dir_readdir_interval(uint64_t inode_idx,uint64_t base,uint64_t entry_count,dentry_out*buf);
    result_code_t direct_file_pwrite(uint64_t inode_idx,uint64_t offset,uint64_t len,void*buf);//左闭右开区间
    result_code_t direct_file_punch_hole(uint64_t inode_idx,uint64_t offset,uint64_t len);
    /* 增大 = 上调 size（pwrite 空写骨架：phase1/1.5 分配判别 + 物化 + 零垫底 +
     * phase4；extent 态纯 size 推进零分配），缩小内部编译为 punch_hole 的尾巴
     * 打洞条件 + 形态出口（坍缩 direct / ≤160 强制回迁 inline）——
     * 规格 Docs/punch_truncate_pread_spec.md §6 */
    result_code_t direct_file_trunacate(uint64_t inode_idx,uint64_t len_new);
    /* 左闭右开区间；*out_bytes = 实际读字节数（EOF 短读/越界读=0，Linux 短读语义）
     * 规格 Docs/punch_truncate_pread_spec.md §4 */
    result_code_t direct_file_pread(uint64_t inode_idx,uint64_t offset,uint64_t len,void*buf,uint64_t*out_bytes);//左闭右开区间
    /* 归零回收（link_count==0 时的 REG/SOFT_SYM 全量空间回收，2026-09-07 补票）：
     * 按 ftype 分流 —— inline：inode 内联数据随槽释放（destroy 时先清零数据域）；
     * direct：data_root 直指区间 [data_root, ceil(size/4K)) 释放（destroy 先清零）；
     * extent：最左叶下钻 → 叶链遍历逐记录释放数据块（destroy 先清零）→ 叶块整体
     * 释放 → 自底向上逐层释放内节点 → 根 → inode_free。零泄露。
     * 失败：已释放部分不回滚（位图幂等），返回错误码；调用方（dentry 删除路径）
     * 按孤儿挂账处理（树删已生效）。destroy 清零分段批量写，成本高（销毁语义）。 */
    result_code_t regular_file_reclaim(uint64_t inode_idx,bool destroy_data);
    result_code_t dir_lookup(uint64_t inode_idx,const char *name, uint64_t *out_inum);
    /* pwrite 五段管线 phase1：资源预分配 + 区间规划（pwrite_pipeline.md §5）。
     * 入参 inode = 磁盘 inode 内存映像指针（调用方 InodeSession 作用域内、会话存活期
     * 有效；self_reliat 后端 get 占坑 accessor 防淘汰，见 inode_cache_radix.hpp）。
     * 本函数纯读映像（不 dirty）；phase4 提交由调用方另开 session 调整字段写回。
     * 输出 affect_extents = 纯新段（fblkbase 升序、互不重叠、与现映射零相交、
     * length=块数、block_base 恒有效）；列表即回滚账本 —— 本函数失败 = 逆序
     * block_free 整账本后清空返回错误码，inode 恒未动。
     * inline→inline（off+len ≤ 160）不在受理范围：防御性空返 RC_OK。
     * 分配策略（2026-09-08 定案 + 09-09 拍板）：非树态先 direct 候选
     * （inline 迁出 = hard 整域 [0,ceil(newsize/4K))；direct 增长 = occupy_soft
     * 锚定旧 run 物理尾），失败降级裁剪必需段（inline：物化块∪写域，洞留空；
     * direct：增长段）soft 分配；extent 态逐洞规划，段起点紧贴旧记录终点时
     * 锚定左邻 phys 尾（phase2 map 左并前置）。碎片部分满足 = 继续榨 soft_alloc
     * 裂段（每轮 ≥1 块，有界）；唯一失败面 = 某轮一块都榨不出 → 整体回滚。 */
    result_code_t dfprealloc_and_plan(raven_Inode_t *inode, uint64_t offset, uint64_t len,
                                      std::list<extent_t> &affect_extents);
    /* pwrite 五段管线通用体（2026-09-09 从 direct_file_pwrite 抽出）：pwrite 与
     * truncate 增大（P4 空写骨架）共用。data_is_zero=true 时 phase3 不 overlay 用户
     * 数据——新块零垫底 + 可选 inline 迁出拷贝 + 旧块只清零写域（保周边）；false =
     * 原 pwrite 语义（overlay buf）。buf 在 data_is_zero=true 时可为 nullptr。 */
    result_code_t df_pipeline(uint64_t inode_idx, uint64_t offset, uint64_t len,
                              const void *buf, bool data_is_zero);
    /* 孤儿树节点释放（新建树折弃，punch 物化回滚 / truncate 回滚共用）：只释放节点
     * 2 块（最左叶下钻 → 叶链 → 父集去重自底向上），数据块 phys 零碰（归调用方账本）。
     * 结构异常即弃疗（孤儿留盘 fsck 收）。 */
    void extent_orphan_release(uint64_t tree_root, extent_tree_kind_t kind = EXTENT_TREE_FILE);
    /* 数据资产释放（**不 free inode 槽**）：truncate 缩至 ≤160 强制回迁 inline 前，
     * 释放旧 direct run / extent 树+数据块（机械同 regular_file_reclaim 中枢，仅省
     * inode_free 一步）。shape/data_root/fsize 由调用方快照传入（提交前旧态）。成功 =
     * 资产清空（inode 槽仍在，供回迁写）；失败返回错误码（IO/STATE）。 */
    result_code_t file_data_release(regular_file_type_t shape, uint64_t data_root,
                                    uint64_t fsize, bool destroy_data);
    /* 建空根叶（2026-09-09，pwrite_pipeline.md §9 缺口原语补票）：phase2 物化
     * 第一步（inline/direct → extent 跃迁开树）。alloc 2 块（8K 节点，align 2^1）
     * + meta 初始化（根叶，entry_count=0，parent_ptr.ptr=owner_inode_idx 审计锚，
     * ptrs[0..2]=PTR_INVALID —— 文件树根无链表无 ".."）。root_blk 出参。
     * 失败（无块）→ RC_ERR_NO_BLOCK，root_blk 不变。 */
    result_code_t extent_new_root_leaf(uint64_t owner_inode_idx, uint64_t &root_blk,
                                       extent_tree_kind_t kind = EXTENT_TREE_FILE);
    result_code_t df_extentify(uint64_t inode_idx);//把REGULAR/INODE型的
    /* ══════════════════════════════════════════════════════════
     * 位图接口组 A/B：薄转发 → bitmap_slices_mgr_t（独立模块）
     * 2026-09-03 顶出：完整契约 / 锁纪律 / 几何推导见
     * include/ravenfs/bitmap_slices.hpp（bitmap_slices_mgr_t）。
     * 本层仅保留签名不变的转发壳——调用点（dentry/extent/name_data/
     * root_dir）零改动。失败哨兵 PTR_INVALID（0 是合法块号/槽号）。 */
    uint64_t block_alloc_hard(uint64_t require_count, uint8_t align_log2);
    uint64_t block_alloc_soft(uint64_t require_count, uint64_t *remain_count);
    uint64_t block_occupy_soft(uint64_t blkbase, uint64_t require_count, uint64_t *rest_count);
    void     block_free(uint64_t blkbase, uint64_t count);
    bool     block_test(uint64_t blk) const;
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
    result_code_t write_buff(uint64_t blk, uint64_t blk_len, const void *buf);
    /* ── inode I/O（零缓存）── */
    void inode_read(uint64_t inum, raven_Inode_t *out) const;
    void inode_write(uint64_t inum, const raven_Inode_t *in);

    /* detach 原语对（2026-09-06，pwrite_pipeline.md §9）：
     * root_blk in/out —— 入 = 当前树根，出 = 操作后根（根裂建新根 / 根收缩更迭经引用带出）；
     * owner_inode_idx 降级为纯审计锚（根 meta parent_ptr.ptr = "."，只写不读），
     * 不触发任何 inode I/O —— inode.data_root 回写权收归调用方提交点（direct_file
     * 四步管线 phase4）。顶层 inode 态校验（type/direct_bit/data_root 有效性）删除：
     * 调用方负责传入合法文件树根（本原语仅保 RC_ERR_OVERLAP/NOT_FOUND 等树内语义）。 */
    result_code_t extent_map_range(uint64_t &root_blk, uint64_t owner_inode_idx, const extent_t &ext,
                                   extent_tree_kind_t kind = EXTENT_TREE_FILE); //内部有任何区间有交集会报错；同叶 GLB 紧贴+phys 连续时左融合（扩长不插记录）
    /* 子集删除原语（2026-09-07；unmap 家族唯一接口，extent_unmap_range 退役并入其
     * full 分支）：ext 必须被树内单条记录完全覆盖（逻辑域身份 + phys 偏移自洽
     * 校验防错删）；整条删（full，三字段全等）/ 贴头贴尾缩水（零分配零
     * 欠载）/ 中间拆两端（左段留槽缩尾 + 右段新插，满叶预算前移同 map 原子性）
     * 三分支。phase2 回滚（逆序账本 = 整删 + 贴尾裁）与 punch_hole 段化删除共用。 */
    result_code_t extent_unmap_range_loose(uint64_t &root_blk, uint64_t owner_inode_idx, const extent_t &ext);

    /* 激进重建原语（2026-09-07 落地；融合策略演进史见 Docs/pwrite_pipeline.md §1/§2）
     * 整树范围：叶链收集全部记录 → 可并对全并 → 均分重打包（每节点 168..337）→
     * 自下而上构建新树 → free 旧树 → root_blk 换新根。
     * 失败原子性：新树全部节点建成前旧树只读；任何分配失败 = free 新节点、旧树无损。
     * 低配于 entry[0] 跨界残留 / 随机写中间插入残留 / 历史遗留的兜底面。
     * 已退役：extent_merge_range（区间 F/M 融合）与 extent_discard_range（lesson 已否的
     * 补偿原语）设计作废，签名一并移除。 */
    result_code_t extent_rebuild_tree(uint64_t &root_blk, uint64_t owner_inode_idx);
    /* ── 定制接口（2026-09-30，紧急）──
     * 单 extent 树 → direct 降级：传 owner inode 索引，若其数据形态为 extent 树、
     * 且树**内部仅单条记录**（根叶 count==1）且该记录**完整覆盖 [0, ceil(size/4K))**
     * （无前缀洞 / 无尾洞，direct 语义所需）→ 降级：
     *   · 提交点：一次 InodeSession 写 direct_bit=1、data_root=记录 phys 基址（size 不动）；
     *   · 后置：free 旧树节点（根叶 = 2 个 FS_block）。先 inode 后 free（孤儿窗口，无悬垂）。
     * 返回：RC_OK（已检查；*out_collapsed 报是否降级）/ RC_ERR_IO / RC_ERR_STATE。
     * 不适用（非 REG/LNK、inline/direct 态、树非单条、单条非整域覆盖）= RC_OK + 未降级、零副作用。 */
    result_code_t extent_tree_collapse_to_direct(uint64_t owner_inode_idx, bool *out_collapsed = nullptr);
    /* ── 位图核心 ── */
    bool bitmaps_load();     /* mount 时：直写=全量 pread 入内存；mmap 后端=窗口切片（不预取）。
                                * 两模式共用自描述校验(§6.6)；原始内存喂给 bitmap_slices_mgr_t 装配 */
    void bitmaps_flush();    /* 析构时：仅直写模式 pwrite 全量写回（mmap 后端由内核页缓存管） */
    /* 防呆（ctor 最先调，任何写盘前）：当前 fd 指向的设备是否已被 Linux 内核以某 FS 挂载。
     * true = 已挂载 → ctor 拒绝挂载（RC_ERR_BUSY；防双写者 / 撕裂读）。
     * 块设备：命中自身 dev 号 **或整盘任一在挂载的分区**（整盘兜底，防"整盘仍在用分区"）；
     * 普通文件：反查 loop 设备。判定纯 fstat + /proc + /sys，不依赖外部命令。 */
    bool device_is_kernel_mounted() const;
    /* 缓存子模块编排（ctor 调）：决策树第一层 is_device_direct ——
     *   mmap 后端：inode/mdata_piece 全 offload（canvas 从窗口切）
     *   直写模式 ：inode/mdata_piece 按 param.if_will_use_*_cache 选
     *               self_reliat（用户态缓存）/ no_cache（session 直读）
     * 失败 → 非 RC_OK（已清理已建子模块）。mdata_piece 的 self_reliat
     * 未实装（ctor 拒绝），直写+开块缓存目前挂载失败，TODO 待实装。 */
    result_code_t caches_setup();
    void caches_teardown();  /* 卸载：flush_all（同步点）→ delete 子模块 → 指针置空 */
    /* 缓存回调适配器：私有 read_buff/write_buff → 子模块函数指针（io_ctx = this） */
    static result_code_t fs_io_read(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf);
    static result_code_t fs_io_write(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf);

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
    int  fd = -1;                       /* 设备 fd（外部借入，不负责关闭）；-1=未挂载 */
    ravenfs_mount_param_t param{};      /* 挂载选项（ctor 无条件从结构体拷贝；{} 防析构读未初始化） */
    ravenfs_device_type_t dev_type = DEV_UNKNOWN; /* 设备类别（ctor 从结构体拷贝） */
    uint8_t  lba_size_log2 = 0;         /* 设备逻辑块大小 log2（ctor 从结构体拷贝；0=未设置，无默认假设） */
    bool     is_device_direct = false;  /* O_DIRECT（ctor 从结构体拷贝） */
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

    uint8_t *block_bits = nullptr;      /* 全量内存位图（bitmap_blocks×4096），4K 对齐（posix_memalign） */
    uint8_t *inode_bits = nullptr;      /* 全量内存位图（inode_bm_blocks×4096），4K 对齐（posix_memalign） */
    uint64_t root_inum      = 0;        /* 恒 0（根目录 inode 索引） */
    uint64_t root_dentry_blk = 0;       /* root inode.data_root（树根块号） */

    /* ── 缓存子模块（参数决策树编排后接线；暂 nullptr = 未启用） ──
     * 编排点：ctor 按 is_device_direct + param.if_will_use_*_cache 组装 cfg
     * 并构造子模块（见 caches_setup）；未接线时各处 mdata_piece_session
     * 构造恒失败（防御路径），8K 元数据访问全部报错 —— 这是预期
     * （编排完成前只编译不跑）。 */
    Inode_cache_mgr_t      *inode_cache_mgr  = nullptr;   /* inode 缓存（inode_cache_radix.hpp） */
    mdata_piece_caches_mgr *mdata_piece_mgr  = nullptr;   /* 8K 元数据负载片缓存（mdata_piece_caches.hpp） */
    bitmap_slices_mgr_t    *bitmap_mgr       = nullptr;   /* 双位图分片分配器（bitmap_slices.hpp；2026-09-03 顶出，ctor 装配/dtor 拆除） */

    /* ── mmap 后端（非直写挂载；决策树第一层） ──
     * is_device_direct=false 时整盘从 offset 0 映射（含 SB 区 byte[0,4096)），
     * 位图/缓存 canvas 全部从窗口切片；析构 munmap。直写模式不用（保持 0）。
     * 换算基准（block_model.md §3.1）：FS_block[n] 虚拟地址 = fsblk0_base_ + n×4096，
     * 其中 fsblk0_base_ = mmap_base_ + 4096（Primary SB 占 byte[0,4096)）。
     * 所有切片（位图 ×2、inode/mdata canvas）一律从 fsblk0_base_ 派生，禁止散写。 */
    uint8_t  *mmap_base_ = nullptr;   /* mmap 基址（fd offset 0） */
    uint64_t  mmap_size_ = 0;         /* 映射长度（lseek END；文件/块设备均可用） */
    uint8_t  *fsblk0_base_ = nullptr; /* FS_block[0] 虚拟基址 = 窗口 + 4096（仅 mmap 后端有效；直写恒 nullptr） */

    /* ── CRC32（Castagnoli，SB 校验 + 临时 name hash）── */
    static uint32_t crc32_table[256];
    static bool     crc32_ready;
    static void     crc32_init();
    static uint32_t crc32_compute(const void *data, size_t len);
    struct name_cmlpx_t{
        char*name;
        uint64_t hash;
        uint16_t name_len;              /* strlen(name) 缓存（≤ RAVENFS_MAX_NAME_LEN） */
        uint64_t full_char_remain_offset;/* 插入时调用方预分配的数据面逻辑偏移；≤40B 恒 PTR_INVALID */
        uint64_t inum;                  /* 插入时调用方分配的真实 inode 槽；dentry_leaf_entry_insert 落 inode_ptr.ptr（非插入路径恒 0） */
        uint64_t owner_dir_inum = 0;    /* 本次比较/读取所属目录（读长名 tail 需映射树根）；≤40B 不消费 */
    };
    /* ── 目录名字数据面（name data plane）原语（v1.3；取代 v1.2 名字堆）──
     * 每目录一个稠密、无空洞、8K 对齐的逻辑字节空间：
     *   · 映射 name_data_ctl（direct 单连续段 / tree 映射树根）+ name_data_piece_count；
     *   · 空闲区间 name_data_free_ranges_ctl（inline 表 / fr B+树）。
     * 长名（>40B）tail（≤512B）由本层分配逻辑字节区间，ddentry 存逻辑偏移
     * （full_char_remain_offset）；分配优先复用空闲区间，否则逻辑尾追加 8K 数据片。
     * alloc/free/release 需 dir inode（双树控制字在 raven_dir_Inode_t 内联区）；
     * read/write 亦需 dir inode（逻辑偏移→物理须经映射树/直指基址解析）。 */
    result_code_t name_data_alloc(uint64_t dir_inode_idx, uint16_t tail_len, uint64_t *out_offset);
    result_code_t name_data_free(uint64_t dir_inode_idx, uint64_t offset, uint16_t tail_len);
    result_code_t name_data_read(uint64_t dir_inode_idx, uint64_t offset, uint16_t tail_len, char *out_tail);
    result_code_t name_data_write(uint64_t dir_inode_idx, uint64_t offset, uint16_t tail_len, const char *tail);
    /* 目录销毁时全量回收：映射树节点 + 全部数据片 + fr 树节点 + 双控制字复位 */
    result_code_t name_data_release(uint64_t dir_inode_idx);
    /* 名字数据面内部机械（见 name_data_plane.cpp）：
     *   resolve    —— 逻辑偏移 → 物理块+块内偏移（direct 直算 / tree 走映射树）
     *   map_append —— 追加一个 8K 数据片到逻辑尾（direct→tree 物化 / tree 尾插），
     *                 dino 为调用方已持有的 inode 会话映像（本函数只改其控制字） */
    result_code_t name_data_resolve(raven_dir_Inode_t *dino, uint64_t offset,
                                    uint64_t *out_fblk, uint16_t *out_off);
    result_code_t name_data_map_append(uint64_t dir_inode_idx, raven_dir_Inode_t *dino,
                                       uint64_t phys_base);
    /* ── fr 空闲区间树内部机械（见 name_data_freeranges.cpp；字节粒度）──
     * 节点族 Bptree_dir_name_data_free_ranges_*，魔数 NAMEDATA_FREERANGES_MAGIC1..4；
     * leaf 按 (len,base) 升序，inner hint = 子树最大复合键 (max_len,max_base)；
     * 统一 337 階。best-fit 取用（最小 len ≥ need）；本轮不做相邻合并、欠载不
     * 合并（结构仍为合法搜索树），空洞靠 best-fit 复用。 */
    static bool is_namedata_freeranges_node(const Bptree_meta_t *m);
    static void name_data_fr_node_meta_init(void *buf, uint16_t count,
                                            bool is_inner, bool is_root, uint64_t parent_ptr);
    /* 节点子树最大复合键（leaf = 末项 (len,base)；inner = 末 hint 的 (max_len,max_base)）。 */
    static void name_data_fr_max_key(const void *buf, uint64_t *out_len, uint64_t *out_base);
    result_code_t name_data_fr_walk(uint64_t root_blk, uint64_t need_len, uint64_t need_base,
                                    bptree_path_t *trace);
    result_code_t name_data_fr_refresh_key_chain(uint64_t blk);
    /* best-fit：找最小 len ≥ need 的空闲区间（无 → RC_ERR_NOT_FOUND）。 */
    result_code_t name_data_fr_find_fit(uint64_t root_blk, uint64_t need,
                                        uint64_t *out_base, uint64_t *out_len);
    /* 插入一条空闲区间；空态自建根叶，split 级联建新根。 */
    result_code_t name_data_fr_insert(uint64_t dir_inode_idx, raven_dir_Inode_t *dino,
                                      uint64_t base, uint64_t len);
    /* best-fit 取用：定位最小 len ≥ need 的区间，摘除（len>need 时把余段
     * [base+need, len-need) 原样插回），返回分配到的逻辑偏移。无 → NOT_FOUND。 */
    result_code_t name_data_fr_take_fit(uint64_t dir_inode_idx, raven_dir_Inode_t *dino,
                                        uint64_t need, uint64_t *out_base);
    /* 空叶退役：从父删其 hint + 释放节点；父空则继续上溯；根内部剩 1 孩则收缩。
     * 目的：树内**永不留下 count==0 的叶**（空叶 hint=(0,0) 会破坏 hint 单调性→
     * 二分路由错位）。 */
    result_code_t name_data_fr_retire_empty_leaf(uint64_t dir_inode_idx, raven_dir_Inode_t *dino,
                                                 uint64_t leaf_blk);
    /* 全量释放 fr 树节点（不碰数据片）。 */
    result_code_t name_data_fr_release(uint64_t dir_inode_idx, raven_dir_Inode_t *dino);
    /* 数据面增长一个 8K 片（direct 物理尾 soft_occupy，失败 hard_alloc+映射树注入）。 */
    result_code_t name_data_grow(uint64_t dir_inode_idx, raven_dir_Inode_t *dino);
    /* 名字数据面分配回滚守卫（insert 流程 RAII：分配成功后的任何失败返回
     * 都自动释放，成功路径显式 disarm；覆盖 insert/分裂链路全部失败点） */
    struct name_data_guard_t {
        RavenFS_juvenile *fs = nullptr;
        uint64_t dir_ino = 0;
        uint16_t tail_len = 0;
        uint64_t offset = PTR_INVALID;
        bool armed = false;
        void arm(RavenFS_juvenile *f, uint64_t d, uint16_t n, uint64_t o) {
            fs = f; dir_ino = d; tail_len = n; offset = o; armed = true;
        }
        void disarm() { armed = false; }
        ~name_data_guard_t() {
            if (armed && fs) fs->name_data_free(dir_ino, offset, tail_len);
        }
    };
    /* 新 inode 分配回滚守卫（insert 流程 RAII：inode_alloc 成功后的任何失败
     * 返回都自动 inode_free，成功路径显式 disarm；与 name_data_guard 同
     * 生命周期纪律，覆盖 insert/分裂链路全部失败点） */
    struct inode_guard_t {
        RavenFS_juvenile *fs = nullptr;
        uint64_t inum = 0;
        bool armed = false;
        void arm(RavenFS_juvenile *f, uint64_t i) { fs = f; inum = i; armed = true; }
        void disarm() { armed = false; }
        ~inode_guard_t() {
            if (armed && fs) fs->inode_free(inum);
        }
    };
    /* 块分配回滚守卫（dir 型新建树根专用：block_alloc_hard 成功后的任何失败
     * 返回都自动 block_free 回滚，成功路径显式 disarm；与 inode_guard 同
     * 生命周期纪律，覆盖 insert/分裂链路全部失败点） */
    struct block_guard_t {
        RavenFS_juvenile *fs = nullptr;
        uint64_t blk = 0;
        uint64_t count = 0;
        bool armed = false;
        void arm(RavenFS_juvenile *f, uint64_t b, uint64_t c) { fs = f; blk = b; count = c; armed = true; }
        void disarm() { armed = false; }
        ~block_guard_t() {
            if (armed && fs) fs->block_free(blk, count);
        }
    };
    /* dentry 完整名字比较（树内条目 vs 目标名；>40B 经 owner 目录名字数据面
     * 读外置 tail。owner_dir_inum 由 dir_* 打开 inode 后填入 name_cmlpx_t）。 */
    int dentry_name_cmp(const ddentry_t &e, const char *name, uint16_t name_len,
                        uint64_t owner_dir_inum);
    result_code_t dentry_Bptree_walk(uint64_t root_blk_idx,bptree_path_t*trace,name_cmlpx_t*name);
    result_code_t extent_Bptree_walk(uint64_t root_blk_idx,bptree_path_t*trace,uint64_t logical_block_idx,
                                     extent_tree_kind_t kind = EXTENT_TREE_FILE);
    struct leaf_node_result_t{
        uint16_t greates_lower_bound_idx;
        bool is_idx_hitted;
    };
    /* ══════════════════════════════════════════════════════════
     * extent 空洞描述符 — 文件逻辑块域中无映射的区间
     *
     * 三种 type 把“真实空洞”与“树边界”显式区分（左闭右开）：
     *   normal        : 两 extent 之间的真实空洞 [base, base+gap_len)。
     *                    gap_len==0 = 两 extent 紧邻（无洞，是前向/后向
     *                    合并的直接信号，调用方不得优化掉）
     *   tail_infinite : 树最右叶子末尾之后的空洞。interval_base = 最右
     *                    extent 开边界（该叶子末项 fblkbase+length）；
     *                    gap_len 无意义——真实上界是 inode.size 或本次
     *                    操作区间终点，由调用方裁剪（pwrite 追加主场景）
     *   prev_noexist  : 树最左叶子开头之前无前驱 extent。base/gap_len 无
     *                    意义——头部空洞 [0, 首 extent.fblkbase) 由调用方
     *                    自理（树不承诺文件逻辑起点为 0）
     * ══════════════════════════════════════════════════════════ */
    enum gap_desicritor_type_t{
        normal,tail_infinite,prev_noexist
    };
    struct gap_desicritor_t{//左闭右开
        uint64_t interval_base_logical_block;  /* normal: 空洞起点；tail_infinite: 最右 extent 开边界 */
        uint64_t gap_len;                      /* normal: 空洞长度（块数）；其余无意义 */
        gap_desicritor_type_t type;
    };
    /* ══════════════════════════════════════════════════════════
     * extent_iterator — 稀疏文件空洞/区间游走器
     *
     * 定位：extent_Bptree_walk 落点（叶子 + GLB）之后的游走底座，
     * 为 direct_file_pwrite / punch_hole / truncate / pread 提供
     * “逐 extent + 逐空洞推进”的公共骨架。
     *
     * 位置模型：
     *   - idx ∈ [0, count) 指向叶子内一个真实 extent（当前 extent）；
     *     所有 gap 查询与 ++/-- 均基于它。
     *   - idx == EXTENT_LEAF_GLB_NONE(0xFFFF) = 虚拟前位（GLB=NONE 的
     *     walk 落点，target 在落点叶子所有 extent 之前）：只允许
     *     operator++（进入 idx=0）；其余操作置 erro_flag。
     *     路由不变量保证此位之前绝无 extent 覆盖 target（前驱子树末项
     *     开边界 ≤ target，半开不覆盖），故调用方 ++ 后以
     *     prev_gap() 取头部/跨界空洞即可完整描述落点。
     *
     * 空洞查询（紧邻当前 extent）：
     *   - next_gap(): [cur_end, 下一 extent.fblkbase)；当前为叶子末项时
     *     跨界读后继叶子首项；树最右叶子末项 → tail_infinite
     *   - prev_gap(): [上一 extent 开边界, cur.fblkbase)；当前为叶子首项
     *     时跨界读前驱叶子末项；树最左叶子首项 → prev_noexist
     *
     * 游走（++/--，前缀/后缀）：叶子内槽位步进；越界沿叶子兄弟链表
     * （ptrs[2]=后继 / ptrs[1]=前驱）跳相邻叶子；树边界再越界 →
     * iterator_erro_flag = true 且位置保持不变（不产生悬垂移动）。
     *
     * iterator_erro_flag：构造清零；任何成功操作后为 false，跨界失败
     * （兄弟指针 PTR_INVALID / 链表损坏）/ 越界 / 非法操作置 true。
     * 调用方每次操作后必须检查。
     *
     * 注：迭代器只读不写。需要原地修改 extent（合并/拆分/改 block_base）
     * 时，用 leaf_blk()/idx() 自行开 mdata_piece_session 读改写。
     * ══════════════════════════════════════════════════════════ */
    class extent_iterator{
        RavenFS_juvenile *fs;       /* 盘访问：跨界 gap 需读兄弟叶子首/末项 */
        uint64_t leaf_node_blkbase; /* 当前叶子块号 */
        uint16_t idx;               /* 当前槽位：extent 索引或 0xFFFF 虚拟前位 */
        public:
        bool iterator_erro_flag;
        extent_iterator(RavenFS_juvenile *fs,uint64_t leaf_node_blkbase,uint16_t idx);
        /* 紧邻空洞查询（基于当前 extent）；idx==0xFFFF → erro */
        gap_desicritor_t prev_gap();
        gap_desicritor_t next_gap();
        /* extent 游走：叶子内步进，越界跨界跳兄弟叶子 */
        extent_iterator& operator++();    /* 前缀 */
        extent_iterator operator++(int);  /* 后缀 */
        extent_iterator& operator--();    /* 前缀 */
        extent_iterator operator--(int);  /* 后缀 */
        /* 位置访问：供调用方自行开 session 读/改当前 extent */
        uint64_t leaf_blk() const { return leaf_node_blkbase; }
        uint16_t index() const { return idx; }
        /* 当前 extent 只读副本；idx==0xFFFF → false（非错误）；IO/越界 → erro+false */
        bool current_extent(extent_t &out);
    };
    /* 叶子查找无下界哨兵：greates_lower_bound_idx == 此值 → 无下界，
     * 插入位置 = 0（最左插入）。有效索引范围 [0, 126]，0xFFFF 永不冲突。 */
    static constexpr uint16_t DENTRY_LEAF_GLB_NONE = 0xFFFF;
    /* 区段树叶子 GLB 无下界哨兵（有效索引 [0,336]，0xFFFF 永不冲突） */
    static constexpr uint16_t EXTENT_LEAF_GLB_NONE = 0xFFFF;
    struct split_result_t{
        result_code_t rc;
        uint64_t low_node_blkidx;
        uint64_t high_node_blkidx;
        uint64_t low_node_maxhash;
        uint64_t old_root_ptrs1 = PTR_INVALID;   /* 分裂前旧根的 ptrs[1]（dir 根 = '..'）；非根分裂 = PTR_INVALID */
    };
    leaf_node_result_t dentry_leaf_node_lookup(Bptree_ddentry_leaf_node_t*leaf,name_cmlpx_t*name);/* is_idx_hitted=精准命中 */
    uint16_t dentry_inner_node_lookup(Bptree_ddentry_inner_node_t*node,uint64_t hash);
    /* 区段树叶子 GLB 查找：GLB = 最后一个 fblkbase ≤ target 的 extent；
     * is_idx_hitted = GLB **覆盖** target（target-fblkbase < length，半开区间），
     * 与 dentry 精准命中语义不同——区间键无相等，只有被覆盖/落空洞。
     * GLB == EXTENT_LEAF_GLB_NONE → 无下界（最左插入，idx=0）。 */
    leaf_node_result_t extent_leaf_node_lookup(Bptree_extent_leaf_node_t*leaf,uint64_t logical_block_idx);
    /* 区段树内部节点严格上界路由：找第一个 key > target 的 hint（最小上界）。
     * key = 子树最大开边界 (fblkbase+length)；半开区间语义 target == key 属
     * 下一子树（恰为其最小块），故与 dentry 的 ≥ 不同，必须用 >。
     * 全部 key ≤ target（右击穿）→ 0xFFFF。 */
    uint16_t extent_inner_node_lookup(Bptree_extent_inner_node_t*node,uint64_t target);
    /* 删除策略：dentry_leaf_entry_delete 下游资产维护开关（2026-09-01）
     * DELETE_FULL      = unlink/rmdir/rename 覆盖：维护目标 inode 资产
     *                     （dir 型空校验+全量回收；REG/SOFT_SYM link_count--，归零触发全量空间回收）
     * DELETE_TREE_ONLY = fsck 清理悬挂条目：inode 可能无效，只删树条目（预留，暂未使用） */
    enum delete_policy_t : uint8_t { DELETE_FULL = 0, DELETE_TREE_ONLY = 1 };
    result_code_t dentry_leaf_entry_insert(uint64_t leaf_blk_base_idx,name_cmlpx_t* name,uint16_t idx,uint64_t&result_flags,uint8_t inode_type,uint64_t owner_dir_inode);//下沉版（2026-09-01）：分配+初始化目标 inode（DIR 急建树根/REG/SOFT_SYM 内联）+ 长名外置分配 + 树插 + owner 目录计数维护（size++；仅 DIR 型条目 link_count++，POSIX 语义 D-12）；失败 guard 全回滚。result_flags: bit0=需分裂, bit1=父层max需刷新
    result_code_t dentry_leaf_entry_delete(uint64_t leaf_blk_base_idx,uint16_t idx,uint64_t&result_flags,bool destroy_data);//删条目+维护下游资产（DELETE_FULL 语义）：dir 型目标先校验空（NOT_EMPTY 零副作用）再删，删后全量回收（名字堆/树根 2 块/inode 槽；destroy_data 对 dir 无额外效果）；REG/SOFT_SYM 目标 link_count--，归零 → regular_file_reclaim(inum, destroy_data)（inline/direct/extent 全形态释放 + inode 槽，2026-09-07 补票）。回收失败 → RC_ERR_NOT_SUPPORTED 哨兵（树删已生效，inode 孤儿暂挂，fsck 可回收）。成功时result_flags有效，bits[0]=欠载需合并，bits[1]=父层对应entry的max需刷新
    split_result_t dentry_leaf_split(uint64_t victim_node_blkbase);//会内部进行叶子链表的维护，不会进行父节点的修改
    split_result_t dentry_inner_split(uint64_t victim_node_blkbase);//不会进行父节点的修改
    result_code_t dentry_spilt_parent_apply(uint64_t parent_node_blkbase,uint16_t victim_in_node_id,split_result_t result,bool&is_parent_spilt_acquire);
    result_code_t dentry_node_balance(uint64_t parent_node_blkbase,uint16_t idx,bool&is_parent_need_balance);//给定的父节点以及相应引索，让其平衡则是，若节点到达平衡阈值，向前后邻居先看是否存在总entry数目小于等于126的,小于126则合并，不满足则抹匀
    /* 根收缩：根内部节点只剩 1 个孩子时，把唯一孩子提升为新根。
     * 调用场景：dentry_node_balance 对 idx==0 && parent_count==1 返回
     * RC_ERR_STATE（设计上根收缩由上层处理）；以及合并路径留下的
     * “根 entry_count==1”形态收尾。
     * 语义：
     *   - 非内部节点或 entry_count != 1 → RC_OK（无需收缩）
     *   - root_blk 不是根 → RC_ERR_STATE（调用者用错）
     *   - 成功：孩子 is_root_node=1、parent_ptr.ptr=inode_idx（"."）、
     *     叶子则 ptrs[1]=inode_idx（".." 孤儿自指）/ptrs[2]=PTR_INVALID，
     *     更新 root inode.data_root、root_dentry_blk、释放旧根 2 块 */
    result_code_t dentry_root_shrink(uint64_t root_blk, uint64_t root_inode_idx);
    /* 分裂后新建根内部节点（低/高两个子节点提升为新根的 hints[0]/[1]）：
     * 分配 2 块 → meta 初始化 → 填充 hints → 更新两子节点父指针 → 写回
     * → 更新 root inode.data_root。low_blk/low_max 为左子块号及最大 hash，
     * high_blk 为右子块号（其最大 hash 由本函数读节点现算）。 */
    result_code_t dentry_new_root_inner(uint64_t low_blk, uint64_t low_max,
                                        uint64_t high_blk, uint64_t root_inode_idx,
                                        uint64_t root_dotdot_v);
    /* ══════════════════════════════════════════════════════════
     * extent 原语对内部机械（2026-09-03；extent_map_range/unmap_range 支撑）
     * 镜像 dentry 六件套，差异：key = 子树最大开边界 (fblkbase+length)；
     * 根更迭经 root_blk& 引用上抛（2026-09-06 detach，不再回写 inode）；文件树根无
     * ".."（ptrs[1] 恒 PTR_INVALID）；阶 337：欠载阈值 168（非根）／合并上界
     * total ≤ 337 ／分裂左 168 右 count−168。split_result_t 复用，其
     * low_node_maxhash 字段在 extent 侧承载“左半最大开边界 key”。
     * ══════════════════════════════════════════════════════════ */
    /* 分裂三件套（2026-09-07 预算化）：节点块号由调用方预分配传入（prealloc_blk）——
     * map_range 按树高预算整条分裂链节点（depth+1），分裂函数自身不再 alloc →
     * 无 ENOSPC 失败面；剩余预算由调用方收尾归还（map 原子性闭环）。 */
    split_result_t extent_leaf_split(uint64_t victim_blk, uint64_t prealloc_blk,
                                     extent_tree_kind_t kind = EXTENT_TREE_FILE);   /* 叶裂：链表维护，不动父 */
    split_result_t extent_inner_split(uint64_t victim_blk, uint64_t prealloc_blk,
                                      extent_tree_kind_t kind = EXTENT_TREE_FILE);  /* 内裂：右半孩子 reparent 新块 */
    result_code_t extent_spilt_parent_apply(uint64_t parent_blk, uint16_t victim_idx,
                                            split_result_t r, bool &parent_spilt,
                                            extent_tree_kind_t kind = EXTENT_TREE_FILE);
    /* 根更迭沿引用链穿透（detach，2026-09-06）：三者收 root_blk& in/out；
     * owner_inode_idx 仅写根 meta "." 审计锚，零 inode I/O。 */
    result_code_t extent_new_root_inner(uint64_t low_blk, uint64_t low_key,
                                        uint64_t high_blk, uint64_t owner_inode_idx,
                                        uint64_t &root_blk, uint64_t prealloc_blk,
                                        extent_tree_kind_t kind = EXTENT_TREE_FILE);
    result_code_t extent_node_balance(uint64_t parent_blk, uint16_t idx, bool &need_balance,
                                      extent_tree_kind_t kind = EXTENT_TREE_FILE);
    result_code_t extent_root_shrink(uint64_t &root_blk, uint64_t owner_inode_idx,
                                     extent_tree_kind_t kind = EXTENT_TREE_FILE);
    result_code_t extent_balance_from(uint64_t child_blk, uint64_t owner_inode_idx,
                                      uint64_t &root_blk,
                                      extent_tree_kind_t kind = EXTENT_TREE_FILE); /* 欠载链修复（自下而上，含收尾根收缩） */
    result_code_t extent_refresh_key_chain(uint64_t blk,
                                           extent_tree_kind_t kind = EXTENT_TREE_FILE);    /* 节点 max key 变更后沿父链上溯刷新 */
    /* 分裂级联 + 父 key 刷新（2026-09-07 从 map_range 闭包抽取：map 纯插 / unmap_loose
     * 拆两端共用，单一事实源）：叶已超满（count>337，已 dirty）→ 用预算池打完整条分裂
     * 链（叶裂 → 内层逐级 → 根裂建新根）→ 两半叶父链刷新。预算池由调用方预分配
     * （depth+1 个），pool_used 出参记消耗，剩余归还归调用方统一收尾。 */
    result_code_t extent_split_cascade(uint64_t leaf_blk, const bptree_path_t &trace,
                                       std::vector<uint64_t> &reserve_pool, size_t &pool_used,
                                       uint64_t owner_inode_idx, uint64_t &root_blk,
                                       extent_tree_kind_t kind = EXTENT_TREE_FILE);
    /* 节点基建（2026-09-07 拆文件升格：原 extent_tree.cpp 文件级 static，跨
     * split/rebuild 两域使用故入类；定义随主要消费者在 extent_split.cpp）。
     * kind 决定写入哪套魔数（file / namedata）。 */
    static void extent_node_meta_init(void *node_buf, uint16_t entry_count,
                                      bool is_inner, bool is_root, uint64_t parent_ptr,
                                      extent_tree_kind_t kind = EXTENT_TREE_FILE);
    static uint64_t extent_node_max_key(const void *node_buf);

    /* ── audit 内部机械（juvenile_god/audit.cpp；新可达性模型）── */
    /* 读一个 8K 树节点到 dst（8192B，8K 对齐）；失败 false。 */
    bool audit_node_read(uint64_t blk, void *dst);
    /* 单 inode 访问（先序：置影子 → 校验 → 下钻）。parent_inum = 父目录（root 传 PTR_INVALID）。 */
    void audit_walk_inode(audit_walk_ctx_t &ctx, uint64_t inum, uint64_t parent_inum);
    /* DIR：字段校验 + 目录名字数据面（v1.3）+ dentry 树 DFS + 子目录先序下钻。 */
    void audit_walk_dir(audit_walk_ctx_t &ctx, uint64_t inum, uint64_t parent_inum);
    /* REG / LNK：inline/direct/extent 数据面（size 优先解码）。 */
    void audit_walk_file(audit_walk_ctx_t &ctx, uint64_t inum, const raven_Inode_t *ino);
    /* B+树 DFS 递归核：认领节点块 + 结构不变量；返回子树 max key（dentry=hash / extent=开边界）。
     * children = 输出：dentry 条目目标 inode 收集（由 dir 层先序下钻）。 */
    uint64_t audit_walk_btree(audit_walk_ctx_t &ctx, std::vector<uint64_t> &children,
                              audit_walk_tree_t &t, uint64_t blk, uint64_t owner_inum,
                              uint64_t expect_parent, uint64_t dotdot_expect, bool expect_root,
                              bool is_dentry, uint64_t size_bound, int depth, bool &sub_ok);
    /* 叶链一致性：兄弟链双向自洽 + 链序 == DFS 叶序。 */
    void audit_leaf_chain(audit_walk_ctx_t &ctx, const audit_walk_tree_t &t, uint64_t root_blk,
                          bool is_dentry, uint64_t owner_inum);
    /* 目录名字数据面（v1.3）独立审计：空态/direct/tree 不变量校验 + 全物理块认领。
     * tree 态走映射树 DFS（audit_name_data_tree），自识别 NAMEDATA_EXTENT 魔数。 */
    void audit_name_data_plane(audit_walk_ctx_t &ctx, uint64_t inum, const raven_dir_Inode_t *dino);
    /* 名字数据面映射树 DFS 递归核：校验角色/父指针/叶深/欠载/hint key/
     * extent 逻辑稠密与物理合法；认领节点块与 extent 物理块。返回子树最大开边界。 */
    uint64_t audit_name_data_tree(audit_walk_ctx_t &ctx, uint64_t blk, uint64_t owner_inum,
                                  uint64_t expect_parent, bool expect_root, int depth,
                                  audit_name_data_tree_t &w, bool &sub_ok);
    /* 名字数据面映射树叶链：兄弟链双向自洽 + 链序 == DFS 叶序（违例计 heap_bad）。 */
    void audit_name_data_leaf_chain(audit_walk_ctx_t &ctx, const audit_name_data_tree_t &w,
                                    uint64_t root_blk, uint64_t owner_inum);
    /* 名字数据面「空闲区间树（fr）」独立审计：形态（空/inline/tree）分派 +
     * 节点块认领 + 复合键序/界内/计数校验。违例计 heap_bad。 */
    void audit_name_data_free_ranges(audit_walk_ctx_t &ctx, uint64_t inum,
                                     const raven_dir_Inode_t *dino);
    /* fr 树 DFS 递归核：校验角色/父指针/叶深/hint==子树最大复合键/hint 严格递增/
     * leaf 复合键全局严格递增且 len>0、区间 [base,base+len)⊆[0,limit)；认领节点块。
     * 出参 out_max_{len,base} = 本子树最大复合键。 */
    void audit_fr_tree(audit_walk_ctx_t &ctx, uint64_t blk, uint64_t owner_inum,
                       uint64_t expect_parent, bool expect_root, int depth,
                       audit_fr_tree_t &w, bool &sub_ok,
                       uint64_t *out_max_len, uint64_t *out_max_base);
};
bool is_dentry_node(Bptree_meta_t*meta);
bool is_extent_node(Bptree_meta_t*meta);
/* 名字长度把关：strlen ≤ RAVENFS_MAX_NAME_LEN */
bool name_len_limiter(const char*name);
/* 节点最大 hash：叶子末项 entry.hash / 内部末项 hint.hash_max */
uint64_t node_max_hash(const void *node_buf);

/* 8K 对齐临时缓冲轮子：内部节点（Bptree_*_node_t = 8192B）临时分配用。
 * 8K 对齐双满足：节点整块对齐（结构体直读）+ O_DIRECT 直写对齐（≥逻辑块）。
 * alloc 失败返回 nullptr；free(nullptr) 安全。 */
void *node_buf_alloc();
void  node_buf_free(void *p);
