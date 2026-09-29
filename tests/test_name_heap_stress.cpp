/* ═══════════════════════════════════════════════════════════════════
 * test_name_heap_stress.cpp — 名字堆极限压测（榨干所有档位所有空间）
 *
 * 两种模式：
 *   默认（缩水）  ：8B 档灌满 8B_8k + 1000 进 16k（档位升级验证）；
 *                   64B/128B/256B 档各 5000；512B 档**灌满** 512B_8k
 *                   （65536）→ 加塞失败路径 RC_ERR_NO_BLOCK
 *   --full（大魔王）：8B 档 3 堆全满（458752）→ 8B 档耗尽 NO_BLOCK；
 *                   64B/128B/256B 档 2 堆全满（各 196608）；
 *                   512B 档满（65536）→ NO_BLOCK；总计 ~111 万条目
 *
 * 流程：全档位灌入 → 灌入后**全量 lookup 验证**（重复分配/尾部损坏暴露）
 *       → 加塞失败路径 → 位图满校验 → 全删 → 空树验证
 *       → dir_name_heap_release 全部堆归还 → 重挂载可写验证。
 *
 * mix 模式（第 5 参 "mix"）：插入类型按 (组,序号) 确定性三型循环
 *   （DIR / SOFT_SYM / REGULAR，组间错开防扎堆）—— 规模化覆盖 dir 型
 *   建根（block_alloc_hard 2 块）与空目录删除（block_free 2 块）路径；
 *   插删同一生成器，rm 不需要类型参数（读 inode 自判）。
 *
 * 名字确定性生成（gen_name：组前缀 + 10 位序号 + 定长填充），插入/删除
 * 同一生成器，零内存存储。填充字符依赖 idx → 尾部字节随 idx 变化，
 * 重复分配（共享单元）会破坏尾部数据并被 lookup 验证暴露。
 *
 * 用法：ravenfs_juv_name_heap_stress <device> [--full] [mode=offload|selfreliat] [mix]
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "audit_check.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：与名字堆同视角读位图 */
#include "../include/ravenfs/inode_cache_radix.hpp"   /* mix 模式类型抽查：InodeSession 走缓存读 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static linux_userspace_mount_t make_mount(const char *path, bool selfreliat) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, selfreliat ? O_RDWR | O_DIRECT : O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = selfreliat;   /* selfreliat：直写 + 双 self_reliat 缓存；offload：整盘 mmap 元数据走内核页缓存 */
    mnt.param = {};
    if (selfreliat) {
        mnt.param.if_will_use_inode_cache = true;
        mnt.param.if_will_use_mdata_piece_cache = true;
    }
    mnt.logical_processor_count = 1;
    return mnt;
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static const char *rc_name(result_code_t rc) {
    switch (rc) {
    case RC_OK: return "RC_OK";
    case RC_ERR_EXIST: return "RC_ERR_EXIST";
    case RC_ERR_NOT_FOUND: return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK: return "RC_ERR_NO_BLOCK";
    case RC_ERR_NAME_LEN: return "RC_ERR_NAME_LEN";
    case RC_ERR_IO: return "RC_ERR_IO";
    case RC_ERR_STATE: return "RC_ERR_STATE";
    case RC_ERR_NOT_EMPTY: return "RC_ERR_NOT_EMPTY";
    default: return "RC_???";
    }
}

/* mix 模式：确定性三型循环（DIR/SOFT_SYM/REGULAR），组间错开防同型扎堆。
 * 插删同一生成器：删除阶段按 (g,i) 重算同型（rm 不依赖类型，此处仅供插）。 */
static uint8_t mix_type(uint64_t g, uint64_t i) {
    const uint64_t x = (g * 3 + i) % 3;
    return x == 0 ? INODE_TYPE_DIR : (x == 1 ? INODE_TYPE_SOFT_SYM : INODE_TYPE_REGULAR);
}

/* 未激活哨兵 NAME_HEAP_INACTIVE（52 位全 1）由 disk_struct_v1.1.hpp 提供 */

/* 组定义：len=名字长度（→档位），full=全量目标数，lite=缩水目标数 */
struct grp_t {
    const char *tag; int len;
    uint64_t full_count; uint64_t lite_count;
};
static const grp_t GROUPS[] = {
    { "G1/8B档(41B)",   41,  458752, 66536, },   /* 全量：3 堆满；缩水：8B_8k 满+16k 部分 */
    { "G2/64B档(81B)",  81,  196608, 5000,  },
    { "G3/128B档(105B)",105, 196608, 5000,  },
    { "G4/256B档(169B)",169, 196608, 5000,  },
    { "G5/512B档(300B)",300, 65536,  65536, },   /* 两模式都灌满（失败路径载体） */
};
static constexpr int NGROUPS = (int)(sizeof(GROUPS) / sizeof(GROUPS[0]));

/* 确定性名字：组前缀 + 10 位序号 + 定长填充（组内唯一，跨组长度不同）。
 * 填充字符依赖 idx → 尾部字节随 idx 变化，重复分配（共享单元）会破坏
 * 尾部数据并被 lookup 验证暴露（全同尾部会掩盖共享单元）。 */
static void gen_name(char *buf, const grp_t &g, uint64_t idx) {
    int h = snprintf(buf, (size_t)g.len + 1, "g%010llu", (unsigned long long)idx);
    for (int i = h; i < g.len; i++) buf[i] = (char)('a' + ((idx + i) % 26));
    buf[g.len] = 0;
}

/* 全位图置位计数（按控制器实际位图大小读全部片） */
static uint64_t count_bits_full(RavenFS_juvenile &fs, const name_heap_controller_t &c) {
    /* self_reliat 写回缓存：位图面修改未落盘前直读盘会假阳性 → 先同步点 */
    if (fs.mdata_piece_mgr) fs.mdata_piece_mgr->flush_all();
    if (RFS_NHC_BITMAP(c) == NAME_HEAP_INACTIVE) return 0;
    const uint64_t bmp_blks = 1ULL << ((uint64_t)RFS_NHC_BITS(c) - 15);
    uint64_t n = 0;
    for (uint64_t p = 0; p < bmp_blks / 2; p++) {
        alignas(uint8_t) uint8_t bmp[8192];
        if (fs.read_buff(RFS_NHC_BITMAP(c) + p * 2, 2, bmp) != RC_OK) return UINT64_MAX;
        for (int i = 0; i < 8192; i++) n += (uint64_t)__builtin_popcount(bmp[i]);
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [--full] [mode=offload|selfreliat]\n", argv[0]); return 2; }
    /* 任意位置识别开关（兼容历史位置式调用） */
    auto has_arg = [&](const char *tok) -> bool {
        for (int i = 2; i < argc; i++) if (!strcmp(argv[i], tok)) return true;
        return false;
    };
    const bool FULL = has_arg("--full");
    const bool SELFRELIAT = has_arg("selfreliat");
    const bool MIX = has_arg("mix");

    printf("════════════════════════════════════════════════\n"
           " RavenFS v1.1 名字堆极限压测（%s%s）\n device: %s\n"
           "════════════════════════════════════════════════\n",
           FULL ? "大魔王全量" : "缩水", MIX ? " · mix 三型" : "", argv[1]);

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1], SELFRELIAT);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    uint64_t total_inserted = 0;
    {
    RavenFS_juvenile fs(mnt, &rc);
    CHECK(fs.is_mounted(), "mount rc=%s", rc_name(rc));
    if (!fs.is_mounted()) return 1;
    fs.dump_info();

    /* ══ Phase 1：全档位灌入 + 全量 lookup 验证 ══ */
    printf("\n--- Phase 1: 灌入（%s） ---\n", FULL ? "全量" : "缩水");
    char nm[RAVENFS_MAX_NAME_LEN + 1];
    bool p1_ok = true;
    for (int g = 0; g < NGROUPS && p1_ok; g++) {
        const uint64_t count = FULL ? GROUPS[g].full_count : GROUPS[g].lite_count;
        const int prog = count >= 10000 ? (int)(count / 10) : 1;
        uint64_t fail_at = UINT64_MAX;
        result_code_t fail_rc = RC_OK;
        for (uint64_t i = 0; i < count; i++) {
            gen_name(nm, GROUPS[g], i);
            result_code_t r = MIX ? fs.root_dir_insert_dentry(nm, mix_type(g, i))
                                  : fs.root_dir_insert_dentry(nm, INODE_TYPE_REGULAR);
            if (r != RC_OK) { fail_at = i; fail_rc = r; break; }
            total_inserted++;
            if (prog >= 1 && (i + 1) % prog == 0)
                printf("  %s ... %llu/%llu（累计 %llu）\n", GROUPS[g].tag,
                       (unsigned long long)(i + 1), (unsigned long long)count,
                       (unsigned long long)total_inserted);
        }
        if (fail_at != UINT64_MAX) {
            p1_ok = false;
            printf("  [FAIL] %s 在 #%llu 失败: %s\n", GROUPS[g].tag,
                   (unsigned long long)fail_at, rc_name(fail_rc));
            g_fail++;
            break;
        }
        printf("  %s 灌入完成（%llu 条）\n", GROUPS[g].tag, (unsigned long long)count);

        /* 全量 lookup 验证：重复分配/条目丢失/尾部损坏都会暴露 */
        uint64_t miss = 0;
        for (uint64_t i = 0; i < count; i++) {
            gen_name(nm, GROUPS[g], i);
            uint64_t inum = 0;
            if (fs.root_dir_lookup(nm, &inum) != RC_OK) {
                miss++;
                if (miss <= 3) printf("  [MISS] idx=%llu 名字=%s\n",
                                      (unsigned long long)i, nm);
            }
        }
        if (miss) {
            p1_ok = false;
            printf("  [FAIL] %s 灌入后 lookup 缺失 %llu 条\n", GROUPS[g].tag,
                   (unsigned long long)miss);
            g_fail++;
            break;
        }
        printf("  %s 灌入后全量 lookup 命中 ✓\n", GROUPS[g].tag);

        /* mix 模式：类型抽查（每组前 100 条，InodeSession 走缓存 ——
         * self_reliat 下 inode_read 直读盘会读到未回写脏页，必须走缓存） */
        if (MIX) {
            const uint64_t n = count < 100 ? count : 100;
            uint64_t bad = 0;
            for (uint64_t i = 0; i < n; i++) {
                gen_name(nm, GROUPS[g], i);
                uint64_t inum = 0;
                if (fs.root_dir_lookup(nm, &inum) != RC_OK) { bad++; continue; }
                InodeSession s(fs.inode_cache_mgr, inum);
                if (!s) { bad++; continue; }
                if (RFS_INO1_GET_TYPE(s.inode->word1) != mix_type(g, i)) bad++;
            }
            if (bad) {
                p1_ok = false;
                printf("  [FAIL] %s mix 类型抽查 %llu/%llu 不符\n", GROUPS[g].tag,
                       (unsigned long long)bad, (unsigned long long)n);
                g_fail++;
                break;
            }
            printf("  %s mix 类型抽查 ✓（%llu 条）\n", GROUPS[g].tag, (unsigned long long)n);
        }
    }
    CHECK(p1_ok, "Phase 1 全档位灌入（%llu 条）", (unsigned long long)total_inserted);

    /* ══ Phase 2：加塞失败路径 ══ */
    printf("\n--- Phase 2: 加塞失败路径 ---\n");
    {
        /* 512B 档满后加塞 → 该目录长名空间耗尽 */
        gen_name(nm, GROUPS[4], GROUPS[4].full_count);
        result_code_t r = fs.root_dir_insert_dentry(nm, INODE_TYPE_REGULAR);
        CHECK(r == RC_ERR_NO_BLOCK, "512B 档满后加塞 → %s（expect RC_ERR_NO_BLOCK）", rc_name(r));
        uint64_t inum = 0;
        result_code_t lr = fs.root_dir_lookup(nm, &inum);
        CHECK(lr == RC_ERR_NOT_FOUND, "加塞名字 lookup → %s（无痕迹）", rc_name(lr));
    }
    if (FULL) {
        /* 8B 档 3 堆全满后加塞 → 8B 档耗尽 */
        gen_name(nm, GROUPS[0], GROUPS[0].full_count);
        result_code_t r = fs.root_dir_insert_dentry(nm, INODE_TYPE_REGULAR);
        CHECK(r == RC_ERR_NO_BLOCK, "8B 档 3 堆满后加塞 → %s（expect RC_ERR_NO_BLOCK）", rc_name(r));
    }

    /* ══ Phase 3：位图满校验（灌满的堆位图应全 1） ══ */
    printf("\n--- Phase 3: 位图满校验 ---\n");
    {
        raven_Inode_t ino;
        fs.inode_read(fs.root_inum, &ino);
        raven_dir_Inode_t *dino = (raven_dir_Inode_t *)&ino;
        name_heap_controller_t *c512 = &dino->heaps_512B_8k;
        CHECK(RFS_NHC_BITMAP(*c512) != NAME_HEAP_INACTIVE, "512B_8k 已激活");
        uint64_t bits = count_bits_full(fs, *c512);
        CHECK(bits == 65536, "512B_8k 位图置位 %llu == 65536（全满）", (unsigned long long)bits);

        name_heap_controller_t *c8 = &dino->heaps_8B_8k;
        CHECK(RFS_NHC_BITMAP(*c8) != NAME_HEAP_INACTIVE, "8B_8k 已激活");
        uint64_t bits8 = count_bits_full(fs, *c8);
        if (FULL) {
            CHECK(bits8 == 65536, "8B_8k 位图置位 %llu == 65536（全满）", (unsigned long long)bits8);
            name_heap_controller_t *c8b = &dino->heaps_8B_16k;
            CHECK(RFS_NHC_BITMAP(*c8b) != NAME_HEAP_INACTIVE, "8B_16k 已激活（8B_8k 满后升级）");
            uint64_t b16 = count_bits_full(fs, *c8b);
            CHECK(b16 == 131072, "8B_16k 位图置位 %llu == 131072（全满）", (unsigned long long)b16);
            name_heap_controller_t *c8c = &dino->heaps_8B_32k;
            CHECK(RFS_NHC_BITMAP(*c8c) != NAME_HEAP_INACTIVE, "8B_32k 已激活（8B_16k 满后升级）");
            uint64_t b32 = count_bits_full(fs, *c8c);
            CHECK(b32 == 262144, "8B_32k 位图置位 %llu == 262144（全满）", (unsigned long long)b32);
        } else {
            CHECK(bits8 == 65536, "8B_8k 位图置位 %llu == 65536（缩水版全满）", (unsigned long long)bits8);
            name_heap_controller_t *c8b = &dino->heaps_8B_16k;
            CHECK(RFS_NHC_BITMAP(*c8b) != NAME_HEAP_INACTIVE, "8B_16k 已激活（8B_8k 满后升级）");
        }
    }

    /* ══ Phase 4：全删（组逆序 × 序号逆序，删光为止） ══ */
    printf("\n--- Phase 4: 全删 ---\n");
    bool p4_ok = true;
    uint64_t deleted = 0;
    uint64_t fail_at = UINT64_MAX;
    result_code_t fail_rc = RC_OK;
    for (int g = NGROUPS - 1; g >= 0 && p4_ok; g--) {
        const uint64_t count = FULL ? GROUPS[g].full_count : GROUPS[g].lite_count;
        const int prog = count >= 10000 ? (int)(count / 10) : 1;
        for (uint64_t k = 0; k < count; k++) {
            const uint64_t i = count - 1 - k;   /* 逆序 */
            gen_name(nm, GROUPS[g], i);
            result_code_t r = fs.root_dir_rm_dentry(nm, false);
            if (r != RC_OK) { p4_ok = false; fail_at = i; fail_rc = r; break; }
            deleted++;
            if (prog >= 1 && (k + 1) % prog == 0)
                printf("  %s ... 已删 %llu/%llu（累计 %llu）\n", GROUPS[g].tag,
                       (unsigned long long)(k + 1), (unsigned long long)count,
                       (unsigned long long)deleted);
        }
        if (p4_ok) printf("  %s 删除完成\n", GROUPS[g].tag);
    }
    if (!p4_ok) {
        printf("  [FAIL] 删除在 %s #%llu 失败: %s\n",
               GROUPS[fail_at >= NGROUPS ? 0 : 0].tag, (unsigned long long)fail_at, rc_name(fail_rc));
    }
    CHECK(p4_ok, "Phase 4 全删 %llu 条", (unsigned long long)deleted);
    CHECK(deleted == total_inserted, "删除数 %llu == 插入数 %llu",
          (unsigned long long)deleted, (unsigned long long)total_inserted);

    /* ══ Phase 5：空树 + 全部堆释放 ══ */
    printf("\n--- Phase 5: 空树验证 + dir_name_heap_release ---\n");
    {
        uint64_t inum = 0;
        gen_name(nm, GROUPS[0], 0);
        result_code_t lr = fs.root_dir_lookup(nm, &inum);
        CHECK(lr == RC_ERR_NOT_FOUND, "删光后 lookup 任意名字 → %s", rc_name(lr));

        CHECK(fs.dir_name_heap_release(fs.root_inum) == RC_OK, "dir_name_heap_release(0) → RC_OK");
        raven_Inode_t ino;
        fs.inode_read(fs.root_inum, &ino);
        raven_dir_Inode_t *dino = (raven_dir_Inode_t *)&ino;
        bool all_inactive = true;
        for (int s = 0; s < 10; s++) {
            if (RFS_NHC_BITMAP(((name_heap_controller_t *)&dino->heaps_8B_8k)[s])
                    != NAME_HEAP_INACTIVE) { all_inactive = false; break; }
        }
        CHECK(all_inactive, "10 个堆全部重置未激活（空间全归还）");
        fs.dump_info();
    }
    CHECK(audit_clean(fs, "name-heap-stress-final") == 0, "audit_inode 全净 (name_heap_stress)");
    }   /* fs 析构：位图全量写回 */

    /* ══ Phase 6：持久化 + 可写验证 ══ */
    printf("\n--- Phase 6: 重挂载 + 可写 ---\n");
    {
        RavenFS_juvenile fs2(mnt, &rc);
        CHECK(fs2.is_mounted(), "重挂载 rc=%s", rc_name(rc));
        if (fs2.is_mounted()) {
            char nm[RAVENFS_MAX_NAME_LEN + 1];
            uint64_t inum = 0;
            gen_name(nm, GROUPS[0], 0);
            CHECK(fs2.root_dir_lookup(nm, &inum) == RC_ERR_NOT_FOUND, "重挂载后仍为空");
            /* 删光后可写：重新插长名 → 堆重新激活 */
            gen_name(nm, GROUPS[1], 9999);
            CHECK(fs2.root_dir_insert_dentry(nm, INODE_TYPE_REGULAR) == RC_OK, "重插长名（堆重新激活）→ RC_OK");
            CHECK(fs2.root_dir_lookup(nm, &inum) == RC_OK, "重插后可查");
            CHECK(fs2.root_dir_rm_dentry(nm, false) == RC_OK, "重插后删除 → RC_OK");
        }
    }
    ::close(mnt.dev.fd);

    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
