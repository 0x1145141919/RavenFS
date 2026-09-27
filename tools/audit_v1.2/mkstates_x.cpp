/* ═══════════════════════════════════════════════════════════════════════════
 * mkstates_x.cpp — M6（xattr / ACL / capability）**靶子构造器**
 *                  （mkstates_a.cpp 的姊妹件；写副本；不改 M4.1–M4.5 任何资产）
 *
 * 目的：造一个「**盘上已带各种 xattr 块**」的 v1.2 镜像 + 配套清单：
 *   · <out.img>           初始镜像（可挂载可读写；含 user.* / security.capability /
 *                         system.posix_acl_{access,default} / 无 xattr 对照 / inline+extent）
 *   · <out.img>.cexpect   内容清单（M4.1 格式；供 `readback verify` 自证 + 供 guest cmp）
 *   · <out.img>.xexpect   **xattr 期望清单**（M6 格式；供 `m6_xattr_dump verify` 自证）
 *
 * ── 靶子面（覆盖 M6 判据；见 Docs/linux/ravenfs-linux-ko-M6-acceptance.md）────────
 *   x_no.dat     REG inline 80    **无 xattr**（ptr 哨兵对照）
 *   x_inline.dat REG inline 100   user.m6key="hello"
 *   x_ext.dat    REG extent 8192  user.a / user.b（多短值）
 *   x_long.dat   REG inline 16    user.big = **4000B**（单块近满值；验 R-XA-04 单块上限内）
 *   x_cap.dat    REG inline 32    security.capability（vfs_cap_data 20B，CAP_NET_RAW）
 *   x_acl.dat    REG extent 4096  system.posix_acl_access（**5 条**：USER_OBJ/USER/GROUP_OBJ/MASK/OTHER）
 *   x_multi.dat  REG extent 12288 **多命名空间同块**：user.m6key + user.m6second +
 *                                 security.capability + system.posix_acl_access（最小 3 条）
 *   x_dir/       DIR  (1 子)      user.small + system.posix_acl_access + **system.posix_acl_default**
 *   x_dir/child.dat REG inline 40 **无 xattr**（默认 ACL 继承靶子的子项）
 *   x_link       LNK → x_ext.dat  **无 xattr**（符号链接，不挂 xattr）
 *
 * 盘面真值（disk_layout_v1.2_inode.md §5）：
 *   · inode `mode_xattr` 高 52 位 = xattr 块号（无 = 全 1 哨兵）；
 *   · xattr 块 = 1 个 FS_block(4096B)：32B 头 + 条目区（向高增长，首个 u32==0 终止）
 *     + 值区（自块尾向低堆叠，4B 对齐）；净可用 4060B，放不下 → -E2BIG（ko 行为）。
 *   · 命名空间 index 抄 ext4；**ACL 键即前缀 ⇒ 后缀为空**（e_name_len==0）。
 *
 * 纪律：**不改** mkstates*.cpp（既有）/ readback.cpp / m4*_*.h / audit_v1.2.cpp；
 *       只在副本上写（auditor 本身只读）。
 * 用法： ./mkstates_x <in_empty.img> <out.img> [prefix]
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "disk_struct_v1.2.h"
#include "magics.h"
#include "m41_expect.h"
#include "m42_fill.h"
#include "m6_xexpect.h"
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

static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

/* ── 两版 fill 公式一致性交叉校验（防 m42_fill.h 与 m41_expect.h 漂移）── */
static void assert_fill_consistent() {
    const char *names[] = { "", "a", "x_ext.dat", "x_long.dat", "x_dir/child.dat" };
    for (const char *nm : names)
        for (uint64_t off = 0; off < 5000; off += 97)
            if (m42_fill_byte(nm, off) != m41_fill_byte(nm, off)) {
                fprintf(stderr, "FATAL: m42_fill_byte != m41_fill_byte (name=%s off=%llu)\n",
                        nm, (unsigned long long)off);
                exit(2);
            }
}

/* ═══════════════════════════ 镜像读/写器（照抄 mkstates_a） ═══════════════════════════ */
struct Img {
    int fd = -1;
    uint64_t total_blocks = 0, inode_count = 0, root_inode = 0;
    uint64_t bitmap_blocks = 0, bitmap_start = 0, inode_blocks = 0, inode_array_base = 0;
    uint64_t inode_bm_blocks = 0, inode_bm_base = 0;
    std::vector<uint8_t> bm, ibm;
    uint64_t scan_cur = 2;

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
        fprintf(stderr, "mkstates_x: no free %llu contiguous blocks (align=%llu)\n",
                (unsigned long long)n, (unsigned long long)align);
        exit(1);
    }
    uint64_t alloc_one() {
        for (uint64_t b = scan_cur; b < total_blocks; b++)
            if (!blk_lit(b)) { mark_blk(b); scan_cur = b + 1; return b; }
        for (uint64_t b = 2; b < total_blocks; b++)
            if (!blk_lit(b)) { mark_blk(b); return b; }
        fprintf(stderr, "mkstates_x: no free block\n"); exit(1);
    }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkstates_x: no free inode\n"); exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
    bool read_inode(uint64_t idx, void *ino, size_t n) {
        return rd(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

/* ── 目录项（与 mkstates_a 同构；名字 ≤40B → 不进名字堆）── */
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

/* ── extent 叶写入（照抄 mkstates_a）── */
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

/* ═══════════════════════════ xattr 块构造 ═══════════════════════════ */
struct XEnt { uint8_t index; std::string name; std::vector<uint8_t> value; };

/* 造一块（≤4096B）；返回块缓冲。放不下 → exit(1)（构造期即暴露设计越限）。*/
static void build_xattr_block(std::vector<uint8_t> &blk, const std::vector<XEnt> &ents) {
    blk.assign(BLK, 0);
    auto *h = (rfs_xattr_block_hdr *)blk.data();
    h->h_magic = RFS_XATTR_H_MAGIC; h->h_refcount = 1; h->h_blocks = 1; h->h_hash = 0;

    /* 条目区：先布 offset */
    std::vector<uint64_t> entoff(ents.size(), 0);
    uint64_t off = RFS_XATTR_HDR_SIZE;
    for (size_t i = 0; i < ents.size(); i++) {
        entoff[i] = off;
        off += RFS_XATTR_LEN(ents[i].name.size());
    }
    uint64_t entry_end = off;

    /* 值区：自块尾向低堆叠，4B 对齐 */
    std::vector<uint64_t> voff(ents.size(), 0);
    uint64_t v = BLK, vmin = BLK;
    for (size_t i = 0; i < ents.size(); i++) {
        if (ents[i].value.empty()) { voff[i] = 0; continue; }
        uint64_t sz = ents[i].value.size();
        if (v < sz) { fprintf(stderr, "mkstates_x: xattr 值区溢出\n"); exit(1); }
        v = (v - sz) & ~3ULL;
        voff[i] = v;
        vmin = v;
    }
    if (vmin < entry_end) {
        fprintf(stderr, "mkstates_x: xattr 块放不下（entry_end=%llu vmin=%llu > 4060 净容）\n",
                (unsigned long long)entry_end, (unsigned long long)vmin);
        exit(1);
    }

    /* 落条目（头 + 名 + 值） */
    for (size_t i = 0; i < ents.size(); i++) {
        auto *xe = (rfs_xattr_entry *)(blk.data() + entoff[i]);
        xe->e_name_len   = (uint8_t)ents[i].name.size();
        xe->e_name_index = ents[i].index;
        xe->e_value_size = (uint32_t)ents[i].value.size();
        xe->e_value_offs = (uint16_t)voff[i];
        if (!ents[i].name.empty())
            memcpy(blk.data() + entoff[i] + RFS_XATTR_ENTRY_HDR_SIZE,
                   ents[i].name.data(), ents[i].name.size());
        if (!ents[i].value.empty())
            memcpy(blk.data() + voff[i], ents[i].value.data(), ents[i].value.size());
    }
    /* 终止符：entry_end 起保持 0（首个 u32 == 0）—— blk 已零初始化。 */
}

/* ═══════════════════════════ 靶子模型 ═══════════════════════════ */
enum Kind { K_REG, K_DIR, K_LNK };

struct Tgt {
    std::string name;               /* 根下名；子项为 "x_dir/child.dat" */
    Kind        kind = K_REG;
    uint64_t    inode = 0;
    uint64_t    size  = 0;
    uint32_t    mode  = 0644;
    uint32_t    uid = 0, gid = 0;

    std::vector<uint8_t> prov;      /* REG：1=填充（'D'） */
    std::string          link;      /* LNK */
    std::vector<Ent>     children;  /* DIR */
    uint64_t             dir_root = 0;
    std::vector<ExtRec>  recs;      /* REG extent */
    uint64_t             root = 0;

    std::vector<XEnt>    xents;     /* xattr 条目（空 = 无 xattr） */
    uint64_t             xptr = RFS_XATTR_NONE;
};

/* ═══════════════════════════ 值构造器 ═══════════════════════════ */
static std::vector<uint8_t> v_bytes(const std::string &s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
static std::vector<uint8_t> v_fill(const std::string &name, uint64_t n) {
    std::vector<uint8_t> b((size_t)n);
    for (uint64_t i = 0; i < n; i++) b[(size_t)i] = m42_fill_byte(name.c_str(), i);
    return b;
}
/* Linux vfs_cap_data（20B，VFS_CAP_REVISION_2；CAP_NET_RAW=13）小端 */
static std::vector<uint8_t> v_capability(void) {
    std::vector<uint8_t> b(20, 0);
    auto put32 = [&](size_t o, uint32_t w) {
        b[o+0] = (uint8_t)(w & 0xff); b[o+1] = (uint8_t)((w >> 8) & 0xff);
        b[o+2] = (uint8_t)((w >> 16) & 0xff); b[o+3] = (uint8_t)((w >> 24) & 0xff);
    };
    put32(0,  0x02000000u);          /* magic_etc: VFS_CAP_REVISION_2 */
    put32(4,  1u << 13);             /* data[0].permitted = CAP_NET_RAW */
    put32(8,  0u);                   /* data[0].inheritable */
    put32(12, 0u);                   /* data[1].permitted */
    put32(16, 0u);                   /* data[1].inheritable */
    return b;
}
/* POSIX ACL xattr 值（posix_acl_xattr_header + 条目 8B/条）小端 */
struct AclEnt { uint16_t tag; uint16_t perm; uint32_t id; };
static std::vector<uint8_t> v_acl(uint32_t version, const std::vector<AclEnt> &ents) {
    std::vector<uint8_t> b;
    auto put16 = [&](uint16_t w) { b.push_back((uint8_t)(w & 0xff)); b.push_back((uint8_t)((w >> 8) & 0xff)); };
    auto put32 = [&](uint32_t w) {
        b.push_back((uint8_t)(w & 0xff)); b.push_back((uint8_t)((w >> 8) & 0xff));
        b.push_back((uint8_t)((w >> 16) & 0xff)); b.push_back((uint8_t)((w >> 24) & 0xff));
    };
    put32(version);                  /* POSIX_ACL_XATTR_VERSION = 0x0002 */
    for (const AclEnt &e : ents) { put16(e.tag); put16(e.perm); put32(e.id); }
    return b;
}
static constexpr uint16_t ACL_UNDEF = 0xffffffffu & 0xffff;   /* id 未定义（存 u32 全 1） */
static constexpr uint16_t TAG_USER_OBJ  = 0x01, TAG_USER  = 0x02;
static constexpr uint16_t TAG_GROUP_OBJ = 0x04, TAG_GROUP = 0x08;
static constexpr uint16_t TAG_MASK      = 0x10, TAG_OTHER = 0x20;
static constexpr uint16_t P_R = 4, P_W = 2, P_X = 1;

static std::vector<uint8_t> acl_access_min(void) {   /* 3 条：USER_OBJ/GROUP_OBJ/OTHER（0644） */
    return v_acl(2, { {TAG_USER_OBJ, P_R|P_W, 0xffffffffu},
                      {TAG_GROUP_OBJ, P_R,    0xffffffffu},
                      {TAG_OTHER,     P_R,    0xffffffffu} });
}
static std::vector<uint8_t> acl_access_5(void) {     /* 5 条：含命名 USER + MASK */
    return v_acl(2, { {TAG_USER_OBJ,  P_R|P_W, 0xffffffffu},
                      {TAG_USER,      P_R|P_W, 1000u},
                      {TAG_GROUP_OBJ, P_R,     0xffffffffu},
                      {TAG_MASK,      P_R|P_W, 0xffffffffu},
                      {TAG_OTHER,     P_R,     0xffffffffu} });
}
static std::vector<uint8_t> acl_default_dir(void) {  /* 默认 ACL 3 条：USER_OBJ/GROUP_OBJ/OTHER */
    return v_acl(2, { {TAG_USER_OBJ,  P_R|P_W|P_X, 0xffffffffu},
                      {TAG_GROUP_OBJ, P_R|P_X,     0xffffffffu},
                      {TAG_OTHER,     P_R|P_X,     0xffffffffu} });
}

/* ═══════════════════════════ main ═══════════════════════════ */
int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <in_empty.img> <out.img> [prefix]\n", argv[0]);
        return 1;
    }
    assert_fill_consistent();
    std::string base = (argc >= 4) ? argv[3] : std::string(argv[2]);
    std::string cexpect_path = base + ".cexpect";
    std::string xexpect_path = base + ".xexpect";

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

    printf("mkstates_x: geo total_blocks=%llu inode_count=%llu root=%llu "
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

    auto write_block = [&](uint64_t phys, const char *nm, uint64_t foff,
                           const std::vector<uint8_t> &prov, uint64_t size) {
        std::vector<uint8_t> b(BLK, 0);
        for (uint64_t j = 0; j < BLK; j++)
            if (foff + j < size && prov[(size_t)(foff + j)]) b[(size_t)j] = m42_fill_byte(nm, foff + j);
        im.wr(b.data(), b.size(), blkoff(phys));
    };
    /* 造一个 REG 靶子：size≤160 → inline；否则 extent 单叶根 */
    auto mk_reg = [&](const char *nm, uint64_t size) -> Tgt & {
        Tgt &t = add(nm, K_REG);
        t.size = size;
        t.prov.assign((size_t)size, 1);
        if (size > 160) {
            uint64_t blocks = (size + BLK - 1) / BLK;
            uint64_t run = im.alloc_run(blocks, 1);
            t.recs.push_back({0, blocks, run});
            uint64_t nb = im.alloc_run(2, 2);
            write_extent_leaf(im, nb, t.inode, true, 0, t.recs);
            t.root = nb;
            for (const ExtRec &r : t.recs)
                for (uint64_t k = 0; k < r.len; k++)
                    write_block(r.phys + k, t.name.c_str(), (r.fblk + k) * BLK, t.prov, t.size);
        }
        return t;
    };
    auto set_x = [&](Tgt &t, const std::vector<XEnt> &ents) {
        t.xents = ents;
        if (ents.empty()) return;
        std::vector<uint8_t> blk;
        build_xattr_block(blk, ents);
        uint64_t xb = im.alloc_one();
        im.wr(blk.data(), blk.size(), blkoff(xb));
        t.xptr = xb;
    };

    /* ═══════════════════ 靶子 ═══════════════════ */
    mk_reg("x_no.dat", 80);                                   /* 无 xattr 对照 */

    { Tgt &t = mk_reg("x_inline.dat", 100);
      set_x(t, { { RFS_XATTR_INDEX_USER, "m6key", v_bytes("hello") } }); }

    { Tgt &t = mk_reg("x_ext.dat", 8192);
      set_x(t, { { RFS_XATTR_INDEX_USER, "a", v_bytes("AAA") },
                 { RFS_XATTR_INDEX_USER, "b", v_bytes("BBBBBBBB") } }); }

    { Tgt &t = mk_reg("x_long.dat", 16);
      set_x(t, { { RFS_XATTR_INDEX_USER, "big", v_fill("x_long.dat", 4000) } }); }

    { Tgt &t = mk_reg("x_cap.dat", 32);
      set_x(t, { { RFS_XATTR_INDEX_SECURITY, "capability", v_capability() } }); }

    { Tgt &t = mk_reg("x_acl.dat", 4096);
      set_x(t, { { RFS_XATTR_INDEX_POSIX_ACL_ACCESS, "", acl_access_5() } }); }

    { Tgt &t = mk_reg("x_multi.dat", 12288);
      set_x(t, { { RFS_XATTR_INDEX_USER, "m6key", v_bytes("hello") },
                 { RFS_XATTR_INDEX_USER, "m6second", v_bytes("world!") },
                 { RFS_XATTR_INDEX_SECURITY, "capability", v_capability() },
                 { RFS_XATTR_INDEX_POSIX_ACL_ACCESS, "", acl_access_min() } }); }

    /* 子目录内容（先分配子项 inode） */
    std::vector<Ent> dir_children;
    { Tgt &t = mk_reg("x_dir/child.dat", 40);
      dir_children.push_back({ "child.dat", t.inode }); }
    sort_ents(dir_children);

    { Tgt &t = add("x_dir", K_DIR);
      t.mode = 0755;
      t.children = dir_children;
      t.size = t.children.size();
      uint64_t dblk = im.alloc_run(2, 2);
      t.dir_root = dblk;
      std::vector<uint8_t> node;
      build_dentry_node(node, t.inode, im.root_inode, t.children);
      im.wr(node.data(), node.size(), blkoff(dblk));
      set_x(t, { { RFS_XATTR_INDEX_USER, "small", v_bytes("abc") },
                 { RFS_XATTR_INDEX_POSIX_ACL_ACCESS, "", acl_access_min() },
                 { RFS_XATTR_INDEX_POSIX_ACL_DEFAULT, "", acl_default_dir() } }); }

    { Tgt &t = add("x_link", K_LNK);
      t.link = "x_ext.dat"; t.size = t.link.size(); t.mode = 0777; }

    /* ── 落盘每个靶子 inode ── */
    for (Tgt &t : T) {
        if (t.kind == K_REG) {
            raven_Inode_t ino; memset(&ino, 0, 256);
            uint32_t direct = 0; uint64_t droot = PTR_INV;
            if (t.size > 160) { droot = t.root; direct = 0; }
            ino.word1      = RFS_INO1_PACK(INODE_TYPE_REGULAR, direct, 0);
            ino.uid = t.uid; ino.gid = t.gid; ino.link_count = 1;
            ino.mode_xattr = RFS_MODE_XATTR_PACK(t.mode, t.xptr);
            ino.size = t.size;
            ino.mtime = ino.ctime = ino.atime = 1700000000ULL;
            ino.data_root = droot;
            if (t.size <= 160)
                for (uint64_t i = 0; i < t.size; i++)
                    ino.inline_data[i] = t.prov[(size_t)i] ? m42_fill_byte(t.name.c_str(), i) : 0;
            im.write_inode(t.inode, &ino, 256);
        } else if (t.kind == K_LNK) {
            raven_Inode_t ino; memset(&ino, 0, 256);
            ino.word1      = RFS_INO1_PACK(INODE_TYPE_SOFT_SYM, 0, 0);
            ino.uid = t.uid; ino.gid = t.gid; ino.link_count = 1;
            ino.mode_xattr = RFS_MODE_XATTR_PACK(t.mode, t.xptr);
            ino.size = t.size;
            ino.mtime = ino.ctime = ino.atime = 1700000000ULL;
            ino.data_root = PTR_INV;
            memcpy(ino.inline_data, t.link.data(), t.link.size());
            im.write_inode(t.inode, &ino, 256);
        } else {
            raven_dir_Inode_t d; memset(&d, 0, 256);
            d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
            d.uid = t.uid; d.gid = t.gid;
            d.link_count = (uint32_t)(t.size + 2);
            d.mode_xattr = RFS_MODE_XATTR_PACK(t.mode, t.xptr);
            d.size = t.size;
            d.mtime = d.ctime = d.atime = 1700000000ULL;
            d.data_root = t.dir_root;
            for (int i = 0; i < RFS_DIR_HEAP_COUNT; i++)
                RFS_NHC_SET(*rfs_dir_heap(&d, i), 0, 0, NAME_HEAP_INACTIVE, 0, NAME_HEAP_INACTIVE);
            im.write_inode(t.inode, &d, 256);
        }
    }

    /* ── root 目录条目 + root inode ── */
    std::vector<Ent> root_ents;
    for (Tgt &t : T)
        if (t.name.find('/') == std::string::npos && t.kind != K_REG) root_ents.push_back({ t.name, t.inode });
    for (Tgt &t : T)
        if (t.name.find('/') == std::string::npos && t.kind == K_REG) root_ents.push_back({ t.name, t.inode });
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

    /* ── 位图落盘 ── */
    im.wr(im.bm.data(),  im.bm.size(),  blkoff(im.bitmap_start));
    im.wr(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    /* ═══════════════════ 清单 ═══════════════════ */
    std::vector<M41File> cfiles;
    std::vector<XExpectIno> xfiles;
    for (Tgt &t : T) {
        if (t.kind == K_REG)
            cfiles.push_back(M41File{ t.name, t.inode, t.size,
                                      std::vector<M41Seg>{ { 0, t.size, 'D' } }, "" });
        XExpectIno x; x.path = t.name; x.inode = t.inode;
        x.type = t.xents.empty() ? 0 : 1;
        for (const XEnt &e : t.xents) {
            XExpectEntry xe;
            xe.index = e.index; xe.name = e.name;
            xe.size  = (uint32_t)e.value.size();
            xe.hash  = e.value.empty() ? xexp_hash("", 0)
                                       : xexp_hash(e.value.data(), e.value.size());
            x.ents.push_back(xe);
        }
        xfiles.push_back(x);
    }
    if (!m41_write_expect(cexpect_path.c_str(), cfiles)) { fprintf(stderr, "mkstates_x: 写 %s 失败\n", cexpect_path.c_str()); return 1; }
    if (!m6_write_xexpect(xexpect_path.c_str(), xfiles)) { fprintf(stderr, "mkstates_x: 写 %s 失败\n", xexpect_path.c_str()); return 1; }

    printf("mkstates_x: cexpect → %s（%zu REG 内容）\n", cexpect_path.c_str(), cfiles.size());
    printf("mkstates_x: xexpect → %s（%zu inode）\n", xexpect_path.c_str(), xfiles.size());
    printf("  xattr_blocks 期望 = %llu\n",
           (unsigned long long)std::count_if(T.begin(), T.end(),
               [](const Tgt &t) { return !t.xents.empty(); }));
    for (Tgt &t : T) {
        printf("  [%s] %-18s inode=%-3llu size=%-6llu xattr=%s",
               t.kind == K_REG ? "reg" : (t.kind == K_DIR ? "dir" : "lnk"),
               t.name.c_str(), (unsigned long long)t.inode, (unsigned long long)t.size,
               t.xents.empty() ? "none" : "yes");
        if (!t.xents.empty()) {
            printf("  (block=%llu, %zu 条:", (unsigned long long)t.xptr, t.xents.size());
            for (const XEnt &e : t.xents)
                printf(" idx%u/%s=%zuB", e.index, e.name.empty() ? "•" : e.name.c_str(), e.value.size());
            printf(")");
        }
        printf("\n");
    }
    close(im.fd);
    printf("mkstates_x: done → %s\n", argv[2]);
    return 0;
}
