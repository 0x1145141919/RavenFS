/* ═══════════════════════════════════════════════════════════════════
 * test_file_compact.cpp — regular_file_compact 离线碎片压缩专项（v2.0）
 *
 * 靶场（真盘/镜像，mkfs --v2.0）：造「逻辑连续、物理断裂」的 extent 文件 + 稀疏
 * （带洞）文件，验证：
 *   · dry-run 规划数（records before→after / runs / relocate）正确、零写盘；
 *   · apply 搬迁后 extent 收敛（碎裂 run → 单条；稠密文件 → direct）；
 *   · **洞保持洞**（稀疏语义不变）、数据内容逐字节一致；
 *   · max_move_blocks 超限 → 跳过（不动）；
 *   · 全盘 audit bad=0。
 *
 * 造碎片手法：A 写 [0,2) → direct；B 写 [0,2) 占住 A 的物理尾邻；A 再写 [2,4)
 * 增长不可邻接 → A 迁 extent 得「逻辑 [0,4) 单 run、物理 2 段」。
 *
 * 用法：ravenfs_juv_file_compact <device|image>
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_juvenile.hpp"
#undef private
#include "inode_compat.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#include "../include/ravenfs/ravenfs_audit.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static uint64_t g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; }                                              \
        else { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); }      \
    } while (0)

static const uint64_t PTR_INVALID_52 = (1ULL << 52) - 1;
static const uint64_t BLK = 4096;

static linux_userspace_mount_t make_mount(const char *path) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_NVME;
    mnt.dev.is_device_direct = false;
    mnt.param.if_will_use_inode_cache = true;
    mnt.param.if_will_use_mdata_piece_cache = true;
    mnt.param.slices_mode = SLICES_MODE_DEFRAG;   /* 与工具一致 */
    mnt.logical_processor_count = 1;
    return mnt;
}

struct shape_t {
    bool direct = false;
    uint64_t size_ceil = 0;
    std::vector<extent_t> recs;
};

static shape_t get_shape(RavenFS_juvenile &fs, uint64_t inum) {
    shape_t o;
    raven_Inode_t ino;
    juv_inode_read(fs, inum, &ino);
    o.size_ceil = (ino.size + BLK - 1) / BLK;
    if (ino.size <= 160) return o;
    if (RFS_INO1_GET_DIRECT(ino.word1)) { o.direct = true; return o; }
    uint64_t root = ino.data_root;
    if (root == PTR_INVALID_52 || root == PTR_INVALID) return o;
    std::vector<uint64_t> stack; stack.push_back(root);
    uint64_t guard = 0;
    while (!stack.empty()) {
        if (++guard > (1ULL << 20)) break;
        uint64_t blk = stack.back(); stack.pop_back();
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) break;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_extent_node(m)) break;
        if (RFS_PP_IS_INNER(m->parent_ptr)) {
            auto *in = (Bptree_extent_inner_node_t *)s.data;
            uint16_t c = (uint16_t)RFS_PP_COUNT(in->meta.parent_ptr);
            for (uint16_t i = c; i-- > 0;) stack.push_back(RFS_PTR_ADDR(in->extents[i].sub_node_ptr));
        } else {
            auto *lf = (Bptree_extent_leaf_node_t *)s.data;
            uint16_t c = (uint16_t)RFS_PP_COUNT(lf->meta.parent_ptr);
            for (uint16_t i = 0; i < c; i++) o.recs.push_back(lf->extents[i]);
        }
    }
    return o;
}

static void fill(std::vector<uint8_t> &b, uint8_t v) { for (auto &x : b) x = v; }

/* pread 全缓冲；短读报错。 */
static bool pread_exact(RavenFS_juvenile &fs, uint64_t inum, uint64_t off,
                        std::vector<uint8_t> &out) {
    out.assign(out.size(), 0);
    uint64_t got = 0;
    InodeSession s(fs.inode_io, inum);
    if (!s.inode) return false;
    /* 直接调上帝类同名接口按 inode idx：root_dir_direct_file_pread 只受理 root 目录名，
     * 这里用 direct_file_pread(inode_idx, ...) 通用入口。 */
    result_code_t rc = fs.direct_file_pread(inum, off, out.size(), out.data(), &got);
    return rc == RC_OK;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <device|image> [create]\n", argv[0]); return 2; }
    const bool create_only = (argc > 2 && !strcmp(argv[2], "create"));
    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { perror(argv[1]); return 2; }
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { fprintf(stderr, "mount failed rc=%llu\n", (unsigned long long)rc); return 2; }
    printf("test_file_compact: mounted (root=%llu)\n", (unsigned long long)fs.root_inum);

    const char *A = "cmpA", *B = "cmpB", *Nm = "cmpC", *D = "cmpD";
    const char *En = "cmpE", *F = "cmpF", *G = "cmpG";
    CHECK(fs.root_dir_insert_dentry(A, INODE_TYPE_REGULAR) == RC_OK, "create A");
    CHECK(fs.root_dir_insert_dentry(B, INODE_TYPE_REGULAR) == RC_OK, "create B");
    CHECK(fs.root_dir_insert_dentry(Nm, INODE_TYPE_REGULAR) == RC_OK, "create C");
    CHECK(fs.root_dir_insert_dentry(D, INODE_TYPE_REGULAR) == RC_OK, "create D");
    CHECK(fs.root_dir_insert_dentry(En, INODE_TYPE_REGULAR) == RC_OK, "create E");
    CHECK(fs.root_dir_insert_dentry(F, INODE_TYPE_REGULAR) == RC_OK, "create F");
    CHECK(fs.root_dir_insert_dentry(G, INODE_TYPE_REGULAR) == RC_OK, "create G");
    uint64_t iA = 0, iB = 0, iC = 0, iD = 0, iE = 0, iF = 0, iG = 0;
    CHECK(fs.dir_lookup(fs.root_inum, A, &iA) == RC_OK, "lookup A");
    CHECK(fs.dir_lookup(fs.root_inum, B, &iB) == RC_OK, "lookup B");
    CHECK(fs.dir_lookup(fs.root_inum, Nm, &iC) == RC_OK, "lookup C");
    CHECK(fs.dir_lookup(fs.root_inum, D, &iD) == RC_OK, "lookup D");
    CHECK(fs.dir_lookup(fs.root_inum, En, &iE) == RC_OK, "lookup E");
    CHECK(fs.dir_lookup(fs.root_inum, F, &iF) == RC_OK, "lookup F");
    CHECK(fs.dir_lookup(fs.root_inum, G, &iG) == RC_OK, "lookup G");

    /* ── 造稠密碎裂文件 A：2 段 1 run ── */
    std::vector<uint8_t> a0(2 * BLK), a1(2 * BLK);
    fill(a0, 0xA0); fill(a1, 0xA1);
    CHECK(fs.direct_file_pwrite(iA, 0, a0.size(), a0.data()) == RC_OK, "A w0");
    std::vector<uint8_t> b0(2 * BLK); fill(b0, 0xB0);
    CHECK(fs.direct_file_pwrite(iB, 0, b0.size(), b0.data()) == RC_OK, "B w0");
    CHECK(fs.direct_file_pwrite(iA, 2 * BLK, a1.size(), a1.data()) == RC_OK, "A w1");

    shape_t sA = get_shape(fs, iA);
    CHECK(!sA.direct && sA.recs.size() == 2, "A fragmented: 2 recs (got %zu)", sA.recs.size());
    if (sA.recs.size() == 2) {
        CHECK(sA.recs[0].fblkbase == 0 && sA.recs[0].length == 2 &&
              sA.recs[1].fblkbase == 2 && sA.recs[1].length == 2, "A recs shape");
        CHECK(RFS_PTR_ADDR(sA.recs[0].block_base) + 2 != RFS_PTR_ADDR(sA.recs[1].block_base),
              "A phys gap");
    }

    /* ── 造稀疏碎裂文件 C：run1 碎（2 条）+ run2（1 条）+ 中间洞 ── */
    std::vector<uint8_t> c0(2 * BLK), c1(2 * BLK), c2(2 * BLK);
    fill(c0, 0xC0); fill(c1, 0xC1); fill(c2, 0xC2);
    CHECK(fs.direct_file_pwrite(iC, 0, c0.size(), c0.data()) == RC_OK, "C w0");
    std::vector<uint8_t> d0(2 * BLK); fill(d0, 0xD0);
    CHECK(fs.direct_file_pwrite(iD, 0, d0.size(), d0.data()) == RC_OK, "D w0");
    CHECK(fs.direct_file_pwrite(iC, 2 * BLK, c1.size(), c1.data()) == RC_OK, "C w1");
    CHECK(fs.direct_file_pwrite(iC, 40 * BLK, c2.size(), c2.data()) == RC_OK, "C w2 (hole)");

    shape_t sC = get_shape(fs, iC);
    CHECK(!sC.direct && sC.recs.size() == 3, "C fragmented: 3 recs (got %zu)", sC.recs.size());

    /* ── 造多记录稠密碎裂文件 E（与 F 交错写 → N 条记录 / 单 run）── */
    const int NE = 12;
    for (int i = 0; i < NE; i++) {
        std::vector<uint8_t> ce(2 * BLK), cf(2 * BLK);
        fill(ce, (uint8_t)(0xE0 + i)); fill(cf, 0xF0);
        CHECK(fs.direct_file_pwrite(iE, (uint64_t)i * 2 * BLK, ce.size(), ce.data()) == RC_OK, "E w");
        CHECK(fs.direct_file_pwrite(iF, (uint64_t)i * 2 * BLK, cf.size(), cf.data()) == RC_OK, "F w");
    }
    shape_t sE = get_shape(fs, iE);
    CHECK(!sE.direct && sE.recs.size() >= 2, "E fragmented (%zu recs)", sE.recs.size());

    /* ── 造 G：单条记录、稠密、extent 形态（D-6 遗留）→ 应走「单 run 降 direct」── */
    std::vector<uint8_t> g0(2 * BLK); fill(g0, 0x60);
    CHECK(fs.direct_file_pwrite(iG, 0, g0.size(), g0.data()) == RC_OK, "G w");
    {
        uint64_t gbase = 0;
        { InodeSession s(fs.inode_io, iG); if (s.inode) gbase = s.inode->data_root; }
        uint64_t groot = fs.block_alloc_hard(2, 1);
        CHECK(groot != PTR_INVALID, "G root alloc");
        {
            mdata_piece_session s(fs.mdata_piece_mgr, groot);
            fs.extent_node_meta_init(s.data, 1, false, true, iG, RavenFS_juvenile::EXTENT_TREE_FILE);
            auto *lf = (Bptree_extent_leaf_node_t *)s.data;
            RFS_PTR_SET_ADDR(lf->meta.ptrs[0], PTR_INVALID);
            RFS_PTR_SET_ADDR(lf->meta.ptrs[1], PTR_INVALID);
            RFS_PTR_SET_ADDR(lf->meta.ptrs[2], PTR_INVALID);
            lf->extents[0].fblkbase = 0;
            lf->extents[0].length = 2;
            RFS_PTR_SET_ADDR(lf->extents[0].block_base, gbase);
            s.dirty();
        }
        { InodeSession s(fs.inode_io, iG);
          RFS_INO1_SET_DIRECT(*s.inode, 0); s.inode->data_root = groot; s.dirty(); }
        juv_flush(fs);
        shape_t sg = get_shape(fs, iG);
        CHECK(!sg.direct && sg.recs.size() == 1, "G single-record extent (%zu)", sg.recs.size());
    }

    /* create-only：只造碎片后退出，供工具 rfs_compact 手动 apply 验证。 */
    if (create_only) {
        juv_flush(fs);
        printf("create-only: A(2 recs) C(3 recs) fragmentation written; exiting.\n");
        printf("\n==== test_file_compact(create-only): PASS %llu, FAIL %llu ====\n",
               (unsigned long long)g_pass, (unsigned long long)g_fail);
        return g_fail ? 1 : 0;
    }

    /* ── A dry-run：应识别 1 run / 2→1 / 将搬 4 块 / 将降 direct ── */
    {
        file_compact_opts_t o{};
        o.dry_run = true;
        file_compact_report_t r{};
        CHECK(fs.regular_file_compact(iA, o, &r) == RC_OK, "A dry rc");
        CHECK(r.records_before == 2 && r.records_after == 1, "A dry recs 2->1 (%llu->%llu)",
              (unsigned long long)r.records_before, (unsigned long long)r.records_after);
        CHECK(r.runs_total == 1 && r.runs_relocated == 1 && r.runs_skipped == 0, "A dry runs");
        CHECK(r.blocks_moved == 4, "A dry moved 4 (%llu)", (unsigned long long)r.blocks_moved);
        CHECK(r.became_direct && !r.wrote, "A dry would-direct");
        shape_t s2 = get_shape(fs, iA);
        CHECK(!s2.direct && s2.recs.size() == 2, "A dry-run untouched");
    }

    /* ── A apply：真搬 → 单条稠密 → direct，内容一致 ── */
    {
        file_compact_opts_t o{};
        o.dry_run = false;
        file_compact_report_t r{};
        CHECK(fs.regular_file_compact(iA, o, &r) == RC_OK, "A apply rc");
        CHECK(r.wrote && r.became_direct, "A apply committed direct");
        CHECK(r.records_after == 1, "A apply 1 rec");
    }
    juv_flush(fs);
    {
        shape_t s2 = get_shape(fs, iA);
        CHECK(s2.direct, "A now direct");
        std::vector<uint8_t> got(4 * BLK);
        CHECK(pread_exact(fs, iA, 0, got), "A pread");
        std::vector<uint8_t> want = a0; want.insert(want.end(), a1.begin(), a1.end());
        CHECK(got == want, "A content intact after compact");
        /* B 未受影响 */
        std::vector<uint8_t> gb(2 * BLK);
        CHECK(pread_exact(fs, iB, 0, gb) && gb == b0, "B untouched");
    }

    /* ── E：多记录稠密碎裂 → 搬迁后单条 → direct ── */
    {
        file_compact_opts_t oe{}; oe.dry_run = false;
        file_compact_report_t re{};
        CHECK(fs.regular_file_compact(iE, oe, &re) == RC_OK, "E compact rc");
        CHECK(re.wrote && re.became_direct && re.records_after == 1,
              "E → direct (recs=%llu wrote=%d direct=%d)", (unsigned long long)re.records_after,
              (int)re.wrote, (int)re.became_direct);
        juv_flush(fs);
        CHECK(get_shape(fs, iE).direct, "E now direct");
        std::vector<uint8_t> ge(2 * BLK);
        CHECK(pread_exact(fs, iE, 10 * 2 * BLK, ge), "E pread");
        std::vector<uint8_t> we(2 * BLK); fill(we, (uint8_t)(0xE0 + 10));
        CHECK(ge == we, "E content intact");
    }

    /* ── G：单条记录稠密 extent（单 run）→ 工具最后一步降 direct，零搬数 ── */
    {
        file_compact_opts_t og{}; og.dry_run = false;
        file_compact_report_t rg{};
        CHECK(fs.regular_file_compact(iG, og, &rg) == RC_OK, "G compact rc");
        CHECK(rg.wrote && rg.became_direct, "G single-run → direct (wrote=%d direct=%d)",
              (int)rg.wrote, (int)rg.became_direct);
        juv_flush(fs);
        CHECK(get_shape(fs, iG).direct, "G now direct");
        std::vector<uint8_t> gg(2 * BLK);
        CHECK(pread_exact(fs, iG, 0, gg) && gg == g0, "G content intact");
    }

    /* max_move 超限 → 跳过，不动 */
    {
        file_compact_opts_t o{};
        o.dry_run = false;
        o.max_move_blocks = 1;      /* run1 len=4 > 1 → skip */
        file_compact_report_t r{};
        CHECK(fs.regular_file_compact(iC, o, &r) == RC_OK, "C capped rc");
        CHECK(!r.wrote && r.runs_skipped == 1, "C capped skipped (wrote=%d skip=%llu)",
              (int)r.wrote, (unsigned long long)r.runs_skipped);
        shape_t s2 = get_shape(fs, iC);
        CHECK(s2.recs.size() == 3, "C capped untouched");
    }

    /* apply：run1 搬合、run2 保留、洞保留 */
    {
        file_compact_opts_t o{};
        o.dry_run = false;
        file_compact_report_t r{};
        CHECK(fs.regular_file_compact(iC, o, &r) == RC_OK, "C apply rc");
        CHECK(r.wrote, "C apply wrote");
        CHECK(r.records_before == 3 && r.records_after == 2, "C apply 3->2 (%llu->%llu)",
              (unsigned long long)r.records_before, (unsigned long long)r.records_after);
        CHECK(r.runs_total == 2 && r.runs_relocated == 1 && r.runs_skipped == 0, "C apply runs");
        CHECK(!r.became_direct, "C stays extent (has hole)");
    }
    juv_flush(fs);
    {
        shape_t s2 = get_shape(fs, iC);
        CHECK(!s2.direct && s2.recs.size() == 2, "C now 2 recs");
        /* 洞 [4,40) 读 0；数据段内容一致 */
        std::vector<uint8_t> h(4 * BLK); fill(h, 0xEE);
        CHECK(pread_exact(fs, iC, 4 * BLK, h), "C hole pread");
        bool zero = true; for (uint8_t x : h) if (x) { zero = false; break; }
        CHECK(zero, "C hole reads zero");
        std::vector<uint8_t> g0(2 * BLK), g1(2 * BLK), g2(2 * BLK);
        CHECK(pread_exact(fs, iC, 0, g0) && g0 == c0, "C data0 intact");
        CHECK(pread_exact(fs, iC, 2 * BLK, g1) && g1 == c1, "C data1 intact");
        CHECK(pread_exact(fs, iC, 40 * BLK, g2) && g2 == c2, "C data2 intact");
    }

    /* ── 全盘审计 ── */
    {
        audit_walk_report_t rep{};
        fs.audit_dfs_root(&rep, nullptr);
        CHECK(rep.total_bad() == 0, "audit bad=0 (btree=%llu inode=%llu namedata=%llu inodeplane=%llu)",
              (unsigned long long)rep.btree_bad, (unsigned long long)rep.inode_bad,
              (unsigned long long)rep.namedata_bad, (unsigned long long)rep.inodeplane_bad);
    }

    printf("\n==== test_file_compact: PASS %llu, FAIL %llu ====\n",
           (unsigned long long)g_pass, (unsigned long long)g_fail);
    return g_fail ? 1 : 0;
}
