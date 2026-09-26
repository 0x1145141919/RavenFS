/* ═══════════════════════════════════════════════════════════════════════════
 * mkbigdir.cpp — 「大目录」靶子构造器（写**副本**；auditor 自身永不写盘）
 *
 * 用途：把 mkfs 造出的空盘副本填成一个**大目录**，供 audit_v1.2 递归校验，
 *       也用 dents_dump 在 guest 里压测 getdents64 续读。
 *
 * 目标结构：
 *   root/ 含 N 个 REG 文件（f%05d.dat，内联）+ 1 个 sub/ 子目录
 *   sub/  含 3 个 REG 文件（内联）
 *   dentry 树**多叶**：按 (XXH64(seed=0)(name), name) 排序后按 ≤126 切叶；
 *   叶建成**双向链**：非根叶 ptrs[1]=前驱块号 / ptrs[2]=后继块号（首尾 = 52 位全 1）；
 *   多叶时根为**根内部**：parent_ptr.is_root=1/is_inner=1/ptr=root inode，
 *   ptrs[1]='..' inode，hints[i]={sub_ptr=子块号, hash_max=子树最大 hash} 严格递增；
 *   叶数 >126 时中间再插一层非根内部（多层）。
 *   所有条目分配 REG inode（内联小内容）、点亮 inode bitmap；
 *   所有节点（8K=2 块，偶对齐）点亮 block bitmap；
 *   同步 root inode 的 size(=条目数) / link_count(=size+2) / data_root。
 *
 * 用法： ./mkbigdir <in_empty.img> <out.img> [N]      （N 默认 5000）
 *   in_empty.img 由 `mkfs.ravenfs --inode-count=$((2*N+256))` 造出（1G 文件即可）。
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <disk_struct_v1.2.h>
#include <magics.h>
#include <xxhash.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static constexpr uint64_t BLK   = 4096;
static constexpr uint64_t NODE  = 8192;
static constexpr uint64_t PTR52 = (1ULL << 52) - 1;
static constexpr uint64_t PTR_INV = PTR_INVALID;
static constexpr size_t   LEAF_CAP = 126;      /* dentry 树阶数（count <= 126） */

static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }
static inline uint64_t H(const std::string &s) { return (uint64_t)XXH64(s.data(), s.size(), 0); }

/* ─────────────────────────── 镜像 I/O ─────────────────────────── */
struct Img {
    int fd = -1;
    uint64_t total_blocks = 0, inode_count = 0, root_inode = 0;
    uint64_t bitmap_blocks = 0, bitmap_start = 0;
    uint64_t inode_blocks = 0, inode_array_base = 0;
    uint64_t inode_bm_blocks = 0, inode_bm_base = 0;
    std::vector<uint8_t> bm;    /* block bitmap */
    std::vector<uint8_t> ibm;   /* inode bitmap */

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

    /* 偶对齐 2 连续块（8K 节点） */
    uint64_t alloc_pair_aligned() {
        for (uint64_t b = 2; b + 2 <= total_blocks; b += 2) {
            if (!blk_lit(b) && !blk_lit(b + 1)) { mark_blk(b); mark_blk(b + 1); return b; }
        }
        fprintf(stderr, "mkbigdir: no free aligned 8K pair\n");
        exit(1);
    }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkbigdir: no free inode\n");
        exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

struct Ent { std::string name; uint64_t inode; };

static void sort_ents(std::vector<Ent> &v) {
    std::sort(v.begin(), v.end(), [](const Ent &a, const Ent &b) {
        uint64_t ha = H(a.name), hb = H(b.name);
        if (ha != hb) return ha < hb;
        return a.name < b.name;
    });
}

/* 一个 dentry 树节点（叶或内部）的构造计划 */
struct NodePlan {
    uint64_t blk = 0;
    bool     is_leaf = false;
    bool     is_root = false;
    uint64_t owner = 0;                 /* dir inode */
    size_t   begin = 0, end = 0;        /* 叶：条目区间 */
    std::vector<std::pair<uint64_t, uint64_t>> children;  /* 内部：(子块, 子树 max_hash) */
};

/* 构造并落盘一个目录的 dentry 树；返回树根块号。
 *   reuse0=true  → 树根复用块 0（mkfs 已分配的 root dentry 块，避免孤儿块） */
static uint64_t build_dir_tree(Img &im, uint64_t owner, uint64_t dotdot,
                               const std::vector<Ent> &ents, bool reuse0)
{
    std::vector<NodePlan> nodes;
    const size_t n = ents.size();
    const size_t nl = (n == 0) ? 1 : ((n + LEAF_CAP - 1) / LEAF_CAP);
    const bool root_is_leaf = (nl == 1);

    /* 叶 */
    for (size_t k = 0; k < nl; k++) {
        size_t b = k * LEAF_CAP;
        size_t e = std::min(b + LEAF_CAP, n);
        NodePlan nd;
        nd.is_leaf = true;
        nd.is_root = root_is_leaf;
        nd.owner = owner;
        nd.begin = b; nd.end = e;
        nd.blk = root_is_leaf ? (reuse0 ? 0 : im.alloc_pair_aligned())
                              : im.alloc_pair_aligned();
        nodes.push_back(nd);
    }

    uint64_t root_blk = 0;
    if (root_is_leaf) {
        root_blk = nodes[0].blk;
    } else {
        std::vector<std::pair<uint64_t, uint64_t>> level;   /* (块, max_hash) */
        for (const NodePlan &L : nodes) {
            uint64_t mh = (L.end > L.begin) ? H(ents[L.end - 1].name) : 0;
            level.push_back({L.blk, mh});
        }
        for (;;) {
            const bool make_root = (level.size() <= LEAF_CAP);
            std::vector<std::pair<uint64_t, uint64_t>> parents;
            for (size_t i = 0; i < level.size(); i += LEAF_CAP) {
                size_t j = std::min(i + LEAF_CAP, level.size());
                NodePlan nd;
                nd.is_leaf = false;
                nd.is_root = make_root;
                nd.owner = owner;
                nd.blk = make_root ? (reuse0 ? 0 : im.alloc_pair_aligned())
                                   : im.alloc_pair_aligned();
                for (size_t t = i; t < j; t++) nd.children.push_back(level[t]);
                parents.push_back({nd.blk, level[j - 1].second});
                if (make_root) root_blk = nd.blk;
                nodes.push_back(nd);
            }
            level = parents;
            if (make_root) break;
        }
    }

    /* 父块映射（叶/内部统一） */
    std::unordered_map<uint64_t, uint64_t> parent;
    for (const NodePlan &nd : nodes)
        if (!nd.is_leaf)
            for (const auto &c : nd.children) parent[c.first] = nd.blk;

    /* 落盘 */
    for (size_t t = 0; t < nodes.size(); t++) {
        const NodePlan &nd = nodes[t];
        std::vector<uint8_t> buf(NODE, 0);
        auto *meta = (Bptree_meta_t *)buf.data();
        meta->magics[0] = DENTRY_MAGIC1; meta->magics[1] = DENTRY_MAGIC2;
        meta->magics[2] = DENTRY_MAGIC3; meta->magics[3] = DENTRY_MAGIC4;

        uint64_t pptr = nd.is_root ? nd.owner : parent[nd.blk];

        if (nd.is_leaf) {
            uint32_t cnt = (uint32_t)(nd.end - nd.begin);
            meta->parent_ptr.v = RFS_PP_PACK(cnt, 0, nd.is_root ? 1 : 0, pptr);
            meta->ptrs[0].v = RFS_PTR_PACK(0, PTR52);
            if (nd.is_root) {
                meta->ptrs[1].v = RFS_PTR_PACK(0, dotdot);      /* '..' inode */
                meta->ptrs[2].v = RFS_PTR_PACK(0, PTR52);
            } else {
                uint64_t prev = (t > 0) ? nodes[t - 1].blk : PTR52;
                uint64_t next = (t + 1 < nl) ? nodes[t + 1].blk : PTR52;
                meta->ptrs[1].v = RFS_PTR_PACK(0, prev);
                meta->ptrs[2].v = RFS_PTR_PACK(0, next);
            }
            auto *leaf = (Bptree_dentry_leaf_node_t *)buf.data();
            for (size_t i = nd.begin; i < nd.end; i++) {
                const Ent &e = ents[i];
                Dentry_t de;
                memset(&de, 0, sizeof(de));
                de.inode_ptr.v = RFS_PTR_PACK((uint16_t)e.name.size(), e.inode);
                de.hash = H(e.name);
                de.full_char_remain_ptr = PTR_INV;   /* 全部 ≤40B 内联 */
                memcpy(de.name, e.name.data(), e.name.size());
                memcpy(&leaf->entries[i - nd.begin], &de, sizeof(de));
            }
        } else {
            uint32_t cnt = (uint32_t)nd.children.size();
            meta->parent_ptr.v = RFS_PP_PACK(cnt, 1, nd.is_root ? 1 : 0, pptr);
            meta->ptrs[0].v = RFS_PTR_PACK(0, PTR52);
            meta->ptrs[1].v = nd.is_root ? RFS_PTR_PACK(0, dotdot) : RFS_PTR_PACK(0, PTR52);
            meta->ptrs[2].v = RFS_PTR_PACK(0, PTR52);
            auto *inner = (Bptree_dentry_inner_node_t *)buf.data();
            for (size_t i = 0; i < nd.children.size(); i++) {
                inner->hints[i].sub_ptr.v = RFS_PTR_PACK(0, nd.children[i].first);
                inner->hints[i].hash_max   = nd.children[i].second;
            }
        }
        im.wr(buf.data(), buf.size(), blkoff(nd.blk));
    }

    size_t leaves = 0, inners = 0;
    for (const NodePlan &nd : nodes) { if (nd.is_leaf) leaves++; else inners++; }
    printf("  tree owner=%llu dotdot=%llu root_blk=%llu entries=%zu leaves=%zu inners=%zu\n",
           (unsigned long long)owner, (unsigned long long)dotdot,
           (unsigned long long)root_blk, n, leaves, inners);
    return root_blk;
}

static void set_reg_inline(raven_Inode_t *ino, uint64_t size, const char *data, uint64_t mode) {
    memset(ino, 0, 256);
    ino->word1      = RFS_INO1_PACK(INODE_TYPE_REGULAR, 0, 0);
    ino->link_count = 1;
    ino->mode_xattr = RFS_MODE_XATTR_PACK(mode, RFS_XATTR_NONE);
    ino->size       = size;
    ino->data_root  = PTR_INV;
    memcpy(ino->inline_data, data, size);
}

static void set_dir(raven_dir_Inode_t *d, uint64_t size, uint64_t data_root, uint64_t mode) {
    memset(d, 0, 256);
    d->word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
    d->link_count = (uint32_t)(size + 2);
    d->mode_xattr = RFS_MODE_XATTR_PACK(mode, RFS_XATTR_NONE);
    d->size       = size;
    d->data_root  = data_root;
    for (int i = 0; i < 10; i++)
        RFS_NHC_SET(d->heaps[i], 0, 0, NAME_HEAP_INACTIVE, 0, NAME_HEAP_INACTIVE);
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <in_empty.img> <out.img> [N]\n", argv[0]);
        return 1;
    }
    uint64_t N = (argc == 4) ? strtoull(argv[3], nullptr, 0) : 5000;

    /* ── 复制 ── */
    {
        FILE *fi = fopen(argv[1], "rb");
        if (!fi) { perror("open in"); return 1; }
        FILE *fo = fopen(argv[2], "wb");
        if (!fo) { perror("open out"); return 1; }
        char buf[1 << 20]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) fwrite(buf, 1, n, fo);
        fclose(fi); fclose(fo);
    }

    Img im;
    im.fd = open(argv[2], O_RDWR);
    if (im.fd < 0) { perror("open out O_RDWR"); return 1; }

    RavenSuperblock sb;
    if (!im.rd(&sb, sizeof(sb), 0)) { fprintf(stderr, "read sb failed\n"); return 1; }
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

    printf("mkbigdir: N=%llu  total_blocks=%llu inode_count=%llu root_inode=%llu\n",
           (unsigned long long)N, (unsigned long long)im.total_blocks,
           (unsigned long long)im.inode_count, (unsigned long long)im.root_inode);

    /* ── inode 分配 ── */
    uint64_t i_sub = im.alloc_inode();
    std::vector<uint64_t> file_ino(N);
    for (uint64_t i = 0; i < N; i++) file_ino[i] = im.alloc_inode();
    uint64_t i_a = im.alloc_inode(), i_b = im.alloc_inode(), i_c = im.alloc_inode();

    /* ── root 条目 ── */
    std::vector<Ent> root_ents;
    root_ents.reserve(N + 1);
    for (uint64_t i = 0; i < N; i++) {
        char nm[48];
        snprintf(nm, sizeof(nm), "f%05llu.dat", (unsigned long long)i);
        root_ents.push_back({nm, file_ino[i]});
    }
    root_ents.push_back({"sub", i_sub});
    sort_ents(root_ents);

    /* ── sub 条目 ── */
    std::vector<Ent> sub_ents = { {"a.txt", i_a}, {"b.txt", i_b}, {"c.txt", i_c} };
    sort_ents(sub_ents);

    /* ── 建树：root 复用块 0；sub 新分配 ── */
    uint64_t root_blk = build_dir_tree(im, im.root_inode, im.root_inode, root_ents, true);
    uint64_t sub_blk  = build_dir_tree(im, i_sub, im.root_inode, sub_ents, false);

    /* ── 写 root inode ── */
    {
        raven_dir_Inode_t d;
        set_dir(&d, root_ents.size(), root_blk, 0755);
        im.write_inode(im.root_inode, &d, 256);
    }
    /* ── 写 sub inode ── */
    {
        raven_dir_Inode_t d;
        set_dir(&d, sub_ents.size(), sub_blk, 0750);
        im.write_inode(i_sub, &d, 256);
    }
    /* ── 写文件 inodes（内联小内容）── */
    for (uint64_t i = 0; i < N; i++) {
        char data[16];
        snprintf(data, sizeof(data), "blob%05llu", (unsigned long long)(i % 100000));
        raven_Inode_t ino;
        set_reg_inline(&ino, strlen(data), data, 0644);
        im.write_inode(file_ino[i], &ino, 256);
    }
    {
        raven_Inode_t ino;
        set_reg_inline(&ino, 3, "aaa", 0644); im.write_inode(i_a, &ino, 256);
        set_reg_inline(&ino, 3, "bbb", 0644); im.write_inode(i_b, &ino, 256);
        set_reg_inline(&ino, 3, "ccc", 0644); im.write_inode(i_c, &ino, 256);
    }

    /* ── 落盘位图 ── */
    im.wr(im.bm.data(), im.bm.size(), blkoff(im.bitmap_start));
    im.wr(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    close(im.fd);
    printf("mkbigdir: done → %s  (root entries=%zu, sub entries=%zu)\n",
           argv[2], root_ents.size(), sub_ents.size());
    return 0;
}
