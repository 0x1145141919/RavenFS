/* ═══════════════════════════════════════════════════════════════════════════
 * mkstates_a.cpp — M4.4（setattr + 时间戳）**靶子/初始镜像构造器**
 *                  （mkstates_w.cpp / mkstates_t.cpp 的姊妹件；写副本；不改任何既有资产）
 *
 * 目的：造一个「**可被 ko 做 chmod/chown/utimes/truncate**」的 v1.2 镜像 + 五份配套清单：
 *   · <out.img>           初始镜像（可挂载可写）
 *   · <out.img>.expect    **属性期望（初始）**：mode / uid / gid / mtime / atime / ctime（含 ns）
 *   · <out.img>.wexpect   **属性期望（操作后）**：同上（ctime 记 -1 = 不断言）
 *   · <out.img>.plan      **操作序列**（chmod / chown / lchown / utimes / truncate）
 *   · <out.img>.cexpect   初始内容清单（M4.1 格式；供 `readback verify` 自证 + `audit_v1.2`）
 *   · <out.img>.cexpect.post  操作后内容清单（M4.1 格式；`readback dump` 产 ground-truth → guest cmp）
 *
 * 初始靶子（覆盖 M4.4 判据：chmod 含 suid/sgid/sticky / chown / utimes / truncate；
 *           普通文件 + 目录 + symlink；改后 umount/remount 保留）：
 *
 *   ── 普通文件 ──
 *   a_reg.bin      extent 8K（D）  mode 0644 0:0 @T0 → chmod 0755 + chown 1000:1000 + touch @T1 + truncate 4096
 *   a_suid.bin     inline 100      mode 0644         → chmod 4755（setuid）
 *   a_sgid.bin     inline 100      mode 0644         → chmod 2755（setgid）
 *   a_stky.bin     inline 100      mode 0644         → chmod 1755（sticky）
 *   a_7777.bin     inline 100      mode 0644         → chmod 7777（suid|sgid|sticky 全开）
 *   a_ts.bin       extent 4K（D）  mode 0644 @T0     → touch -d @T2.123456789（mtime+atime）
 *   a_ts2.bin      extent 4K（D）  mode 0644 @T0     → touch -m @T4 ; touch -a @T5（分别设 m/a）
 *   a_own.bin      inline 0        mode 0644 0:0     → chown 4321:8765（仅属主/组）
 *   a_trunc.bin    extent 16K（D） mode 0644         → truncate 5000（缩小）
 *   a_trunc_g.bin  inline 100（D） mode 0644         → truncate 300（增大；跨 160 升格）
 *
 *   ── 目录 ──
 *   a_dir/         DIR size=2      mode 0755 0:0     → chmod 0700 + chown 1500:1500（子项 c1/c2 保留）
 *   a_sgdir/       DIR size=0      mode 0755         → chmod 2755（setgid 目录）
 *
 *   ── 符号链接（setattr 走 no-follow：lchown / lutimes）──
 *   a_slink        LNK → "a_reg.bin"   mode 0777     → chown -h 2000:2000 + touch -h -d @T3
 *   a_slink2       LNK → "no/such/target"（悬垂）    → chown -h 2500:2500（不跟随、不炸）
 *
 * 清单格式（纯文本；`#` 注释；path 不含空白）：
 *   .expect / .wexpect：
 *     A <inode> <kind:reg|dir|lnk> <size> <mode4oct> <uid> <gid> \
 *       <mtime_s> <mtime_ns> <atime_s> <atime_ns> <ctime_s> <ctime_ns> <path>
 *     （ctime = -1 表示不断言）
 *   .plan：
 *     M  <path> <mode_octal>              → chmod <mode> <path>
 *     O  <path> <uid> <gid>              → chown <uid>:<gid> <path>（跟随）
 *     H  <path> <uid> <gid>              → chown -h <uid>:<gid> <path>（不跟随）
 *     D  <path> <sec> <ns>               → touch -d @<sec>.<ns> <path>（mtime+atime）
 *     DM <path> <sec> <ns>               → touch -m -d @<sec>.<ns>（仅 mtime）
 *     DA <path> <sec> <ns>               → touch -a -d @<sec>.<ns>（仅 atime）
 *     DH <path> <sec> <ns>               → touch -h -d @<sec>.<ns>（symlink，不跟随）
 *     T  <path> <size>                   → truncate -s <size> <path>
 *
 * 真值来源：include/disk_struct_v1.2.h（mode/uid/gid/mtime/ctime/atime + ns 字段）、magics.h、
 *           Docs/linux/ravenfs-linux-ko-M4-data-spec.md §11（M4.4）、
 *           Docs/linux/ravenfs-linux-ko-requirements-spec.md §4.5（R-MD-02/03/04/06/07/09）、
 *           Docs/disk_layout_v1.2_inode.md（i_mode = type<<12 | mode；mode 含 suid/sgid/sticky）。
 * 纪律：**不改** mkstates.cpp / mkstates_w.cpp / mkstates_t.cpp / readback.cpp / m41_expect.h /
 *       m42_fill.h / audit_v1.2.cpp；只在副本上写（auditor 本身只读）。
 *
 * 用法： ./mkstates_a <in_empty.img> <out.img> [prefix]
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

/* ── 时间基准（全部 2023 前，便于与运行期 now 区分）── */
static constexpr uint64_t T0 = 1700000000ULL;   /* 2023-11-14T22:13:20Z */
static constexpr uint64_t T1 = 1610000000ULL;   /* 2021-01-07 */
static constexpr uint64_t T2 = 1510000000ULL;   /* 2017-11-06 */
static constexpr uint64_t T3 = 1410000000ULL;   /* 2014-09-06 */
static constexpr uint64_t T4 = 1350000000ULL;   /* 2012-10-12 */
static constexpr uint64_t T5 = 1310000000ULL;   /* 2011-07-07 */
static constexpr uint32_t T2_NS = 123456789U;   /* 纳秒：验 sec+ns 双字段 */

static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

/* ── 两版 fill 公式一致性交叉校验（防 m42_fill.h 与 m41_expect.h 漂移）── */
static void assert_fill_consistent() {
    const char *names[] = { "", "a", "a_reg.bin", "a_trunc.bin", "a_dir/c1.bin", "x/y.z" };
    for (const char *nm : names)
        for (uint64_t off = 0; off < 5000; off += 97)
            if (m42_fill_byte(nm, off) != m41_fill_byte(nm, off)) {
                fprintf(stderr, "FATAL: m42_fill_byte != m41_fill_byte (name=%s off=%llu)\n",
                        nm, (unsigned long long)off);
                exit(2);
            }
}

/* ═══════════════════════════ 镜像读/写器（照抄 mkstates_t） ═══════════════════════════ */
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
        fprintf(stderr, "mkstates_a: no free %llu contiguous blocks (align=%llu)\n",
                (unsigned long long)n, (unsigned long long)align);
        exit(1);
    }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkstates_a: no free inode\n"); exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
    bool read_inode(uint64_t idx, void *ino, size_t n) {
        return rd(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

/* ── 目录项（与 mkcontent/mkstates/mkstates_t 同构）── */
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

/* ── extent 节点写入（照抄 mkstates_t）── */
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

/* ═══════════════════════════ 靶子模型 ═══════════════════════════ */
enum Kind { K_REG, K_DIR, K_LNK };
enum OpKind { OP_CHMOD, OP_CHOWN, OP_LCHOWN, OP_TIME_BOTH, OP_TIME_M, OP_TIME_A, OP_TIME_LH, OP_TRUNC };

struct AOp {
    OpKind   k;
    uint64_t a = 0, b = 0;           /* chmod:a=mode；chown:a=uid,b=gid；time:a=sec,b=ns；trunc:a=size */
};

struct Tgt {
    std::string name;                /* 根下名；子项目为 "a_dir/c1.bin" 前缀仅用于清单 */
    Kind        kind = K_REG;
    uint64_t    inode = 0;

    /* 初始属性 */
    uint64_t size  = 0;
    uint32_t mode  = 0644;           /* 12 位权限（含 suid/sgid/sticky） */
    uint32_t uid   = 0, gid = 0;
    uint64_t mtime = T0, atime = T0, ctime = T0;
    uint32_t mns = 0, ans = 0, cns = 0;

    /* 数据 */
    std::vector<uint8_t> prov;       /* REG：1=填充字节 0=零 */
    std::string          link;       /* LNK：目标串 */
    std::vector<Ent>     children;   /* DIR：子项（inode 已分配） */
    uint64_t             dir_root = 0;   /* DIR 的 dentry 树根块 */
    std::vector<ExtRec>  recs;       /* REG extent 布局（size>160） */
    uint64_t             root = 0;   /* REG extent 树根块 */

    std::vector<AOp>     ops;

    /* 操作后属性 */
    uint64_t psize = 0, pmtime = T0, patime = T0, pctime = (uint64_t)-1;
    uint32_t pmode = 0644, puid = 0, pgid = 0;
    uint32_t pmns = 0, pans = 0, pcns = 0;
    std::vector<uint8_t> postprov;
};

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

static void apply_ops(Tgt &t) {
    t.psize = t.size; t.pmode = t.mode; t.puid = t.uid; t.pgid = t.gid;
    t.pmtime = t.mtime; t.pmns = t.mns;
    t.patime = (uint64_t)-1; t.pans = 0;   /* atime 默认不断言（读会扰动）；仅时间操作显式设定时才断言 */
    t.pctime = (uint64_t)-1;              /* 操作会刷新 ctime（now）→ 不断言 */
    t.postprov = t.prov;
    for (const AOp &o : t.ops) {
        switch (o.k) {
        case OP_CHMOD:     t.pmode = (uint32_t)o.a; break;
        case OP_CHOWN:
        case OP_LCHOWN:    t.puid = (uint32_t)o.a; t.pgid = (uint32_t)o.b; break;
        case OP_TIME_BOTH:
        case OP_TIME_LH:   t.pmtime = o.a; t.pmns = (uint32_t)o.b;
                           t.patime = o.a; t.pans = (uint32_t)o.b; break;
        case OP_TIME_M:    t.pmtime = o.a; t.pmns = (uint32_t)o.b; break;
        case OP_TIME_A:    t.patime = o.a; t.pans = (uint32_t)o.b; break;
        case OP_TRUNC:     t.postprov.resize((size_t)o.a, 0); t.psize = o.a;
                           t.pmtime = (uint64_t)-1; t.pmns = 0;   /* truncate 刷 mtime=now（Linux do_truncate）→ 不断言 */
                           break;
        }
    }
}

static const char *kind_str(Kind k) {
    return k == K_REG ? "reg" : (k == K_DIR ? "dir" : "lnk");
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <in_empty.img> <out.img> [prefix]\n", argv[0]);
        return 1;
    }
    assert_fill_consistent();
    std::string base = (argc >= 4) ? argv[3] : std::string(argv[2]);
    std::string expect_path   = base + ".expect";
    std::string wexpect_path  = base + ".wexpect";
    std::string plan_path     = base + ".plan";
    std::string cexpect_path  = base + ".cexpect";
    std::string cpost_path    = base + ".cexpect.post";

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

    printf("mkstates_a: geo total_blocks=%llu inode_count=%llu root=%llu "
           "bitmap=[%llu,+%llu) inode_array=[%llu,+%llu) inode_bm=[%llu,+%llu)\n",
           (unsigned long long)im.total_blocks, (unsigned long long)im.inode_count,
           (unsigned long long)im.root_inode,
           (unsigned long long)im.bitmap_start, (unsigned long long)im.bitmap_blocks,
           (unsigned long long)im.inode_array_base, (unsigned long long)im.inode_blocks,
           (unsigned long long)im.inode_bm_base, (unsigned long long)im.inode_bm_blocks);

    std::vector<Tgt> T;
    auto add = [&](const char *nm, Kind k) -> Tgt & {
        Tgt t; t.name = nm; t.kind = k; t.inode = im.alloc_inode();
        T.push_back(std::move(t));
        return T.back();
    };

    /* ── 数据块写入（按 prov；越 size 部分补 0）── */
    auto write_block = [&](uint64_t phys, const char *nm, uint64_t foff,
                           const std::vector<uint8_t> &prov, uint64_t size) {
        std::vector<uint8_t> b(BLK, 0);
        for (uint64_t j = 0; j < BLK; j++)
            if (foff + j < size && prov[(size_t)(foff + j)]) b[(size_t)j] = m42_fill_byte(nm, foff + j);
        im.wr(b.data(), b.size(), blkoff(phys));
    };

    auto place_records = [&](Tgt &t, const std::vector<std::pair<uint64_t,uint64_t>> &logical) {
        uint64_t tot = 0; for (auto &l : logical) tot += l.second;
        uint64_t run = im.alloc_run(tot, 1);
        uint64_t off = 0;
        for (auto &l : logical) { t.recs.push_back({l.first, l.second, run + off}); off += l.second; }
    };
    auto build_tree = [&](Tgt &t) {
        uint64_t nb = im.alloc_run(2, 2);
        write_extent_leaf(im, nb, t.inode, true, 0, t.recs);
        t.root = nb;
    };
    auto write_data = [&](Tgt &t) {
        for (const ExtRec &r : t.recs)
            for (uint64_t k = 0; k < r.len; k++)
                write_block(r.phys + k, t.name.c_str(), (r.fblk + k) * BLK, t.prov, t.size);
    };
    /* 造一个 REG 靶子：size≤160 → inline；否则 extent 单叶根 */
    auto mk_reg = [&](const char *nm, uint64_t size) -> Tgt & {
        Tgt &t = add(nm, K_REG);
        t.size = size;
        t.prov.assign((size_t)size, 1);
        if (size > 160) {
            uint64_t blocks = (size + BLK - 1) / BLK;
            place_records(t, { { 0, blocks } });
            build_tree(t);
            write_data(t);
        }
        return t;
    };

    /* ═══════════════════ A. 普通文件 ═══════════════════ */
    {
        Tgt &t = mk_reg("a_reg.bin", 8192);
        t.mode = 0644;
        t.ops.push_back({ OP_CHMOD, 0755, 0 });
        t.ops.push_back({ OP_CHOWN, 1000, 1000 });
        t.ops.push_back({ OP_TRUNC, 4096, 0 });     /* 截断先（会刷 mtime）*/ 
        t.ops.push_back({ OP_TIME_BOTH, T1, 0 });   /* 再 touch → mtime/atime 定值（验时间戳）*/
    }
    {
        Tgt &t = mk_reg("a_suid.bin", 100);
        t.ops.push_back({ OP_CHMOD, 04755, 0 });
    }
    {
        Tgt &t = mk_reg("a_sgid.bin", 100);
        t.ops.push_back({ OP_CHMOD, 02755, 0 });
    }
    {
        Tgt &t = mk_reg("a_stky.bin", 100);
        t.ops.push_back({ OP_CHMOD, 01755, 0 });
    }
    {
        Tgt &t = mk_reg("a_7777.bin", 100);
        t.ops.push_back({ OP_CHMOD, 07777, 0 });
    }
    {
        Tgt &t = mk_reg("a_ts.bin", 4096);
        t.ops.push_back({ OP_TIME_BOTH, T2, T2_NS });
    }
    {
        Tgt &t = mk_reg("a_ts2.bin", 4096);
        t.ops.push_back({ OP_TIME_M, T4, 0 });
        t.ops.push_back({ OP_TIME_A, T5, 0 });
    }
    {
        Tgt &t = add("a_own.bin", K_REG);   /* size 0 inline */
        t.size = 0; t.prov.clear();
        t.ops.push_back({ OP_CHOWN, 4321, 8765 });
    }
    {
        Tgt &t = mk_reg("a_trunc.bin", 16384);
        t.ops.push_back({ OP_TRUNC, 5000, 0 });
    }
    {
        Tgt &t = mk_reg("a_trunc_g.bin", 100);
        t.ops.push_back({ OP_TRUNC, 300, 0 });
    }

    /* ═══════════════════ B. 子目录内容（先分配子项 inode）═══════════════════ */
    std::vector<Ent> dir_children;
    for (const char *cn : { "a_dir/c1.bin", "a_dir/c2.bin" }) {
        Tgt &t = mk_reg(cn, 64);
        const char *slash = strrchr(cn, '/');
        dir_children.push_back({ slash ? slash + 1 : cn, t.inode });
    }
    sort_ents(dir_children);

    /* ═══════════════════ C. 目录 ═══════════════════ */
    {
        Tgt &t = add("a_dir", K_DIR);
        t.mode = 0755;
        t.children = dir_children;
        t.size = t.children.size();
        uint64_t dblk = im.alloc_run(2, 2);   /* DIR dentry 树根（8K，偶对齐）*/
        t.dir_root = dblk;
        std::vector<uint8_t> node;
        build_dentry_node(node, t.inode, im.root_inode, t.children);
        im.wr(node.data(), node.size(), blkoff(dblk));
        t.ops.push_back({ OP_CHMOD, 0700, 0 });
        t.ops.push_back({ OP_CHOWN, 1500, 1500 });
    }
    {
        Tgt &t = add("a_sgdir", K_DIR);
        t.mode = 0755;
        t.children.clear();
        t.size = 0;
        uint64_t dblk = im.alloc_run(2, 2);
        t.dir_root = dblk;
        std::vector<uint8_t> node;
        build_dentry_node(node, t.inode, im.root_inode, t.children);
        im.wr(node.data(), node.size(), blkoff(dblk));
        t.ops.push_back({ OP_CHMOD, 02755, 0 });
    }

    /* ═══════════════════ D. 符号链接 ═══════════════════ */
    {
        Tgt &t = add("a_slink", K_LNK);
        t.link = "a_reg.bin";
        t.size = t.link.size();
        t.mode = 0777;
        t.ops.push_back({ OP_LCHOWN, 2000, 2000 });
        t.ops.push_back({ OP_TIME_LH, T3, 0 });
    }
    {
        Tgt &t = add("a_slink2", K_LNK);
        t.link = "no/such/target";
        t.size = t.link.size();
        t.mode = 0777;
        t.ops.push_back({ OP_LCHOWN, 2500, 2500 });
    }

    /* ── 落盘每个靶子 inode ── */
    for (Tgt &t : T) {
        if (t.kind == K_REG) {
            raven_Inode_t ino;
            memset(&ino, 0, 256);
            uint32_t direct = 0;
            uint64_t droot = PTR_INV;
            if (t.size > 160) { droot = t.root; direct = 0; }
            ino.word1      = RFS_INO1_PACK(INODE_TYPE_REGULAR, direct, 0);
            ino.uid        = t.uid;
            ino.gid        = t.gid;
            ino.m_ns_stamp = t.mns;
            ino.c_ns_stamp = t.cns;
            ino.a_ns_stamp = t.ans;
            ino.link_count = 1;
            ino.mode_xattr = RFS_MODE_XATTR_PACK(t.mode, RFS_XATTR_NONE);
            ino.size       = t.size;
            ino.mtime      = t.mtime;
            ino.ctime      = t.ctime;
            ino.atime      = t.atime;
            ino.data_root  = droot;
            if (t.size <= 160)
                for (uint64_t i = 0; i < t.size; i++)
                    ino.inline_data[i] = t.prov[(size_t)i] ? m42_fill_byte(t.name.c_str(), i) : 0;
            im.write_inode(t.inode, &ino, 256);
        } else if (t.kind == K_LNK) {
            raven_Inode_t ino;
            memset(&ino, 0, 256);
            ino.word1      = RFS_INO1_PACK(INODE_TYPE_SOFT_SYM, 0, 0);
            ino.uid        = t.uid;
            ino.gid        = t.gid;
            ino.link_count = 1;
            ino.mode_xattr = RFS_MODE_XATTR_PACK(t.mode, RFS_XATTR_NONE);
            ino.size       = t.size;
            ino.mtime      = t.mtime;
            ino.ctime      = t.ctime;
            ino.atime      = t.atime;
            ino.data_root  = PTR_INV;
            memcpy(ino.inline_data, t.link.data(), t.link.size());
            im.write_inode(t.inode, &ino, 256);
        } else { /* K_DIR */
            raven_dir_Inode_t d;
            memset(&d, 0, 256);
            d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
            d.uid        = t.uid;
            d.gid        = t.gid;
            d.link_count = (uint32_t)(t.size + 2);
            d.mode_xattr = RFS_MODE_XATTR_PACK(t.mode, RFS_XATTR_NONE);
            d.size       = t.size;
            d.mtime      = t.mtime;
            d.ctime      = t.ctime;
            d.atime      = t.atime;
            d.data_root  = t.dir_root;
            for (int i = 0; i < RFS_DIR_HEAP_COUNT; i++)
                RFS_NHC_SET(*rfs_dir_heap(&d, i), 0, 0, NAME_HEAP_INACTIVE, 0, NAME_HEAP_INACTIVE);
            im.write_inode(t.inode, &d, 256);
        }
        apply_ops(t);
    }

    /* ── root 目录条目 + root inode ── */
    std::vector<Ent> root_ents;
    for (Tgt &t : T)
        if (t.name.find('/') == std::string::npos && t.kind != K_REG)
            root_ents.push_back({ t.name, t.inode });
    for (Tgt &t : T)
        if (t.name.find('/') == std::string::npos && t.kind == K_REG)
            root_ents.push_back({ t.name, t.inode });
    sort_ents(root_ents);
    std::vector<uint8_t> root_node;
    build_dentry_node(root_node, im.root_inode, im.root_inode, root_ents);
    im.wr(root_node.data(), root_node.size(), blkoff(0));
    {
        raven_dir_Inode_t d;
        im.read_inode(im.root_inode, &d, 256);
        d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
        d.size       = root_ents.size();
        d.link_count = (uint32_t)(root_ents.size() + 2);
        d.data_root  = 0;
        d.mode_xattr = RFS_MODE_XATTR_PACK(0755, RFS_XATTR_NONE);
        for (int i = 0; i < RFS_DIR_HEAP_COUNT; i++)
            RFS_NHC_SET(*rfs_dir_heap(&d, i), 0, 0, NAME_HEAP_INACTIVE, 0, NAME_HEAP_INACTIVE);
        im.write_inode(im.root_inode, &d, 256);
    }

    /* ── 落盘位图 ── */
    im.wr(im.bm.data(),  im.bm.size(),  blkoff(im.bitmap_start));
    im.wr(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    /* ═══════════════════ 清单输出 ═══════════════════ */
    auto write_attrs = [&](const char *path, bool post) -> bool {
        FILE *fp = fopen(path, "w");
        if (!fp) return false;
        fprintf(fp, "# ravenfs-m4.4-attrs v1%s\n", post ? " (post-op)" : "");
        fprintf(fp, "# A <inode> <kind:reg|dir|lnk> <size> <mode4oct> <uid> <gid> "
                    "<mtime_s> <mtime_ns> <atime_s> <atime_ns> <ctime_s> <ctime_ns> <path>\n");
        fprintf(fp, "# ctime = -1 → 不断言\n");
        for (Tgt &t : T) {
            char mbuf[24], abuf[24], cbuf[24];
            auto fmt = [](char *b, size_t n, uint64_t v) {
                if (v == (uint64_t)-1) snprintf(b, n, "-1");
                else snprintf(b, n, "%llu", (unsigned long long)v);
            };
            if (post) {
                fmt(mbuf, sizeof(mbuf), t.pmtime);
                fmt(abuf, sizeof(abuf), t.patime);
                fmt(cbuf, sizeof(cbuf), t.pctime);
                fprintf(fp, "A %llu %s %llu %04o %u %u %s %u %s %u %s %u %s\n",
                        (unsigned long long)t.inode, kind_str(t.kind), (unsigned long long)t.psize,
                        t.pmode & 07777u, t.puid, t.pgid,
                        mbuf, t.pmns, abuf, t.pans, cbuf, t.pcns, t.name.c_str());
            } else {
                fmt(mbuf, sizeof(mbuf), t.mtime);
                fmt(abuf, sizeof(abuf), t.atime);
                fmt(cbuf, sizeof(cbuf), t.ctime);
                fprintf(fp, "A %llu %s %llu %04o %u %u %s %u %s %u %s %u %s\n",
                        (unsigned long long)t.inode, kind_str(t.kind), (unsigned long long)t.size,
                        t.mode & 07777u, t.uid, t.gid,
                        mbuf, t.mns, abuf, t.ans, cbuf, t.cns, t.name.c_str());
            }
        }
        fclose(fp);
        return true;
    };

    if (!write_attrs(expect_path.c_str(), false))  { fprintf(stderr, "mkstates_a: 写 %s 失败\n", expect_path.c_str()); return 1; }
    if (!write_attrs(wexpect_path.c_str(), true))  { fprintf(stderr, "mkstates_a: 写 %s 失败\n", wexpect_path.c_str()); return 1; }

    /* 内容清单（M4.1 格式；仅 REG）—— 初始 + 操作后 */
    {
        std::vector<M41File> ci, cp;
        for (Tgt &t : T) {
            if (t.kind != K_REG) continue;
            ci.push_back(M41File{ t.name, t.inode, t.size, prov_segs(t.prov), "" });
            cp.push_back(M41File{ t.name, t.inode, t.psize, prov_segs(t.postprov), "" });
        }
        if (!m41_write_expect(cexpect_path.c_str(), ci)) { fprintf(stderr, "mkstates_a: 写 %s 失败\n", cexpect_path.c_str()); return 1; }
        if (!m41_write_expect(cpost_path.c_str(), cp))   { fprintf(stderr, "mkstates_a: 写 %s 失败\n", cpost_path.c_str()); return 1; }
    }

    /* 操作序列 plan */
    {
        FILE *fp = fopen(plan_path.c_str(), "w");
        if (!fp) { fprintf(stderr, "mkstates_a: 写 %s 失败\n", plan_path.c_str()); return 1; }
        fprintf(fp, "# ravenfs-m4.4-plan v1\n");
        fprintf(fp, "# M  <path> <mode_octal>       chmod <mode> <path>\n");
        fprintf(fp, "# O  <path> <uid> <gid>       chown <uid>:<gid> <path>\n");
        fprintf(fp, "# H  <path> <uid> <gid>       chown -h <uid>:<gid> <path>\n");
        fprintf(fp, "# D  <path> <sec> <ns>        touch -d @<sec>.<ns> <path>\n");
        fprintf(fp, "# DM <path> <sec> <ns>        touch -m -d @<sec>.<ns> <path>\n");
        fprintf(fp, "# DA <path> <sec> <ns>        touch -a -d @<sec>.<ns> <path>\n");
        fprintf(fp, "# DH <path> <sec> <ns>        touch -h -d @<sec>.<ns> <path>\n");
        fprintf(fp, "# T  <path> <size>            truncate -s <size> <path>\n");
        for (Tgt &t : T) {
            for (const AOp &o : t.ops) {
                switch (o.k) {
                case OP_CHMOD:     fprintf(fp, "M  %s %04o\n", t.name.c_str(), (unsigned)(o.a & 07777u)); break;
                case OP_CHOWN:     fprintf(fp, "O  %s %llu %llu\n", t.name.c_str(), (unsigned long long)o.a, (unsigned long long)o.b); break;
                case OP_LCHOWN:    fprintf(fp, "H  %s %llu %llu\n", t.name.c_str(), (unsigned long long)o.a, (unsigned long long)o.b); break;
                case OP_TIME_BOTH: fprintf(fp, "D  %s %llu %llu\n", t.name.c_str(), (unsigned long long)o.a, (unsigned long long)o.b); break;
                case OP_TIME_M:    fprintf(fp, "DM %s %llu %llu\n", t.name.c_str(), (unsigned long long)o.a, (unsigned long long)o.b); break;
                case OP_TIME_A:    fprintf(fp, "DA %s %llu %llu\n", t.name.c_str(), (unsigned long long)o.a, (unsigned long long)o.b); break;
                case OP_TIME_LH:   fprintf(fp, "DH %s %llu %llu\n", t.name.c_str(), (unsigned long long)o.a, (unsigned long long)o.b); break;
                case OP_TRUNC:     fprintf(fp, "T  %s %llu\n", t.name.c_str(), (unsigned long long)o.a); break;
                }
            }
        }
        fclose(fp);
    }

    printf("mkstates_a: expect   → %s（%zu 靶子，初始属性）\n", expect_path.c_str(), T.size());
    printf("mkstates_a: wexpect  → %s（操作后属性）\n", wexpect_path.c_str());
    printf("mkstates_a: plan     → %s\n", plan_path.c_str());
    printf("mkstates_a: cexpect  → %s（初始内容）\n", cexpect_path.c_str());
    printf("mkstates_a: cexpect.post → %s（操作后内容）\n", cpost_path.c_str());
    for (Tgt &t : T) {
        printf("  [%s] %-16s inode=%-3llu size=%-6llu mode=%04o uid=%u gid=%u "
               "ops=%zu post(mode=%04o size=%llu%s)\n",
               kind_str(t.kind), t.name.c_str(), (unsigned long long)t.inode,
               (unsigned long long)t.size, t.mode & 07777u, t.uid, t.gid, t.ops.size(),
               t.pmode & 07777u, (unsigned long long)t.psize,
               t.pctime == (uint64_t)-1 ? ",ctime=—" : "");
    }
    close(im.fd);
    printf("mkstates_a: done → %s\n", argv[2]);
    return 0;
}
