/* ═══════════════════════════════════════════════════════════════════════════
 * mkstates_s.cpp — M4.5（fsync/崩溃/深层平衡）**深层 extent 树靶子构造器**
 *                  （mkstates_t.cpp 的姊妹件；写副本；不改 M4.1–M4.4 任何资产）
 *
 * 目的：造一个「**深层区段树 + 大面积 extent 删除**」的 v1.2 镜像 + 四份配套清单：
 *   · <out.img>          初始镜像（可挂载可写；含 s_deep.bin = **3 层 extent 树**）
 *   · <out.img>.expect   初始内容清单（M4.1 格式；供 `audit_v1.2` + `readback verify` 自证）
 *   · <out.img>.wexpect  删除后内容清单（M4.1 格式；`readback dump` 产 ground-truth）
 *   · <out.img>.plan     删除操作序列（A PUNCH <off> <len>；由 m45_ops 执行）
 *
 * ── 深层树（本里程碑的核心靶子）─────────────────────────────────────────────
 * 规矩（disk_layout_v1.1 §10 不变量 31–36）：阶 = 337；非根 count ∈ [168,337]；
 *   深度上限 64；节点 = 2×4K 连续（8K 对齐）。
 *
 * **单叶根（深 1）/ 根内+叶（深 2）** 已由 Mk4.2/4.3 覆盖（deep.bin 505 条 → 3 叶）。
 * 本构造器要造 **≥2 层内节点（深 3 = 根内 → 内 → 叶）**：根下有 **2 个内节点**，
 *   ⇒ 需要 **≥ 338 个叶**（阶 337 满 → 第 338 叶触发内裂）。
 *
 * 默认（**忠实**：叶恰在欠载下限 168）：
 *   叶数 = A + B = 168 + 171 = **339**；每叶 **168** 条单块 extent（= 168 块 = 688128 B）
 *   ⇒ 逻辑块 339×168 = **56952** 块 ⇒ 文件 = **233,275,392 B ≈ 222.5 MiB**
 *   节点块 = 1 根 + 2 内 + 339 叶 = **342** 节点 × 2 块 = 684 块
 *   ⇒ 镜像 **256 MiB**（总占用 ~227 MiB，几何余量足够）。
 *   这是「不违反 168 下限」前提下的**最小** 2-内层树；再小只能靠降级（见 env）。
 *
 * 删除序列（guest 侧由 m45_ops 执行；`A PUNCH` = FALLOC_FL_PUNCH_HOLE|KEEP_SIZE）：
 *   op1  punch [0, E·4096)              删「叶 0」→ inner0 = 167 叶 <168 欠载
 *        → 与 inner1（171）合并/抹匀（167+171=338 >337 ⇒ **抹匀** 169/169；仍深 3）
 *   op2  punch [E·4096, 3·E·4096)       再删 2 叶 → 叶数 336 ≤337 ⇒ **内层合并**
 *        → 根仅 1 子 ⇒ **root_shrink**（深 3 → 深 2）
 *   终态内容：off ∈ [0, 3·E·4096) 全 0，其余为 fill（size 不变）。
 *
 * ── 降级（**如实写明**；见 Docs/linux/ravenfs-linux-ko-M4.5-acceptance.md §6）──
 *   env `M45_E`（每叶条目数，默认 168）、`M45_A`（inner0 叶数，默认 168）、
 *       `M45_B`（inner1 叶数，默认 171）。
 *   若 E < 168，叶低于不变量下限（**非 ko 稳态**，仅得树的**形状**，文件显著变小）——
 *   仅在磁盘/投递预算不足时使用，且必须在验收报告里标注为**降级**。
 *
 * 真值来源：include/disk_struct_v1.2.h、magics.h、Docs/disk_layout_v1.1.md §5/§10、
 *           Docs/linux/ravenfs-linux-ko-M4-data-spec.md §8/§11、Docs/punch_truncate_pread_spec.md。
 * 纪律：**不改** mkstates.cpp / mkstates_w.cpp / mkstates_t.cpp / mkstates_a.cpp /
 *       readback.cpp / m4*_*.h；只在副本上写（auditor 本身只读）。
 *
 * 用法： ./mkstates_s <in_empty.img> <out.img> [prefix]
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

static constexpr uint64_t BLK    = 4096;
static constexpr uint64_t NODE   = 8192;
static constexpr uint64_t PTR52  = (1ULL << 52) - 1;
static constexpr uint64_t PTR_INV = PTR_INVALID;

static constexpr uint64_t DEF_E = 168;   /* 每叶条目数（= 非根欠载下限，忠实态） */
static constexpr uint64_t DEF_A = 168;   /* inner0 叶数 */
static constexpr uint64_t DEF_B = 171;   /* inner1 叶数（A+B ≥ 338 → 强制内裂） */

static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

static uint64_t env_u64(const char *k, uint64_t dflt) {
    const char *v = getenv(k);
    if (!v || !*v) return dflt;
    char *end = nullptr;
    unsigned long long x = strtoull(v, &end, 0);
    return (end && *end == 0 && x > 0) ? (uint64_t)x : dflt;
}

/* ── 两版 fill 公式一致性交叉校验（防 m42_fill.h 与 m41_expect.h 漂移）── */
static void assert_fill_consistent() {
    const char *names[] = { "", "a", "s_deep.bin", "x/y.z" };
    for (const char *nm : names)
        for (uint64_t off = 0; off < 5000; off += 97)
            if (m42_fill_byte(nm, off) != m41_fill_byte(nm, off)) {
                fprintf(stderr, "FATAL: m42_fill_byte != m41_fill_byte (name=%s off=%llu)\n",
                        nm, (unsigned long long)off);
                exit(2);
            }
}

/* ═══════════════════════════ 镜像写器（照抄 mkstates_t） ═══════════════════════════ */
struct Img {
    int fd = -1;
    uint64_t total_blocks = 0, inode_count = 0, root_inode = 0;
    uint64_t bitmap_blocks = 0, bitmap_start = 0, inode_blocks = 0, inode_array_base = 0;
    uint64_t inode_bm_blocks = 0, inode_bm_base = 0;
    std::vector<uint8_t> bm, ibm;
    uint64_t scan_cur = 2;   /* alloc_one 游标（避免逐块 O(n²) 重扫）*/

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
        fprintf(stderr, "mkstates_s: no free %llu contiguous blocks (align=%llu)\n",
                (unsigned long long)n, (unsigned long long)align);
        exit(1);
    }
    uint64_t alloc_one() {
        for (uint64_t b = scan_cur; b < total_blocks; b++)
            if (!blk_lit(b)) { mark_blk(b); scan_cur = b + 1; return b; }
        for (uint64_t b = 2; b < total_blocks; b++)
            if (!blk_lit(b)) { mark_blk(b); return b; }
        fprintf(stderr, "mkstates_s: no free block\n"); exit(1);
    }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkstates_s: no free inode\n"); exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

/* ── 目录项（与 mkstates_t 同构；名字 ≤40B → 不进名字堆）── */
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

/* ── extent 节点写器（照抄 mkstates_t；audit 已接受其形状）── */
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

    const uint64_t E = env_u64("M45_E", DEF_E);   /* 每叶条目数 */
    const uint64_t A = env_u64("M45_A", DEF_A);   /* inner0 叶数 */
    const uint64_t B = env_u64("M45_B", DEF_B);   /* inner1 叶数 */
    const uint64_t NL = A + B;                    /* 叶总数 */

    if (A < 2 || B < 1) { fprintf(stderr, "mkstates_s: M45_A/B 过小\n"); return 1; }
    if (A + B < 338)
        fprintf(stderr, "mkstates_s: ⚠ 叶数 %llu < 338 —— **降级**（根下仅 1 内节点，非 2 内层）\n",
                (unsigned long long)(A + B));
    if (E < DEF_E)
        fprintf(stderr, "mkstates_s: ⚠ 每叶条目 %llu < 168 —— **降级**（叶低于欠载下限，非 ko 稳态）\n",
                (unsigned long long)E);

    /* 复制空盘 → 副本（auditor 本身只读） */
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

    printf("mkstates_s: geo total_blocks=%llu inode_count=%llu root=%llu "
           "bitmap=[%llu,+%llu) inode_array=[%llu,+%llu) inode_bm=[%llu,+%llu)\n",
           (unsigned long long)im.total_blocks, (unsigned long long)im.inode_count,
           (unsigned long long)im.root_inode,
           (unsigned long long)im.bitmap_start, (unsigned long long)im.bitmap_blocks,
           (unsigned long long)im.inode_array_base, (unsigned long long)im.inode_blocks,
           (unsigned long long)im.inode_bm_base, (unsigned long long)im.inode_bm_blocks);
    printf("mkstates_s: E=%llu A=%llu B=%llu  => 叶 %llu, 文件 %llu B (%.1f MiB)\n",
           (unsigned long long)E, (unsigned long long)A, (unsigned long long)B,
           (unsigned long long)NL, (unsigned long long)(NL * E * BLK),
           (double)(NL * E * BLK) / (1024.0 * 1024.0));

    /* ═══════════════════ s_deep.bin：3 层区段树 ═══════════════════ */
    const char *NAME = "s_deep.bin";
    uint64_t i_deep = im.alloc_inode();
    uint64_t total_blocks_file = NL * E;                     /* NL 叶 × E 条（单块） */
    uint64_t size = total_blocks_file * BLK;

    /* 数据：逐逻辑块分配物理块（**不要求物理连续**；audit 只看区间合法 + 无重复认领）。
     * 用游标逐块分配：O(total_blocks)，避开 mkfs 散布的少量已用块（物理连续性不保证）。*/
    std::vector<uint64_t> phys(total_blocks_file);
    for (uint64_t lb = 0; lb < total_blocks_file; lb++)
        phys[lb] = im.alloc_one();

    /* 逐叶建叶节点（叶 i：逻辑块 [E·i, E·i+E)）*/
    std::vector<std::pair<uint64_t,uint64_t>> leaf_kids;     /* (maxkey, blk) */
    for (uint64_t i = 0; i < NL; i++) {
        std::vector<ExtRec> ex;
        ex.reserve((size_t)E);
        for (uint64_t j = 0; j < E; j++) {
            uint64_t lb = E * i + j;
            ex.push_back({ lb, 1, phys[lb] });
        }
        uint64_t lb = im.alloc_run(2, 2);
        write_extent_leaf(im, lb, i_deep, false, 0 /*parent 回填 */, ex);
        leaf_kids.push_back({ E * (i + 1), lb });            /* maxkey = 开边界 */
    }

    /* 两个内节点：inner0 = 叶[0,A) ；inner1 = 叶[A,A+B) */
    auto build_inner = [&](uint64_t from, uint64_t cnt) -> std::pair<uint64_t,uint64_t> {
        uint64_t nb = im.alloc_run(2, 2);
        std::vector<std::pair<uint64_t,uint64_t>> kids(leaf_kids.begin() + (long)from,
                                                       leaf_kids.begin() + (long)(from + cnt));
        write_extent_inner(im, nb, i_deep, false, 0, kids);
        /* 回填子叶 parent_ptr = nb */
        for (uint64_t k = 0; k < cnt; k++) {
            uint64_t cb = kids[(size_t)k].second;
            std::vector<uint8_t> node(NODE, 0);
            if (!im.rd(node.data(), NODE, blkoff(cb))) { fprintf(stderr, "mkstates_s: 读叶失败\n"); exit(1); }
            auto *meta = (Bptree_meta_t *)node.data();
            uint64_t cnt_e = RFS_PP_COUNT(meta->parent_ptr);
            meta->parent_ptr.v = RFS_PP_PACK(cnt_e, 0, 0, nb);   /* 叶：inner=0，父 = nb */
            im.wr(node.data(), NODE, blkoff(cb));
        }
        return { kids.back().first, nb };
    };
    auto k0 = build_inner(0, A);
    auto k1 = build_inner(A, B);

    /* 根内 */
    uint64_t root_blk = im.alloc_run(2, 2);
    write_extent_inner(im, root_blk, i_deep, true, 0, { k0, k1 });
    /* 回填两个内节点的 parent_ptr = root_blk */
    for (uint64_t cb : { k0.second, k1.second }) {
        std::vector<uint8_t> node(NODE, 0);
        if (!im.rd(node.data(), NODE, blkoff(cb))) { fprintf(stderr, "mkstates_s: 读内失败\n"); return 1; }
        auto *meta = (Bptree_meta_t *)node.data();
        uint64_t cnt = RFS_PP_COUNT(meta->parent_ptr);
        meta->parent_ptr.v = RFS_PP_PACK(cnt, 1, 0, root_blk);
        im.wr(node.data(), NODE, blkoff(cb));
    }

    /* 数据块内容 = fill_byte(name, 文件偏移) */
    {
        std::vector<uint8_t> b(BLK, 0);
        for (uint64_t lb = 0; lb < total_blocks_file; lb++) {
            for (uint64_t j = 0; j < BLK; j++) b[(size_t)j] = m42_fill_byte(NAME, lb * BLK + j);
            im.wr(b.data(), BLK, blkoff(phys[lb]));
        }
    }

    /* inode（extent 态；direct=0） */
    {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, size, root_blk, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_deep, &ino, 256);
    }

    /* ═══════════════════ root 目录 ═══════════════════ */
    std::vector<Ent> root_ents = { { NAME, i_deep } };
    sort_ents(root_ents);
    std::vector<uint8_t> root_node;
    build_dentry_node(root_node, im.root_inode, im.root_inode, root_ents);
    im.wr(root_node.data(), root_node.size(), blkoff(0));
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

    /* 位图落盘 */
    im.wr(im.bm.data(),  im.bm.size(),  blkoff(im.bitmap_start));
    im.wr(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    /* ═══════════════════ 清单 ═══════════════════ */
    const uint64_t PUN1_LEN = E * BLK;              /* 删 1 叶 = E 块 */
    const uint64_t PUN2_OFF = E * BLK;
    const uint64_t PUN2_LEN = 2 * E * BLK;          /* 再删 2 叶 */
    const uint64_t ZLEN     = PUN2_OFF + PUN2_LEN;  /* 累计清零前缀 = 3·E·4096 */

    std::vector<M41File> init_files = {
        M41File{ NAME, i_deep, size, { { 0, size, 'D' } }, "" }
    };
    std::vector<M41File> post_files = {
        M41File{ NAME, i_deep, size, { { 0, ZLEN, 'Z' }, { ZLEN, size - ZLEN, 'D' } }, "" }
    };
    if (!m41_write_expect(expect_path.c_str(),  init_files)) { fprintf(stderr, "mkstates_s: 写 %s 失败\n", expect_path.c_str()); return 1; }
    if (!m41_write_expect(wexpect_path.c_str(), post_files)) { fprintf(stderr, "mkstates_s: 写 %s 失败\n", wexpect_path.c_str()); return 1; }

    /* plan */
    {
        FILE *fp = fopen(plan_path.c_str(), "w");
        if (!fp) { fprintf(stderr, "mkstates_s: 写 %s 失败\n", plan_path.c_str()); return 1; }
        fprintf(fp, "# ravenfs-m4.5-plan v1\n");
        fprintf(fp, "# A PUNCH <off> <len>  = fallocate(PUNCH_HOLE|KEEP_SIZE, off, len)\n");
        fprintf(fp, "# FILE %s size %llu zlen %llu\n", NAME, (unsigned long long)size, (unsigned long long)ZLEN);
        fprintf(fp, "A PUNCH %llu %llu\n", 0ULL, (unsigned long long)PUN1_LEN);
        fprintf(fp, "A PUNCH %llu %llu\n", (unsigned long long)PUN2_OFF, (unsigned long long)PUN2_LEN);
        fclose(fp);
    }

    printf("mkstates_s: deep inode=%llu root_blk=%llu size=%llu\n",
           (unsigned long long)i_deep, (unsigned long long)root_blk,
           (unsigned long long)size);
    printf("mkstates_s: nodes = 1 根 + 2 内 + %llu 叶 = %llu（extent_nodes 期望）\n",
           (unsigned long long)NL, (unsigned long long)(NL + 3));
    printf("mkstates_s: ops  punch[0,%llu) + punch[%llu,%llu) → 清零前缀 %llu B\n",
           (unsigned long long)PUN1_LEN, (unsigned long long)PUN2_OFF,
           (unsigned long long)(PUN2_OFF + PUN2_LEN), (unsigned long long)ZLEN);
    printf("mkstates_s: expect  → %s\n", expect_path.c_str());
    printf("mkstates_s: wexpect → %s\n", wexpect_path.c_str());
    printf("mkstates_s: plan    → %s（2 操作）\n", plan_path.c_str());
    printf("  [init] %s size=%llu xxh64=%016llx\n", NAME, (unsigned long long)size,
           (unsigned long long)m41_hash(init_files[0]));
    printf("  [post] %s size=%llu xxh64=%016llx\n", NAME, (unsigned long long)size,
           (unsigned long long)m41_hash(post_files[0]));

    close(im.fd);
    printf("mkstates_s: done → %s\n", argv[2]);
    return 0;
}
