/* ═══════════════════════════════════════════════════════════════════════════
 * mkstates_w.cpp — M4.2（写数据）**靶子/初始镜像构造器**（写副本；M4.1 mkstates.cpp 的姊妹件）
 *
 * 目的：造一个「**可直接被 ko 写**」的 v1.2 镜像 + 三份配套清单：
 *   · 供 M4.2 写路径验收：先说清「写之前盘上是什么」（初始态），再说清「一组确定性写之后
 *     应该变成什么」（期望态），最后把「怎么做这组写」和「i_blocks 应是多少」列出。
 *
 * 初始靶子（覆盖 M4.2 判据）：
 *   fresh.bin        inline, size=0             —— 空文件（写 ≤160 保持 inline / 越 160 触发迁移）
 *   inline100.bin    inline, size=100 (fill)    —— inline 源，供覆盖写 / 跨 160 增长
 *   inline160.bin    inline, size=160 (fill)    —— inline 上界，供「越界写远 offset」造洞
 *   direct5k.bin     direct, size=5000 (zeros)  —— direct 源，供覆盖写 / 跨块增长（保持 direct）
 *   extent5k.bin     extent, size=5000 (zeros)  —— extent 源，供覆盖写 / 跨块
 *   extent_hole.bin  extent, size=32768 (fill+洞) —— 洞 [1,5)，供「洞填」
 *   big.bin          inline, size=0             —— 供「>64K 增长」
 *
 * 写场景（确定性；见 WOPS）= 覆盖写 / 洞填 / 跨块增长 / >64K 增长 / inline→extent 迁移。
 *   写字节 = m42_fill_byte(name, off)（与期望清单 D 段同一公式）。
 *
 * 产物（默认后缀）：
 *   <out.img>           初始镜像（可挂载可写）
 *   <out.img>.expect    初始内容的 M4.1 清单（供 `audit_v1.2` + `readback verify` 自证）
 *   <out.img>.wexpect   写后内容的 M4.1 清单（供 `readback dump` 产 ground-truth，guest cmp）
 *   <out.img>.plan      写操作 + i_blocks 期望：`W <name> <off> <len>` / `B <name> <st512>`
 *
 * 真值来源：include/disk_struct_v1.2.h、magics.h、Docs/disk_layout_v1.1.md §5/§10、
 *           Docs/disk_layout_v1.2_inode.md §2、Docs/linux/ravenfs-linux-ko-M4-data-spec.md §3/§8/§11。
 * 纪律：**不改** mkstates.cpp / readback.cpp / m41_expect.h；写副本（auditor 本身只读）。
 *
 * 用法： ./mkstates_w <in_empty.img> <out.img> [prefix]
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "disk_struct_v1.2.h"
#include "magics.h"
#include "m41_expect.h"
#include "m42_fill.h"
#include <xxhash.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static constexpr uint64_t BLK   = 4096;
static constexpr uint64_t NODE  = 8192;
static constexpr uint64_t PTR52 = (1ULL << 52) - 1;
static constexpr uint64_t PTR_INV = PTR_INVALID;

static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

/* ── 两版 fill 公式一致性交叉校验（防 m42_fill.h 与 m41_expect.h 漂移）── */
static void assert_fill_consistent() {
    const char *names[] = { "", "a", "inline100.bin", "extent_hole.bin", "big.bin", "x/y.z" };
    for (const char *nm : names)
        for (uint64_t off = 0; off < 5000; off += 97)
            if (m42_fill_byte(nm, off) != m41_fill_byte(nm, off)) {
                fprintf(stderr, "FATAL: m42_fill_byte != m41_fill_byte (name=%s off=%llu)\n",
                        nm, (unsigned long long)off);
                exit(2);
            }
}

struct Img {
    int fd = -1;
    uint64_t total_blocks = 0, inode_count = 0, root_inode = 0;
    uint64_t bitmap_blocks = 0, bitmap_start = 0, inode_blocks = 0, inode_array_base = 0;
    uint64_t inode_bm_blocks = 0, inode_bm_base = 0;
    std::vector<uint8_t> bm, ibm;

    bool rd(void *buf, size_t n, uint64_t off) {
        uint8_t *p = (uint8_t *)buf; size_t d = 0;
        while (d < n) { ssize_t r = pread(fd, p + d, n - d, (off_t)(off + d)); if (r <= 0) return false; d += (size_t)r; }
        return true;
    }
    bool wr(const void *buf, size_t n, uint64_t off) {
        const uint8_t *p = (const uint8_t *)buf; size_t d = 0;
        while (d < n) { ssize_t r = pwrite(fd, p + d, n - d, (off_t)(off + d)); if (r <= 0) return false; d += (size_t)r; }
        return true;
    }
    bool blk_lit(uint64_t b) const { return (bm[b >> 3] >> (b & 7)) & 1; }
    bool ino_free(uint64_t i) const { return !((ibm[i >> 3] >> (i & 7)) & 1); }
    void mark_blk(uint64_t b) { bm[b >> 3] |= (uint8_t)(1u << (b & 7)); }
    void mark_ino(uint64_t i) { ibm[i >> 3] |= (uint8_t)(1u << (i & 7)); }

    uint64_t alloc_run(uint64_t n, uint64_t align) {
        for (uint64_t b = 2; b + n <= total_blocks; b++) {
            if (align > 1 && (b % align)) continue;
            bool ok = true;
            for (uint64_t k = 0; k < n; k++) if (blk_lit(b + k)) { ok = false; b += k; break; }
            if (ok) { for (uint64_t k = 0; k < n; k++) mark_blk(b + k); return b; }
        }
        fprintf(stderr, "mkstates_w: no free %llu contiguous blocks (align=%llu)\n",
                (unsigned long long)n, (unsigned long long)align);
        exit(1);
    }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkstates_w: no free inode\n"); exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

/* ── 目录项（与 mkcontent/mkstates 同构；名字全 ≤40B → 不进名字堆）── */
struct Ent { std::string name; uint64_t inode; };
static void sort_ents(std::vector<Ent> &v) {
    std::sort(v.begin(), v.end(), [](const Ent &a, const Ent &b) {
        uint64_t ha = (uint64_t)XXH64(a.name.data(), a.name.size(), 0);
        uint64_t hb = (uint64_t)XXH64(b.name.data(), b.name.size(), 0);
        if (ha != hb) return ha < hb;
        return a.name < b.name;
    });
}
static void build_dentry_node(std::vector<uint8_t> &node, uint64_t owner_inum,
                              uint64_t dotdot_inum, const std::vector<Ent> &ents) {
    node.assign(NODE, 0);
    auto *meta = (Bptree_meta_t *)node.data();
    meta->magics[0] = DENTRY_MAGIC1; meta->magics[1] = DENTRY_MAGIC2;
    meta->magics[2] = DENTRY_MAGIC3; meta->magics[3] = DENTRY_MAGIC4;
    meta->parent_ptr.v = RFS_PP_PACK(ents.size(), 0, 1, owner_inum);
    meta->ptrs[0].v = RFS_PTR_PACK(0, PTR52);
    meta->ptrs[1].v = RFS_PTR_PACK(0, dotdot_inum);
    meta->ptrs[2].v = RFS_PTR_PACK(0, PTR52);
    auto *leaf = (Bptree_ddentry_leaf_node_t *)node.data();
    for (size_t i = 0; i < ents.size(); i++) {
        ddentry_t de; memset(&de, 0, sizeof(de));
        de.inode_ptr.v = RFS_PTR_PACK(ents[i].name.size(), ents[i].inode);
        de.hash = (uint64_t)XXH64(ents[i].name.data(), ents[i].name.size(), 0);
        de.full_char_remain_ptr = PTR_INVALID;
        memcpy(de.name, ents[i].name.data(), ents[i].name.size());
        memcpy(&leaf->entries[i], &de, sizeof(de));
    }
}

static void set_common(raven_Inode_t *ino, uint32_t type, uint32_t direct, uint64_t size,
                       uint64_t data_root, uint32_t link_count, uint64_t mode, uint64_t xattr) {
    memset(ino, 0, 256);
    ino->word1      = RFS_INO1_PACK(type, direct, 0);
    ino->link_count = link_count;
    ino->mode_xattr = RFS_MODE_XATTR_PACK(mode, xattr);
    ino->size       = size;
    ino->data_root  = data_root;
}

struct ExtP { uint64_t fblk, len, phys; };

static void write_extent_leaf(Img &im, uint64_t blk, uint64_t owner_inum,
                              const std::vector<ExtP> &ex) {
    std::vector<uint8_t> node(NODE, 0);
    auto *meta = (Bptree_meta_t *)node.data();
    meta->magics[0] = BPTREE_MAGIC1; meta->magics[1] = BPTREE_MAGIC2;
    meta->magics[2] = BPTREE_MAGIC3; meta->magics[3] = BPTREE_MAGIC4;
    meta->parent_ptr.v = RFS_PP_PACK(ex.size(), 0, 1 /*root*/, owner_inum);
    for (int i = 0; i < 3; i++) meta->ptrs[i].v = RFS_PTR_PACK(0, PTR52);
    auto *leaf = (Bptree_extent_leaf_node_t *)node.data();
    for (size_t i = 0; i < ex.size(); i++) {
        leaf->extents[i].length    = ex[i].len;
        leaf->extents[i].fblkbase  = ex[i].fblk;
        leaf->extents[i].block_base.v = RFS_PTR_PACK(0, ex[i].phys);
    }
    im.wr(node.data(), node.size(), blkoff(blk));
}

/* 写一个物理块 = 文件内 offset 起 4096B；zero=true 则整块 0，否则按 fill（越 size 部分补 0） */
static void write_data_block(Img &im, uint64_t phys, const char *name, uint64_t foff,
                             uint64_t size, bool zero) {
    std::vector<uint8_t> b(BLK, 0);
    if (!zero)
        for (uint64_t j = 0; j < BLK; j++)
            if (foff + j < size) b[(size_t)j] = m42_fill_byte(name, foff + j);
    im.wr(b.data(), b.size(), blkoff(phys));
}

/* 覆盖区间 → 段清单（D/Z），无缝覆盖 [0,size) */
static std::vector<M41Seg> extent_segs(const std::vector<ExtP> &ex, uint64_t size) {
    std::vector<std::pair<uint64_t, uint64_t>> cov;
    for (const ExtP &e : ex) {
        uint64_t s = e.fblk * BLK;
        uint64_t en = std::min((e.fblk + e.len) * BLK, size);
        if (s < size && en > s) cov.push_back({s, en});
    }
    std::sort(cov.begin(), cov.end());
    std::vector<M41Seg> segs;
    uint64_t cur = 0;
    for (auto &c : cov) {
        if (c.first > cur) segs.push_back({cur, c.first - cur, 'Z'});
        if (c.second > c.first) segs.push_back({c.first, c.second - c.first, 'D'});
        cur = std::max(cur, c.second);
    }
    if (cur < size) segs.push_back({cur, size - cur, 'Z'});
    return segs;
}

/* ── 写场景（确定性；写字节 = m42_fill_byte(name, off)）── */
struct WOp { const char *name; uint64_t off, len; };
static const WOp WOPS[] = {
    { "fresh.bin",        0,    100    },  /* 空文件写 100B → 保持 inline */
    { "inline100.bin",    0,    300    },  /* inline → 越 160 → 迁移 extent */
    { "inline160.bin",    8192, 100    },  /* inline → 远 offset 写 → 迁移 + 造洞 [160,8192) */
    { "direct5k.bin",     1024, 4096   },  /* direct 覆盖写 + 跨块（size 5000→5120，仍 direct）*/
    { "extent5k.bin",     3072, 2048   },  /* extent 覆盖写 + 跨块（size 5000→5120）*/
    { "extent_hole.bin",  4096, 16384  },  /* 洞填 [1,5) → 洞变数据 */
    { "big.bin",          0,    131072 },  /* >64K 增长（128KB extent）*/
};
#define N_WOPS ((int)(sizeof(WOPS) / sizeof(WOPS[0])))

/* i_blocks 期望（512B 单位）= super.c::ravenfs_inode_blocks4k × 8
 *   blks4k = 数据块 + extent 节点×2（root 叶 1 个 = 2 块）+ xattr(0) */
struct SBE { const char *name; uint64_t st512; };
static const SBE SBEXP[] = {
    { "fresh.bin",        0   },  /* inline size100 ≤160 → 0 */
    { "inline100.bin",    24  },  /* size300 extent：data 1 + node 2 = 3 → 24 */
    { "inline160.bin",    32  },  /* size8292 extent：data 2 + node 2 = 4 → 32 */
    { "direct5k.bin",     16  },  /* size5120 direct：ceil=2 块 → 16 */
    { "extent5k.bin",     32  },  /* size5120 extent：data 2 + node 2 → 32 */
    { "extent_hole.bin",  80  },  /* size32768 extent：data 8 + node 2 → 80 */
    { "big.bin",          272 },  /* size131072 extent：data 32 + node 2 → 272 */
};

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <in_empty.img> <out.img> [prefix]\n", argv[0]);
        return 1;
    }
    assert_fill_consistent();
    std::string base = (argc >= 4) ? argv[3] : std::string(argv[2]);
    std::string expect_path  = base + ".expect";
    std::string wexpect_path = base + ".wexpect";
    std::string plan_path    = base + ".plan";

    /* 复制空盘 → 副本（auditor 本身只读；本工具只在副本上写） */
    {
        FILE *fi = fopen(argv[1], "rb");
        if (!fi) { perror("open in"); return 1; }
        FILE *fo = fopen(argv[2], "wb");
        if (!fo) { perror("open out"); return 1; }
        char buf[1 << 20]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) fwrite(buf, 1, n, fo);
        fclose(fi); fclose(fo);
    }

    Img im; im.fd = open(argv[2], O_RDWR);
    if (im.fd < 0) { perror("open out O_RDWR"); return 1; }

    RavenSuperblock sb;
    if (!im.rd(&sb, sizeof(sb), 0)) return 1;
    im.total_blocks = sb.total_blocks;
    im.inode_count  = sb.inode_array_count;
    im.root_inode   = sb.root_inode;
    uint64_t bb = (im.total_blocks + 32767) / 32768; im.bitmap_blocks = bb; im.bitmap_start = im.total_blocks - bb;
    uint64_t ib = (im.inode_count * 256 + 4095) / 4096; im.inode_blocks = ib; im.inode_array_base = im.bitmap_start - ib;
    uint64_t ibb = (im.inode_count + 32767) / 32768; im.inode_bm_blocks = ibb; im.inode_bm_base = im.inode_array_base - ibb;

    im.bm.assign((size_t)im.bitmap_blocks * BLK, 0);
    im.ibm.assign((size_t)im.inode_bm_blocks * BLK, 0);
    im.rd(im.bm.data(), im.bm.size(), blkoff(im.bitmap_start));
    im.rd(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    printf("mkstates_w: geo total_blocks=%llu inode_count=%llu root=%llu "
           "bitmap=[%llu,+%llu) inode_array=[%llu,+%llu) inode_bm=[%llu,+%llu)\n",
           (unsigned long long)im.total_blocks, (unsigned long long)im.inode_count,
           (unsigned long long)im.root_inode,
           (unsigned long long)im.bitmap_start, (unsigned long long)im.bitmap_blocks,
           (unsigned long long)im.inode_array_base, (unsigned long long)im.inode_blocks,
           (unsigned long long)im.inode_bm_base, (unsigned long long)im.inode_bm_blocks);

    /* ── 分配 inode ── */
    uint64_t i_fresh  = im.alloc_inode();
    uint64_t i_i100   = im.alloc_inode();
    uint64_t i_i160   = im.alloc_inode();
    uint64_t i_d5k    = im.alloc_inode();
    uint64_t i_e5k    = im.alloc_inode();
    uint64_t i_ehole  = im.alloc_inode();
    uint64_t i_big    = im.alloc_inode();

    /* ── 分配块 ── */
    uint64_t blk_d5k   = im.alloc_run(2, 1);                 /* direct 5000 → 2 块 */
    uint64_t blk_e5kn  = im.alloc_run(2, 2), blk_e5kd = im.alloc_run(2, 1);
    uint64_t blk_ehn   = im.alloc_run(2, 2);
    uint64_t blk_eh1   = im.alloc_run(1, 1), blk_eh2 = im.alloc_run(3, 1);

    printf("mkstates_w: inodes fresh=%llu i100=%llu i160=%llu d5k=%llu e5k=%llu ehole=%llu big=%llu\n",
           (unsigned long long)i_fresh, (unsigned long long)i_i100, (unsigned long long)i_i160,
           (unsigned long long)i_d5k, (unsigned long long)i_e5k, (unsigned long long)i_ehole,
           (unsigned long long)i_big);
    printf("mkstates_w: blocks d5k=%llu | e5k[n=%llu d=%llu] | ehole[n=%llu d=%llu,%llu]\n",
           (unsigned long long)blk_d5k, (unsigned long long)blk_e5kn, (unsigned long long)blk_e5kd,
           (unsigned long long)blk_ehn, (unsigned long long)blk_eh1, (unsigned long long)blk_eh2);

    std::vector<M41File> init_files;   /* 初始内容清单（m42.img.expect）*/
    std::vector<M41File> post_files;   /* 写后内容清单（m42.img.wexpect）*/

    /* ── A. fresh.bin：inline, size=0 ── */
    {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, 0, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_fresh, &ino, 256);
        init_files.push_back(M41File{"fresh.bin", i_fresh, 0, {}, ""});
        post_files.push_back(M41File{"fresh.bin", i_fresh, 100, {{0, 100, 'D'}}, ""});
    }
    /* ── B. inline100.bin：inline, size=100 (fill) ── */
    {
        const char *nm = "inline100.bin"; uint64_t sz = 100;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        for (uint64_t i = 0; i < sz; i++) ino.inline_data[i] = m42_fill_byte(nm, i);
        im.write_inode(i_i100, &ino, 256);
        init_files.push_back(M41File{nm, i_i100, sz, {{0, sz, 'D'}}, ""});
        post_files.push_back(M41File{nm, i_i100, 300, {{0, 300, 'D'}}, ""});
    }
    /* ── C. inline160.bin：inline, size=160 (fill；内联上界) ── */
    {
        const char *nm = "inline160.bin"; uint64_t sz = 160;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        for (uint64_t i = 0; i < sz; i++) ino.inline_data[i] = m42_fill_byte(nm, i);
        im.write_inode(i_i160, &ino, 256);
        init_files.push_back(M41File{nm, i_i160, sz, {{0, sz, 'D'}}, ""});
        /* 写后：迁移 extent + 洞 [160,8192) + 尾 [8192,8292) */
        post_files.push_back(M41File{nm, i_i160, 8292,
                                     {{0, 160, 'D'}, {160, 8032, 'Z'}, {8192, 100, 'D'}}, ""});
    }
    /* ── D. direct5k.bin：direct, size=5000, 内容全 0 ── */
    {
        const char *nm = "direct5k.bin"; uint64_t sz = 5000;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 1, sz, blk_d5k, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_d5k, &ino, 256);
        for (uint64_t b = 0; b * BLK < sz; b++) write_data_block(im, blk_d5k + b, nm, b * BLK, sz, true);
        init_files.push_back(M41File{nm, i_d5k, sz, {{0, sz, 'Z'}}, ""});
        /* 写 [1024,5120) → size 5120；[0,1024) 仍 0 */
        post_files.push_back(M41File{nm, i_d5k, 5120, {{0, 1024, 'Z'}, {1024, 4096, 'D'}}, ""});
    }
    /* ── E. extent5k.bin：extent, size=5000, 内容全 0（单条 [0,2)）── */
    {
        const char *nm = "extent5k.bin"; uint64_t sz = 5000;
        std::vector<ExtP> ex = {{0, 2, blk_e5kd}};
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, blk_e5kn, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_e5k, &ino, 256);
        write_extent_leaf(im, blk_e5kn, i_e5k, ex);
        for (auto &e : ex) for (uint64_t k = 0; k < e.len; k++)
            write_data_block(im, e.phys + k, nm, (e.fblk + k) * BLK, sz, true);
        init_files.push_back(M41File{nm, i_e5k, sz, {{0, sz, 'Z'}}, ""});
        /* 写 [3072,5120) → size 5120；[0,3072) 仍 0 */
        post_files.push_back(M41File{nm, i_e5k, 5120, {{0, 3072, 'Z'}, {3072, 2048, 'D'}}, ""});
    }
    /* ── F. extent_hole.bin：extent, size=32768, extents [0,1)+[5,3)，洞 [1,5)（fill + 洞）── */
    {
        const char *nm = "extent_hole.bin"; uint64_t sz = 32768;
        std::vector<ExtP> ex = {{0, 1, blk_eh1}, {5, 3, blk_eh2}};
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, blk_ehn, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_ehole, &ino, 256);
        write_extent_leaf(im, blk_ehn, i_ehole, ex);
        for (auto &e : ex) for (uint64_t k = 0; k < e.len; k++)
            write_data_block(im, e.phys + k, nm, (e.fblk + k) * BLK, sz, false);
        init_files.push_back(M41File{nm, i_ehole, sz, extent_segs(ex, sz), ""});
        /* 洞填 [4096,20480) → 全覆盖 */
        post_files.push_back(M41File{nm, i_ehole, sz, {{0, sz, 'D'}}, ""});
    }
    /* ── G. big.bin：inline, size=0（供 >64K 增长）── */
    {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, 0, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_big, &ino, 256);
        init_files.push_back(M41File{"big.bin", i_big, 0, {}, ""});
        post_files.push_back(M41File{"big.bin", i_big, 131072, {{0, 131072, 'D'}}, ""});
    }

    /* ── root 目录条目 ── */
    std::vector<Ent> root_ents = {
        {"fresh.bin", i_fresh}, {"inline100.bin", i_i100}, {"inline160.bin", i_i160},
        {"direct5k.bin", i_d5k}, {"extent5k.bin", i_e5k}, {"extent_hole.bin", i_ehole},
        {"big.bin", i_big}
    };
    sort_ents(root_ents);
    std::vector<uint8_t> root_node;
    build_dentry_node(root_node, im.root_inode, im.root_inode, root_ents);
    im.wr(root_node.data(), root_node.size(), blkoff(0));

    /* ── root inode ── */
    {
        raven_dir_Inode_t d;
        im.rd(&d, 256, blkoff(im.inode_array_base) + im.root_inode * 256);
        d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
        d.size       = root_ents.size();
        d.link_count = (uint32_t)(root_ents.size() + 2);
        d.data_root  = 0;
        d.mode_xattr = RFS_MODE_XATTR_PACK(0755, RFS_XATTR_NONE);
        for (int i = 0; i < RFS_DIR_HEAP_COUNT; i++)
            RFS_NHC_SET(*rfs_dir_heap(&d, i), 0, 0, NAME_HEAP_INACTIVE, 0, NAME_HEAP_INACTIVE);
        im.wr(&d, 256, blkoff(im.inode_array_base) + im.root_inode * 256);
    }

    /* ── 落盘位图 ── */
    im.wr(im.bm.data(),  im.bm.size(),  blkoff(im.bitmap_start));
    im.wr(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    /* ── 期望清单（初始 / 写后）——均用 M4.1 清单格式，readback 直接消费 ── */
    if (!m41_write_expect(expect_path.c_str(),  init_files)) { fprintf(stderr, "mkstates_w: 写 %s 失败\n", expect_path.c_str()); return 1; }
    if (!m41_write_expect(wexpect_path.c_str(), post_files)) { fprintf(stderr, "mkstates_w: 写 %s 失败\n", wexpect_path.c_str()); return 1; }

    /* ── plan（写操作 + i_blocks 期望）── */
    {
        FILE *fp = fopen(plan_path.c_str(), "w");
        if (!fp) { fprintf(stderr, "mkstates_w: 写 %s 失败\n", plan_path.c_str()); return 1; }
        fprintf(fp, "# ravenfs-m4.2-plan v1\n");
        fprintf(fp, "# W <name> <off> <len>   ：pwrite fill_byte(name,off..) ；B <name> <st512> ：期望 i_blocks\n");
        for (int i = 0; i < N_WOPS; i++)
            fprintf(fp, "W %s %llu %llu\n", WOPS[i].name,
                    (unsigned long long)WOPS[i].off, (unsigned long long)WOPS[i].len);
        for (size_t i = 0; i < sizeof(SBEXP) / sizeof(SBEXP[0]); i++)
            fprintf(fp, "B %s %llu\n", SBEXP[i].name, (unsigned long long)SBEXP[i].st512);
        fclose(fp);
    }

    printf("mkstates_w: expect  → %s（%zu 文件，初始）\n", expect_path.c_str(), init_files.size());
    printf("mkstates_w: wexpect → %s（%zu 文件，写后）\n", wexpect_path.c_str(), post_files.size());
    printf("mkstates_w: plan    → %s（%d 写操作）\n", plan_path.c_str(), N_WOPS);
    for (const M41File &f : init_files)
        printf("  [init] %-16s inode=%-3llu size=%-7llu segs=%zu xxh64=%016llx\n",
               f.name.c_str(), (unsigned long long)f.inode, (unsigned long long)f.size,
               f.segs.size(), (unsigned long long)m41_hash(f));
    for (const M41File &f : post_files)
        printf("  [post] %-16s inode=%-3llu size=%-7llu segs=%zu xxh64=%016llx\n",
               f.name.c_str(), (unsigned long long)f.inode, (unsigned long long)f.size,
               f.segs.size(), (unsigned long long)m41_hash(f));

    close(im.fd);
    printf("mkstates_w: done → %s\n", argv[2]);
    return 0;
}
