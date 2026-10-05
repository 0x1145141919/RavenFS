#pragma once
#include "rc.hpp"                    /* result_code_t / RC_* / PTR_INVALID（轻量头，零盘结构依赖） */
#include "../disk_struct_v2.0.h"     /* inode 数据面 fr 用契约坐标类型 inode_dataplane_coordinate_free_range_t（inode 单位 256B）*/
#include "../util/lock.h"            /* 锁原语（本头现用 std::mutex；其余原语供实现取用） */
#include <cstdint>
#include <cstddef>
#include <forward_list>              /* std::forward_list：allocator 交付/回收的区段单链（调用方持有） */
#include <list>                      /* std::list：all_dump 的全量快照双链（调用方持有） */
#include <set>                       /* std::set：主红黑树 = STL 区间序列（base 升序） */
#include <boost/intrusive/set.hpp>   /* Boost.Intrusive set：从红黑树（侵入式 member_hook） */
#include <mutex>                     /* std::mutex：分片唯一锁 slice_lock（可睡） */
#include <shared_mutex>              /* std::shared_mutex：inode 数据面两把读写锁（翻译/空间，可睡） */
#include <list>                      /* std::list：extent 快照出参（P3 落盘用） */

/* 切片配置（enable_slice 入参）——**建树原始依据**：
 * 只提供「本片空闲状态的源数据 + 其在全局位号系的起点 + 位数」；enable_slice
 * 内一次性消费建出主从红黑树，函数返回后**不再引用**该内存。故它是借阅的原始
 * 信息，**非长期持有资产**（mgr_slice_t 只留一个内部化起点，不保存该内存）。 */
struct slice_config_t {
    uint8_t *slice_base;          /* 建树源（片位图内存，4K 对齐，enable_slice 校验） */
    uint64_t in_global_base_idx;  /* 本片源数据在 group 全局位号系的起点（建树时盖到 run.base） */
    uint64_t slice_bitscount;
};

/* ═══════════════════════════════════════════════════════════════
 * fs_run_t — 运行时物理区段（allocator 对外的交付 / 回收单位）
 *
 * 为什么不复用 extent_t：extent_t 是**盘上**区段记录（file 逻辑块 + 物理块 +
 * 长度，24B packed）；分配器只懂「物理块世界」——文件逻辑块号由 pwrite 管线在
 * 拿到 run 之后才决定。混用会把「逻辑 ↔ 物理」的归属错配进分配器。故运行时
 * 交付用极简物理 run {base, len}。
 *
 * 为什么用单链承载：软分配「分配到最后一刻」可能跨多个空闲区间拼出**多段**
 * （碎片化交付）；释放是同一批段的逆向（free_intervals 消费同型链表）。
 * ═══════════════════════════════════════════════════════════════ */
struct fs_run_t {
    uint64_t base;   /* 全局位号起点（与 slice 接口同一坐标视角） */
    uint64_t len;    /* 段长（块数 / 位数） */
};
/* 调用方持有的 STL 单链。换端（kmod / linux ko 无 STL）只替换此别名：
 *   - 提供等价的「前插 + 逐节点取走 + empty」三操作单链 shim 即可，接口签名不变；
 *   - **列表对象与节点生命周期全归调用方**：mgr_slice_t 只借指针，不持有、不落盘。 */
using fs_run_list_t = std::forward_list<fs_run_t>;

/* 全量 dump 用的 STL 双链表（all_dump 出参载体）。与单链**分开命名**：
 * 「交付（多段 run，软分配）」与「快照（全量区间，同步）」是两种用途，
 * 共用一个别名迟早误用。同样归调用方持有，mgr_slice_t 只借指针。 */
using fs_run_dlist_t = std::list<fs_run_t>;

/* ═══════════════════════════════════════════════════════════════
 * v2.0 全局 inode 数据面 — 运行时类型
 *
 * 盘上是 packed `extent_t`（24B，disk_struct_v2.0.h）；运行时翻译树用下面这个
 * 独立类型，避免 packed/对齐耦合。
 *
 * ★ fr（空闲面）**必须**用契约坐标类型 `inode_dataplane_coordinate_free_range_t`
 *   （inode 单位，1 inode = 256B）——禁止用 fs_run_t / free_range_t：前者是
 *   块/位通用 run，后者是名字数据面**字节**坐标；三者同尺寸异坐标系，混用即腐烂。
 *   该类型直接来自 disk_struct_v2.0.h（本头已 include）。
 *
 *   翻译面（extent 树）：key = lblk（逻辑 4K 块），值 = {phys_base, length}；
 *   空闲面（fr 主从树）：inode_dataplane_coordinate_free_range_t {base, len}。
 *
 * 坐标恒等式（disk_layout_v2.0 §4）：
 *   lblk = inum >> 4 ; 片内偏移 = (inum & 31) * 256 ; 1 片 = 32 inode = 8K = 2 块
 * ═══════════════════════════════════════════════════════════════ */
/* inode 数据面 fr 装载条目 —— **必须**用契约坐标类型（inode 单位，1 inode = 256B）。
 * 禁止用 fs_run_t / free_range_t（前者是块/位通用 run，后者是名字数据面**字节**坐标）；
 * 三者同尺寸异坐标系，混用即「坐标腐烂」。类型真值 = disk_struct_v2.0.h：
 *     struct inode_dataplane_coordinate_free_range_t { uint64_t base; uint64_t len; };
 */

struct fs_inode_extent_t {       /* 运行时翻译树元素（单位 = 4K 块） */
    uint64_t lblk;               /* 逻辑 4K 块起点（主树序键；偶） */
    uint64_t phys_base;          /* 物理 FS_block 起点（偶 = 8K 片对齐） */
    uint64_t length;             /* 逻辑 4K 块数（偶；每 8K 片 = 2） */
};
struct inode_extent_less {
    bool operator()(const fs_inode_extent_t &a, const fs_inode_extent_t &b) const {
        return a.lblk < b.lblk;  /* 只按逻辑键排序；物理/长度由不变量约束 */
    }
};
using inode_extent_tree_t = std::set<fs_inode_extent_t, inode_extent_less>;

/* inode_locate 出参：物理位置（**不做引用计数**——计数归 mdata_piece_session）。 */
struct inode_location_t {
    uint64_t blkbase;        /* 8K 片起始物理 FS_block（偶；mdata_piece blkidx 纪律） */
    uint16_t inblk_offset;   /* 片内字节偏移 = (inum & 31) × 256，值域 [0,8192) */
};

/* inode 数据面无状态初始化配置（调用方组装，ctor 一次消费；**零 I/O**）——
 * 装载读盘（SB + fr 快照 + 映射 B+tree）归上帝类，模块只吃现成数组。
 * ⚠ 生命周期：数组归调用方，ctor 返回后不再引用（建树即消费）。 */
struct inode_data_plane_cfg_t {
    uint64_t pieces = 0;                        /* 8K 片数（= Σ extent.length / 2） */
    const fs_inode_extent_t     *extents = nullptr; uint64_t extent_count = 0;
    const inode_dataplane_coordinate_free_range_t *fr = nullptr; uint64_t fr_count = 0;   /* inode 单位 */
    /* 用尽增长批量（片）：先试 block_alloc_hard(2*batch,1)，失败逐级折半降至 1；
     * 0/1 = 单片。 */
    uint32_t grow_batch_pieces = 1;
};

/* ═══════════════════════════════════════════════════════════════
 * 内部组合：主从红黑树（本后端 = 纯内存、**位粒度**）
 *
 *   主树（STL 区间序列 RB）: key = base 升序           —— 户籍 / 真相源
 *   从树（侵入式 RB，挂在主树元素的 workload 内）: key = (len 主, base 辅)
 *                                                      —— 派生索引，供按尺寸查
 *
 * 为什么单位是「位」而非「块」：后端要同时服务 **block**（区间分配）与
 * **inode**（逐个分配）两个场景，位粒度是二者的共同底座；block 场景里
 * 1 FS_block = 32768 位，块↔位换算由上层承担。
 *
 * 融合纪律（**主树**）：区间融合一律「先删除、再重新插入」——不做原地改
 * key/len（std::set 元素 const，且融合必改 base/len，重插最省心）。从树同拍：
 * 先 erase 被波及节点，再 insert 融合后的新区间。
 *
 * 主树不变式（**硬**）：base 升序下，**相邻区间严格不重叠** ——
 *   prev.base + prev.len ≤ next.base。该不变式 + base 序 ⇒
 *   pos_.lower_bound(base) 的前驱/后继恰为 merge 邻域；free_interval 落位前的
 *   重叠检测是它的守卫（撞重叠 → 报错，绝不静默改写）。
 * ═══════════════════════════════════════════════════════════════ */

/* 主树元素 = 一个空闲区间（workload）；从树钩子直接嵌进来（Boost.Intrusive
 * set_member_hook，零额外分配）。std::set 元素是 const，钩子声明 mutable 才能
 * 被从树改写；挂/摘时对 iterator 取址做一次**有界** const_cast（只动钩子、不动
 * 主树 key base，故安全）。 */
struct fs_interval_t {
    uint64_t base;   /* 全局位号起点 —— 主树序键 */
    uint64_t len;    /* 长度（位数） */
    mutable boost::intrusive::set_member_hook<> by_size;  /* 从树侵入钩子 */
};

/* 主树序：base 升序（区间序列）。
 * ⚠ 该序仅在「主树不变式：相邻区间不重叠」下有意义：它是**邻域定位**的序，
 *   不管辖重叠；重叠由 free_interval 的重叠检测负责。 */
struct fs_pos_less {
    bool operator()(const fs_interval_t &a, const fs_interval_t &b) const {
        return a.base < b.base;
    }
};
struct fs_size_less {  /* 从树序：len 主、base 辅（复合全序；余量相同再按 base） */
    bool operator()(const fs_interval_t &a, const fs_interval_t &b) const {
        return a.len != b.len ? a.len < b.len : a.base < b.base;
    }
};

using fs_pos_tree_t   = std::set<fs_interval_t, fs_pos_less>;          /* 主树：区间序列 */
using fs_slave_tree_t = boost::intrusive::set<                          /* 从树：侵入式 RB */
    fs_interval_t,
    boost::intrusive::member_hook<fs_interval_t,
                                  boost::intrusive::set_member_hook<>,
                                  &fs_interval_t::by_size>,
    boost::intrusive::compare<fs_size_less>,
    boost::intrusive::constant_time_size<false>>;

class fs_allocator_t;   /* 前向：inode 数据面 grow 借外层 block 组取片 */
class RavenFS_juvenile; /* 前向：上帝类友元——同步时直读私有树/位图（all_sycn） */

/* ═══════════════════════════════════════════════════════════════
 * inode_data_plane_t — 全局 inode 数据面（v2.0 弹性面；fs_allocator_t 的 inode 组）
 *
 *   ┌─ 翻译面 extent 树（key=lblk）── 翻译锁 extent_lock_（读/写）
 *   └─ 空闲面 fr 主从红黑树（inode）── 空间锁 fr_lock_（读/写）
 *
 * ★ 全局锁序（硬）：**空间锁(fr) > 翻译锁(extent)**；**空间锁 > block 片锁**（grow
 *   取片在 fr 写锁内）。**禁止任何路径先持翻译锁再取空间锁**（否则与 grow 的
 *   空间→翻译 反序死锁）。
 *   接口分工：locate = 翻译读；alloc 常规 = 空间写、用尽 = 空间写→翻译写；
 *   free = 空间写；test = 空间读。
 *
 * ★ 不变量：
 *   · append-only：运行期只 grow 追加/扩尾，从不改已有 extent 的映射（故
 *     locate 返位后到开 mdata_piece 之间无改址风险）；direct→tree、离线压实
 *     走整树换根（写锁）。
 *   · 逻辑空间稠密无洞 `[0, pieces*32)`；fr ⊆ 其中、互不重叠、不含 idx 0。
 *   · extent：lblk 偶、phys_base 偶、length 偶>0，且 `Σlength == pieces*2`。
 *
 * 移植注记：同 mgr_slice_t —— 锁为可睡 shared_mutex（临界区含 STL malloc）。
 * ═══════════════════════════════════════════════════════════════ */
class inode_data_plane_t {
public:
    inode_data_plane_t() = default;
    ~inode_data_plane_t();
    inode_data_plane_t(const inode_data_plane_t &) = delete;
    inode_data_plane_t &operator=(const inode_data_plane_t &) = delete;
    friend class RavenFS_juvenile;   /* 同步直读私有树（all_sycn） */

    /* 装配：校验 cfg（对齐/稠密/无洞/无重叠/Σ/idx0）+ 建 extent 树 + fr 主从树。
     * owner = 外层分配器（grow 走 owner->block_alloc_hard / block_free 取/退片）。
     * 失败返回非 RC_OK（对象已自清理，可弃）。 */
    result_code_t init(const inode_data_plane_cfg_t &cfg, fs_allocator_t *owner);

    /* 翻译：inum → 物理 8K 片基 + 片内字节偏移。翻译读锁。 */
    result_code_t locate(uint64_t inum, inode_location_t *out) const;
    /* 分配一个 inode（best-fit）；用尽 → grow 增片（空间写→翻译写）。 */
    result_code_t alloc(uint64_t &out_inum);
    /* 释放一个 inode（fr 插入自动融合）；幂等（重复释放忽略）；inum==0 拒绝。空间写。 */
    void          free_inode(uint64_t inum);
    /* 是否已分配（= 在 [0,pieces*32) 且不在 fr）。空间读。 */
    bool          test_inode(uint64_t inum) const;

    /* 几何/快照（读锁内取一致量；P3 落盘用） */
    uint64_t pieces() const;
    uint64_t free_total() const;
    uint64_t extent_count() const;
    result_code_t dump_fr(std::list<inode_dataplane_coordinate_free_range_t> *out) const;   /* inode 坐标区间 */
    result_code_t dump_extents(std::list<fs_inode_extent_t> *out) const; /* 翻译面全量 */
private:
    fs_allocator_t *owner_ = nullptr;
    uint32_t grow_batch_ = 1;
    uint64_t pieces_ = 0;

    inode_extent_tree_t extents_;                 /* 翻译面（extent_lock_ 保护） */
    mutable std::shared_mutex extent_lock_;       /* 翻译锁 */

    fs_pos_tree_t   fr_pos_;                      /* 空闲面主树：base 升序 */
    fs_slave_tree_t fr_size_;                     /* 空闲面从树：len 主 base 辅 */
    uint64_t fr_free_ = 0;                        /* 空闲 inode 计数（增量） */
    mutable std::shared_mutex fr_lock_;           /* 空间锁 */

    /* grow：须持 fr_lock_ 写锁；内部再取 extent_lock_ 写锁 + owner_ block 片锁。 */
    result_code_t grow_locked();
    /* fr 取一个 inode（best-fit）；须持 fr_lock_ 写锁。 */
    result_code_t alloc_locked(uint64_t &out_inum);
    /* fr 是否已含 inum（重复释放/幂等判定）。须持 fr_lock_。 */
    bool fr_contains(uint64_t inum) const;
};

class mgr_slice_t {
private:
    bool is_workable = false;//构造时候必须为false（NSDMI 兜底）；enable/shutdown 分别校验 false->true / true->false
    //其它的外部公开接口必须在workable的时候才能使用
    /* 本片源数据在 group 全局位号系的起点（enable 时由 slice_config_t 拷入）。
     * **内部化**：内部即红黑树、直接产出**全局 run**，本字段只在建树时用来给
     * 每个 run.base 盖全局起点；对外一律全局区间语言——不暴露此偏移，上层也
     * 无需做局部↔全局换算。 */
    uint64_t in_global_base_idx = 0;
    /* 本片位数（enable 时由 slice_config_t 拷入）——与起点共同界定片的全局半开
     * 区间 [in_global_base_idx, in_global_base_idx + slice_bitscount_)。
     * 供 test_interval/free_interval 的**上界**校验与 soft_occupy 的片尾夹断；
     * 同样内部化，对外不暴露。 */
    uint64_t slice_bitscount_ = 0;

    /* ── 内部组合：主从红黑树（纯内存，位粒度）── */
    fs_pos_tree_t   pos_;                 /* 主树：区间序列（base 升序）= 户籍/真相源 */
    fs_slave_tree_t size_;                /* 从树：Boost.Intrusive，钩子在区间里 */
    uint64_t        last_finish_hint_ = 0;/* 分配游标（只进不退，连续性来源） */
    uint64_t        free_count_ = 0;      /* 空闲位数（增量维护；get_free_count 读它） */
public:
    /* 锁型**确立 = 可睡 std::mutex**（非自旋）。理由：片上临界区要 push/pop STL
     * 链表节点，节点走 malloc —— 用户态 malloc **不保证非阻塞**（arena 锁竞争 /
     * brk-mmap 系统调用 / 缺页 I/O 都可能睡眠），自旋锁持锁者一旦被 malloc 挂起
     * 就是空转甚至死锁/convoy。故**持锁方必须允许睡眠**。
     * 片内临界区仍**零主动 IO**（全量快照走 all_dump，IO 在锁外）；锁只是允许被
     * malloc 短暂挂起。路由若要「忙则换片」，用 std::mutex::try_lock。 */
    std::mutex slice_lock;
    uint64_t get_free_count(){return free_count_;};
    /* 下面均为**全局区间语言**：传入/传出/内部红黑树 key 均为全局位号——
     * 本片只持有全局空间里的自己那一段，无局部坐标系、无换算。 */
    /* enable_slice：借阅 config 原始信息一次性建出主从红黑树；返回后不再引用该
     * 内存（故可随即释放 / 复用）。 */
    result_code_t enable_slice(slice_config_t config);
    /* 硬分配：片内找 bitscount 连续空闲位、起始片内偏移 2^align_log2 对齐；
     * 从 last_finish_hint 起扫，段尾绕回 [0, hint) 再扫一次；不部分分配。 */
    result_code_t alloc_hard(uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx);
    /* 软分配（无锚版·**碎片优先**）：从从红黑树 size_（len 升序）的**小碎片端**
     * 起逐段消费，「分配到最后一刻」——直到满足 bitscount 或片内无空闲可摘。
     * 只碰小端，大块留在 size_ 尾巴上不动（不抢 alloc_hard 的连续大段）。
     * 段与段之间不要求连续（碎片化交付）；每段自身连续、起始 2^align_log2 对齐。
     *
     * 策略（用户裁 2026-10-03）：**碎片优先**——按 len 从小到大消费；**整段优先**，
     * 只有最后一段不足时才劈（挖尾，余段 base 不变）。不做 opcode/双函数：无第二
     * 调用者前不引入策略旋钮；真需要时另开函数而非开关。
     *
     * 契约：
     *   入：*out_runs 由调用方持有（期望空；函数内 **push_front 构建**，不 clear、
     *       不 delete → 容器生命周期归调用方）；align_log2 通常为 0；
     *   *out_runs == nullptr / align_log2 > 63 / bitscount == 0 → RC_ERR_STATE；
     *   出：≥1 段 → RC_OK（**允许部分交付**：Σ len < bitscount 也算成功，
     *       这正是 best-effort 的本分）；片内一位未扣 → RC_ERR_NO_BLOCK；
     *   all_satisfied（引用出参）= 本次交付是否已达 bitscount（= Σ len ≥ bitscount）。
     *       ⚠ **分片级**判决：本片单靠自己是否够。跨分片「穷尽所有分片后是否完全
     *       满足」的全局判决由路由层聚合（逐片累计 Σ len）；若要 in/out 链式语义
     *       回一句改为读入参。
     *   总量单源：Σ run.len（all_satisfied 只是它的布尔归类，不替代总量）。
     * ⚠ 片上调用须持 slice_lock（锁为**可睡 std::mutex**：临界区含 malloc，见锁注记）。
     *   内核端 shim 喂给 STL 容器的分配器须与锁型匹合（可睡锁 ↔ 可睡分配器）。 */
    result_code_t soft_alloc(uint8_t align_log2, uint64_t bitscount,
                             fs_run_list_t *out_runs, bool &all_satisfied);
    result_code_t test_interval(bool val, uint64_t base, uint64_t len);//断言一个区间是否属于全空闲，或者全占用
    result_code_t free_interval(uint64_t base, uint64_t len);//返还一个空闲记录，会先在主红黑树里面重叠检测，检测到重叠会报错
    /* 返还区间链表（free_intervals）：函数内**消费（drain）**调用方链 *runs——
     * 逐段取走节点，每段走一遍 free_interval（主树重叠检测 + 邻接合并）；
     * 容器对象仍归调用方（函数不 delete 容器，
     * 消费后链为空、可复用）。段间无顺序要求，段内物理连续。
     *
     * 契约：
     *   入：*runs 为待释放段（典型来源 = soft_alloc 产出 / 上层 extent 拆分）；
     *   *runs == nullptr → RC_ERR_STATE（无操作，不动任何段）；
     *   出：全部段处理完 → RC_OK（*runs 已空）；遇越界 / 非法段 → RC_ERR_*，
     *       **已消费段不回滚**（释放天然不可逆），余段保留在链上供调用方续查。
     *   别名：free_intervals = 多段版 free_interval（单段是 n==1 的退化）。
     * ⚠ 片上调用须持 slice_lock。 */
    result_code_t free_intervals(fs_run_list_t *runs);
    /* all_dump：把本片**全部空闲区间按 base 递增序** dump 进调用方双链表。
     *   - 入：*out_runs 由调用方持有（期望空；函数内**尾插**构建，不 clear/delete，
     *       容器生命周期归调用方）；*out_runs == nullptr → RC_ERR_STATE；
     *   - 纯内存遍历主树 by_pos（中序 = 递增序），**不碰任何 IO**；
     *   - shutdown 前同步 + 定时同步均走此机制：调用方持锁取快照 → 解锁 → 落盘
     *     （IO 永远在锁外）；**外部仍须持 slice_lock 调用**；
     *   - 出：处理完 → RC_OK（片内全空时列表保持空，同为 RC_OK）。 */
    result_code_t all_dump(fs_run_dlist_t *out_runs);
    /* 锚定软占领（定点对·对偶 free_interval）：从 base 起贪心认领连续空闲
     * 前缀，撞占用/额度上限即停。real_occupy=len−*rest_bits(入=已满足量,
     * 链式记账) 为上限；出 = len−claimed 总缺口；撞片尾如实回传 rest（跨界
     * 续圈归路由层）；claimed==0 → RC_ERR_NO_BLOCK；cap 片内夹断永不越片；
     * hint 只进不退。 */
    result_code_t soft_occupy(uint64_t base, uint64_t len, uint64_t *rest_bits);
    result_code_t shutdown();//必须是从is_workable==true转变为is_workable==false才算成功，is_workable==false初始化状态则返回错误
};
struct slices_cfg_t {
    /* 位图原始内存（已加载/已映射），生命周期归调用方；4K 对齐 */
    uint8_t  *block_bits_raw  = nullptr;
    uint64_t  block_bits_count = 0;    /* block 组位数（= bitmap_blocks×4096×8） */
    /* v2.0：inode 组**不再是位图源**，改为全局弹性数据面（extent 翻译 + fr 空闲面）。
     * 装载读盘归上帝类；本 cfg 只吃现成数组，ctor 一次消费即弃（见 inode_data_plane_cfg_t）。 */
    inode_data_plane_cfg_t inode{};
    /* preferred 槽数 + 2^n 上界推导输入；0 → 退化单槽 */
    uint32_t  logical_processor_count = 1;
    /* 片下限（位数）：1 个位图块 = 32768 bit，4K 对齐切片约束，勿动 */
    uint64_t  min_slice_bits = 32768;
};//只会使用一次给各分片构建自己的树，出了构造函数就可以释放原始位图内存
class fs_allocator_t {
public:
    /* ctor 即装配：推导 slice_count → 双组 enable_slice → preferred 初始化。
     * 失败时 *res_code 给出错误码且对象已自清理（成员为空/未分配态）。
     * 位图原始内存不取所有权（cfg 指针只读消费）。 */
    fs_allocator_t(const slices_cfg_t &cfg, uint64_t *res_code);
    ~fs_allocator_t();
    fs_allocator_t(const slices_cfg_t &) = delete;
    fs_allocator_t &operator=(const slices_cfg_t &) = delete;
    friend class RavenFS_juvenile;   /* 同步直读 block 片树（位图回写） */

    /* ── block 组（FS_block 分配器）── */
    /* 硬分配：require_count 连续 + 起始块号 2^align_log2 对齐；单片内完成
     * （不跨片拼接）；全片失败 → PTR_INVALID（原子，不部分分配）。 */
    uint64_t block_alloc_hard(uint64_t require_count, uint8_t align_log2);
    /* 软分配（跨片·碎片优先·**穷尽所有分片**）：自 preferred 起逐片调片内
     * soft_alloc 从小碎片端消费，累计到 require_count 或分片穷尽。**all-or-nothing**：
     * 中途发现所有分片都吃干抹净却仍不够 → 自动回滚已取段 + 返回空间不足。
     *   入：*runs 由调用方持有（期望空）；align_log2 通常为 0；
     *   出：凑满 → RC_OK（*runs = 多段链，可直接逐段 map extent）；
     *       不够 → RC_ERR_NO_BLOCK（*runs 已回滚为空）。 */
    result_code_t block_alloc_soft(uint64_t require_count, uint8_t align_log2,
                                   fs_run_list_t *runs);
    /* 软锚定占领（定点对·对偶 block_free）——尾部延展法轮子：blkbase 起
     * 认领 ≤ require_count 连续空闲块。锚点片唯一 → 片锁不可换（阻塞抢）；片尾
     * 跨界续圈内部消化（下一片 base 0 继续，逐片单锁 = 真实连续）。
     *   出：认领 ≥1 块 → RC_OK（**结果即入参 blkbase 原址**，claimed =
     *       require_count − *rest_count）；一块未占 → RC_ERR_NO_BLOCK。
     *   *rest_count 出 = 缺口。⚠ 可能产出跨片 run → block_intervals_free 链式归还。 */
    result_code_t block_occupy_soft(uint64_t blkbase, uint64_t require_count, uint64_t *rest_count);
    /* 区间释放（跨片循环）：物理连续 run 可能跨片——逐段定位所属片释放；
     * 释放无连续要求，无需跨片双持。 */
    result_code_t     block_free(uint64_t blkbase, uint64_t count);
    /* 链式释放（跨片）：消费 run 链，逐段定位所属片 free_interval；
     * 已消费段不回滚（释放不可逆），余段留链。 */
    result_code_t block_intervals_free(fs_run_list_t *runs);
    bool     block_test(uint64_t blk) const;

    /* ── inode 组（v2.0 全局数据面）：薄转发 → inode_data_plane_t ──
     * inode_locate 只翻译物理位置（不做引用计数——计数归 mdata_piece_session）。 */
    result_code_t inode_alloc(uint64_t &out_inum);
    void          inode_free(uint64_t inum);
    bool          inode_test(uint64_t inum) const;
    result_code_t inode_locate(uint64_t inum, inode_location_t *result);
    /* 数据面探针（审计/测试；几何与快照走 inode_data_plane_t 自身接口） */
    inode_data_plane_t       &inode_plane()       { return inode_plane_; }
    const inode_data_plane_t &inode_plane() const { return inode_plane_; }
    /* ── 几何/审计 ── */
    uint64_t slice_count() const        { return slice_count_; }
    uint32_t cpu_slots() const          { return cpu_slots_; }
    uint64_t block_per_slice_bits() const { return blk_.per_slice_bits; }
    uint64_t block_free_total() const;   /* 各片 free_count 求和（空闲块总数） */
    uint64_t inode_free_total() const;   /* 数据面空闲 inode 数 */
    uint32_t block_preferred_of(uint32_t cpu) const;  /* dump_info 用 */
    //全部刷回实际上涉及到io的事情，实质上是属于上帝类，模块不碰
private:
    /* 一组分片的全套状态（block 组）。
     * ⚠ 树版**不持全局位图描述符**（片自持主从树、无全局坐标回指）；每片的建树源
     *   （slice_config_t）由 ctor 按 **2^n 上界 ∩ 满片数** 推导切好后，一次性喂给
     *   各片 enable_slice，不驻留（enable 返回即可弃）。 */
    struct group_t {
        mgr_slice_t *slices = nullptr;   /* [slice_count_]：树版片自治单元 */
        uint64_t per_slice_bits = 0;
        uint32_t *preferred = nullptr;   /* [cpu_slots_]：每 CPU 亲和片游标 */
    };
    group_t blk_;                        /* block 组分片 */
    inode_data_plane_t inode_plane_;     /* v2.0 全局 inode 数据面（extent 翻译 + fr 空闲） */
    uint32_t slice_count_ = 0;
    uint32_t cpu_slots_ = 0;
};