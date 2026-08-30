#pragma once
#include "../disk_struct_v1.1.hpp"
#include "cache_mgr_mode.hpp"
#include "../magics.h"

/* 缓存子模块前置声明（仅成员指针用，不拉完整头）：
 * 完整类型由使用方 cpp 各自 include inode_cache_radix.hpp /
 * mdata_piece_caches.hpp —— 这两个头会拉 util/tsc.h → <chrono>，
 * 若在 ravenfs_v1.1.hpp 直接 include，任何用
 * `#define private public` 包裹本头的测试（test_root_dir_*）会把
 * 标准库模板头一并污染，gcc16 下 sstream 可访问性检查直接报错。 */
class Inode_cache_mgr_t;        /* inode 缓存管理器（inode_cache_radix.hpp） */
class mdata_piece_caches_mgr;   /* 8K 元数据负载片缓存管理器（mdata_piece_caches.hpp） */

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

/* 磁盘格式版本：v1=1，v1.1=2（disk_layout_v1.1.md §1：v1.1 起 bump）
 * 与 mkfs/mkfs.hpp 同值；挂载校验用 */
constexpr uint32_t RAVENFS_VERSION = 2;

/* ── readdir 输出项 ──
 * 名字 ≤ RAVENFS_MAX_NAME_LEN（552B），character 含结尾 '\0'。 */
struct dentry_out {
    uint64_t inum;          /* 目标 inode 编号 */
    uint32_t name_len;      /* 名字长度（不含 '\0'） */
    char     character[RAVENFS_MAX_NAME_LEN];
};
struct dentry_bptree_path_t {
    uint64_t blocks[64];
    uint16_t  child_idx[64];
    int      depth;
    bool  is_finally_hit;
    // bool  is_break_biggest_record;
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

    /* 创建名为 name 的普通文件：
     *   inode_alloc → 初始化(REGULAR, 0644, link_count=1, data_root=PTR_INVALID
     *   内联空文件) → inode_write → dentry 树插入。
     * name 已存在 → RC_ERR_EXIST；> RAVENFS_MAX_NAME_LEN → RC_ERR_NAME_LEN。 */
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
    /* 读写尝试自旋锁：读共享 / 写独占，全部 try 语义（拿不到立即返回
     * false，不阻塞自旋）。调用方拿不到锁时自行决定重试或换片。
     * 用户态实现用 std::atomic；sparrowos 内核端移植时替换为 arch
     * 原子原语（xchg/cmpxchg），接口语义不变。
     * 无公平性保证：try 语义下饥饿由调用方路由策略承担。 */
    class rw_try_spinlock_t {
    public:
        rw_try_spinlock_t() : state_(0) {}
        rw_try_spinlock_t(const rw_try_spinlock_t &) = delete;
        rw_try_spinlock_t &operator=(const rw_try_spinlock_t &) = delete;

        bool try_lock_read() {
            uint32_t s = state_.load(std::memory_order_relaxed);
            do {
                if (s & WRITE_BIT) return false;        /* 写者持有 → 放弃 */
            } while (!state_.compare_exchange_weak(s, s + 1,
                         std::memory_order_acquire, std::memory_order_relaxed));
            return true;                                /* 读者计数 +1 */
        }
        void unlock_read() {
            state_.fetch_sub(1, std::memory_order_release);
        }
        bool try_lock_write() {
            uint32_t expect = 0;
            return state_.compare_exchange_strong(expect, WRITE_BIT,
                         std::memory_order_acquire, std::memory_order_relaxed);
        }
        void unlock_write() {
            state_.store(0, std::memory_order_release);
        }
    private:
        static constexpr uint32_t WRITE_BIT = 0x80000000u;  /* 高 1 位=写锁，低 31 位=读者计数 */
        std::atomic<uint32_t> state_;
    };
    uint64_t get_ctrl_word(){
        return God_CTRL_WORD;
    }
private:
    uint64_t God_CTRL_WORD;
    struct fs_bitmap_descriptor_t{
        uint64_t FS_blk_base;
        uint64_t*bitmap_raw;//必须保证4k对齐
        uint64_t bits_count;
    };
    struct slice_config_t{
        uint8_t*slice_base;//也是必须保证4k对齐，根据
        uint64_t slice_bitscount;
    };
class bitmap_slice_t{
        private:
        uint8_t*slice_base;//也是必须保证4k对齐，根据
        uint64_t base_idx;//根据(slice_base-parent_descriptor->bitmap_raw)<<3推断
        uint64_t slice_bitscount;
        fs_bitmap_descriptor_t*parent_descriptor;
        uint64_t free_count;
        uint64_t last_finish_hint;
        public:
        rw_try_spinlock_t slice_lock;
        uint64_t get_free_count();
        uint64_t get_hint();
        void set_hint(uint64_t new_hint);
        //很明显下面是全局位图区间，传入的参数以及传出的引索基址，不过内部路由换算
        result_code_t enable_slice(fs_bitmap_descriptor_t*global_descriptor,slice_config_t config);
        result_code_t alloc_hard(uint8_t align_log2,uint64_t bitscount,uint64_t&out_base_idx);
        result_code_t alloc_soft(uint8_t align_log2,uint64_t bitscount,uint64_t&out_base_idx,uint64_t&bits_left_to_be_allocated);
        result_code_t test_interval(bool val,uint64_t base,uint64_t len);//看根据parent_descriptor的[base,base+len]区间的所有bit是否完全同val的bool值
        result_code_t free_interval(uint64_t base,uint64_t len);
        void disable_slice();
        //为了内存规整而刻意不搞构造折构函数，往64B缓存行对齐而设计
    };
    struct file_blk_interval_t{
        uint64_t logical_blkbase;
        uint64_t len;
    };//管辖[logical_blkbase,logical_blkbase+len]的闭区间
    result_code_t dir_insert_dentry(uint64_t inode_idx,const char *name);

    result_code_t dir_rm_dentry(uint64_t inode_idx,const char *name);

    result_code_t dir_name_heap_release(uint64_t inode_idx);//作为dir关闭时调用，遍历十个堆，全空则释放
    result_code_t dir_lookup(uint64_t inode_idx,const char *name, uint64_t *out_inum);
    int64_t dir_readdir(uint64_t inode_idx,dentry_out **out_buf);
    /* ══════════════════════════════════════════════════════════
     * 位图接口组 A：FS_block 分配器（分片路由）
     *   内存：block_bits[bitmap_blocks×4096]（4K 对齐，posix_memalign）
     *   分片：block_bitmap_slices[slice_count]，每片自治（锁+游标）
     *   路由：preferred 片优先 → 逐片 try 锁扫描（仿 FPA::alloc）；
     *   失败哨兵：PTR_INVALID（0 是合法块号）
     *   对齐约束：align_log2 ∈ [0,16]（片内 bit 索引对齐，见 bitmap_slice_t）
     * ══════════════════════════════════════════════════════════ */
    /* 硬分配：必须 require_count 块连续 + 起始块号 2^align_log2 对齐。
     * 单片内完成（不跨片拼接）；所有片失败 → PTR_INVALID（原子，不部分分配）。 */
    uint64_t block_alloc_hard(uint64_t require_count, uint8_t align_log2);
    /* 软分配：一次只从单片取（preferred 优先），实际 = min(区间长, require_count)。
     * *remain_count 输出缺口数 = require_count − 实际分配（0 = 全满足，
     * 调用者可据此再调一次补足）；一块都拿不到 → PTR_INVALID
     * 且 *remain_count = require_count。 */
    uint64_t block_alloc_soft(uint64_t require_count, uint64_t *remain_count);
    /* 区间释放：定位所属片（blkbase / block_per_slice_bits）→ 持该片写锁 → 清位 */
    void     block_free(uint64_t blkbase, uint64_t count);
    /* 测试：直读位图内存（无锁；调用方保证并发安全） */
    bool     block_test(uint64_t blk) const;

    /* ══════════════════════════════════════════════════════════
     * 位图接口组 B：inode 槽分配器（分片路由）
     *   内存：inode_bits[inode_bitmap_blocks×4096]（4K 对齐）
     *   分片：inode_bitmap_slices[slice_count]，同 slice_count 分片数
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
    result_code_t write_buff(uint64_t blk, uint64_t blk_len, const void *buf);
    /* ── inode I/O（零缓存）── */
    void inode_read(uint64_t inum, raven_Inode_t *out) const;
    void inode_write(uint64_t inum, const raven_Inode_t *in);

    /* ── 位图核心 ── */
    bool bitmaps_load();     /* mount 时：直写=全量 pread 入内存；mmap 后端=窗口切片（不预取）。
                                * 两模式共用自描述校验(§6.6) + 填 descriptor */
    void bitmaps_flush();    /* 析构时：仅直写模式 pwrite 全量写回（mmap 后端由内核页缓存管） */
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
    /* 分片初始化：slice_count = min(2^n 上界(logical_processor_count),
     * block/inode 位图各能切出的满片数)。每片 slice_bitscount = 32768
     * 的倍数（非末片），slice_base 4K 对齐；逐片 enable_slice。
     * 失败 → false（构造返回 RC_ERR_STATE）。 */
    bool slices_setup(uint32_t logical_processor_count);
    void slices_teardown();  /* delete[] 两个 slices 数组 */

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

    fs_bitmap_descriptor_t block_bitsmap_descriptor;
    fs_bitmap_descriptor_t inode_bitsmap_descriptor;
    bitmap_slice_t *block_bitmap_slices = nullptr;   /* 必须默认初始化：挂载失败路径 dtor 会 delete[]（未初始化=垃圾指针崩溃） */
    bitmap_slice_t *inode_bitmap_slices = nullptr;
    uint32_t slice_count;//约束上面两个的分片的分片数目，取构造时传入的logical_processor_count取最小的2^n上界
    uint8_t *block_bits = nullptr;      /* 全量内存位图（bitmap_blocks×4096），4K 对齐（posix_memalign） */
    uint8_t *inode_bits = nullptr;      /* 全量内存位图（inode_bm_blocks×4096），4K 对齐（posix_memalign） */
    uint64_t block_per_slice_bits = 0;  /* block 组名义片长（非末片 32768 倍数；定位片用） */
    uint64_t inode_per_slice_bits = 0;  /* inode 组名义片长 */
    uint64_t block_preferred_slice = 0; /* block 组亲和片（分配成功回写，仿 FPA preferred） */
    uint64_t inode_preferred_slice = 0; /* inode 组亲和片 */

    uint64_t root_inum      = 0;        /* 恒 0（根目录 inode 索引） */
    uint64_t root_dentry_blk = 0;       /* root inode.data_root（树根块号） */

    /* ── 缓存子模块（参数决策树编排后接线；暂 nullptr = 未启用） ──
     * 编排点：ctor 按 is_device_direct + param.if_will_use_*_cache 组装 cfg
     * 并构造子模块（见 caches_setup）；未接线时各处 mdata_piece_session
     * 构造恒失败（防御路径），8K 元数据访问全部报错 —— 这是预期
     * （编排完成前只编译不跑）。 */
    Inode_cache_mgr_t      *inode_cache_mgr  = nullptr;   /* inode 缓存（inode_cache_radix.hpp） */
    mdata_piece_caches_mgr *mdata_piece_mgr  = nullptr;   /* 8K 元数据负载片缓存（mdata_piece_caches.hpp） */

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
        uint64_t full_char_remain_ptr;  /* 插入时调用方预分配的外置定位；≤40B 恒 PTR_INVALID */
        uint64_t inum;                  /* 插入时调用方分配的真实 inode 槽；dentry_leaf_entry_insert 落 inode_ptr.ptr（非插入路径恒 0） */
    };
    /* ── 名字堆（name heap）原语（disk_layout_v1.1.md §7）──
     * 长名（>40B）外置部分 ≤512B，由每目录名字堆分配；档位级懒激活
     * （激活 = 位图面+数据面按容量一次分配，alloc_hard 连续区间）。
     * alloc/free 需 dir inode（控制器在 raven_dir_Inode_t 内联区）；
     * read/write 只需物理定位（full_char_remain_ptr），可能跨 8K 片
     * （起点在片内第二块高偏移时），按两段拷贝处理。 */
    struct name_heap_loc_t {
        uint64_t fblk;    /* 尾部起点 FS_block（remain_ptr[12:63]） */
        uint16_t offset;  /* 块内偏移（remain_ptr[0:11]） */
        uint8_t  slot;    /* 命中堆槽 0~9（调试/审计用；释放不依赖） */
    };
    result_code_t name_heap_alloc(uint64_t dir_inode_idx, uint16_t tail_len, name_heap_loc_t *out);
    result_code_t name_heap_free(uint64_t dir_inode_idx, uint16_t name_len, uint64_t full_char_remain_ptr);
    result_code_t name_heap_read(uint16_t name_len, uint64_t full_char_remain_ptr, char *out_tail);
    result_code_t name_heap_write(uint16_t name_len, uint64_t full_char_remain_ptr, const char *tail);
    /* 名字堆分配回滚守卫（insert 流程 RAII：堆分配成功后的任何失败返回
     * 都自动释放，成功路径显式 disarm；覆盖 insert/分裂链路全部失败点） */
    struct name_heap_guard_t {
        RavenFS_juvenile *fs = nullptr;
        uint64_t dir_ino = 0;
        uint16_t name_len = 0;
        uint64_t remain = PTR_INVALID;
        bool armed = false;
        void arm(RavenFS_juvenile *f, uint64_t d, uint16_t n, uint64_t r) {
            fs = f; dir_ino = d; name_len = n; remain = r; armed = true;
        }
        void disarm() { armed = false; }
        ~name_heap_guard_t() {
            if (armed && fs) fs->name_heap_free(dir_ino, name_len, remain);
        }
    };
    /* 新 inode 分配回滚守卫（insert 流程 RAII：inode_alloc 成功后的任何失败
     * 返回都自动 inode_free，成功路径显式 disarm；与 name_heap_guard 同
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
    /* 激活堆槽：位图面+数据面一次分配（各自连续、2^1 对齐）+ 位图清零 + 控制器落盘 */
    result_code_t name_heap_activate(raven_dir_Inode_t *dino, uint16_t slot, uint64_t dir_inode_idx);
    /* dentry 完整名字比较（树内条目 vs 目标名；>40B 内部读堆外置部分） */
    int dentry_name_cmp(const Dentry_t &e, const char *name, uint16_t name_len);
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
                                        uint64_t high_blk, uint64_t root_inode_idx);
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
