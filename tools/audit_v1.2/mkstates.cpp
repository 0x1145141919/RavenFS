/* ═══════════════════════════════════════════════════════════════════════════
 * mkstates.cpp — M4.1 只读靶子构造器（**写副本**；auditor/mkcontent 的姊妹件）
 *
 * 目的：造一个覆盖 M4.1 判据的 v1.2 镜像，供 `audit_v1.2` 全绿自证 + guest `cat/dd` 比对。
 *   · 三态齐：inline(size≤160) / direct(size>160 ∧ direct_bit=1) / extent(size>160 ∧ direct_bit=0)
 *   · >4K 文件（跨页/跨块）：direct5k(5000) / direct13k(12345) / extent1(5000) / 2 级树(163840)
 *   · 稀疏洞：extent_hole（extent 树内洞，读返 0）
 *   · ≥1 棵 2 级 extent 树：extent_2lvl（内节点 2 子叶；hints key 严格递增 = 子树 max 开边界）
 *   · 附带 sub/ 子目录（递归/叶链路径）+ 一个 inline symlink 之外不额外造。
 *
 * 真值来源：include/disk_struct_v1.2.h（结构）、magics.h、Docs/disk_layout_v1.1.md §5/§10。
 * 与 mkcontent.cpp 同构（复制空盘 → 就地写结构 → 回写位图）；**不改 mkcontent.cpp**。
 *
 * 用法： ./mkstates <in_empty.img> <out.img> [expect.txt] [elfpath]
 *          expect 缺省 = "<out.img>.expect"（供 readback 消费）
 *          elfpath 可选：把一个真实 ELF（推荐静态）原样嵌入为 extent 态 `elfexec.bin`，
 *          供 guest `execve` / mmap(PROT_EXEC) 判据（期望内容走清单 RAW 行）。
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "disk_struct_v1.2.h"
#include "magics.h"
#include "m41_expect.h"
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

    /* 首个「起点对齐到 align 块」的 n 连续空闲块（节点用 align=2，数据用 align=1） */
    uint64_t alloc_run(uint64_t n, uint64_t align) {
        for (uint64_t b = 2; b + n <= total_blocks; b++) {
            if (align > 1 && (b % align)) continue;
            bool ok = true;
            for (uint64_t k = 0; k < n; k++) if (blk_lit(b + k)) { ok = false; b += k; break; }
            if (ok) { for (uint64_t k = 0; k < n; k++) mark_blk(b + k); return b; }
        }
        fprintf(stderr, "mkstates: no free %llu contiguous blocks (align=%llu)\n",
                (unsigned long long)n, (unsigned long long)align);
        exit(1);
    }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkstates: no free inode\n"); exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

/* ── 目录项 ── */
struct Ent { std::string name; uint64_t inode; };
static void sort_ents(std::vector<Ent> &v) {
    std::sort(v.begin(), v.end(), [](const Ent &a, const Ent &b) {
        uint64_t ha = (uint64_t)XXH64(a.name.data(), a.name.size(), 0);
        uint64_t hb = (uint64_t)XXH64(b.name.data(), b.name.size(), 0);
        if (ha != hb) return ha < hb;
        return a.name < b.name;
    });
}
/* 单叶 dentry 节点（与 mkcontent 同构；名字全 ≤40B → 不进名字堆） */
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
        de.full_char_remain_ptr = PTR_INV;
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
static void set_dir_heap_inactive(raven_dir_Inode_t *d) {
    for (int i = 0; i < RFS_DIR_HEAP_COUNT; i++)
        RFS_NHC_SET(*rfs_dir_heap(d, i), 0, 0, NAME_HEAP_INACTIVE, 0, NAME_HEAP_INACTIVE);
}

/* ── extent 树节点写入 ── */
struct ExtP { uint64_t fblk, len, phys; };

static void write_extent_leaf(Img &im, uint64_t blk, uint64_t owner_inum,
                              uint64_t parent_blk, bool is_root,
                              const std::vector<ExtP> &ex) {
    std::vector<uint8_t> node(NODE, 0);
    auto *meta = (Bptree_meta_t *)node.data();
    meta->magics[0] = BPTREE_MAGIC1; meta->magics[1] = BPTREE_MAGIC2;
    meta->magics[2] = BPTREE_MAGIC3; meta->magics[3] = BPTREE_MAGIC4;
    meta->parent_ptr.v = RFS_PP_PACK(ex.size(), 0, is_root ? 1u : 0u,
                                     is_root ? owner_inum : parent_blk);
    for (int i = 0; i < 3; i++) meta->ptrs[i].v = RFS_PTR_PACK(0, PTR52);
    auto *leaf = (Bptree_extent_leaf_node_t *)node.data();
    for (size_t i = 0; i < ex.size(); i++) {
        leaf->extents[i].length    = ex[i].len;
        leaf->extents[i].fblkbase  = ex[i].fblk;
        leaf->extents[i].block_base.v = RFS_PTR_PACK(0, ex[i].phys);
    }
    im.wr(node.data(), node.size(), blkoff(blk));
}
static void write_extent_inner(Img &im, uint64_t blk, uint64_t owner_inum,
                               const std::vector<std::pair<uint64_t, uint64_t>> &hints /* key, child */) {
    std::vector<uint8_t> node(NODE, 0);
    auto *meta = (Bptree_meta_t *)node.data();
    meta->magics[0] = BPTREE_MAGIC1; meta->magics[1] = BPTREE_MAGIC2;
    meta->magics[2] = BPTREE_MAGIC3; meta->magics[3] = BPTREE_MAGIC4;
    meta->parent_ptr.v = RFS_PP_PACK(hints.size(), 1 /*inner*/, 1 /*root*/, owner_inum);
    for (int i = 0; i < 3; i++) meta->ptrs[i].v = RFS_PTR_PACK(0, PTR52);
    auto *nd = (Bptree_extent_inner_node_t *)node.data();
    for (size_t i = 0; i < hints.size(); i++) {
        nd->extents[i].key = hints[i].first;
        nd->extents[i].sub_node_ptr.v = RFS_PTR_PACK(0, hints[i].second);
    }
    im.wr(node.data(), node.size(), blkoff(blk));
}

/* 写一个物理块 = 文件内 offset 起 4096B（越 size 的部分补 0） */
static void write_data_block(Img &im, uint64_t phys, const char *name, uint64_t foff, uint64_t size) {
    std::vector<uint8_t> b(BLK, 0);
    for (uint64_t j = 0; j < BLK; j++)
        if (foff + j < size) b[(size_t)j] = m41_fill_byte(name, foff + j);
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

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s <in_empty.img> <out.img> [expect.txt] [elfpath]\n", argv[0]);
        return 1;
    }
    const char *expect_path;
    std::string expect_buf;
    if (argc >= 4) expect_path = argv[3];
    else { expect_buf = std::string(argv[2]) + ".expect"; expect_path = expect_buf.c_str(); }
    const char *elf_path = (argc >= 5) ? argv[4] : nullptr;

    /* 复制空盘 → 副本（auditor 本身只读；mkstates 只在副本上写） */
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

    printf("mkstates: geo total_blocks=%llu inode_count=%llu root=%llu "
           "bitmap=[%llu,+%llu) inode_array=[%llu,+%llu) inode_bm=[%llu,+%llu)\n",
           (unsigned long long)im.total_blocks, (unsigned long long)im.inode_count,
           (unsigned long long)im.root_inode,
           (unsigned long long)im.bitmap_start, (unsigned long long)im.bitmap_blocks,
           (unsigned long long)im.inode_array_base, (unsigned long long)im.inode_blocks,
           (unsigned long long)im.inode_bm_base, (unsigned long long)im.inode_bm_blocks);

    /* ── 分配 inode ── */
    uint64_t i_inline  = im.alloc_inode();
    uint64_t i_i160    = im.alloc_inode();
    uint64_t i_d5k     = im.alloc_inode();
    uint64_t i_d13k    = im.alloc_inode();
    uint64_t i_e1      = im.alloc_inode();
    uint64_t i_ehole   = im.alloc_inode();
    uint64_t i_e2      = im.alloc_inode();
    uint64_t i_sub     = im.alloc_inode();
    uint64_t i_nest    = im.alloc_inode();
    uint64_t i_elf     = elf_path ? im.alloc_inode() : 0;

    /* ── 分配块 ── */
    uint64_t blk_sub  = im.alloc_run(2, 2);
    uint64_t blk_d5k  = im.alloc_run(2, 1);
    uint64_t blk_d13k = im.alloc_run(4, 1);
    uint64_t blk_e1n  = im.alloc_run(2, 2), blk_e1d  = im.alloc_run(2, 1);
    uint64_t blk_ehn  = im.alloc_run(2, 2), blk_eh1  = im.alloc_run(1, 1), blk_eh2 = im.alloc_run(3, 1);
    uint64_t blk_e2lA = im.alloc_run(2, 2), blk_e2lB = im.alloc_run(2, 2), blk_e2in = im.alloc_run(2, 2);
    uint64_t blk_e2d1 = im.alloc_run(2, 1), blk_e2d2 = im.alloc_run(2, 1);
    uint64_t blk_e2d3 = im.alloc_run(3, 1), blk_e2d4 = im.alloc_run(5, 1);

    /* 可选 ELF：extent 态（单条覆盖全文件），供 exec/mmap-exec 判据 */
    uint64_t blk_elfn = 0, blk_elfd = 0, elf_size = 0;
    if (elf_path) {
        struct stat es;
        if (stat(elf_path, &es) != 0 || es.st_size <= 0) {
            fprintf(stderr, "mkstates: 读 ELF %s 失败\n", elf_path); return 1;
        }
        elf_size = (uint64_t)es.st_size;
        uint64_t eblocks = (elf_size + BLK - 1) / BLK;
        blk_elfn = im.alloc_run(2, 2);
        blk_elfd = im.alloc_run(eblocks, 1);
    }

    printf("mkstates: inodes inline=%llu i160=%llu d5k=%llu d13k=%llu e1=%llu ehole=%llu e2=%llu sub=%llu nest=%llu\n",
           (unsigned long long)i_inline, (unsigned long long)i_i160, (unsigned long long)i_d5k,
           (unsigned long long)i_d13k, (unsigned long long)i_e1, (unsigned long long)i_ehole,
           (unsigned long long)i_e2, (unsigned long long)i_sub, (unsigned long long)i_nest);
    printf("mkstates: blocks sub=%llu d5k=%llu d13k=%llu | e1[n=%llu d=%llu] | hole[n=%llu d=%llu,%llu] | "
           "e2lvl[lA=%llu lB=%llu in=%llu d1=%llu d2=%llu d3=%llu d4=%llu]\n",
           (unsigned long long)blk_sub, (unsigned long long)blk_d5k, (unsigned long long)blk_d13k,
           (unsigned long long)blk_e1n, (unsigned long long)blk_e1d,
           (unsigned long long)blk_ehn, (unsigned long long)blk_eh1, (unsigned long long)blk_eh2,
           (unsigned long long)blk_e2lA, (unsigned long long)blk_e2lB, (unsigned long long)blk_e2in,
           (unsigned long long)blk_e2d1, (unsigned long long)blk_e2d2,
           (unsigned long long)blk_e2d3, (unsigned long long)blk_e2d4);

    std::vector<M41File> files;

    /* ── A. inline.bin（REG inline, size=100）── */
    {
        const char *nm = "inline.bin"; uint64_t sz = 100;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        for (uint64_t i = 0; i < sz; i++) ino.inline_data[i] = m41_fill_byte(nm, i);
        im.write_inode(i_inline, &ino, 256);
        M41File f{nm, i_inline, sz, {{0, sz, 'D'}}, {}};
        files.push_back(f);
    }
    /* ── B. inline160.bin（size=160，内联上界；判据 size≤160）── */
    {
        const char *nm = "inline160.bin"; uint64_t sz = 160;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        for (uint64_t i = 0; i < sz; i++) ino.inline_data[i] = m41_fill_byte(nm, i);
        im.write_inode(i_i160, &ino, 256);
        files.push_back(M41File{nm, i_i160, sz, {{0, sz, 'D'}}, {}});
    }
    /* ── C. direct5k.bin（direct, size=5000 >4K=2 块）── */
    {
        const char *nm = "direct5k.bin"; uint64_t sz = 5000;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 1, sz, blk_d5k, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_d5k, &ino, 256);
        for (uint64_t b = 0; b * BLK < sz; b++) write_data_block(im, blk_d5k + b, nm, b * BLK, sz);
        files.push_back(M41File{nm, i_d5k, sz, {{0, sz, 'D'}}, {}});
    }
    /* ── D. direct13k.bin（direct, size=12345 → 4 块，跨页/跨块）── */
    {
        const char *nm = "direct13k.bin"; uint64_t sz = 12345;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 1, sz, blk_d13k, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_d13k, &ino, 256);
        for (uint64_t b = 0; b * BLK < sz; b++) write_data_block(im, blk_d13k + b, nm, b * BLK, sz);
        files.push_back(M41File{nm, i_d13k, sz, {{0, sz, 'D'}}, {}});
    }
    /* ── E. extent1.bin（extent, size=5000，单条覆盖 [0,2)，无洞）── */
    {
        const char *nm = "extent1.bin"; uint64_t sz = 5000;
        std::vector<ExtP> ex = {{0, 2, blk_e1d}};
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, blk_e1n, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_e1, &ino, 256);
        write_extent_leaf(im, blk_e1n, i_e1, 0, true, ex);
        for (auto &e : ex) for (uint64_t k = 0; k < e.len; k++)
            write_data_block(im, e.phys + k, nm, (e.fblk + k) * BLK, sz);
        files.push_back(M41File{nm, i_e1, sz, extent_segs(ex, sz), {}});
    }
    /* ── F. extent_hole.bin（extent, size=32768=8 块；extents [0,1)+[5,8)，洞 [1,5) 读 0）── */
    {
        const char *nm = "extent_hole.bin"; uint64_t sz = 32768;
        std::vector<ExtP> ex = {{0, 1, blk_eh1}, {5, 3, blk_eh2}};
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, blk_ehn, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_ehole, &ino, 256);
        write_extent_leaf(im, blk_ehn, i_ehole, 0, true, ex);
        for (auto &e : ex) for (uint64_t k = 0; k < e.len; k++)
            write_data_block(im, e.phys + k, nm, (e.fblk + k) * BLK, sz);
        files.push_back(M41File{nm, i_ehole, sz, extent_segs(ex, sz), {}});
    }
    /* ── G. extent_2lvl.bin（2 级树：1 内节点 + 2 子叶；size=163840=40 块）── */
    {
        const char *nm = "extent_2lvl.bin"; uint64_t sz = 163840;
        std::vector<ExtP> lA = {{0, 2, blk_e2d1}, {4, 2, blk_e2d2}};   /* max open = 6  */
        std::vector<ExtP> lB = {{10, 3, blk_e2d3}, {20, 5, blk_e2d4}}; /* max open = 25 */
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, blk_e2in, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_e2, &ino, 256);
        write_extent_leaf(im, blk_e2lA, i_e2, blk_e2in, false, lA);
        write_extent_leaf(im, blk_e2lB, i_e2, blk_e2in, false, lB);
        write_extent_inner(im, blk_e2in, i_e2, {{6, blk_e2lA}, {25, blk_e2lB}});
        for (auto *lv : {&lA, &lB}) for (auto &e : *lv) for (uint64_t k = 0; k < e.len; k++)
            write_data_block(im, e.phys + k, nm, (e.fblk + k) * BLK, sz);
        std::vector<ExtP> all = lA; all.insert(all.end(), lB.begin(), lB.end());
        files.push_back(M41File{nm, i_e2, sz, extent_segs(all, sz), {}});
    }
    /* ── H. nested.bin（sub/ 内 REG inline, size=7）── */
    {
        const char *full = "sub/nested.bin"; uint64_t sz = 7;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, PTR_INV, 1, 0600, RFS_XATTR_NONE);
        for (uint64_t i = 0; i < sz; i++) ino.inline_data[i] = m41_fill_byte(full, i);
        im.write_inode(i_nest, &ino, 256);
        files.push_back(M41File{full, i_nest, sz, {{0, sz, 'D'}}, {}});
    }
    /* ── I.（可选）elfexec.bin：真实 ELF 原样嵌入，extent 单条；供 exec / mmap(PROT_EXEC) ── */
    if (elf_path) {
        const char *nm = "elfexec.bin";
        uint64_t eblocks = (elf_size + BLK - 1) / BLK;
        std::vector<uint8_t> raw((size_t)elf_size, 0);
        {
            FILE *fe = fopen(elf_path, "rb");
            if (!fe || fread(raw.data(), 1, raw.size(), fe) != raw.size()) {
                fprintf(stderr, "mkstates: 读 ELF 内容失败 %s\n", elf_path); return 1;
            }
            fclose(fe);
        }
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, elf_size, blk_elfn, 1, 0755, RFS_XATTR_NONE);
        im.write_inode(i_elf, &ino, 256);
        std::vector<ExtP> ex = {{0, eblocks, blk_elfd}};
        write_extent_leaf(im, blk_elfn, i_elf, 0, true, ex);
        for (uint64_t b = 0; b < eblocks; b++) {
            std::vector<uint8_t> bb(BLK, 0);
            uint64_t n = std::min(BLK, elf_size - b * BLK);
            memcpy(bb.data(), raw.data() + b * BLK, (size_t)n);
            im.wr(bb.data(), bb.size(), blkoff(blk_elfd + b));
        }
        M41File f; f.name = nm; f.inode = i_elf; f.size = elf_size; f.src = elf_path;
        files.push_back(f);
        printf("mkstates: elf %s size=%llu blocks=%llu node=%llu data=%llu i_elf=%llu\n",
               elf_path, (unsigned long long)elf_size, (unsigned long long)eblocks,
               (unsigned long long)blk_elfn, (unsigned long long)blk_elfd, (unsigned long long)i_elf);
    }

    /* ── root 目录条目 ── */
    std::vector<Ent> root_ents = {
        {"inline.bin", i_inline}, {"inline160.bin", i_i160}, {"direct5k.bin", i_d5k},
        {"direct13k.bin", i_d13k}, {"extent1.bin", i_e1}, {"extent_hole.bin", i_ehole},
        {"extent_2lvl.bin", i_e2}, {"sub", i_sub}
    };
    if (elf_path) root_ents.push_back({"elfexec.bin", i_elf});
    sort_ents(root_ents);
    std::vector<uint8_t> root_node;
    build_dentry_node(root_node, im.root_inode, im.root_inode, root_ents);
    im.wr(root_node.data(), root_node.size(), blkoff(0));

    /* ── sub 目录条目 ── */
    std::vector<Ent> sub_ents = {{"nested.bin", i_nest}};
    sort_ents(sub_ents);
    std::vector<uint8_t> sub_node;
    build_dentry_node(sub_node, i_sub, im.root_inode, sub_ents);
    im.wr(sub_node.data(), sub_node.size(), blkoff(blk_sub));

    /* ── root inode（改写 size/link_count/heaps）── */
    {
        raven_dir_Inode_t d;
        im.rd(&d, 256, blkoff(im.inode_array_base) + im.root_inode * 256);
        d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
        d.size       = root_ents.size();
        d.link_count = (uint32_t)(root_ents.size() + 2);
        d.data_root  = 0;
        d.mode_xattr = RFS_MODE_XATTR_PACK(0755, RFS_XATTR_NONE);
        set_dir_heap_inactive(&d);
        im.wr(&d, 256, blkoff(im.inode_array_base) + im.root_inode * 256);
    }
    /* ── sub inode ── */
    {
        raven_dir_Inode_t d;
        memset(&d, 0, 256);
        d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
        d.link_count = (uint32_t)(sub_ents.size() + 2);
        d.mode_xattr = RFS_MODE_XATTR_PACK(0750, RFS_XATTR_NONE);
        d.size       = sub_ents.size();
        d.data_root  = blk_sub;
        set_dir_heap_inactive(&d);
        im.write_inode(i_sub, &d, 256);
    }

    /* ── 落盘位图 ── */
    im.wr(im.bm.data(),  im.bm.size(),  blkoff(im.bitmap_start));
    im.wr(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    /* ── 期望清单 ── */
    if (!m41_write_expect(expect_path, files)) {
        fprintf(stderr, "mkstates: 写 expect 失败 %s\n", expect_path); return 1;
    }
    printf("mkstates: expect → %s（%zu 文件）\n", expect_path, files.size());
    for (const M41File &f : files)
        printf("  %-16s inode=%-3llu size=%-7llu segs=%zu xxh64=%016llx\n",
               f.name.c_str(), (unsigned long long)f.inode, (unsigned long long)f.size,
               f.segs.size(), (unsigned long long)m41_hash(f));

    close(im.fd);
    printf("mkstates: done → %s\n", argv[2]);
    return 0;
}
