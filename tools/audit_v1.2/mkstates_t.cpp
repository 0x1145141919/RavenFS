/* ═══════════════════════════════════════════════════════════════════════════
 * mkstates_t.cpp — M4.3（truncate / punch_hole / fallocate）**靶子/初始镜像构造器**
 *                  （mkstates_w.cpp 的姊妹件；写副本；不改 M4.1/M4.2 任何资产）
 *
 * 目的：造一个「**可被 ko 做 truncate/punch/fallocate**」的 v1.2 镜像 + 四份配套清单：
 *   · <out.img>          初始镜像（可挂载可写）
 *   · <out.img>.expect   初始内容清单（M4.1 格式；供 `audit_v1.2` + `readback verify` 自证）
 *   · <out.img>.wexpect  操作后内容清单（M4.1 格式；`readback dump` 产 ground-truth，guest cmp）
 *   · <out.img>.plan     操作序列 + 期望 size / i_blocks / 终态形态（见下）
 *
 * 初始靶子（覆盖 M4.3 判据：punch / truncate / fallocate 各自往返 + 形态×操作矩阵 + 三态迁移）：
 *
 *   ── 多级 extent 树（M4.2 欠的压力：>337 条强制叶裂成多叶；>168 触发欠载/平衡）──
 *   deep.bin      extent, 505 条单块记录 → 根内 + 3 叶（168/168/169），只读锚点（i_blocks 可定）
 *   deep_p.bin    extent, 同上；punch 叶 0 首块 → 该叶 167 条 <168 触发欠载/合并链
 *
 *   ── punch_hole（FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE）──
 *   p_al.bin      extent 16K（单条 [0,4)）  → punch 对齐 [4K,8K)
 *   p_ua.bin      extent 16K（单条 [0,4)）  → punch 非对齐 [100,5000)（头/尾残块就地清零）
 *   p_eof.bin     extent 16K（单条 [0,4)）  → punch [9000,20000) 跨 EOF（收口 round_up(size)）
 *   p_sb.bin      extent 4K（单条 [0,1)）   → punch 单块内 [300,600)（零释放）
 *   p_whole.bin   extent 8K（单条 [0,2)）   → punch 整块域 [0,8K)（空根叶终态）
 *
 *   ── truncate ──
 *   t_inl.bin     extent 8K（单条 [0,2)）   → truncate 到 100（≤160 **强制回迁 inline**）
 *   t_dir.bin     direct 16K（4 块连续）    → truncate 到 8192（**direct 缩 run**，态不变）
 *   t_ext.bin     extent 16K（单条 [0,4)）  → truncate 到 8192（单条覆盖 → **坍缩 direct**）
 *   t_grow.bin    inline 100（fill）        → truncate 增大到 300（**跨 160 升格 extent**）
 *
 *   ── fallocate ──
 *   f_plain.bin   inline 0                  → fallocate(flags==0,0,8192)：抬 size（普通预分配）
 *   f_keep.bin    inline 100（fill）        → fallocate(KEEP_SIZE,0,4096)：**期望 -EOPNOTSUPP**
 *   f_punch.bin   extent 8K（单条 [0,2)）   → fallocate(PUNCH_HOLE|KEEP_SIZE,0,4096)
 *   f_zero.bin    extent 8K（单条 [0,2)）   → fallocate(ZERO_RANGE,4096,4096)：就地清零、size 不变
 *
 * plan 语法（纯文本；`#` 注释；名不含空白）：
 *   A <name> <mode> <off> <len> <err>   fallocate；mode∈{0,KEEP,PUNCH,ZERO}；err = 0 / 95(EOPNOTSUPP)
 *   T <name> <len_new> <err>            ftruncate；err = 0
 *   B <name> <st512>                    操作后期望 i_blocks（512B 单位；缺省 = 不断言）
 *   S <name> <size>                     操作后期望 size（字节）
 *   X <name> <inline|direct|extent>     操作后期望终态形态（缺省 = any）
 *
 * 真值来源：include/disk_struct_v1.2.h、magics.h、Docs/disk_layout_v1.1.md §5/§10、
 *           Docs/linux/ravenfs-linux-ko-M4-data-spec.md §3/§8/§11（含 D-M4-8/11/14）、
 *           Docs/punch_truncate_pread_spec.md §5/§6/§8（punch/truncate 契约 + 形态×操作矩阵）。
 * 纪律：**不改** mkstates.cpp / mkstates_w.cpp / readback.cpp / m41_expect.h / m42_fill.h；
 *       只在副本上写（auditor 本身只读）。
 *
 * 用法： ./mkstates_t <in_empty.img> <out.img> [prefix]
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
#include <utility>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static constexpr uint64_t BLK    = 4096;
static constexpr uint64_t NODE   = 8192;
static constexpr uint64_t PTR52  = (1ULL << 52) - 1;
static constexpr uint64_t PTR_INV = PTR_INVALID;

static constexpr uint64_t DEEP_N   = 505;                 /* deep 记录数：>337 → 强制多叶 */
static constexpr uint64_t DEEP_PHYS = DEEP_N;             /* 每记录 1 块 */

static constexpr int ERR_OK  = 0;
static constexpr int ERR_NOSUP = 95;                      /* EOPNOTSUPP（D-M4-11：拒 KEEP_SIZE 纯预留） */

static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

/* ── 两版 fill 公式一致性交叉校验（防 m42_fill.h 与 m41_expect.h 漂移）── */
static void assert_fill_consistent() {
    const char *names[] = { "", "a", "deep.bin", "p_ua.bin", "t_grow.bin", "x/y.z" };
    for (const char *nm : names)
        for (uint64_t off = 0; off < 5000; off += 97)
            if (m42_fill_byte(nm, off) != m41_fill_byte(nm, off)) {
                fprintf(stderr, "FATAL: m42_fill_byte != m41_fill_byte (name=%s off=%llu)\n",
                        nm, (unsigned long long)off);
                exit(2);
            }
}

/* ═══════════════════════════ 镜像读/写器（照抄 mkstates_w） ═══════════════════════════ */
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
        fprintf(stderr, "mkstates_t: no free %llu contiguous blocks (align=%llu)\n",
                (unsigned long long)n, (unsigned long long)align);
        exit(1);
    }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkstates_t: no free inode\n"); exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

/* ── 目录项（与 mkcontent/mkstates/mkstates_w 同构）── */
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

/* ── extent 节点写入 ── */
struct ExtRec { uint64_t fblk, len, phys; };

static void write_extent_leaf(Img &im, uint64_t blk, uint64_t owner, bool is_root,
                              uint64_t parent, const std::vector<ExtRec> &ex) {
    std::vector<uint8_t> node(NODE, 0);
    auto *meta = (Bptree_meta_t *)node.data();
    meta->magics[0] = BPTREE_MAGIC1; meta->magics[1] = BPTREE_MAGIC2;
    meta->magics[2] = BPTREE_MAGIC3; meta->magics[3] = BPTREE_MAGIC4;
    meta->parent_ptr.v = RFS_PP_PACK(ex.size(), 0 /*leaf*/, is_root ? 1 : 0,
                                     is_root ? owner : parent);
    for (int i = 0; i < 3; i++) meta->ptrs[i].v = RFS_PTR_PACK(0, PTR52);
    auto *leaf = (Bptree_extent_leaf_node_t *)node.data();
    for (size_t i = 0; i < ex.size(); i++) {
        leaf->extents[i].length       = ex[i].len;
        leaf->extents[i].fblkbase     = ex[i].fblk;
        leaf->extents[i].block_base.v = RFS_PTR_PACK(0, ex[i].phys);
    }
    im.wr(node.data(), node.size(), blkoff(blk));
}

static void write_extent_inner(Img &im, uint64_t blk, uint64_t owner, bool is_root,
                               uint64_t parent, const std::vector<std::pair<uint64_t,uint64_t>> &kids) {
    std::vector<uint8_t> node(NODE, 0);
    auto *meta = (Bptree_meta_t *)node.data();
    meta->magics[0] = BPTREE_MAGIC1; meta->magics[1] = BPTREE_MAGIC2;
    meta->magics[2] = BPTREE_MAGIC3; meta->magics[3] = BPTREE_MAGIC4;
    meta->parent_ptr.v = RFS_PP_PACK(kids.size(), 1 /*inner*/, is_root ? 1 : 0,
                                     is_root ? owner : parent);
    for (int i = 0; i < 3; i++) meta->ptrs[i].v = RFS_PTR_PACK(0, PTR52);
    auto *nd = (Bptree_extent_inner_node_t *)node.data();
    for (size_t i = 0; i < kids.size(); i++) {
        nd->extents[i].key = kids[i].first;
        nd->extents[i].sub_node_ptr.v = RFS_PTR_PACK(0, kids[i].second);
    }
    im.wr(node.data(), node.size(), blkoff(blk));
}

/* ── 目标描述 ── */
enum Form { F_INLINE = 0, F_DIRECT = 1, F_EXTENT = 2 };

/* mode: 0=flags0 1=KEEP_SIZE 2=PUNCH_HOLE|KEEP_SIZE 3=ZERO_RANGE */
struct Op {
    char     kind;      /* 'T' truncate | 'A' fallocate */
    int      mode;      /* 仅 'A' */
    uint64_t a, b;      /* 'T': a=len_new; 'A': a=off, b=len */
    int      err;       /* 期望 errno：0 成功；95 EOPNOTSUPP */
};

struct Tgt {
    std::string name;
    uint64_t    inode = 0;
    Form        form  = F_INLINE;
    uint64_t    size  = 0;
    uint64_t    direct_base = 0, direct_blocks = 0;      /* F_DIRECT */
    std::vector<ExtRec>  recs;                            /* F_EXTENT */
    std::vector<int>     leaf_split;                      /* 空 = 单叶根；否则多叶分片 */
    uint64_t    root = 0;                                 /* F_EXTENT 根块（构树后回填） */
    std::vector<uint8_t> prov;                            /* size 字节；1=填充字节 0=零（洞/未写） */
    std::vector<Op>      ops;
    bool        has_B = false;
    uint64_t    st512 = 0;
    std::string xstate;                                   /* ""=any */
};

/* 把 [0,size) 的 prov 段（D/Z 无缝覆盖）导出 */
static std::vector<M41Seg> prov_segs(const std::vector<uint8_t> &prov) {
    std::vector<M41Seg> segs;
    size_t i = 0, n = prov.size();
    while (i < n) {
        uint8_t k = prov[i]; size_t j = i;
        while (j < n && prov[j] == k) j++;
        segs.push_back({(uint64_t)i, (uint64_t)(j - i), k ? 'D' : 'Z'});
        i = j;
    }
    return segs;
}

/* 边界上的操作语义模拟（host 侧真值；仅作用于 prov/size 影子） */
static void op_apply(std::vector<uint8_t> &prov, uint64_t &size, const Op &o) {
    if (o.err != ERR_OK) return;                          /* 期望失败 → 盘面零变化 */
    if (o.kind == 'T') {
        uint64_t nn = o.a;
        prov.resize((size_t)nn, 0);                       /* 缩：截断；增：纯零推进（§6.5）*/
        size = nn;
        return;
    }
    /* 'A' */
    if (o.mode == 1) return;                              /* KEEP_SIZE 纯预留：已假定失败 */
    if (o.mode == 2) {                                    /* PUNCH_HOLE|KEEP_SIZE */
        if (o.a >= size) return;                          /* offset ≥ size：no-op */
        uint64_t end = o.a + o.b;
        uint64_t ru  = ((size + BLK - 1) / BLK) * BLK;    /* clamp 到 round_up(size) */
        if (end > ru)  end = ru;
        if (end > size) end = size;
        for (uint64_t i = o.a; i < end; i++) prov[(size_t)i] = 0;
        return;
    }
    if (o.mode == 3) {                                    /* ZERO_RANGE */
        uint64_t end = o.a + o.b;
        uint64_t ns  = std::max(size, end);               /* 无 KEEP_SIZE：可越过 EOF 抬 size */
        prov.resize((size_t)ns, 0);
        size = ns;
        uint64_t z1 = std::min(end, size);
        for (uint64_t i = o.a; i < z1; i++) prov[(size_t)i] = 0;
        return;
    }
    /* mode 0：flags==0 普通预分配；已存在区不改，新区按零（新分配块零垫底）*/
    uint64_t end = o.a + o.b;
    uint64_t ns  = std::max(size, end);
    prov.resize((size_t)ns, 0);
    size = ns;
}

static const char *mode_str(int m) {
    switch (m) { case 0: return "0"; case 1: return "KEEP"; case 2: return "PUNCH"; case 3: return "ZERO"; }
    return "?";
}

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

    /* 复制空盘 → 副本 */
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

    printf("mkstates_t: geo total_blocks=%llu inode_count=%llu root=%llu "
           "bitmap=[%llu,+%llu) inode_array=[%llu,+%llu) inode_bm=[%llu,+%llu)\n",
           (unsigned long long)im.total_blocks, (unsigned long long)im.inode_count,
           (unsigned long long)im.root_inode,
           (unsigned long long)im.bitmap_start, (unsigned long long)im.bitmap_blocks,
           (unsigned long long)im.inode_array_base, (unsigned long long)im.inode_blocks,
           (unsigned long long)im.inode_bm_base, (unsigned long long)im.inode_bm_blocks);

    /* ── 数据块写入（按 prov；越 size 部分补 0）── */
    auto write_block = [&](uint64_t phys, const char *nm, uint64_t foff,
                           const std::vector<uint8_t> &prov, uint64_t size) {
        std::vector<uint8_t> b(BLK, 0);
        for (uint64_t j = 0; j < BLK; j++)
            if (foff + j < size && prov[(size_t)(foff + j)]) b[(size_t)j] = m42_fill_byte(nm, foff + j);
        im.wr(b.data(), b.size(), blkoff(phys));
    };

    std::vector<Tgt> T;
    auto add = [&](const char *nm) -> Tgt & {
        Tgt t; t.name = nm; t.inode = im.alloc_inode();
        T.push_back(std::move(t));
        return T.back();
    };
    /* 顺序记录为整文件分配一段连续物理区；记录内物理连续 */
    auto place_records = [&](Tgt &t, const std::vector<std::pair<uint64_t,uint64_t>> &logical) {
        uint64_t tot = 0; for (auto &l : logical) tot += l.second;
        uint64_t run = im.alloc_run(tot, 1);
        uint64_t off = 0;
        for (auto &l : logical) { t.recs.push_back({l.first, l.second, run + off}); off += l.second; }
    };
    auto build_tree = [&](Tgt &t) {
        if (t.leaf_split.empty()) {
            uint64_t nb = im.alloc_run(2, 2);
            write_extent_leaf(im, nb, t.inode, true, 0, t.recs);
            t.root = nb;
        } else {
            uint64_t nb = im.alloc_run(2, 2);
            std::vector<std::pair<uint64_t,uint64_t>> kids;
            size_t off = 0;
            for (int c : t.leaf_split) {
                uint64_t lb = im.alloc_run(2, 2);
                std::vector<ExtRec> sub(t.recs.begin() + (long)off, t.recs.begin() + (long)(off + c));
                write_extent_leaf(im, lb, t.inode, false, nb, sub);
                kids.push_back({ sub.back().fblk + sub.back().len, lb });
                off += (size_t)c;
            }
            if (off != t.recs.size()) { fprintf(stderr, "mkstates_t: leaf_split 不匹配\n"); exit(1); }
            write_extent_inner(im, nb, t.inode, true, 0, kids);
            t.root = nb;
        }
    };
    auto write_data = [&](Tgt &t) {
        for (const ExtRec &r : t.recs)
            for (uint64_t k = 0; k < r.len; k++)
                write_block(r.phys + k, t.name.c_str(), (r.fblk + k) * BLK, t.prov, t.size);
    };
    auto emit_inode = [&](Tgt &t) {
        raven_Inode_t ino;
        uint32_t direct = (t.form == F_DIRECT) ? 1 : 0;
        uint64_t root = (t.form == F_INLINE) ? PTR_INV : (t.form == F_DIRECT ? t.direct_base : t.root);
        set_common(&ino, INODE_TYPE_REGULAR, direct, t.size, root, 1, 0644, RFS_XATTR_NONE);
        if (t.form == F_INLINE)
            for (uint64_t i = 0; i < t.size; i++)
                ino.inline_data[i] = t.prov[(size_t)i] ? m42_fill_byte(t.name.c_str(), i) : 0;
        im.write_inode(t.inode, &ino, 256);
    };

    /* ═══════════════════ A. 多级 extent 树（只读锚点 + 欠载触发件） ═══════════════════ */
    for (const char *nm : { "deep.bin", "deep_p.bin" }) {
        Tgt &t = add(nm);
        t.form = F_EXTENT;
        t.size = DEEP_N * BLK;
        t.prov.assign((size_t)t.size, 1);
        std::vector<std::pair<uint64_t,uint64_t>> L;
        for (uint64_t i = 0; i < DEEP_N; i++) L.push_back({ i, 1 });
        place_records(t, L);
        t.leaf_split = { 168, 168, 169 };   /* 505 条 → 3 叶；叶 0/1 恰在欠载阈值 168 */
        build_tree(t);
        write_data(t);
        t.xstate = "extent";
        if (std::string(nm) == "deep.bin") {
            t.has_B = true;
            t.st512 = (DEEP_N + 8) * 8;     /* 505 数据 + 4 节点×2 = 513 块 → ×8 */
        } else {
            /* punch 叶 0 首块：叶 0 残留 167 条 <168 → 触发欠载/合并链（终态节点数不定 → 不断言 B）*/
            t.ops.push_back({ 'A', 2, 0, 4096, ERR_OK });
        }
        emit_inode(t);
    }

    /* ═══════════════════ B. punch_hole ═══════════════════ */
    struct PS { const char *nm; uint64_t blocks; uint64_t off, len; uint64_t st512; } pset[] = {
        { "p_al.bin",    4, 4096,  4096,  40 },   /* 对齐中段整块释放：data 3 + node 2 */
        { "p_ua.bin",    4, 100,   4900,  48 },   /* 非对齐头/尾残块就地清零：data 4 + node 2 */
        { "p_eof.bin",   4, 9000,  11000, 40 },   /* 跨 EOF：尾整块释放 → data 3 + node 2 */
        { "p_sb.bin",    1, 300,   300,   24 },   /* 单块内：零释放 → data 1 + node 2 */
        { "p_whole.bin", 2, 0,     8192,  16 },   /* 整块域全释放 → 空根叶：data 0 + node 2 */
    };
    for (PS &p : pset) {
        Tgt &t = add(p.nm);
        t.form = F_EXTENT;
        t.size = p.blocks * BLK;
        t.prov.assign((size_t)t.size, 1);
        std::vector<std::pair<uint64_t,uint64_t>> L = { { 0, p.blocks } };
        place_records(t, L);
        build_tree(t);
        write_data(t);
        t.ops.push_back({ 'A', 2, p.off, p.len, ERR_OK });
        t.has_B = true; t.st512 = p.st512; t.xstate = "extent";
        emit_inode(t);
    }

    /* ═══════════════════ C. truncate ═══════════════════ */
    {
        /* t_inl：extent 8K → truncate 100 → **强制回迁 inline** */
        Tgt &t = add("t_inl.bin");
        t.form = F_EXTENT; t.size = 8192; t.prov.assign((size_t)t.size, 1);
        place_records(t, { { 0, 2 } }); build_tree(t); write_data(t);
        t.ops.push_back({ 'T', 0, 100, 0, ERR_OK });
        t.has_B = true; t.st512 = 0; t.xstate = "inline";
        emit_inode(t);
    }
    {
        /* t_dir：direct 16K（4 块）→ truncate 8192 → **direct 缩 run**（态不变）*/
        Tgt &t = add("t_dir.bin");
        t.form = F_DIRECT; t.size = 16384; t.prov.assign((size_t)t.size, 1);
        t.direct_blocks = 4; t.direct_base = im.alloc_run(4, 1);
        for (uint64_t b = 0; b < 4; b++) write_block(t.direct_base + b, t.name.c_str(), b * BLK, t.prov, t.size);
        t.ops.push_back({ 'T', 0, 8192, 0, ERR_OK });
        t.has_B = true; t.st512 = 16; t.xstate = "direct";
        emit_inode(t);
    }
    {
        /* t_ext：extent 16K 单条 [0,4) → truncate 8192 → 单条覆盖 [0,2) → **坍缩 direct** */
        Tgt &t = add("t_ext.bin");
        t.form = F_EXTENT; t.size = 16384; t.prov.assign((size_t)t.size, 1);
        place_records(t, { { 0, 4 } }); build_tree(t); write_data(t);
        t.ops.push_back({ 'T', 0, 8192, 0, ERR_OK });
        t.has_B = true; t.st512 = 16; t.xstate = "direct";
        emit_inode(t);
    }
    {
        /* t_grow：inline 100 → truncate 增大到 300 → 跨 160 **升格 extent**（D-M4-8）*/
        Tgt &t = add("t_grow.bin");
        t.form = F_INLINE; t.size = 100; t.prov.assign((size_t)t.size, 1);
        t.ops.push_back({ 'T', 0, 300, 0, ERR_OK });
        t.has_B = true; t.st512 = 24; t.xstate = "extent";
        emit_inode(t);
    }

    /* ═══════════════════ D. fallocate ═══════════════════ */
    {
        /* f_plain：inline 0 → fallocate(flags==0,0,8192) → 抬 size（普通预分配）*/
        Tgt &t = add("f_plain.bin");
        t.form = F_INLINE; t.size = 0; t.prov.clear();
        t.ops.push_back({ 'A', 0, 0, 8192, ERR_OK });
        t.xstate = "";                       /* extent/direct 取决于实装 → 不断言 */
        emit_inode(t);
    }
    {
        /* f_keep：inline 100 → fallocate(KEEP_SIZE) → **期望 -EOPNOTSUPP**（D-M4-11）*/
        Tgt &t = add("f_keep.bin");
        t.form = F_INLINE; t.size = 100; t.prov.assign((size_t)t.size, 1);
        t.ops.push_back({ 'A', 1, 0, 4096, ERR_NOSUP });
        t.has_B = true; t.st512 = 0; t.xstate = "inline";
        emit_inode(t);
    }
    {
        /* f_punch：extent 8K 单条 [0,2) → fallocate(PUNCH_HOLE|KEEP_SIZE,0,4096) */
        Tgt &t = add("f_punch.bin");
        t.form = F_EXTENT; t.size = 8192; t.prov.assign((size_t)t.size, 1);
        place_records(t, { { 0, 2 } }); build_tree(t); write_data(t);
        t.ops.push_back({ 'A', 2, 0, 4096, ERR_OK });
        t.has_B = true; t.st512 = 24; t.xstate = "extent";
        emit_inode(t);
    }
    {
        /* f_zero：extent 8K 单条 [0,2) → fallocate(ZERO_RANGE,4096,4096)：就地清零、size 不变
         * （终态是否把全零块降为洞依实装 → 不断言 B）*/
        Tgt &t = add("f_zero.bin");
        t.form = F_EXTENT; t.size = 8192; t.prov.assign((size_t)t.size, 1);
        place_records(t, { { 0, 2 } }); build_tree(t); write_data(t);
        t.ops.push_back({ 'A', 3, 4096, 4096, ERR_OK });
        t.xstate = "extent";
        emit_inode(t);
    }

    /* ── root 目录条目 ── */
    std::vector<Ent> root_ents;
    for (Tgt &t : T) root_ents.push_back({ t.name, t.inode });
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

    /* ── 期望清单（初始 / 操作后）── */
    std::vector<M41File> init_files, post_files;
    for (Tgt &t : T) {
        init_files.push_back(M41File{ t.name, t.inode, t.size, prov_segs(t.prov), "" });
        std::vector<uint8_t> pv = t.prov; uint64_t sz = t.size;
        for (const Op &o : t.ops) op_apply(pv, sz, o);
        post_files.push_back(M41File{ t.name, t.inode, sz, prov_segs(pv), "" });
    }
    if (!m41_write_expect(expect_path.c_str(),  init_files)) { fprintf(stderr, "mkstates_t: 写 %s 失败\n", expect_path.c_str()); return 1; }
    if (!m41_write_expect(wexpect_path.c_str(), post_files)) { fprintf(stderr, "mkstates_t: 写 %s 失败\n", wexpect_path.c_str()); return 1; }

    /* ── plan（操作序列 + 期望 size/B/X）── */
    {
        FILE *fp = fopen(plan_path.c_str(), "w");
        if (!fp) { fprintf(stderr, "mkstates_t: 写 %s 失败\n", plan_path.c_str()); return 1; }
        fprintf(fp, "# ravenfs-m4.3-plan v1\n");
        fprintf(fp, "# A <name> <mode:0|KEEP|PUNCH|ZERO> <off> <len> <err>   fallocate\n");
        fprintf(fp, "# T <name> <len_new> <err>                              ftruncate\n");
        fprintf(fp, "# B <name> <st512>  # i_blocks 期望（512B）\n");
        fprintf(fp, "# S <name> <size>   # size 期望（字节）\n");
        fprintf(fp, "# X <name> <inline|direct|extent>  # 终态形态期望\n");
        for (Tgt &t : T) {
            for (const Op &o : t.ops) {
                if (o.kind == 'T') fprintf(fp, "T %s %llu %d\n", t.name.c_str(), (unsigned long long)o.a, o.err);
                else               fprintf(fp, "A %s %s %llu %llu %d\n", t.name.c_str(), mode_str(o.mode),
                                           (unsigned long long)o.a, (unsigned long long)o.b, o.err);
            }
        }
        for (Tgt &t : T) {
            if (t.has_B) fprintf(fp, "B %s %llu\n", t.name.c_str(), (unsigned long long)t.st512);
            std::vector<uint8_t> pv = t.prov; uint64_t sz = t.size;
            for (const Op &o : t.ops) op_apply(pv, sz, o);
            fprintf(fp, "S %s %llu\n", t.name.c_str(), (unsigned long long)sz);
            if (!t.xstate.empty()) fprintf(fp, "X %s %s\n", t.name.c_str(), t.xstate.c_str());
        }
        fclose(fp);
    }

    printf("mkstates_t: expect  → %s（%zu 文件，初始）\n", expect_path.c_str(), init_files.size());
    printf("mkstates_t: wexpect → %s（%zu 文件，操作后）\n", wexpect_path.c_str(), post_files.size());
    printf("mkstates_t: plan    → %s\n", plan_path.c_str());
    for (size_t i = 0; i < T.size(); i++) {
        const char *st = (T[i].form == F_INLINE) ? "inline" : (T[i].form == F_DIRECT ? "direct" : "extent");
        printf("  [init] %-14s inode=%-3llu init=%-6s size=%-8llu segs=%zu xxh64=%016llx\n",
               T[i].name.c_str(), (unsigned long long)T[i].inode, st,
               (unsigned long long)T[i].size, init_files[i].segs.size(),
               (unsigned long long)m41_hash(init_files[i]));
        printf("  [post] %-14s            post=%-6s size=%-8llu segs=%zu xxh64=%016llx%s\n",
               T[i].name.c_str(), T[i].xstate.empty() ? "?" : T[i].xstate.c_str(),
               (unsigned long long)post_files[i].size, post_files[i].segs.size(),
               (unsigned long long)m41_hash(post_files[i]),
               T[i].has_B ? "" : "  [B: 不断言]");
    }

    close(im.fd);
    printf("mkstates_t: done → %s\n", argv[2]);
    return 0;
}
