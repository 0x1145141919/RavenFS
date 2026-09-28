#pragma once
#include "../disk_struct_v1.1.hpp"   /* result_code_t / RC_* / PTR_INVALID */
#include "../util/lock.h"            /* rw_try_spinlock_t（2026-09-03 收编：原嵌 God 类，随位图模块顶出） */
#include <cstdint>
#include <cstddef>
#include <mutex>                      /* bitmap_mutex_t（03 §4 chg_mu，可睡眠；ko 移植换内核 mutex） */
#include <atomic>                     /* record 节点池原子 LIFO（03 §6，刀②） */

/* ═══════════════════════════════════════════════════════════════════
 * bitmap_slices.hpp — 双位图分片分配器独立模块（2026-09-03 从上帝类顶出）
 *
 * 原形态：fs_bitmap_descriptor_t / slice_config_t / bitmap_slice_t /
 * rw_try_spinlock_t 全部嵌套在 RavenFS_juvenile 内，路由函数（block_alloc_*
 * / block_occupy_soft / block_free / inode_alloc...）是上帝类私有方法——
 * 测试被迫挂整个上帝类 + 镜像才能测路由（抽象泄露）。
 *
 * 现形态（对齐 inode_cache / mdata_piece 的"cfg 大结构体无状态初始化"范式）：
 *   bitmap_slices_mgr_t = 双组（block + inode）分片分配器，纯内存、零 I/O、
 *   零设备知识（比 inode_cache 更纯：连 read/write 回调都不需要）。
 *   - cfg 只喂"已加载/已映射的位图原始内存 + 位数 + CPU 数"，生命周期归调用方
 *   - ctor：2^n 上界 ∩ 满片数约束推导 slice_count → 双组切片 enable →
 *     per-CPU preferred 初始化（inode 片 0 hint=1 跳过 root）
 *   - dtor：delete[] 自有 slices/preferred 数组（位图内存不碰）
 *   - 调用方责任：位图原始内存的加载（pread）/窗口映射（mmap）/校验/写回，
 *     及平台身份 get_processor_id()（外部合约，见 cpu_id_stub.cpp 头注释）
 *
 * 设计文档：Docs/runtime_tech/02_bitmap_slices.md（含锁纪律/不变量契约）；
 * 三端移植注记：换锁实现与 cpu 身份即可，纯算法零平台依赖。
 * ═══════════════════════════════════════════════════════════════════ */

/* ── 位图全局描述符（bit i ↔ block[i] / inode[i]，FS_blk_base=0）── */
struct fs_bitmap_descriptor_t {
    uint64_t FS_blk_base;
    uint64_t *bitmap_raw;   /* 4K 对齐（posix_memalign / mmap 窗口切片） */
    uint64_t bits_count;
};

/* 切片配置（enable_slice 入参；坐标 = parent_descriptor 全局位号） */
struct slice_config_t {
    uint8_t *slice_base;    /* 片位图基址（4K 对齐，enable_slice 校验） */
    uint64_t slice_bitscount;
};

/* ═══════════════════════════════════════════════════════════════
 * CHUNK 后端类型（03_chunk_backed_slices.md §2/§4/§6 增量，2026-09-28）
 * FULL 模式不参与：下列类型只在 backing_mode=BITMAP_BACKING_CHUNK 时被构造/消费。
 * ═══════════════════════════════════════════════════════════════ */

/* backing 模式（装配期二选一，§2 A/B 开关） */
enum bitmap_backing_mode : uint8_t {
    BITMAP_BACKING_FULL  = 0,   /* 默认：全量常驻 = 02 文档现行为（逐位不变红线） */
    BITMAP_BACKING_CHUNK = 1,   /* 懒加载：每片一个 chunk 缓冲 + 控制器数组 */
};

/* 2bit 摘要（§7）：ALL_USED = 盘态无空闲（alloc 扫描可免载入跳过）/
 * ALL_FREE = 整窗可判空闲 / MIXED = 须上线查。
 * record 追加必降级到 MIXED（§6 摘要降级铁律，否则错误 ENOSPC＝§10.5）。 */
enum : uint8_t {
    CHUNK_SUM_ALL_USED = 0,
    CHUNK_SUM_ALL_FREE = 1,
    CHUNK_SUM_MIXED    = 2,
};

/* online_chunk 无窗上线哨兵（§4；换片读失败后回落到此） */
constexpr uint32_t BITMAP_CHUNK_NONE = 0xFFFFFFFFu;

/* CHUNK 分支内部结果码（对齐 RC_NEED_SPLIT「内部使用」惯例：不进全局码表，
 * 只由 mgr 路由层消化不外泄给调用方）。RC_CHUNK_BUSY = 片锁忙/换片被抢
 * → 路由换片（god try 纪律，§10.10）。 */
constexpr result_code_t RC_CHUNK_BUSY = 0x1100;

/* 换片互斥锁（§4 chg_mu）：可睡眠，evict/load/record 回灌/TTL 写全串行于此；
 * 铁律 = 盘 I/O 永不进 slice_lock（§10.3）。util/lock.h 只有 try 自旋锁（无睡锁），
 * 故用户态用 std::mutex；ko 移植换内核 mutex（§12），接口语义不变。 */
using bitmap_mutex_t = std::mutex;

/* CHUNK 模式的盘后端回调（保持本模块“零设备知识”契约：模块只认“某组的位图
 * 块区间”，fd/sb_bread 全由 ctx 解释）。CHUNK 必填；FULL 忽略（§2）。 */
struct bitmap_chunk_io_t {
    /* 读/写 is_inode 组的 [disk_block_rel, +nblocks) 到/自 buf（4K 块粒度、4K 对齐）。
     * 返回 0 = 成功。 */
    int (*read)(void *ctx, bool is_inode, uint64_t disk_block_rel,
                void *buf, uint64_t nblocks);
    int (*write)(void *ctx, bool is_inode, uint64_t disk_block_rel,
                 const void *buf, uint64_t nblocks);
};

/* chunk 控制器（§4：上线前即构造好，靠它做一切定位/摘要/脏标/record 凭据） */
struct chunk_ctl_t {
    uint64_t disk_block_rel = 0;          /* 本 chunk 首位图块（相对本组盘起点，4K 粒度） */
    uint64_t tail_bits = 0;               /* 尾 chunk 有效位数；非尾 = chunk_bits（§3） */
    uint8_t  summary = CHUNK_SUM_MIXED;   /* 2bit 摘要（mount 扫描初始 / evict 重算） */
    uint8_t  dirty = 0;                   /* ONLINE 且被改（TTL 按它跳过干净 chunk，§9） */
    uint8_t  has_recs = 0;                /* 片级链上有覆盖本 chunk 的 record（降级凭据，刀②） */
};

/* §6 片级 record 链节点（池分配）。挂片级链、不按 chunk 切分——block_free 每片
 * 本就只产生单段（02 §5 逐段模式），单次 free 至多每片一个节点，长 run 也只 1
 * 节点，节点数天然有界（池深）。 */
struct rec_node {
    uint64_t slice_off = 0;   /* 片内偏移视角的连续区间 [slice_off, +len) */
    uint64_t len = 0;
    rec_node *next = nullptr;
};

/* §6 record 节点池：per-group 共享原子 LIFO（预建数组 + 下标空闲链，头 =
 * {tag32|idx32}，tag 每次 CAS +1 抗 ABA）→ slice_lock 内取/还零堆分配、零睡眠，
 * 是“锁内零分配”（§5/§10.3）与“记录数天然有界 = 池深”（§6）的结构性保证。 */
struct rec_pool_t {
    rec_node *nodes = nullptr;     /* 预建数组（cfg.rec_pool_depth，缺省 64） */
    uint32_t *free_link = nullptr; /* 空闲链下标（与 nodes 平行） */
    uint32_t  cap = 0;
    std::atomic<uint64_t> head{0}; /* {tag32|idx32}；idx=NIL → 空 */
    static constexpr uint32_t NIL = 0xFFFFFFFFu;
    rec_node *acquire();           /* 池空 → nullptr（调用方走强制上线直清兜底，§6） */
    void release(rec_node *n);     /* 归还（须来自本池） */
};

/* 片级 backing（§4；挂在 bitmap_slice_t 下。mode=FULL 时全字段惰性不建——
 * backing 指针恒 nullptr）。buffer/snapshot 装配期一次分配；chg_mu 换片互斥；
 * rec_head 片级 record 链（池节点，backing 不拥有池）。 */
struct slice_backing_t {
    uint8_t  mode = BITMAP_BACKING_CHUNK;          /* cfg 拷贝 */
    uint8_t  chunk_order = 4;                      /* chunk = 4K<<order 字节；有效 0..8 */
    uint32_t chunks_per_slice = 0;
    uint32_t online_chunk = BITMAP_CHUNK_NONE;     /* 当前上线窗；变更一律持 slice_lock */
    uint32_t gen = 0;                              /* 快路径复检（下线/发布时递增，§4） */
    uint8_t *buffer = nullptr;                     /* 固定 chunk 字节，4K 对齐 */
    uint8_t *snapshot = nullptr;                   /* 换片 evict / TTL 快照（chg_mu 下独占） */
    bitmap_mutex_t chg_mu;                         /* 换片互斥：evict/load/drain/TTL 写 串行 */
    chunk_ctl_t *ctls = nullptr;                   /* 控制器数组，chunks_per_slice 个 */
    rec_node *rec_head = nullptr;                  /* 片级 record 链（刀②；池节点，不拥有） */
    rec_pool_t *pool = nullptr;                    /* per-group 共享池（§6，不拥有） */
    const bitmap_chunk_io_t *io = nullptr;
    void    *io_ctx = nullptr;
    bool     is_inode = false;
    /* mount 自描述三段校验参数（§7.4；装配期拷自 cfg，FULL 忽略） */
    bool     skip_self_check = false;
    uint64_t self_check_lo = 0;
    uint64_t self_check_hi = 0;
};

/* 测试钩子（03 §10.3 审计）：本线程当前是否正持某片 slice_lock（CHUNK 临界区
 * 经 bitmap_slices.cpp 内部包装计数）。mock chunk_io 在回调里断言它恒为 false
 * ——「盘 I/O 永不进 slice_lock」由此可被测试直接验证。FULL 模式恒 false。 */
extern "C" bool ravenfs_bitmap_in_slice_lock(void);

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slice_t — 分片位图自治单元（原 God 嵌套类，原样顶出）
 *
 * 自治纪律（详细见 Docs/runtime_tech/02_bitmap_slices.md §3/§8）：
 *   1. 片与片零共享：无跨片状态、无跨片锁 → 并行度 = 片数
 *   2. 本类所有方法一律不加锁：临界区由外部用 slice_lock 编排
 *      （try_lock_write → 操作 → unlock_write），方法是纯位操作
 *   3. 刻意不搞构造/析构函数：往 64B 缓存行对齐设计（数组化分配）
 *   4. 坐标双视角：对外全部用全局位号（parent_descriptor 视角），
 *      内部换算 片内偏移 = 全局位号 − base_idx
 * ═══════════════════════════════════════════════════════════════ */
class bitmap_slice_t {
private:
    uint8_t *slice_base;            /* 4K 对齐 */
    uint64_t base_idx;              /* 片首全局位号（=(slice_base−parent->bitmap_raw)×8） */
    uint64_t slice_bitscount;
    fs_bitmap_descriptor_t *parent_descriptor;
    uint64_t free_count;            /* enable 时 O(片) 扫出，增量维护 */
    uint64_t last_finish_hint;      /* 分配游标（片内偏移，分配连续性来源） */
public:
    rw_try_spinlock_t slice_lock;
    /* ── CHUNK 后端（03 §4；FULL 恒 nullptr，装配/释放全由 mgr 编排）──
     * 挂上后下列 chunk_* 方法才参与；上述 FULL 方法逐字不动（基线红线）。 */
    slice_backing_t *backing = nullptr;
    uint64_t get_free_count();
    uint64_t get_hint();
    void set_hint(uint64_t new_hint);
    /* 下面均为全局位图区间视角：传入/传出均为全局位号，内部路由换算 */
    result_code_t enable_slice(fs_bitmap_descriptor_t *global_descriptor, slice_config_t config);
    /* 硬分配：片内找 bitscount 连续空闲位、起始片内偏移 2^align_log2 对齐；
     * 从 last_finish_hint 起扫，段尾绕回 [0, hint) 再扫一次；不部分分配。 */
    result_code_t alloc_hard(uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx);
    /* 软分配：第一个空闲段取对齐候选，实际 = min(段长, bitscount)；
     * 缺口回传 bits_left_to_be_allocated。 */
    result_code_t alloc_soft(uint8_t align_log2, uint64_t bitscount, uint64_t &out_base_idx,
                             uint64_t &bits_left_to_be_allocated);
    /* 区间测试：[base, base+len) 全同 val → RC_OK / 有异位 → RC_ERR_NOT_FOUND */
    result_code_t test_interval(bool val, uint64_t base, uint64_t len);
    /* 区间释放：清位，free_count 按实际清掉的 1→0 位计数（重复释放不虚高） */
    result_code_t free_interval(uint64_t base, uint64_t len);
    /* 锚定软占领（定点对·对偶 free_interval）：从 base 起贪心认领连续空闲
     * 前缀，撞占用/额度上限即停。real_occupy=len−*rest_bits(入=已满足量,
     * 链式记账) 为上限；出 = len−claimed 总缺口；撞片尾如实回传 rest（跨界
     * 续圈归路由层）；claimed==0 → RC_ERR_NO_BLOCK；cap 片内夹断永不越片；
     * hint 只进不退。 */
    result_code_t soft_occupy(uint64_t base, uint64_t len, uint64_t *rest_bits);
    void disable_slice();

    /* ═══ CHUNK 懒加载分支（03 §5/§8；仅 backing 模式有效，FULL 不参与）
     * 锁纪律例外声明：上述 FULL 方法一律不加锁（02 §3，临界区由 mgr 编排）；
     * 下列 chunk_* 方法**自管锁**——位操作持 slice_lock（try 语义同 god 纪律，
     * 忙则返回 RC_CHUNK_BUSY 交路由换片），一切盘 I/O 持 chg_mu 且零进 slice_lock
     * （§10.3 铁律），快路径不碰 chg_mu（§5，与 FULL 同速）。 */

    /* 装配：几何登记 + 挂 backing（与 enable_slice 差异：不碰常驻全图，raw 可为
     * null；free_count 留 0 由 mount 顺序扫描填——计数不驻留，§7） */
    result_code_t enable_slice_chunked(fs_bitmap_descriptor_t *global_descriptor,
                                       uint64_t base_bit, uint64_t bitscount,
                                       slice_backing_t *bk);
    /* §7 mount 扫描单窗：chunk_io 顺序读入 buffer → 一趟吃入本片 free_count +
     * ctl 2bit 摘要 + 自描述三段校验（失败 RC_ERR_STATE 拒挂；I/O 失败 RC_ERR_IO）。
     * 构造期唯一 I/O 源（持 chg_mu，§10.3 字面成立；FULL ctor 零 I/O 不进本函数）。 */
    result_code_t chunk_mount_scan(uint32_t ctl);
    /* §8 路由迭代器（alloc 族外壳）：hint 窗起顺序向片尾推进，扫到片尾未中 →
     * 绕回 [0, hint) 窗序列再来一遍（02 §4 语义不变）；每窗 = 快/慢路径 + 单窗 find。
     * 返回 RC_OK / RC_ERR_NO_BLOCK（全片扫毕含绕回 = SLICE_EMPTY）/
     * RC_CHUNK_BUSY（锁忙换片）/ RC_ERR_STATE（换片 I/O 失败）。 */
    result_code_t chunk_alloc_hard(uint8_t align_log2, uint64_t bitscount, uint64_t &out);
    result_code_t chunk_alloc_soft(uint8_t align_log2, uint64_t bitscount, uint64_t &out,
                                   uint64_t &bits_left_to_be_allocated);
    /* §8 occupy 路由：锚点窗起逐窗贪心前缀，顶到窗尾换下一窗（跨 chunk 续圈 =
     * 原跨片续圈同构）；*rest_bits 口径与 soft_occupy 完全一致（入 = 已满足量，
     * 出 = len − claimed 总缺口；撞占用如实回传不硬跳）。 */
    result_code_t chunk_soft_occupy(uint64_t base, uint64_t len, uint64_t *rest_bits);
    /* §6 区间释放：在线直改（dirty=1，零 record 零 I/O）/ 离线挂 record（零 I/O、
     * O(1)）/ 池空兜底强制上线直清（零第二批记录）。自管锁（兜底路径须解锁走慢路径），
     * 区间须整落本片（路由层已按片切段）。 */
    result_code_t chunk_free_interval(uint64_t base, uint64_t len);
    /* block_test / inode_test 的 CHUNK 版（§1.1 语义不变）：在线窗直读 / record
     * 覆盖 = 逻辑空闲 / 摘要裁决（ALL_USED/ALL_FREE 免载入）/ MIXED 离线 → 上线查。 */
    bool     chunk_test_bit(uint64_t base);
    /* §9 TTL 三态分派（单片遍历 chunk 数组）：ONLINE&&dirty → 快照写；OFFLINE&&
     * 链上有覆盖 record → 读→应用→写→摘节点；其余跳过。 */
    void     chunk_sync();
    /* mgr teardown 用：释放 buffer/snapshot/ctls/backing（幂等；FULL = 空操作） */
    void     release_backing();

private:
    /* §5 快/慢路径状态机（盘 I/O 零进 slice_lock）：锁片 → 复检在线窗（快路径
     * HIT，纯内存）→ 不在线：摘要裁决（ALL_USED 免载入跳过）→ chg_mu 下换片
     * → 重试。返回 RC_OK（已持 slice_lock 且窗已上线）/ RC_ERR_NO_BLOCK /
     * RC_CHUNK_BUSY / RC_ERR_STATE。 */
    result_code_t chunk_lock_window(uint32_t ctl);
    /* 慢路径（chg_mu 下全串行）：下线旧窗（online→NONE + 快照 + 清 dirty）→ evict
     * 写（锁外）→ 读目标窗（锁外）→ ★ 发布前先 drain record（§5/§6 顺序铁律）→
     * 发布（online=ctl、gen++、slice_base 重指，锁内零 I/O 零睡眠）。 */
    result_code_t chunk_ensure_online(uint32_t ctl);
    /* 单窗原语（调用方已持 slice_lock 且窗已上线）：口径与 FULL 版完全一致，仅
     * 扫描区间夹到本窗（§8 窗口化；越界由调用方迭代器防）。 */
    result_code_t window_alloc_hard(uint64_t from, uint64_t to, uint8_t align_log2,
                                    uint64_t bitscount, uint64_t &out);
    result_code_t window_alloc_soft(uint64_t from, uint64_t to, uint8_t align_log2,
                                    uint64_t bitscount, uint64_t &out,
                                    uint64_t &bits_left_to_be_allocated);
    result_code_t window_soft_occupy(uint64_t wlo, uint64_t whi, uint64_t base,
                                     uint64_t len, uint64_t *rest_bits);
    /* ★ record 还账核心（slice_lock 内、零 I/O，刀②）：链 ∩ 本窗 1→0 清位 + 摘/缩/
     * 劈分节点。target_words 下标 = slice_off − wlo（发布时 = buffer；TTL 态 2 =
     * snapshot，buffer 归线上窗所有不碰）。free_count 挂账时已记，此处不动计数（§5）。
     * 返回本窗实际 1→0 清位数：>0 ⇒ 发布时须置 dirty（还账后 buffer ≠ 盘态，
     * 否则 evict 按 dirty==0 跳写 = 已还账的 free 永不落盘，§10.6）。 */
    uint64_t drain_window_locked(uint32_t ctl, uint64_t *target_words);
    /* §7 摘要重算（须持 slice_lock）：链上有待还 record → 恒 MIXED（§10.5 防
     * 错误 ENOSPC）；src = buffer（evict）或 snapshot（TTL 态 2 写回后）。 */
    void     recompute_summary_locked(uint32_t ctl, const void *src);
    /* 在线窗直清 [from,to)（须持 slice_lock 且窗在线）：按实际 1→0 计入 free_count
     *（同 FULL 口径）+ 置 dirty（TTL 按脏回写，§9） */
    void     clear_bits_locked(uint64_t from, uint64_t to);
    /* 本窗区间（片内偏移）：[ctl<<15+order, min(+chunk_bits, 片尾))（§3 索引合成） */
    void     ctl_window(uint32_t ctl, uint64_t *wlo, uint64_t *whi) const;
    /* §9 态 2 单 chunk 三态分派（已持 chg_mu 时调用） */
    void     chunk_sync_one(uint32_t ctl);
};

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slices_cfg_t — 无状态初始化配置（调用方组装，mgr 只消费不持有）
 * ═══════════════════════════════════════════════════════════════ */
struct bitmap_slices_cfg_t {
    /* 位图原始内存（已加载/已映射），生命周期归调用方；4K 对齐 */
    uint8_t  *block_bits_raw  = nullptr;
    uint64_t  block_bits_count = 0;    /* block 组位数（= bitmap_blocks×4096×8） */
    uint8_t  *inode_bits_raw  = nullptr;
    uint64_t  inode_bits_count = 0;    /* inode 组位数（= inode_bm_blocks×4096×8） */
    /* preferred 槽数 + 2^n 上界推导输入；0 → 退化单槽 */
    uint32_t  logical_processor_count = 1;
    /* 片下限（位数）：1 个位图块 = 32768 bit，4K 对齐切片约束，勿动 */
    uint64_t  min_slice_bits = 32768;
    /* ── CHUNK 后端（03 §2 增量；下面默认值 = 不传任何新字段的现有调用方零变化）── */
    uint8_t  backing_mode  = BITMAP_BACKING_FULL;
    uint8_t  chunk_order   = 4;      /* 4K << order 字节；有效 0..8（4K..1MB），默认 64K */
    const bitmap_chunk_io_t *chunk_io = nullptr;   /* CHUNK 必填；FULL 忽略 */
    void    *chunk_io_ctx  = nullptr;
    uint32_t rec_pool_depth = 64;    /* §6 record 节点池深度（0 → 缺省 64） */
    bool     skip_self_check = false;/* 跳过 mount 自描述校验（单测合成位图用，§7.4） */
    /* mount 自描述三段校验的 block 组尾段 [lo,hi)（02 §1「尾巴全 1 ×2 段」并段 =
     * inode_bm_base..total_blocks 位视角；inode 组恒查 bit0(root) 亮灯）。FULL 忽略
     * （gong 侧 bitmaps_load 自行校验）。注：03 §7.4 只写了校验没给字段名，此两字段
     * 为实现该校验的最小补充。 */
    uint64_t self_check_lo = 0;
    uint64_t self_check_hi  = 0;
};

/* ═══════════════════════════════════════════════════════════════
 * bitmap_slices_mgr_t — 双位图分片分配器（block 组 + inode 组）
 *
 * slice_count 推导是两组**共享**的（2^n 上界 ∩ 两组满片数最小值）——
 * 故合并单 mgr 而非双实例（双实例会重复推导且几何可能分叉）。
 *
 * 锁纪律（继承自旧上帝类路由，Docs §5/§8）：
 *   - alloc_*：try 锁，忙则换片（preferred 亲和扫描，仿 FPA::alloc）
 *   - free / occupy_soft（定点对）：自旋强制抢锁（定点不可换片/释放不可丢）
 *   - block_occupy_soft 片尾跨界续圈在内部消化（逐片单锁前缀链 = 真实
 *     连续，永不跨片双持）；可能产出跨片 run → block_free 跨片循环归还
 *
 * 移植注记：get_processor_id() 为外部合约（本模块 cpp extern 声明），
 * 内核端必须提供强定义；用户态由 weak 桩/测试强定义覆盖（cpu_id_stub.cpp）。
 * ═══════════════════════════════════════════════════════════════ */
class bitmap_slices_mgr_t {
public:
    /* ctor 即装配：推导 slice_count → 双组 enable_slice → preferred 初始化。
     * 失败时 *res_code 给出错误码且对象已自清理（成员为空/未分配态）。
     * 位图原始内存不取所有权（cfg 指针只读消费）。 */
    bitmap_slices_mgr_t(const bitmap_slices_cfg_t &cfg, uint64_t *res_code);
    ~bitmap_slices_mgr_t();
    bitmap_slices_mgr_t(const bitmap_slices_mgr_t &) = delete;
    bitmap_slices_mgr_t &operator=(const bitmap_slices_mgr_t &) = delete;

    /* ── block 组（FS_block 分配器）── */
    /* 硬分配：require_count 连续 + 起始块号 2^align_log2 对齐；单片内完成
     * （不跨片拼接）；全片失败 → PTR_INVALID（原子，不部分分配）。 */
    uint64_t block_alloc_hard(uint64_t require_count, uint8_t align_log2);
    /* 软分配：一次只从单片取（preferred 优先），实际 = min(区间长, require)；
     * *remain_count 出 = 缺口；一块没有 → PTR_INVALID。 */
    uint64_t block_alloc_soft(uint64_t require_count, uint64_t *remain_count);
    /* 软锚定占领（定点对·对偶 block_free）——尾部延展法轮子：blkbase 起
     * 认领 ≤ require_count 连续空闲块。锚点片唯一 → 自旋强制抢锁；片尾
     * 跨界续圈内部消化（下一片 base 0 继续，逐片单锁 = 真实连续）。
     * 返回：占领 ≥1 块 → blkbase（原址）；一块未占 → PTR_INVALID。
     * *rest_count 出 = 缺口。⚠ 可能产出跨片 run → block_free 跨片循环归还。 */
    uint64_t block_occupy_soft(uint64_t blkbase, uint64_t require_count, uint64_t *rest_count);
    /* 区间释放（跨片循环）：物理连续 run 可能跨片——逐段定位所属片释放；
     * 释放无连续要求，无需跨片双持。 */
    void     block_free(uint64_t blkbase, uint64_t count);
    /* 测试：直读位图内存（无锁；调用方保证并发安全） */
    bool     block_test(uint64_t blk) const;

    /* ── inode 组（inode 槽分配器；inode 0 = root，片 0 hint 从 1 起）── */
    uint64_t inode_alloc();
    void     inode_free(uint64_t inum);
    bool     inode_test(uint64_t inum) const;

    /* ── 几何/审计 ── */
    uint64_t slice_count() const        { return slice_count_; }
    uint32_t cpu_slots() const          { return cpu_slots_; }
    uint64_t block_per_slice_bits() const { return blk_.per_slice_bits; }
    uint64_t inode_per_slice_bits() const { return ino_.per_slice_bits; }
    uint64_t block_free_total() const;   /* 各片 free_count 求和（空闲块总数） */
    uint64_t inode_free_total() const;
    uint32_t block_preferred_of(uint32_t cpu) const;  /* dump_info 用 */
    uint32_t inode_preferred_of(uint32_t cpu) const;

    /* §9 TTL 三态分派（CHUNK 专属；FULL = 空操作，维持 gong 侧现全量回写行为）：
     * 遍历双组 chunk 数组，只写脏/有待还 record 的 chunk，不拿全片锁——
     * 替代 02 §5 的“每 30s 全量回写 + 全片锁”，锁阻塞面降为零（§0 写放大消除）。 */
    void     chunk_ttl_sync();

private:
    /* 一组位图的全套状态（block / inode 各一） */
    struct group_t {
        fs_bitmap_descriptor_t desc{};
        bitmap_slice_t *slices = nullptr;
        uint64_t per_slice_bits = 0;
        uint32_t *preferred = nullptr;   /* [cpu_slots_]：每 CPU 亲和片游标 */
        /* ── CHUNK 后端装配参数（03 §2；backing_mode=FULL 时全字段不消费）── */
        uint8_t  backing_mode = BITMAP_BACKING_FULL;
        bool     is_inode = false;
        uint8_t  chunk_order = 4;
        const bitmap_chunk_io_t *chunk_io = nullptr;
        void    *chunk_io_ctx = nullptr;
        uint32_t rec_pool_depth = 64;
        bool     skip_self_check = false;
        uint64_t self_check_lo = 0;
        uint64_t self_check_hi = 0;
        rec_pool_t *rec_pool = nullptr;  /* §6 per-group 共享 record 池 */
    };
    group_t blk_, ino_;
    uint32_t slice_count_ = 0;
    uint32_t cpu_slots_ = 0;

    void teardown();                     /* delete[] 自有数组（幂等） */
    /* 装配一组：按 nbits 与 slice_count_ 计算 per_slice_bits 并逐片 enable；
     * 组内 preferred 由 ctor 统一初始化。失败 → 非 RC_OK（已清理本组 slices）。 */
    result_code_t setup_group(group_t &g, uint8_t *raw, uint64_t nbits);

    /* ── CHUNK 分支（同在 bitmap_slices.cpp；FULL 路径只多一条装配期常量分支）──
     * 03 §13 刀①②③ 落点：换片状态机 / record+drain+摘要 / mount 扫描+TTL。 */
    result_code_t setup_group_chunk(group_t &g, uint64_t nbits); /* 装配 + §7 mount 顺序扫描 */
    void     release_group_chunk(group_t &g);      /* 释放 backing 资产与 record 池 */
    uint64_t chunk_alloc_hard(group_t &g, uint64_t require_count, uint8_t align_log2);
    uint64_t chunk_alloc_soft(group_t &g, uint64_t require_count, uint64_t *remain_count);
    uint64_t chunk_occupy_soft(group_t &g, uint64_t blkbase, uint64_t require_count,
                               uint64_t *rest_count);
    void     chunk_free(group_t &g, uint64_t blkbase, uint64_t count);
    bool     chunk_test(const group_t &g, uint64_t bit) const;
};
