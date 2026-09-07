/* ═══════════════════════════════════════════════════════════════════
 * test_name_heap.cpp — 名字堆（name heap）专项测试
 *
 * 覆盖（disk_layout_v1.1.md §7）：
 *   1. 档位覆盖：41B(8B档)/80B(8B档5单元)/81B(64B档)/105B(128B档)/
 *      169B(256B档)/297B(512B档)/552B(上限，512B档)
 *   2. 长名完整比较：同长名重复插 → EXIST（尾部读堆比较命中）；
 *      前缀相同后缀不同 → 各自独立条目
 *   3. 控制器激活状态：应激活 8B_8k/64B_8k/128B_8k/256B_8k/512B_8k，
 *      其余槽未激活（哨兵）；位图置位单元数 == 期望
 *   4. 释放：rm 长名 → lookup NOT_FOUND → 位图置位数回落
 *   5. 持久化：析构 flush → 重挂载 → 长名全部可查
 *
 * 用法：ravenfs_juv_name_heap <device>   （已 mkfs 的 ravenfs v1.1 盘/镜像）
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* verify 同步点：flush_all 需完整类型 */

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
    mnt.dev.is_device_direct = selfreliat;   /* selfreliat：直写 + 双 self_reliat 缓存；offload：整盘 mmap */
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
    case RC_OK:             return "RC_OK";
    case RC_ERR_OPEN:       return "RC_ERR_OPEN";
    case RC_ERR_MAGIC:      return "RC_ERR_MAGIC";
    case RC_ERR_VERSION:    return "RC_ERR_VERSION";
    case RC_ERR_CRC:        return "RC_ERR_CRC";
    case RC_ERR_BITMAP_SELF:return "RC_ERR_BITMAP_SELF";
    case RC_ERR_EXIST:      return "RC_ERR_EXIST";
    case RC_ERR_NOT_FOUND:  return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK:   return "RC_ERR_NO_BLOCK";
    case RC_ERR_NO_INODE:   return "RC_ERR_NO_INODE";
    case RC_ERR_NAME_LEN:   return "RC_ERR_NAME_LEN";
    case RC_ERR_IO:         return "RC_ERR_IO";
    case RC_ERR_STATE:      return "RC_ERR_STATE";
    default:                return "RC_???";
    }
}

/* 造名字：len 个字符，字符循环取自 seed，可指定后缀偏移（区分前缀相同名字） */
static void make_name(char *buf, int len, char fill, int tail_shift = 0) {
    for (int i = 0; i < len; i++) buf[i] = (char)(fill + (i + tail_shift) % 26);
    buf[len] = 0;
}

/* 读目录 inode 的名字堆控制器（root inode = 0） */
static raven_dir_Inode_t read_dir_inode(RavenFS_juvenile &fs) {
    raven_Inode_t ino;
    fs.inode_read(0, &ino);
    return *(raven_dir_Inode_t *)&ino;
}

/* 数位图面置位 bit 数（位图面 = 8K 负载片，从 bitmap_base 起） */
static uint64_t count_bits(RavenFS_juvenile &fs, uint64_t bmp_base) {
    /* self_reliat 写回缓存：位图面修改未落盘前直读盘会假阳性 → 先同步点 */
    if (fs.mdata_piece_mgr) fs.mdata_piece_mgr->flush_all();
    alignas(uint8_t) uint8_t bmp[8192];
    if (fs.read_buff(bmp_base, 2, bmp) != RC_OK) return UINT64_MAX;
    uint64_t n = 0;
    for (int i = 0; i < 8192; i++) n += (uint64_t)__builtin_popcount(bmp[i]);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [mode=offload|selfreliat]\n", argv[0]); return 2; }
    const bool SELFRELIAT = (argc >= 3 && strcmp(argv[2], "selfreliat") == 0);

    printf("════════════════════════════════════════════════\n"
           " RavenFS v1.1 名字堆专项测试\n"
           " device: %s\n"
           "════════════════════════════════════════════════\n", argv[1]);

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1], SELFRELIAT);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    /* ── 各档位边界名字（长度 → 档位映射见 §7.4） ── */
    char n41[64], n80[96], n81[96], n104[128], n105[128];
    char n168[192], n169[192], n296[320], n297[320], n552[RAVENFS_MAX_NAME_LEN + 1];
    make_name(n41, 41, 'A');      /* tail=1   → 8B 档 1 单元 */
    make_name(n80, 80, 'B');      /* tail=40  → 8B 档 5 单元 */
    make_name(n81, 81, 'C');      /* tail=41  → 64B 档 */
    make_name(n104, 104, 'D');    /* tail=64  → 64B 档 */
    make_name(n105, 105, 'E');    /* tail=65  → 128B 档 */
    make_name(n168, 168, 'F');    /* tail=128 → 128B 档 */
    make_name(n169, 169, 'G');    /* tail=129 → 256B 档 */
    make_name(n296, 296, 'H');    /* tail=256 → 256B 档 */
    make_name(n297, 297, 'I');    /* tail=257 → 512B 档 */
    make_name(n552, 552, 'J');    /* tail=512 → 512B 档（上限） */

    char n41x[64], n41y[64];      /* 前缀相同、尾部不同（41B，8B 档各 1 单元） */
    make_name(n41x, 41, 'K');
    make_name(n41y, 41, 'K', 1);

    /* 期望单元数（位图置位） */
    const uint64_t exp_8B   = 1 + 5 + 1 + 1;   /* n41 + n80 + n41x + n41y = 8 */
    const uint64_t exp_64B  = 1 + 1;           /* n81 + n104 */
    const uint64_t exp_128B = 1 + 1;           /* n105 + n168 */
    const uint64_t exp_256B = 1 + 1;           /* n169 + n296 */
    const uint64_t exp_512B = 1 + 1;           /* n297 + n552 */

    {
    RavenFS_juvenile fs(mnt, &rc);
    CHECK(fs.is_mounted(), "mount rc=%s", rc_name(rc));
    if (!fs.is_mounted()) return 1;

    /* ══ Phase 1：档位覆盖插入 + lookup ══ */
    printf("\n--- Phase 1: 各档位长名插入 + 可查 ---\n");
    const char *names[] = { n41, n80, n81, n104, n105, n168, n169, n296, n297, n552, n41x, n41y };
    const char *tags[]  = { "41B/8B档", "80B/8B档5单元", "81B/64B档", "104B/64B档",
                            "105B/128B档", "168B/128B档", "169B/256B档", "296B/256B档",
                            "297B/512B档", "552B上限/512B档", "41B前缀变体x", "41B前缀变体y" };
    bool p1_ok = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        result_code_t r = fs.root_dir_insert_dentry(names[i], INODE_TYPE_REGULAR);
        if (r != RC_OK) { p1_ok = false; printf("  [FAIL] insert(%s) → %s\n", tags[i], rc_name(r)); g_fail++; continue; }
        uint64_t inum = 0;
        result_code_t lr = fs.root_dir_lookup(names[i], &inum);
        bool ok = (lr == RC_OK);
        if (!ok) p1_ok = false;
        printf("  [%s] insert+lookup(%s) → %s\n", ok ? "PASS" : "FAIL", tags[i], rc_name(lr));
        ok ? g_pass++ : g_fail++;
    }
    CHECK(p1_ok, "Phase 1 全部档位插入且可查");

    /* ══ Phase 2：完整名字比较（尾部读堆） ══ */
    printf("\n--- Phase 2: 长名完整比较 ---\n");
    {
        result_code_t r = fs.root_dir_insert_dentry(n41, INODE_TYPE_REGULAR);
        CHECK(r == RC_ERR_EXIST, "重复插 41B → %s（expect RC_ERR_EXIST，尾部比较命中）", rc_name(r));
        result_code_t r2 = fs.root_dir_insert_dentry(n552, INODE_TYPE_REGULAR);
        CHECK(r2 == RC_ERR_EXIST, "重复插 552B → %s（expect RC_ERR_EXIST）", rc_name(r2));
        /* 前缀相同尾部不同：n41x/n41y 都已插入且各自可查（上阶段已验证） */
        uint64_t inum = 0;
        result_code_t r3 = fs.root_dir_lookup(n41x, &inum);
        CHECK(r3 == RC_OK && fs.root_dir_lookup(n41y, &inum) == RC_OK,
              "前缀相同后缀不同两条目独立可查");
        /* 后缀偏移一位的名字不应命中 */
        char n41z[64];
        make_name(n41z, 41, 'K', 2);
        result_code_t r4 = fs.root_dir_lookup(n41z, &inum);
        CHECK(r4 == RC_ERR_NOT_FOUND, "第三变体（未插入）→ %s（expect NOT_FOUND）", rc_name(r4));
    }

    /* ══ Phase 3：控制器激活状态 + 位图置位计数 ══ */
    printf("\n--- Phase 3: 控制器激活与位图一致性 ---\n");
    {
        raven_dir_Inode_t dino = read_dir_inode(fs);
        struct { const char *tag; name_heap_controller_t *ctrl; uint64_t expect_bits; } slots[] = {
            { "8B_8k",   &dino.heaps_8B_8k,   exp_8B   },
            { "8B_16k",  &dino.heaps_8B_16k,  0        },
            { "8B_32k",  &dino.heaps_8B_32k,  0        },
            { "64B_8k",  &dino.heaps_64B_8k,  exp_64B  },
            { "64B_16k", &dino.heaps_64B_16k, 0        },
            { "128B_8k", &dino.heaps_128B_8k, exp_128B },
            { "128B_16k",&dino.heaps_128B_16k,0        },
            { "256B_8k", &dino.heaps_256B_8k, exp_256B },
            { "256B_16k",&dino.heaps_256B_16k,0        },
            { "512B_8k", &dino.heaps_512B_8k, exp_512B },
        };
        for (const auto &s : slots) {
            bool active = (s.ctrl->bitmap_base_block_ptr != NAME_HEAP_INACTIVE);
            if (s.expect_bits > 0) {
                CHECK(active, "槽 %s 已激活（bitmap_base=%llu data_base=%llu gran=%u bits=%u）",
                      s.tag,
                      (unsigned long long)s.ctrl->bitmap_base_block_ptr,
                      (unsigned long long)s.ctrl->data_base_block_ptr,
                      (unsigned)s.ctrl->bytes_per_bit_log2,
                      (unsigned)s.ctrl->total_bits_count_log2);
                if (active) {
                    uint64_t bits = count_bits(fs, s.ctrl->bitmap_base_block_ptr);
                    CHECK(bits == s.expect_bits, "槽 %s 位图置位 %llu == 期望 %llu",
                          s.tag, (unsigned long long)bits, (unsigned long long)s.expect_bits);
                }
            } else {
                CHECK(!active, "槽 %s 未激活（懒分配零开销）", s.tag);
            }
        }
    }

    /* ══ Phase 4：rm 长名 → 释放 → 位图回落 ══ */
    printf("\n--- Phase 4: 长名删除与堆释放 ---\n");
    {
        result_code_t r = fs.root_dir_rm_dentry(n41, false);
        CHECK(r == RC_OK, "rm(41B) → %s", rc_name(r));
        uint64_t inum = 0;
        result_code_t lr = fs.root_dir_lookup(n41, &inum);
        CHECK(lr == RC_ERR_NOT_FOUND, "lookup(41B) 删除后 → %s", rc_name(lr));
        raven_dir_Inode_t dino = read_dir_inode(fs);
        uint64_t bits = count_bits(fs, dino.heaps_8B_8k.bitmap_base_block_ptr);
        CHECK(bits == exp_8B - 1, "8B_8k 位图置位 %llu == 期望 %llu（释放后回落）",
              (unsigned long long)bits, (unsigned long long)(exp_8B - 1));
        /* 再删一条（n41x） */
        result_code_t r2 = fs.root_dir_rm_dentry(n41x, false);
        CHECK(r2 == RC_OK, "rm(41B 变体 x) → %s", rc_name(r2));
        uint64_t bits2 = count_bits(fs, dino.heaps_8B_8k.bitmap_base_block_ptr);
        CHECK(bits2 == exp_8B - 2, "8B_8k 位图置位 %llu == 期望 %llu",
              (unsigned long long)bits2, (unsigned long long)(exp_8B - 2));
        /* 删 512B 档一条 */
        result_code_t r3 = fs.root_dir_rm_dentry(n552, false);
        CHECK(r3 == RC_OK, "rm(552B) → %s", rc_name(r3));
        uint64_t bits3 = count_bits(fs, dino.heaps_512B_8k.bitmap_base_block_ptr);
        CHECK(bits3 == exp_512B - 1, "512B_8k 位图置位 %llu == 期望 %llu",
              (unsigned long long)bits3, (unsigned long long)(exp_512B - 1));
        uint64_t inum2 = 0;
        CHECK(fs.root_dir_lookup(n552, &inum2) == RC_ERR_NOT_FOUND, "lookup(552B) 删除后 NOT_FOUND");
    }

    /* ══ Phase 4b：dir_name_heap_release 堆回收 ══ */
    printf("\n--- Phase 4b: 全空堆释放（dir_name_heap_release） ---\n");
    {
        uint64_t inum2 = 0;
        /* 清空 64B 档两条 → 64B_8k 变全空；8B 档还剩 n80/n41y（6 单元） */
        CHECK(fs.root_dir_rm_dentry(n81, false) == RC_OK, "rm(81B) 清空 64B 档");
        CHECK(fs.root_dir_rm_dentry(n104, false) == RC_OK, "rm(104B) 清空 64B 档");

        raven_dir_Inode_t dino = read_dir_inode(fs);
        const uint64_t b64_bmp  = dino.heaps_64B_8k.bitmap_base_block_ptr;
        const uint64_t b64_data = dino.heaps_64B_8k.data_base_block_ptr;
        CHECK(fs.dir_name_heap_release(0) == RC_OK, "dir_name_heap_release(0) → RC_OK");

        raven_dir_Inode_t dino2 = read_dir_inode(fs);
        CHECK(dino2.heaps_64B_8k.bitmap_base_block_ptr == NAME_HEAP_INACTIVE,
              "64B_8k 已释放（控制器重置未激活哨兵）");
        CHECK(dino2.heaps_8B_8k.bitmap_base_block_ptr != NAME_HEAP_INACTIVE,
              "8B_8k 仍有分配（n80/n41y），保留不释放");
        CHECK(dino2.heaps_128B_8k.bitmap_base_block_ptr != NAME_HEAP_INACTIVE,
              "128B_8k 仍有分配（n105/n168），保留");
        /* 归还验证：64B_8k 的位图/数据块在全局 bitmap 中已空闲 */
        CHECK(!fs.block_test(b64_bmp) && !fs.block_test(b64_data),
              "64B_8k 位图/数据块已归还（全局 bitmap 空闲）");
        /* 归还的块可被再分配：插 41B 名字应激活新 64B 堆？——不，41B 走 8B 档；
         * 插 81B 应重新激活 64B_8k（且分配到的基址可能复用归还块） */
        CHECK(fs.root_dir_insert_dentry(n81, INODE_TYPE_REGULAR) == RC_OK, "重新插 81B → 64B_8k 重新激活");
        raven_dir_Inode_t dino3 = read_dir_inode(fs);
        CHECK(dino3.heaps_64B_8k.bitmap_base_block_ptr != NAME_HEAP_INACTIVE,
              "64B_8k 已重新激活（懒激活复用）");
        CHECK(fs.root_dir_lookup(n81, &inum2) == RC_OK, "重插 81B 可查");
    }
    }   /* fs 析构：位图全量写回 */

    /* ══ Phase 5：持久化（重挂载） ══ */
    printf("\n--- Phase 5: 持久化（重挂载后长名可查） ---\n");
    {
        RavenFS_juvenile fs2(mnt, &rc);
        CHECK(fs2.is_mounted(), "重挂载 rc=%s", rc_name(rc));
        if (fs2.is_mounted()) {
            const char *names[] = { n80, n105, n168, n169, n296, n297, n41y };
            bool ok = true;
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                uint64_t inum = 0;
                result_code_t lr = fs2.root_dir_lookup(names[i], &inum);
                if (lr != RC_OK) { ok = false; printf("  [FAIL] lookup 残留长名 #%zu → %s\n", i, rc_name(lr)); }
            }
            CHECK(ok, "重挂载后全部残留长名可查");
            uint64_t inum = 0;
            CHECK(fs2.root_dir_lookup(n41, &inum) == RC_ERR_NOT_FOUND, "已删 41B 仍不存在");
            CHECK(fs2.root_dir_lookup(n552, &inum) == RC_ERR_NOT_FOUND, "已删 552B 仍不存在");
            CHECK(fs2.root_dir_lookup(n81, &inum) == RC_OK, "重插 81B 持久化后可查");
        }
    }
    ::close(mnt.dev.fd);

    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
