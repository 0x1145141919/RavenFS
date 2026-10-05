/* ═══════════════════════════════════════════════════════════════════
 * ravenfs_juvenile.cpp — RavenFS v1.1 幼年期上帝类实现
 *
 * 本文件负责（Raven 分工）：
 *   构造/析构（挂载/卸载）、块 I/O、inode I/O、
 *   位图原始内存加载/写回 + 装配 bitmap_slices_mgr_t（分配器本体已顶出
 *   到 bitmap_slices.cpp，2026-09-03）
 *   CRC32、name_hash（XXH64）、dump_info
 *
 * 不实现（Sparrow 手写，见同目录下）：
 *   dentry_tree.cpp — dentry B+tree 全套（walk/lookup/insert/delete/split/balance）
 *   root_dir.cpp    — root_dir_insert_dentry / root_dir_rm_dentry
 *
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* inode_io_t / InodeSession（v2.0 会话） */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_caches_cfg_t：编排构造 */
#include "../include/ravenfs/fs_allocator.hpp"       /* fs_allocator_t / slices_cfg_t：双组分配器 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>   /* mmap/munmap：非直写后端整盘映射 + offload canvas 切片 */
#include <sys/stat.h>      /* fstat/lstat/stat：防呆钩子取设备身份 */
#include <sys/sysmacros.h> /* major/minor：st_rdev → 内核挂载 dev 号比对 */
#include <dirent.h>        /* opendir/readdir：扫 /sys/block 反查 loop 设备 */
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <vector>
extern "C" uint32_t get_processor_id();

bool name_len_limiter(const char*name){
    return strlen(name)<=RAVENFS_MAX_NAME_LEN;
}
/* 新条目名字合法性（fs 级）：空名 / 含 '/' 非法，超长另码。见头声明。 */
result_code_t name_check_create(const char *name){
    if (!name) return RC_ERR_NAME_INVALID;
    const size_t n = strlen(name);
    if (n == 0) return RC_ERR_NAME_INVALID;              /* 空名（首字节 '\0'） */
    if (n > RAVENFS_MAX_NAME_LEN) return RC_ERR_NAME_LEN;
    if (strchr(name, '/') != nullptr) return RC_ERR_NAME_INVALID;
    return RC_OK;
}
bool is_extent_node(Bptree_meta_t *meta)
{
    return meta->magics[0]==BPTREE_MAGIC1&&
    meta->magics[1]==BPTREE_MAGIC2&&
    meta->magics[2]==BPTREE_MAGIC3&&
    meta->magics[3]==BPTREE_MAGIC4;
}
/* 通用 extent 机械的按 kind 魔数校验：同一套算法服务三族树（文件区段树 /
 * 目录名字数据面映射树 / 全局 inode 数据面映射树），节点布局同构，仅 256-bit 魔数不同。 */
bool RavenFS_juvenile::is_extent_node_kind(const Bptree_meta_t *meta, extent_tree_kind_t kind)
{
    if (kind == EXTENT_TREE_NAMEDATA)
        return meta->magics[0]==NAMEDATA_EXTENT_MAGIC1 &&
               meta->magics[1]==NAMEDATA_EXTENT_MAGIC2 &&
               meta->magics[2]==NAMEDATA_EXTENT_MAGIC3 &&
               meta->magics[3]==NAMEDATA_EXTENT_MAGIC4;
    if (kind == EXTENT_TREE_INODE)
        return meta->magics[0]==INODE_DATA_EXTENT_MAGIC1 &&
               meta->magics[1]==INODE_DATA_EXTENT_MAGIC2 &&
               meta->magics[2]==INODE_DATA_EXTENT_MAGIC3 &&
               meta->magics[3]==INODE_DATA_EXTENT_MAGIC4;
    return meta->magics[0]==BPTREE_MAGIC1 &&
           meta->magics[1]==BPTREE_MAGIC2 &&
           meta->magics[2]==BPTREE_MAGIC3 &&
           meta->magics[3]==BPTREE_MAGIC4;
}
bool is_dentry_node(Bptree_meta_t *meta)
{
    return meta->magics[0]==DENTRY_MAGIC1&&
    meta->magics[1]==DENTRY_MAGIC2&&
    meta->magics[2]==DENTRY_MAGIC3&&
    meta->magics[3]==DENTRY_MAGIC4;
}
/* ═══════════════════════════════════════════════════════════════
 * 8K 节点临时缓冲轮子（node_buf_alloc / node_buf_free）
 * 内部节点临时分配必须 8K 对齐：结构体直读 + O_DIRECT 直写双满足。
 * ═══════════════════════════════════════════════════════════════ */
void *node_buf_alloc() {
    void *p = nullptr;
    if (posix_memalign(&p, 8192, 8192) != 0) return nullptr;
    return p;
}
void node_buf_free(void *p) { free(p); }
/* ═══════════════════════════════════════════════════════════════
 * CRC32（Castagnoli 0xEDB88320，查表）— SB 校验用
 * ═══════════════════════════════════════════════════════════════ */
uint32_t RavenFS_juvenile::crc32_table[256];
bool     RavenFS_juvenile::crc32_ready = false;

void RavenFS_juvenile::crc32_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0);
        crc32_table[i] = crc;
    }
    crc32_ready = true;
}

uint32_t RavenFS_juvenile::crc32_compute(const void *data, size_t len) {
    if (!crc32_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ═══════════════════════════════════════════════════════════════
 * 名字 hash：XXH64（seed=0）— v1.1 §9 决策（2026-08-20）
 *
 * 实现：移植自 xxHash v0.5.1（/home/PS/PS_git/xxHash，Cyan4973/xxHash，
 * BSD 2-Clause），xxhash.c 的 XXH64_endian_align 精简版：
 *   - 删除 endian/align 宏分支：目标平台 x64 固定小端，读 64/32 位
 *     统一走 memcpy（天然处理非对齐，sparrowos 内核端可移植）
 *   - seed 硬编码 0（本函数签名无 seed 参数）
 *   - 无 XXH_ACCEPT_NULL_INPUT_POINTER 分支（name 恒非 NULL）
 * 算法本体（PRIME 常量、round/mergeRound、尾部分档）与官方逐行同构。
 *
 * 为什么不是 crc32（原临时方案）：
 *   - 只有 32 位熵：~77k 条目即出现生日碰撞；碰撞簇过大时叶子无法
 *     正确分裂（不变量 #2：hints[i].hash_max 严格递增，全同 hash 时
 *     左半最大 == 右半最大，separator 提升直接违反上界路由）
 *   - crc32 线性可构造：攻击者可批量构造同 crc32 名字灌爆单叶
 *   XXH64：64 位熵（~5e9 条目才期望首碰撞）、雪崩质量好、无 key、
 *   自包含、x64 极快（名字 ≤2047B 耗时无感）。
 *
 * 稳定性约束：hash 值落盘（每个 dentry 都存），跨挂载必须可复现，
 * 故 seed 固定 0，禁止按挂载随机 key。将来上网络服务（ravenfs_server）
 * 要抗 DoS 时：换 SipHash-2-4，key mkfs 生成一次存 Superblock
 * （pad 现成空间），接口签名不变。
 * ═══════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::name_hash(const char *name) const {
    /* xxHash PRIME64 常量（xxhash.c 原值） */
    const uint64_t P1 = 11400714785074694791ULL;
    const uint64_t P2 = 14029467366897019727ULL;
    const uint64_t P3 = 1609587929392839161ULL;
    const uint64_t P4 = 9650029242287828579ULL;
    const uint64_t P5 = 2870177450012600261ULL;
    const uint64_t seed = 0;

    const uint8_t *p  = (const uint8_t *)name;
    const size_t   len = strlen(name);
    const uint8_t *bEnd = p + len;
    uint64_t h64;

    auto rotl64 = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    auto round  = [&](uint64_t acc, uint64_t input) {
        acc += input * P2;
        acc = rotl64(acc, 31);
        acc *= P1;
        return acc;
    };
    auto mergeRound = [&](uint64_t &acc, uint64_t val) {
        val = round(0, val);
        acc ^= val;
        acc = acc * P1 + P4;
    };

    if (len >= 32) {
        const uint8_t *limit = bEnd - 32;
        uint64_t v1 = seed + P1 + P2;
        uint64_t v2 = seed + P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - P1;
        do {
            uint64_t l1, l2, l3, l4;
            memcpy(&l1, p, 8);        p += 8;
            memcpy(&l2, p, 8);        p += 8;
            memcpy(&l3, p, 8);        p += 8;
            memcpy(&l4, p, 8);        p += 8;
            v1 = round(v1, l1);
            v2 = round(v2, l2);
            v3 = round(v3, l3);
            v4 = round(v4, l4);
        } while (p <= limit);

        h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        mergeRound(h64, v1);
        mergeRound(h64, v2);
        mergeRound(h64, v3);
        mergeRound(h64, v4);
    } else {
        h64 = seed + P5;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= bEnd) {
        uint64_t k1;
        memcpy(&k1, p, 8);
        k1 *= P2;
        k1 = rotl64(k1, 31);
        k1 *= P1;
        h64 ^= k1;
        h64 = rotl64(h64, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= bEnd) {
        uint32_t k2;
        memcpy(&k2, p, 4);
        h64 ^= (uint64_t)k2 * P1;
        h64 = rotl64(h64, 23) * P2 + P3;
        p += 4;
    }
    while (p < bEnd) {
        h64 ^= (uint64_t)(*p) * P5;
        h64 = rotl64(h64, 11) * P1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= P2;
    h64 ^= h64 >> 29;
    h64 *= P3;
    h64 ^= h64 >> 32;
    return h64;
}
/* ═══════════════════════════════════════════════════════════════
 * 防呆钩子：设备已被 Linux 内核以某个 FS 挂载 → 拒绝挂载
 *   · 双写者：内核 FS + god 同时写同一块设备 → 元数据互相引爆；
 *   · 撕裂读：挂载中读原始设备可能拿到写一半的状态 → 审计结论不可信。
 * 判定不依赖任何外部命令（blkid / findmnt / lsblk 一律不用），只用：
 *   fstat(fd) → 设备身份；/proc/self/mountinfo → 内核已挂载 dev 号集合；
 *   /sys/dev/block/<maj:min> + /sys/class/block 分区 → 整盘兜底（分区在挂载也算）；
 *   /sys/block/loopN/loop/backing_file → 普通文件（loop 镜像）反查 loop dev。
 * ═══════════════════════════════════════════════════════════════ */

/* /proc/self/mountinfo 第 3 栏恒为 "major:minor"（在挂载点路径之前，含空格也安全） */
static bool rfs_guard_mountinfo_has_dev(unsigned int maj, unsigned int min) {
    FILE *fp = fopen("/proc/self/mountinfo", "r");
    if (!fp) return false;
    char line[4096];
    bool hit = false;
    while (fgets(line, sizeof(line), fp)) {
        unsigned int mj, mn;
        if (sscanf(line, "%*u %*u %u:%u", &mj, &mn) == 2 && mj == maj && mn == min) { hit = true; break; }
    }
    fclose(fp);
    return hit;
}

/* /sys/block/loopN/loop/backing_file 反查：backing 文件 == 目标普通文件时，取该 loop 的 dev 号 */
static bool rfs_guard_loop_dev_of_file(const struct stat &st, unsigned int *maj, unsigned int *min) {
    DIR *d = opendir("/sys/block");
    if (!d) return false;
    struct dirent *e;
    bool hit = false;
    while (!hit && (e = readdir(d))) {
        if (strncmp(e->d_name, "loop", 4) != 0) continue;
        char p[512];
        snprintf(p, sizeof(p), "/sys/block/%s/loop/backing_file", e->d_name);
        FILE *bf = fopen(p, "r");
        if (!bf) continue;
        char backing[4096] = {0};
        bool got = (fgets(backing, sizeof(backing), bf) != nullptr);
        fclose(bf);
        if (!got) continue;
        size_t n = strlen(backing);
        while (n && (backing[n-1] == '\n' || backing[n-1] == '\r')) backing[--n] = '\0';
        if (n == 0) continue;
        struct stat bst;
        if (stat(backing, &bst) != 0) continue;
        if (bst.st_dev != st.st_dev || bst.st_ino != st.st_ino) continue;
        snprintf(p, sizeof(p), "/sys/block/%s/dev", e->d_name);
        FILE *dv = fopen(p, "r");
        if (!dv) continue;
        unsigned int mj, mn;
        if (fscanf(dv, "%u:%u", &mj, &mn) == 2) { *maj = mj; *min = mn; hit = true; }
        fclose(dv);
    }
    closedir(d);
    return hit;
}

/* /proc/self/mounts 第 1 栏（源设备路径）与 fd 真实路径逐字比对（兜底：绑定挂载 / 直挂文件等） */
static bool rfs_guard_mounts_has_source(const char *path) {
    if (!path || !*path) return false;
    FILE *fp = fopen("/proc/self/mounts", "r");
    if (!fp) return false;
    char line[4096], src[4096];
    bool hit = false;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%4095s %*s %*s", src) == 1 && strcmp(src, path) == 0) { hit = true; break; }
    }
    fclose(fp);
    return hit;
}

/* 整盘兜底：fd 是「整盘块设备」（本体无 partition 标记）时，枚举其分区，
 * 任一分区设备号命中内核挂载集合 → 视为已挂载。
 * 动机：开整盘（如 /dev/nvme1n1）而只有分区（nvme1n1p6）在挂载时，
 * 整盘与分区 dev 号不同 → 精确命中抓不到，需此二级检查兜底。
 * 分区枚举走 realpath(/sys/dev/block/maj:min) 拿到设备目录，
 * 只取带 partition 标记的子项 —— 彻底避开 "nvme1n1" vs "nvme1n10" 名前缀坑。 */
static bool rfs_guard_whole_disk_part_mounted(unsigned int maj, unsigned int min) {
    char lnk[64], devdir[4096];
    snprintf(lnk, sizeof(lnk), "/sys/dev/block/%u:%u", maj, min);
    if (!realpath(lnk, devdir)) return false;

    char f[4300];
    snprintf(f, sizeof(f), "%s/partition", devdir);
    if (access(f, F_OK) == 0) return false;   /* 自身是分区：精确匹配已管，兜底不适用 */

    DIR *d = opendir(devdir);                 /* 整盘：枚举带 partition 标记的子目录（分区） */
    if (!d) return false;
    struct dirent *e;
    bool hit = false;
    while (!hit && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char pf[4400], df[4400];
        snprintf(pf, sizeof(pf), "%s/%s/partition", devdir, e->d_name);
        if (access(pf, F_OK) != 0) continue;
        snprintf(df, sizeof(df), "%s/%s/dev", devdir, e->d_name);
        FILE *dv = fopen(df, "r");
        if (!dv) continue;
        unsigned int pmj, pmn;
        if (fscanf(dv, "%u:%u", &pmj, &pmn) == 2 && rfs_guard_mountinfo_has_dev(pmj, pmn)) hit = true;
        fclose(dv);
    }
    closedir(d);
    return hit;
}

bool RavenFS_juvenile::device_is_kernel_mounted() const {
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) return false;

    /* fd 对应的真实路径（/proc/self/fd/N）——mounts 源比对用 */
    char linkp[64], realp[4096];
    snprintf(linkp, sizeof(linkp), "/proc/self/fd/%d", fd);
    ssize_t rl = readlink(linkp, realp, sizeof(realp) - 1);
    if (rl >= 0) realp[rl] = '\0'; else realp[0] = '\0';

    if (S_ISBLK(st.st_mode)) {                       /* 块设备：st_rdev 即设备号 */
        unsigned int maj = major(st.st_rdev), min = minor(st.st_rdev);
        if (rfs_guard_mountinfo_has_dev(maj, min)) return true;       /* 自身命中 */
        if (rfs_guard_whole_disk_part_mounted(maj, min)) return true; /* 整盘兜底：分区在挂载也算 */
        return rfs_guard_mounts_has_source(realp);
    }
    if (S_ISREG(st.st_mode)) {                       /* 普通文件（loop 镜像）：反查 loop dev */
        unsigned int maj, min;
        if (rfs_guard_loop_dev_of_file(st, &maj, &min) && rfs_guard_mountinfo_has_dev(maj, min)) return true;
        return rfs_guard_mounts_has_source(realp);
    }
    return false;                                    /* 其它类型（chr 等）不适用 */
}

/* ── 获取节点最大 hash（叶子末项 hash 或内部末项 hint.hash_max） ── */

RavenFS_juvenile::RavenFS_juvenile(const linux_userspace_mount_t &mnt, uint64_t *res_code) {
    if (res_code) *res_code = RC_ERR_OPEN;   /* 默认失败态 */

    /* 无状态消费：全部参数来自调用者结构体，类不做设备几何的内部推导/校验
     * （设备探测是外部职责）。fd 生命周期归外部：只借不关。 */
    fd = mnt.dev.fd;
    param = mnt.param;
    dev_type = mnt.dev.type;
    lba_size_log2 = mnt.dev.lba_size_log2;
    is_device_direct = mnt.dev.is_device_direct;

    /* 契约（结构体自述）：readonly（rfs-audit 静态 mmap 审核）不得配 O_DIRECT */
    if (param.is_readonly && is_device_direct) {
        fd = -1;                /* 未挂载态（fd 归外部，只弃引用不 close） */
        if (res_code) *res_code = RC_ERR_STATE;
        return;
    }

    /* ── 防呆：设备已被 Linux 内核挂载 → 立即拒绝（RC_ERR_BUSY）──
     * 双写者（内核 FS + god）或挂载中读原始设备（撕裂读）都会毁数据/毁审计；
     * 必须在任何写盘（如下面的 mount_time 落盘）之前。逃生阀 = param.allow_mounted_device。 */
    if (!param.allow_mounted_device && device_is_kernel_mounted()) {
        fd = -1;                /* 未挂载态（fd 归外部，只弃引用不 close） */
        if (res_code) *res_code = RC_ERR_BUSY;
        return;
    }

    /* ── 读 Primary Superblock（fd byte[0,4096)，block_model §4.1）──
     * O_DIRECT 契约：4Kn 设备要求长度/缓冲 4096 对齐 → 整 4K 读入对齐缓冲，
     * 再拷前 sizeof(sb) 字节（SB 区固定 4096，长度按结构体实际尺寸）。 */
    alignas(4096) uint8_t sb_raw[4096];
    if (pread(fd, sb_raw, 4096, 0) != 4096) {
        fd = -1;
        if (res_code) *res_code = RC_ERR_IO;
        return;
    }
    memcpy(&sb, sb_raw, sizeof(sb));
    if (sb.magic[0] != RFS_SB_MAGIC_W0) {          /* v2.0：256-bit 魔数只比对 magic[0] 前缀 */
        fd = -1;
        if (res_code) *res_code = RC_ERR_MAGIC;
        return;
    }
    if (sb.version != RAVENFS_VERSION) {
        fd = -1;
        if (res_code) *res_code = RC_ERR_VERSION;
        return;
    }
    {
        RavenSuperblock sb_copy = sb;
        sb_copy.crc32 = 0;
        if (sb.crc32 != crc32_compute(&sb_copy, sizeof(sb_copy))) {
            fd = -1;
            if (res_code) *res_code = RC_ERR_CRC;
            return;
        }
    }

    /* ── 挂载时间戳（运维信息，2026-09-09）：非 readonly 挂载在 SB 校验通过后
     * 立即落盘（此时尚未建 mmap 窗口，纯 pwrite 最干净）。umount_time 不在此
     * 动——干净卸载由 dtor 写；崩溃/掉电则留在上次干净卸载值 → 下次挂载可见
     * umount 陈旧 = 非正常关机证据。readonly 审计挂载（rfs-audit）不写。
     * 失败静默：时间戳写失败不阻断挂载（读校验已通过，盘面可用）。 */
    if (!param.is_readonly) {
        sb.latest_mount_time = (uint64_t)time(nullptr);   /* v2.0：latest_mount_time（原 mount_time） */
        RavenSuperblock tmp = sb;
        tmp.crc32 = 0;
        tmp.crc32 = crc32_compute(&tmp, sizeof(tmp));
        alignas(4096) uint8_t sb_out[4096];
        memcpy(sb_out, &tmp, sizeof(tmp));
        (void)pwrite(fd, sb_out, 4096, 0);
    }

    /* ── 几何推导（v2.0：只有 block bitmap 尾巴；inode 存储 = 超级块统辖的弹性数据面）──
     * inode_count 由数据面片数派生（每 8K 片 32 个 256B inode）；装载读盘归本类，
     * 现成 extent/fr 数组喂 fs_allocator（模块零 I/O）。 */
    total_blocks = sb.total_blocks;
    bitmap_blocks = (total_blocks + 32767) / 32768;
    bitmap_start  = total_blocks - bitmap_blocks;
    inode_data_pieces = RFS_IDP_PIECE_COUNT(sb.inode_data_plane_pieces_count);
    inode_count       = inode_data_pieces * RFS_IDP_INODES_PER_PIECE;
    inode_array_base  = RFS_IDP_CTL_PTR(sb.inode_data_ctl);   /* direct 段基块（tree 态装载待接线）*/

    /* ── 决策树第一层：mmap 后端（非直写）/ 直写模式 ──
     * 非直写（普通文件/回环镜像/rfs-audit）：整盘从 offset 0 mmap（含 SB 区
     * byte[0,4096)，FS_block[n] = base+4096+n×4096），位图与缓存 canvas
     * 全部从窗口切片，不刻意预取（内核按需调页）。
     * readonly 配 MAP_PRIVATE+PROT_READ（写不落盘）；否则 MAP_SHARED 读写。 */
    if (!is_device_direct) {
        const off_t sz = lseek(fd, 0, SEEK_END);   /* 文件/块设备均返回尺寸 */
        if (sz <= 0) {
            fd = -1;
            if (res_code) *res_code = RC_ERR_STATE;
            return;
        }
        mmap_size_ = (uint64_t)sz;
        const int prot  = param.is_readonly ? PROT_READ : (PROT_READ | PROT_WRITE);
        const int flags = param.is_readonly ? MAP_PRIVATE : MAP_SHARED;
        mmap_base_ = (uint8_t *)mmap(nullptr, mmap_size_, prot, flags, fd, 0);
        if (mmap_base_ == MAP_FAILED) {
            mmap_base_ = nullptr;
            mmap_size_ = 0;
            fd = -1;
            if (res_code) *res_code = RC_ERR_STATE;
            return;
        }
        fsblk0_base_ = mmap_base_ + 4096;   /* FS_block[0] 虚拟基址（Primary SB 占 byte[0,4096)，§3.1） */
    }

    /* ctor 失败统一清理：按后端释放（mmap munmap / 直写 free）+ 子模块/分配器拆除 */
    auto fail = [&](uint64_t code) {
        caches_teardown();
        delete allocator; allocator = nullptr;
        if (mmap_base_) {
            munmap(mmap_base_, mmap_size_);
            mmap_base_ = nullptr;
            mmap_size_ = 0;
            fsblk0_base_ = nullptr;
        } else {
            free(block_bits);
        }
        block_bits = nullptr;
        fd = -1;
        if (res_code) *res_code = code;
    };

    /* ── block 位图入内存/切片 + v2.0 尾巴白名单校验 ── */
    if (!bitmaps_load()) { fail(RC_ERR_BITMAP_SELF); return; }

    /* ── 建 mdata_piece 缓存（inode 数据面 tree 态 bulk-load 经它读映射树）── */
    if (mdata_cache_setup() != RC_OK) { fail(RC_ERR_STATE); return; }

    /* ── inode 数据面装载（SB 四字段 + 映射树 bulk-load + fr 扁快照）──
     * 读盘归本类；组装现成数组喂 fs_allocator（模块零 I/O）。 */
    std::vector<fs_inode_extent_t>     inode_ext;
    std::vector<inode_dataplane_coordinate_free_range_t> inode_fr;
    {
        const uint64_t ctl = sb.inode_data_ctl;
        const uint64_t ptr = RFS_IDP_CTL_PTR(ctl);
        if (ptr == RFS_IDP_CTL_PTR_INVALID) { fail(RC_ERR_STATE); return; }

        if (RFS_IDP_CTL_IS_DIRECT(ctl)) {
            inode_ext.push_back(fs_inode_extent_t{0, ptr, inode_data_pieces * 2ULL});
        } else {
            /* tree 态 bulk-load：栈式 DFS 收集全部叶记录（子倒序入栈 → 出栈 = 逻辑序） */
            const uint64_t PTR52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
            std::vector<uint64_t> stack; stack.push_back(ptr);
            uint64_t guard = 0;
            bool bad = false;
            while (!stack.empty() && !bad) {
                if (++guard > (1ull << 26)) { bad = true; break; }
                const uint64_t blk = stack.back(); stack.pop_back();
                if (blk == PTR52) { bad = true; break; }
                mdata_piece_session sess(mdata_piece_mgr, blk);
                if (sess.index_error() || !sess) { bad = true; break; }
                Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
                if (!is_extent_node_kind(meta, EXTENT_TREE_INODE)) { bad = true; break; }
                if (RFS_PP_IS_INNER(meta->parent_ptr)) {
                    Bptree_extent_inner_node_t *inner = (Bptree_extent_inner_node_t *)sess.data;
                    const uint16_t cnt = (uint16_t)RFS_PP_COUNT(inner->meta.parent_ptr);
                    if (cnt == 0) { bad = true; break; }
                    for (uint16_t i = cnt; i-- > 0;)
                        stack.push_back(RFS_PTR_ADDR(inner->extents[i].sub_node_ptr));
                } else {
                    Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
                    const uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
                    for (uint16_t i = 0; i < cnt; i++)
                        inode_ext.push_back(fs_inode_extent_t{
                            leaf->extents[i].fblkbase,
                            RFS_PTR_ADDR(leaf->extents[i].block_base),
                            leaf->extents[i].length});
                }
            }
            if (bad) { fail(RC_ERR_STATE); return; }
        }

        /* fr 扁快照：按条目数算区大小（1 块对齐；多块可跨） */
        const uint64_t fptr = RFS_IDP_FR_CTL_PTR(sb.inode_data_fr_ctl);
        if (fptr != RFS_IDP_FR_CTL_PTR_INVALID) {
            const uint64_t n = RFS_IDP_FR_COUNT(sb.inode_data_fr_count);
            uint64_t fbytes = sizeof(inode_data_fr_snapshot_hdr)
                            + n * sizeof(inode_dataplane_coordinate_free_range_t);
            uint64_t fblks = (fbytes + 4095) / 4096; if (fblks == 0) fblks = 1;
            std::vector<uint8_t> frbuf((size_t)fblks * 4096, 0);
            bool ok;
            if (is_device_direct) {
                ok = (pread(fd, frbuf.data(), (size_t)fblks * 4096,
                            (off_t)fblock_to_byte(fptr)) == (ssize_t)(fblks * 4096));
            } else {
                memcpy(frbuf.data(), fsblk0_base_ + fptr * 4096, (size_t)fblks * 4096);
                ok = true;
            }
            const inode_data_fr_snapshot_hdr *h = (const inode_data_fr_snapshot_hdr *)frbuf.data();
            if (!ok || h->magic[0] != INODE_DATA_FR_SNAPSHOT_MAGIC1 || h->entry_count != n) {
                fail(RC_ERR_STATE);
                return;
            }
            const inode_dataplane_coordinate_free_range_t *e =
                (const inode_dataplane_coordinate_free_range_t *)(frbuf.data() + sizeof(*h));
            for (uint64_t i = 0; i < n; i++)
                inode_fr.push_back(e[i]);   /* 同型直拷（inode 坐标，256B 单位） */
        }
    }

    /* ── 双组分配器装配（block 位图源 + inode 数据面；cfg 数组 ctor 内一次性消费）── */
    {
        slices_cfg_t acfg{};
        acfg.block_bits_raw   = block_bits;
        acfg.block_bits_count = bitmap_blocks * 4096 * 8;
        acfg.logical_processor_count = mnt.logical_processor_count;
        acfg.inode.pieces = inode_data_pieces;
        acfg.inode.extents = inode_ext.data(); acfg.inode.extent_count = inode_ext.size();
        acfg.inode.fr = inode_fr.data();       acfg.inode.fr_count = inode_fr.size();
        acfg.inode.grow_batch_pieces = 1;
        uint64_t arc = RC_ERR_STATE;
        allocator = new fs_allocator_t(acfg, &arc);
        if (arc != RC_OK || allocator == nullptr) {
            delete allocator; allocator = nullptr;
            fail(RC_ERR_STATE);
            return;
        }
    }

    /* ── inode 会话绑定（解析器 = 数据面分配器 + 8K 片缓存；生命周期归本类）── */
    inode_io = new inode_io_t{ allocator, mdata_piece_mgr };

    /* ── root inode：v2.0 契约级硬编码 idx 0；经 InodeSession 读（数据面 translate + 8K 片缓存）── */
    root_inum = RFS_ROOT_INODE_IDX;
    {
        InodeSession rs(inode_io, root_inum);
        if (!rs) { fail(RC_ERR_IO); return; }
        if (RFS_INO1_GET_TYPE(rs.inode->word1) != INODE_TYPE_DIR ||
            rs.inode->data_root == PTR_INVALID) {
            fail(RC_ERR_STATE);
            return;
        }
        root_dentry_blk = rs.inode->data_root;
    }

    if (res_code) *res_code = RC_OK;
}

/* 析构即卸载：缓存同步点（flush_all → delete）→ （P3）落盘 → 释放分配器/后端。
 * readonly 不 flush（mmap MAP_PRIVATE/PROT_READ 下写回本就禁止）。
 * fd 生命周期归外部（借入不关）。 */
RavenFS_juvenile::~RavenFS_juvenile() {
    /* ── 同步落盘（P3）：inode 数据面维护 → block 位图回写 → SB。
     * 必须在 caches_teardown 之前（依赖 mdata_piece 写映射树节点 / inode 片）。── */
    if (!param.is_readonly && fd >= 0) (void)all_sycn(allocator);
    caches_teardown();
    /* ── 卸载时间戳（2026-09-09）：干净卸载标记。
     * 崩溃/掉电则无此写 → 下次挂载可见 umount 停留在上次干净卸载值。
     * 失败静默：析构路径不因时间戳失败抛错/报错。 */
    if (!param.is_readonly && fd >= 0) {
        sb.latest_umount_time = (uint64_t)time(nullptr);   /* v2.0：latest_umount_time（原 umount_time） */
        RavenSuperblock tmp = sb;
        tmp.crc32 = 0;
        tmp.crc32 = crc32_compute(&tmp, sizeof(tmp));
        alignas(4096) uint8_t sb_out[4096];
        memcpy(sb_out, &tmp, sizeof(tmp));
        (void)pwrite(fd, sb_out, 4096, 0);
    }
    delete allocator; allocator = nullptr;
    if (mmap_base_) {
        munmap(mmap_base_, mmap_size_);   /* mmap 后端：block 位图在窗口内 */
        mmap_base_ = nullptr;
        mmap_size_ = 0;
        fsblk0_base_ = nullptr;
    } else {
        free(block_bits);
    }
    block_bits = nullptr;
}

/* ═══════════════════════════════════════════════════════════════
 * 块 I/O（零缓存）
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::read_block(uint64_t blk, void *buf) {
    pread(fd, buf, 4096, (off_t)fblock_to_byte(blk));
}

void RavenFS_juvenile::write_block(uint64_t blk, const void *buf) {
    pwrite(fd, buf, 4096, (off_t)fblock_to_byte(blk));
}

/* 批量块 I/O（零缓存）：blk 起连续 blk_len 个 FS_block，一次 pread/pwrite。
 * 参数非法（buf 为空 / 区间越界）→ RC_ERR_STATE；实际读写字节不符 → RC_ERR_IO。 */
result_code_t RavenFS_juvenile::read_buff(uint64_t blk, uint64_t blk_len, void *buf) {
    if (!buf || blk_len == 0) return RC_ERR_STATE;
    if (blk >= total_blocks || blk_len > total_blocks - blk) return RC_ERR_STATE;
    size_t bytes = blk_len * 4096;
    if (pread(fd, buf, bytes, (off_t)fblock_to_byte(blk)) != (ssize_t)bytes)
        return RC_ERR_IO;
    return RC_OK;
}

result_code_t RavenFS_juvenile::write_buff(uint64_t blk, uint64_t blk_len, const void *buf) {
    if (!buf || blk_len == 0) return RC_ERR_STATE;
    if (blk >= total_blocks || blk_len > total_blocks - blk) return RC_ERR_STATE;
    size_t bytes = blk_len * 4096;
    if (pwrite(fd, buf, bytes, (off_t)fblock_to_byte(blk)) != (ssize_t)bytes)
        return RC_ERR_IO;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 缓存回调适配器（static 成员，可访问私有 read_buff/write_buff）
 * 两个子模块回调签名一致（(io_ctx, blk, blk_len, buf)），共用一对。
 * io_ctx = this（ctor 组装 cfg 时传入）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::fs_io_read(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf) {
    return static_cast<RavenFS_juvenile *>(io_ctx)->read_buff(blk, blk_len, buf);
}
result_code_t RavenFS_juvenile::fs_io_write(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf) {
    return static_cast<RavenFS_juvenile *>(io_ctx)->write_buff(blk, blk_len, buf);
}

/* ═══════════════════════════════════════════════════════════════
 * mdata_cache_setup — 建统一 mdata_piece 8K 片缓存（v2.0）
 *
 * 键 = 物理 FS_block；覆盖 [0, bitmap_start)，同时服务 B+tree 节点与 inode 数据面片。
 * **须在 inode 数据面装载之前**（tree 态 bulk-load 经它读映射树）。
 *   mmap 后端：offload（canvas = FS_block[0] 窗口切片）
 *   直写    ：param.if_will_use_mdata_piece_cache ? self_reliat : no_cache
 * ═══════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::mdata_cache_setup() {
    result_code_t rc = RC_OK;

    mdata_piece_caches_cfg_t mcfg{};
    mcfg.read_block  = fs_io_read;
    mcfg.write_block = fs_io_write;
    mcfg.io_ctx      = this;
    mcfg.Piece_base_FS_block_idx = 0;      /* 数据区起点（v2.0：元数据节点/inode 片散布 [0, bitmap_start)） */
    mcfg.Piece_count = bitmap_start / 2;   /* 覆盖 [0, bitmap_start)，每片 2 连续块（8K 偶对齐） */
    if (!is_device_direct) {
        mcfg.mode = cache_work_mode_t::offload_cache;
        mcfg.offload_specific.virtual_canvas_base = (uint64_t)fsblk0_base_;   /* FS_block[0] */
    } else if (param.if_will_use_mdata_piece_cache) {
        mcfg.mode = cache_work_mode_t::self_reliat_cache;
    } else {
        mcfg.mode = cache_work_mode_t::no_cache;
    }
    mdata_piece_mgr = new mdata_piece_caches_mgr(mcfg, &rc);
    if (rc != RC_OK) {
        delete mdata_piece_mgr;
        mdata_piece_mgr = nullptr;
        return rc;
    }
    return RC_OK;
}

/* caches_teardown — 卸载：同步点（flush_all）→ delete 子模块/绑定 → 指针置空。
 * readonly 不 flush（mmap MAP_PRIVATE/PROT_READ 下 msync 无意义，写回本就禁止）。 */
void RavenFS_juvenile::caches_teardown() {
    if (!param.is_readonly && mdata_piece_mgr) mdata_piece_mgr->flush_all();
    delete inode_io; inode_io = nullptr;
    delete mdata_piece_mgr; mdata_piece_mgr = nullptr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * inode_read / inode_write —— **已废除**（v2.0）。
 *
 * inode 访问不再直读/直写盘，统一走 InodeSession：
 *      InodeSession s(&inode_io, inum);
 *      if (!s) { ... }  s.inode->...;  s.dirty();
 * InodeSession 内部 = 数据面 inode_locate 取物理 8K 片 → mdata_piece_session
 * （accessor/脏标记/回写由缓存层管）。见 include/ravenfs/inode_cache_radix.hpp。
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════
 * block 位图核心（v2.0：盘上只有 block bitmap 一段）
 * ═══════════════════════════════════════════════════════════════ */

/* mount：block 位图入内存/切片 + 尾巴白名单校验。
 * v2.0 无 inode 位图；inode 状态 = 全局数据面（extent+fr），由 ctor 的
 * inode_plane 装载喂 fs_allocator。
 * 直写：全量 pread（4K 对齐）；mmap：窗口切片（不预取）。
 * 校验：[bitmap_start, total_blocks) 恒 1（结构区）。 */
bool RavenFS_juvenile::bitmaps_load() {
    if (is_device_direct) {
        if (posix_memalign((void **)&block_bits, 4096, bitmap_blocks * 4096) != 0)
            return false;
        if (pread(fd, block_bits, bitmap_blocks * 4096,
                  (off_t)fblock_to_byte(bitmap_start)) != (ssize_t)(bitmap_blocks * 4096)) {
            free(block_bits); block_bits = nullptr;
            return false;
        }
    } else {
        /* mmap 后端：block 位图直接从窗口切片，不预取（内核按需调页）。 */
        block_bits = fsblk0_base_ + bitmap_start * 4096;
    }
    for (uint64_t i = bitmap_start; i < total_blocks; i++)
        if (!(block_bits[i / 8] & (1u << (i % 8)))) goto fail;
    return true;

fail:
    if (is_device_direct) free(block_bits);
    block_bits = nullptr;
    return false;
}

/* block 位图回写（P3，未接线）：树版 block 组不原地改 block_bits，回写须先由
 * fs_allocator 反向重建位图（树 → 补集）。此处 **no-op**，宁可不落盘也不写陈旧
 * 位图。cross-mount 的 block 持久化待 P3。 */
void RavenFS_juvenile::bitmaps_flush() {
    /* TODO(P3)：block_bitmap = allocator 树 → 补集重建 → pwrite。 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * all_sycn — 同步落盘（P3）
 *
 * 顺序（硬）：① inode 数据面维护 → ② block 位图回写 → ③ SB。理由：① 的取块会改
 * block 分配器，② 在其后重建位图才能捕获「维护事实」。
 *   ① extent：**重建**——单连续段写 direct；否则整棵映射 B+tree 重建（新根叶+逐
 *      extent 插入 → 换 SB 指针 → 释放旧树）。fr：**先全量释放老区，再 hard_alloc
 *      新区（1 块对齐）**，最后写 hdr+entries。
 *   ② 位图：全占用起步 → 各 block 片 all_dump 清空闲位 → pwrite。
 *   ③ SB：crc 重算，最后写。
 * 友元访问 fs_allocator_t / inode_data_plane_t 私有树（dump 面 + block 片树）。
 * ═══════════════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::all_sycn(fs_allocator_t *a) {
    if (a == nullptr) return RC_ERR_STATE;
    if (param.is_readonly || fd < 0) return RC_OK;

    /* ── ①a. inode 翻译面（extent）：重建快照 ── */
    std::list<fs_inode_extent_t> el;
    if (a->inode_plane().dump_extents(&el) != RC_OK) return RC_ERR_IO;
    const uint64_t pieces = a->inode_plane().pieces();

    const uint64_t old_ctl = sb.inode_data_ctl;
    const uint64_t old_ptr = RFS_IDP_CTL_PTR(old_ctl);
    const bool old_is_tree = (old_ptr != RFS_IDP_CTL_PTR_INVALID) && !RFS_IDP_CTL_IS_DIRECT(old_ctl);

    const bool direct_single = (el.size() == 1 && el.front().lblk == 0 &&
                                el.front().length == pieces * 2ULL);
    if (direct_single) {
        /* 单连续段 → direct 直写（无映射树）；旧树（若有）整棵释放 */
        if (old_is_tree) extent_orphan_release(old_ptr, EXTENT_TREE_INODE);
        sb.inode_data_ctl = RFS_IDP_CTL_PACK(1, el.front().phys_base);
    } else {
        /* 重建映射 B+tree（bulk build）：新根叶 → 逐 extent 插入 → 换根 → 释放旧树。
         * 失败原子：新树建成前旧树只读；失败释放新节点、旧树无损。 */
        uint64_t new_root = PTR_INVALID;
        result_code_t rc = extent_new_root_leaf(RFS_ROOT_INODE_IDX, new_root, EXTENT_TREE_INODE);
        if (rc != RC_OK) return rc;
        for (const fs_inode_extent_t &e : el) {
            extent_t x;
            x.fblkbase = e.lblk;
            x.length   = e.length;
            RFS_PTR_SET_ADDR(x.block_base, e.phys_base);
            RFS_PTR_SET_FREE(x.block_base, 0);
            rc = extent_map_range(new_root, RFS_ROOT_INODE_IDX, x, EXTENT_TREE_INODE);
            if (rc != RC_OK) { extent_orphan_release(new_root, EXTENT_TREE_INODE); return rc; }
        }
        if (old_is_tree) extent_orphan_release(old_ptr, EXTENT_TREE_INODE);
        sb.inode_data_ctl = RFS_IDP_CTL_PACK(0, new_root);
    }
    RFS_IDP_PIECE_SET_COUNT(sb.inode_data_plane_pieces_count, pieces);

    /* ── ①b. inode 空闲面（fr）：全量释放老区 → hard_alloc 新区（1 块对齐）→ 写 ── */
    std::list<inode_dataplane_coordinate_free_range_t> fl;
    if (a->inode_plane().dump_fr(&fl) != RC_OK) return RC_ERR_IO;
    {
        const uint64_t old_fr  = RFS_IDP_FR_CTL_PTR(sb.inode_data_fr_ctl);
        const uint64_t old_cnt = RFS_IDP_FR_COUNT(sb.inode_data_fr_count);
        if (old_fr != RFS_IDP_FR_CTL_PTR_INVALID) {
            uint64_t ob  = sizeof(inode_data_fr_snapshot_hdr)
                         + old_cnt * sizeof(inode_dataplane_coordinate_free_range_t);
            uint64_t obl = (ob + 4095) / 4096; if (obl == 0) obl = 1;
            block_free(old_fr, obl);
        }
    }
    {
        const uint64_t cnt   = fl.size();
        const uint64_t bytes = sizeof(inode_data_fr_snapshot_hdr)
                             + cnt * sizeof(inode_dataplane_coordinate_free_range_t);
        uint64_t blks = (bytes + 4095) / 4096; if (blks == 0) blks = 1;
        const uint64_t new_fr = block_alloc_hard(blks, 0);   /* align_log2=0 → 1 块对齐 */
        if (new_fr == PTR_INVALID) return RC_ERR_NO_BLOCK;

        std::vector<uint8_t> buf((size_t)blks * 4096, 0);
        inode_data_fr_snapshot_hdr *h = (inode_data_fr_snapshot_hdr *)buf.data();
        h->magic[0] = INODE_DATA_FR_SNAPSHOT_MAGIC1;
        h->magic[1] = INODE_DATA_FR_SNAPSHOT_MAGIC2;
        h->magic[2] = INODE_DATA_FR_SNAPSHOT_MAGIC3;
        h->magic[3] = INODE_DATA_FR_SNAPSHOT_MAGIC4;
        h->entry_count = cnt;
        h->capacity    = cnt;
        h->rsv0        = 0;
        h->rsv1[0]     = 0;
        inode_dataplane_coordinate_free_range_t *arr =
            (inode_dataplane_coordinate_free_range_t *)(buf.data() + sizeof(*h));
        uint64_t i = 0;
        for (const inode_dataplane_coordinate_free_range_t &f : fl) arr[i++] = f;
        h->crc32 = 0;
        h->crc32 = crc32_compute(buf.data(),
                                 sizeof(*h) + cnt * sizeof(inode_dataplane_coordinate_free_range_t));
        if (write_buff(new_fr, blks, buf.data()) != RC_OK) {
            block_free(new_fr, blks);
            return RC_ERR_IO;
        }
        sb.inode_data_fr_ctl   = RFS_IDP_FR_CTL_PACK(new_fr);
        sb.inode_data_fr_count = cnt;
    }

    /* 新树节点 / inode 片内容经 mdata_piece 落盘（先于 SB，避免 SB 指向未刷节点）*/
    if (mdata_piece_mgr) mdata_piece_mgr->flush_all();

    /* ── ② block 位图回写：树 → 补集重建（全占用起步，清各片空闲位）── */
    {
        const uint64_t bytes = bitmap_blocks * 4096;
        if (block_bits == nullptr) return RC_ERR_STATE;
        memset(block_bits, 0xFF, bytes);
        const uint32_t nsl = a->slice_count();
        for (uint32_t si = 0; si < nsl; si++) {
            mgr_slice_t &s = a->blk_.slices[si];
            fs_run_dlist_t dl;
            s.slice_lock.lock();
            s.all_dump(&dl);
            s.slice_lock.unlock();
            for (const fs_run_t &r : dl)
                for (uint64_t k = 0; k < r.len; k++) {
                    const uint64_t b = r.base + k;
                    if (b < bytes * 8ULL) block_bits[b >> 3] &= (uint8_t)~(1u << (b & 7));
                }
        }
        if (is_device_direct) {
            if (pwrite(fd, block_bits, bytes, (off_t)fblock_to_byte(bitmap_start)) != (ssize_t)bytes)
                return RC_ERR_IO;
        }
    }

    /* ── ③ SB 写（最后：指针全部落定）── */
    sb.latest_syn_time = (uint64_t)time(nullptr);
    {
        RavenSuperblock tmp = sb;
        tmp.crc32 = 0;
        tmp.crc32 = crc32_compute(&tmp, sizeof(tmp));
        alignas(4096) uint8_t o[4096];
        memcpy(o, &tmp, sizeof(tmp));
        if (pwrite(fd, o, 4096, 0) != 4096) return RC_ERR_IO;
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 分配接口组：薄转发 → fs_allocator_t（block 分片 + 全局 inode 数据面）
 * 契约见 include/ravenfs/fs_allocator.hpp；block_soft/occupy 做形状适配。
 * ═══════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::block_alloc_hard(uint64_t require_count, uint8_t align_log2) {
    return allocator ? allocator->block_alloc_hard(require_count, align_log2) : PTR_INVALID;
}
uint64_t RavenFS_juvenile::block_alloc_soft(uint64_t require_count, uint64_t *remain_count) {
    /* 形状适配：fs_allocator 的 soft 是 all-or-nothing 多段；本壳保持旧「单段 + 缺口」
     * 语义——先整段硬分配（连续、原子）；失败退化为单块（有任意空闲即可 ≥1 块）。
     * 调用方（direct_file_ops::alloc_piece）本就是循环凑碎片，故单块渐进正确。 */
    if (remain_count) *remain_count = require_count;
    if (!allocator || require_count == 0) return PTR_INVALID;
    uint64_t b = allocator->block_alloc_hard(require_count, 0);
    if (b != PTR_INVALID) { if (remain_count) *remain_count = 0; return b; }
    b = allocator->block_alloc_hard(1, 0);
    if (b == PTR_INVALID) return PTR_INVALID;
    if (remain_count) *remain_count = require_count - 1;
    return b;
}
uint64_t RavenFS_juvenile::block_occupy_soft(uint64_t blkbase, uint64_t require_count,
                                             uint64_t *rest_count) {
    if (rest_count) *rest_count = require_count;
    if (!allocator) return PTR_INVALID;
    uint64_t rest = require_count;
    result_code_t rc = allocator->block_occupy_soft(blkbase, require_count, &rest);
    if (rest_count) *rest_count = rest;
    return (rc == RC_OK) ? blkbase : PTR_INVALID;   /* 结果即入参锚点原址（≥1 块） */
}
void RavenFS_juvenile::block_free(uint64_t blkbase, uint64_t count) {
    if (allocator) (void)allocator->block_free(blkbase, count);
}
bool RavenFS_juvenile::block_test(uint64_t blk) const {
    return allocator ? allocator->block_test(blk) : false;
}
uint64_t RavenFS_juvenile::inode_alloc() {
    if (!allocator) return PTR_INVALID;
    uint64_t n = PTR_INVALID;
    if (allocator->inode_alloc(n) != RC_OK) return PTR_INVALID;
    /* 数据面可能因用尽而 grow：刷新 inode_count（audit/dump 等消费者用） */
    inode_count = allocator->inode_plane().pieces() * RFS_IDP_INODES_PER_PIECE;
    return n;
}
void RavenFS_juvenile::inode_free(uint64_t inum) {
    if (allocator) allocator->inode_free(inum);
}
bool RavenFS_juvenile::inode_test(uint64_t inum) const {
    return allocator ? allocator->inode_test(inum) : false;
}
/* ═══════════════════════════════════════════════════════════════
 * 调试：dump_info（不碰 dentry 树）
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::dump_info() const {
    const uint64_t blk_free    = allocator ? allocator->block_free_total() : 0;
    const uint64_t ino_free    = allocator ? allocator->inode_free_total() : 0;
    const uint64_t used_inodes = (inode_count > ino_free) ? (inode_count - ino_free) : 0;

    printf("RavenFS_juvenile v2.0\n"
           "  fd=%d  mounted=%s\n"
           "  magic[0]=%#llx  magic[1]=%#llx  version=%u  uuid=%02x%02x..%02x%02x  label=%.48s\n"
           "  total_blocks=%llu  inode_count=%llu  root_inode=%llu\n"
           "  block bm    [%llu, %llu)  %llu blk\n"
           "  inode plane direct_base=%llu  pieces=%llu\n"
           "  blocks free: %llu\n"
           "  inodes used: %llu/%llu (%.2f%%)\n"
           "  root_dentry_blk=%llu  slices=%u  cpu_slots=%u  blk_pref0=%llu\n",
           fd, fd >= 0 ? "yes" : "no",
           (unsigned long long)sb.magic[0], (unsigned long long)sb.magic[1], sb.version,
           sb.uuid[0], sb.uuid[1], sb.uuid[14], sb.uuid[15],
           sb.volume_name,
           (unsigned long long)total_blocks, (unsigned long long)inode_count,
           (unsigned long long)root_inum,
           (unsigned long long)bitmap_start, (unsigned long long)total_blocks,
           (unsigned long long)bitmap_blocks,
           (unsigned long long)inode_array_base, (unsigned long long)inode_data_pieces,
           (unsigned long long)blk_free,
           (unsigned long long)used_inodes, (unsigned long long)inode_count,
           inode_count ? 100.0 * (double)used_inodes / (double)inode_count : 0.0,
           (unsigned long long)root_dentry_blk,
           allocator ? (unsigned)allocator->slice_count() : 0u,
           allocator ? (unsigned)allocator->cpu_slots() : 0u,
           allocator ? (unsigned long long)allocator->block_preferred_of(0) : 0ULL);
}

/* ═══════════════════════════════════════════════════════════════
 * 以下方法由 人类手写实现（dentry_tree.cpp）：
 *   root_dir_insert_dentry / root_dir_rm_dentry / root_dir_lookup /
 *   root_dir_readdir
 * ═══════════════════════════════════════════════════════════════ */