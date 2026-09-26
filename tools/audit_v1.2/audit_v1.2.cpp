/* ═══════════════════════════════════════════════════════════════════════════
 * audit_v1.2 — RavenFS v1.2 递归只读校验员（host 侧独立执行）
 *
 * 契约真值来源：
 *   · include/disk_struct_v1.2.h  (盘面头，唯一真值；掩码+宏)
 *   · include/magics.h
 *   · Docs/disk_layout_v1.2_inode.md  (v1.2 inode 详版)
 *   · Docs/disk_layout_v1.1.md        (§4 B+tree / §6 dentry / §7 名字堆 / §10 不变量)
 *   · mkfs/mkfs.cpp  (几何 calc_geometry，逐式一致)
 * 参考：juvenile_god/audit.cpp（v1.1 单 inode readonly audit 逻辑；本工具不链接它）
 *
 * 纪律：
 *   · 只读（O_RDONLY；无 pread 之外写路径；不建副文件）
 *   · 日志只走 stdout（stderr 仅致命错误）
 *   · 递归先序遍历（pre-order DFS），带已访问 inode 集合 + 目录祖先栈（环检测）
 *
 * 编译： g++ -Wall -Wextra -O2 -g -I../../include audit_v1.2.cpp -o audit_v1.2 -lxxhash
 * 运行： ./audit_v1.2 <image-or-device>
 * 退出码： 0 = 无 BAD；1 = 有 BAD；2 = 用法 / IO 错误
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <disk_struct_v1.2.h>
#include <magics.h>

#include <xxhash.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#ifndef BLKGETSIZE64
#  define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

/* ─────────────────────────── 编译期常量 ─────────────────────────── */
static constexpr uint64_t FS_BLOCK_BYTES = 4096;   /* 1 个 FS_block 字节数 */
static constexpr uint64_t NODE_BYTES      = 8192;  /* B+tree 节点 = 2 FS_block */
static constexpr uint64_t PTR52           = (1ULL << 52) - 1;
static constexpr uint64_t PTR_INV         = ~0ULL;

static constexpr uint16_t DENTRY_ORDER = 126;      /* dentry 树阶数（count <= 126） */
static constexpr uint16_t EXTENT_ORDER = 337;      /* extent 树阶数 */

/* 名字堆静态几何（与 name_heap.cpp / raven_dir_Inode_t 字段序一致；v1.1 §7.4） */
static const uint8_t HEAP_GRAN[10] = { 3, 3, 3, 6, 6, 7, 7, 8, 8, 9 };
static const uint8_t HEAP_BITS[10] = { 16, 17, 18, 16, 17, 16, 17, 16, 17, 16 };

/* ═══════════════════════════ CRC32（同 mkfs：反射 0xEDB88320） ═══════════════════════════ */
static uint32_t crc32_tab[256];
static bool     crc32_tab_init = false;
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
        crc32_tab[i] = c;
    }
    crc32_tab_init = true;
}
static uint32_t crc32_calc(const void *data, size_t len) {
    if (!crc32_tab_init) crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        c = crc32_tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ═══════════════════════════ 小工具 ═══════════════════════════ */
static inline void bit_set(std::vector<uint8_t> &v, uint64_t i)  { v[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline bool bit_test(const std::vector<uint8_t> &v, uint64_t i) { return (v[i >> 3] >> (i & 7)) & 1u; }
static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd_le64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v;
}

static const char *type_name(uint32_t t) {
    switch (t) {
    case INODE_TYPE_FIFO:    return "FIFO";
    case INODE_TYPE_CHR:     return "CHR";
    case INODE_TYPE_DIR:     return "DIR";
    case INODE_TYPE_BLK:     return "BLK";
    case INODE_TYPE_REGULAR: return "REG";
    case INODE_TYPE_SOFT_SYM:return "LNK";
    case INODE_TYPE_SOCK:    return "SOCK";
    default:                 return "??";
    }
}
static bool type_valid(uint32_t t) {
    return t == INODE_TYPE_FIFO || t == INODE_TYPE_CHR || t == INODE_TYPE_DIR ||
           t == INODE_TYPE_BLK  || t == INODE_TYPE_REGULAR || t == INODE_TYPE_SOFT_SYM ||
           t == INODE_TYPE_SOCK;
}

/* ═══════════════════════════ 审计器 ═══════════════════════════ */
struct HeapRegion {
    uint64_t data_lo, data_hi;
    uint8_t  gran_log2;
    uint64_t bmp_base;
    uint64_t total_bits;
};

struct DentEntry {
    std::string name;
    uint64_t    hash;
    uint64_t    inode;
};

/* 目录树的一个叶子（叶链校验用；按 DFS 中序收集） */
struct LeafLink {
    uint64_t blk;
    uint64_t ptr0, ptr1, ptr2;   /* ptrs[0..2] 的 52 位地址切片 */
    bool     is_root;
};

struct Auditor {
    /* 设备 */
    int      fd = -1;
    uint64_t dev_bytes = 0;

    /* SB */
    RavenSuperblock sb{};

    /* 几何（FS_block 世界） */
    uint64_t total_blocks = 0;
    uint64_t inode_count  = 0;
    uint64_t root_inode   = 0;
    uint64_t bitmap_blocks = 0, bitmap_start = 0;
    uint64_t inode_blocks  = 0, inode_array_base = 0;
    uint64_t inode_bm_blocks = 0, inode_bm_base = 0;

    /* 位图镜像 */
    std::vector<uint8_t> bmp;      /* block bitmap（bitmap_blocks*4096 B） */
    std::vector<uint8_t> ibmp;     /* inode bitmap（inode_bm_blocks*4096 B） */

    /* 认领账本 */
    std::vector<uint8_t>  claim;
    std::vector<uint64_t> claim_owner;
    bool                  track_owner = true;

    /* 引用账本 */
    std::vector<uint32_t> ref_count;      /* dentry 引用数（对 link_count） */
    std::vector<uint8_t>  reachable;      /* 可达 inode 位图 */
    std::vector<uint64_t> parent_of;      /* 首次引用者目录 */
    std::vector<uint64_t> dotdot_of;      /* dir 树根 ptrs[1]（'..'） */

    std::unordered_set<uint64_t> visited_inode;
    std::unordered_set<uint64_t> visited_node;
    std::vector<uint64_t>        ancestor_stack;   /* 当前目录祖先链 */

    std::vector<uint64_t> backup_blocks;

    std::vector<HeapRegion> cur_heaps;    /* 当前目录激活堆数据区 */
    std::vector<LeafLink>   cur_leaf_links;/* 当前目录叶链（DFS 中序） */

    /* 计数 */
    uint64_t n_ok = 0, n_bad = 0, n_warn = 0, n_info = 0;
    uint64_t c_dir = 0, c_reg = 0, c_lnk = 0, c_fifo = 0, c_sock = 0, c_chr = 0, c_blk = 0;
    uint64_t c_dentry_nodes = 0, c_dentry_entries = 0, c_dentry_leaves = 0;
    uint64_t c_extent_nodes = 0;
    uint64_t c_double_claim = 0, c_leaked = 0, c_orphan_blk = 0, c_orphan_ino = 0;
    uint64_t c_dangling = 0, c_link_mismatch = 0, c_dir_cycle = 0, c_heap_bad = 0;
    uint64_t c_chain_bad = 0;             /* v1.2 新增：叶链校验失败数 */
    uint64_t c_xattr = 0;

    /* ───────── 日志（只 stdout） ───────── */
    void emit(const char *lvl, const char *fmt, va_list ap) {
        char line[2048];
        int n = snprintf(line, sizeof(line), "[%s] ", lvl);
        if (n < 0) n = 0;
        vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
        fputs(line, stdout);
        fputc('\n', stdout);
    }
    void ok(const char *fmt, ...)   { n_ok++;   va_list ap; va_start(ap, fmt); emit("OK",   fmt, ap); va_end(ap); }
    void warn(const char *fmt, ...) { n_warn++; va_list ap; va_start(ap, fmt); emit("WARN", fmt, ap); va_end(ap); }
    void info(const char *fmt, ...) { n_info++; va_list ap; va_start(ap, fmt); emit("INFO", fmt, ap); va_end(ap); }
    void bad(const char *fmt, ...)  { n_bad++;  va_list ap; va_start(ap, fmt); emit("BAD",  fmt, ap); va_end(ap); }

    /* ───────── I/O ───────── */
    uint64_t blkoff(uint64_t b) const { return FS_BLOCK_BYTES * (b + 1); }

    bool pread_full(void *buf, size_t n, uint64_t off) {
        uint8_t *p = (uint8_t *)buf;
        size_t done = 0;
        while (done < n) {
            ssize_t r = pread(fd, p + done, n - done, (off_t)(off + done));
            if (r <= 0) return false;
            done += (size_t)r;
        }
        return true;
    }
    bool read_block(uint64_t b, void *buf) { return pread_full(buf, FS_BLOCK_BYTES, blkoff(b)); }
    bool read_node(uint64_t b, void *buf)  { return pread_full(buf, NODE_BYTES, blkoff(b)); }
    bool read_bytes(uint64_t devoff, void *buf, size_t n) { return pread_full(buf, n, devoff); }

    bool blk_lit(uint64_t b) const { return b < total_blocks ? bit_test(bmp, b) : false; }
    bool ino_lit(uint64_t i) const { return i < inode_count  ? bit_test(ibmp, i) : false; }

    bool read_inode_raw(uint64_t idx, uint8_t *buf256) {
        return read_bytes(blkoff(inode_array_base) + idx * 256, buf256, 256);
    }

    /* ───────── 认领 ───────── */
    bool claim_test(uint64_t b) const { return b < total_blocks ? bit_test(claim, b) : false; }
    void claim_block(uint64_t b, uint64_t owner, const char *kind) {
        if (b >= total_blocks) { bad("block %llu 超出 total_blocks（owner=inode %llu %s）",
                                     (unsigned long long)b, (unsigned long long)owner, kind); return; }
        if (claim_test(b)) {
            c_double_claim++;
            bad("block %llu 双重认领（owner=inode %llu kind=%s；先前 owner=inode %llu）",
                (unsigned long long)b, (unsigned long long)owner, kind,
                (unsigned long long)(track_owner ? claim_owner[b] : PTR_INV));
        } else {
            bit_set(claim, b);
            if (track_owner) claim_owner[b] = owner;
        }
    }

    /* ───────── 名字堆 ───────── */
    int heap_bit(const HeapRegion &r, uint64_t bit) {
        if (bit >= r.total_bits) return -1;
        uint64_t byte = bit >> 3;
        uint64_t blk = r.bmp_base + byte / 4096;
        uint64_t inb = byte % 4096;
        uint8_t buf[4096];
        if (!read_block(blk, buf)) return -1;
        return (buf[inb] >> (bit & 7)) & 1;
    }

    void parse_heaps(uint64_t inum, const raven_dir_Inode_t *d) {
        cur_heaps.clear();
        for (int s = 0; s < (int)RFS_DIR_HEAP_COUNT; s++) {
            const name_heap_controller_t *cp = rfs_dir_heap_c(d, (unsigned)s);
            if (!cp) continue;
            const name_heap_controller_t &c = *cp;
            uint64_t bmpb = RFS_NHC_BITMAP(c);
            uint64_t datab = RFS_NHC_DATA(c);
            uint8_t  gran = (uint8_t)RFS_NHC_GRAN(c);
            uint8_t  bits = (uint8_t)RFS_NHC_BITS(c);
            bool inactive = (bmpb == NAME_HEAP_INACTIVE && datab == NAME_HEAP_INACTIVE);
            if (inactive) continue;
            if (bmpb == NAME_HEAP_INACTIVE || datab == NAME_HEAP_INACTIVE) {
                c_heap_bad++;
                bad("DIR inode %llu heap[%d] 半激活（bmp=0x%llx data=0x%llx）",
                    (unsigned long long)inum, s, (unsigned long long)bmpb, (unsigned long long)datab);
                continue;
            }
            if (gran != HEAP_GRAN[s] || bits != HEAP_BITS[s]) {
                c_heap_bad++;
                warn("DIR inode %llu heap[%d] log2 非规范 gran=%u bits=%u（期望 %u/%u）",
                     (unsigned long long)inum, s, gran, bits, (unsigned)HEAP_GRAN[s], (unsigned)HEAP_BITS[s]);
            }
            uint64_t bmp_blks  = (1ULL << bits) / 32768;
            uint64_t data_blks = 1ULL << (bits + gran - 12);
            if (bmpb >= total_blocks || datab >= total_blocks ||
                bmpb + bmp_blks > total_blocks || datab + data_blks > total_blocks) {
                c_heap_bad++;
                bad("DIR inode %llu heap[%d] 区间越界 bmp=[%llu,+%llu) data=[%llu,+%llu)",
                    (unsigned long long)inum, s, (unsigned long long)bmpb, (unsigned long long)bmp_blks,
                    (unsigned long long)datab, (unsigned long long)data_blks);
                continue;
            }
            for (uint64_t k = 0; k < bmp_blks;  k++) claim_block(bmpb + k,  inum, "heap_bmp");
            for (uint64_t k = 0; k < data_blks; k++) claim_block(datab + k, inum, "heap_data");

            uint64_t used = 0;
            {
                std::vector<uint8_t> bm((size_t)bmp_blks * 4096, 0);
                if (!pread_full(bm.data(), bm.size(), blkoff(bmpb))) {
                    c_heap_bad++;
                    bad("DIR inode %llu heap[%d] 位图读取失败", (unsigned long long)inum, s);
                } else {
                    for (uint8_t v : bm) used += (uint64_t)__builtin_popcount(v);
                }
            }
            uint64_t total_bits = 1ULL << bits;
            info("DIR inode %llu heap[%d] gran=%uB bits_log2=%u bmp=[%llu,+%llu) data=[%llu,+%llu) 占用 %llu/%llu",
                 (unsigned long long)inum, s, 1u << gran, bits,
                 (unsigned long long)bmpb, (unsigned long long)bmp_blks,
                 (unsigned long long)datab, (unsigned long long)data_blks,
                 (unsigned long long)used, (unsigned long long)total_bits);

            HeapRegion r; r.data_lo = datab; r.data_hi = datab + data_blks;
            r.gran_log2 = gran; r.bmp_base = bmpb; r.total_bits = total_bits;
            cur_heaps.push_back(r);
        }
    }

    /* 读长名尾部（>40）；返回 false = 失败 */
    bool read_long_name(const Dentry_t &e, uint16_t name_len, char *full) {
        uint64_t ptr  = e.full_char_remain_ptr;
        uint64_t fblk = ptr >> 12;
        uint32_t off  = (uint32_t)(ptr & 0xFFF);
        uint16_t tail = (uint16_t)(name_len - DENTRY_INLINE_NAME_MAX);
        uint64_t devoff = blkoff(fblk) + off;
        if (fblk >= total_blocks) return false;
        if (!read_bytes(devoff, full + DENTRY_INLINE_NAME_MAX, tail)) return false;
        return true;
    }

    /* ═══════════════════ 入口 ═══════════════════ */
    int run(const char *path);

    /* ═══════════════════ inode 审计 ═══════════════════ */
    void audit_inode_dispatch(uint64_t inum);
    void audit_dir(uint64_t inum, const uint8_t *raw);
    void audit_file(uint64_t inum, const uint8_t *raw);
    void audit_xattr(uint64_t inum, uint64_t xptr);

    /* B+tree */
    uint64_t dentry_node(uint64_t blk, uint64_t owner, uint64_t expect_parent,
                         bool expect_root, int depth, std::vector<DentEntry> &out, bool &ok);
    void audit_dentry_tree(uint64_t root, uint64_t owner, std::vector<DentEntry> &out);
    void check_leaf_chain(uint64_t inum);   /* v1.2 新增：叶链校验 */
    uint64_t extent_node(uint64_t blk, uint64_t owner, uint64_t expect_parent,
                         bool expect_root, int depth, uint64_t size_bound, bool &ok);
    void audit_extent_tree(uint64_t root, uint64_t owner, uint64_t size_bound);

    /* 收口 */
    void load_backup_blocks();
    void reconcile_blocks();
    void reconcile_inodes();
    void summary() const;
};

/* ─────────────────────── B+tree：dentry ─────────────────────── */
uint64_t Auditor::dentry_node(uint64_t blk, uint64_t owner, uint64_t expect_parent,
                              bool expect_root, int depth, std::vector<DentEntry> &out, bool &ok)
{
    ok = false;
    if (depth > 64)          { bad("dentry tree inum=%llu depth 超限 (%d) blk=%llu", (unsigned long long)owner, depth, (unsigned long long)blk); return 0; }
    if (blk == PTR_INV || blk == PTR52 || blk >= total_blocks || (blk & 1)) {
        bad("dentry tree inum=%llu 非法节点块号 %llu", (unsigned long long)owner, (unsigned long long)blk);
        return 0;
    }
    if (visited_node.count(blk)) {
        bad("dentry tree inum=%llu blk=%llu 节点重入（环/共享）", (unsigned long long)owner, (unsigned long long)blk);
        return 0;
    }
    visited_node.insert(blk);
    claim_block(blk, owner, "dentry_node");
    if (blk + 1 < total_blocks) claim_block(blk + 1, owner, "dentry_node");

    std::vector<uint8_t> nbuf(NODE_BYTES);
    if (!read_node(blk, nbuf.data())) { bad("dentry tree blk=%llu 读取失败", (unsigned long long)blk); return 0; }
    const Bptree_meta_t *meta = (const Bptree_meta_t *)nbuf.data();

    if (meta->magics[0] != DENTRY_MAGIC1 || meta->magics[1] != DENTRY_MAGIC2 ||
        meta->magics[2] != DENTRY_MAGIC3 || meta->magics[3] != DENTRY_MAGIC4) {
        bad("dentry tree inum=%llu blk=%llu magic 不符（非 DENTRY 节点）", (unsigned long long)owner, (unsigned long long)blk);
        return 0;
    }

    uint32_t cnt = RFS_PP_COUNT(meta->parent_ptr);
    uint32_t inner = RFS_PP_IS_INNER(meta->parent_ptr);
    uint32_t isroot = RFS_PP_IS_ROOT(meta->parent_ptr);
    uint64_t ptr = RFS_PP_PTR(meta->parent_ptr);
    c_dentry_nodes++;

    if ((bool)isroot != expect_root)
        bad("dentry tree inum=%llu blk=%llu is_root=%u 期望 %d", (unsigned long long)owner, (unsigned long long)blk, isroot, (int)expect_root);
    if (expect_root) {
        if (ptr != owner) {
            /* v1.2 根节点 '.' = 自身 inode 号；mkfs 仍写 0（v1.1 残留）→ WARN，否则 BAD */
            if (ptr == 0 && owner == root_inode)
                warn("dentry tree root blk=%llu '.' ptr=0 != root inode %llu（v1.1 mkfs 残留，非 v1.2 规范）",
                     (unsigned long long)blk, (unsigned long long)owner);
            else
                bad("dentry tree inum=%llu root blk=%llu '.' ptr=%llu != 自身", (unsigned long long)owner, (unsigned long long)blk, (unsigned long long)ptr);
        }
    } else {
        if (ptr != expect_parent)
            bad("dentry tree inum=%llu blk=%llu parent_ptr=%llu != 实际父 %llu", (unsigned long long)owner, (unsigned long long)blk, (unsigned long long)ptr, (unsigned long long)expect_parent);
    }
    if (cnt > DENTRY_ORDER)
        bad("dentry tree inum=%llu blk=%llu count=%u > 阶 %u（瞬时槽落盘？）", (unsigned long long)owner, (unsigned long long)blk, cnt, (unsigned)DENTRY_ORDER);

    ok = true;

    if (!inner) {
        c_dentry_leaves++;
        cur_leaf_links.push_back(LeafLink{ blk,
            RFS_PTR_ADDR(meta->ptrs[0]), RFS_PTR_ADDR(meta->ptrs[1]),
            RFS_PTR_ADDR(meta->ptrs[2]), (bool)isroot });
        const Bptree_dentry_leaf_node_t *leaf = (const Bptree_dentry_leaf_node_t *)nbuf.data();
        for (uint32_t i = 0; i < cnt; i++) {
            const Dentry_t &e = leaf->entries[i];
            uint16_t nlen = (uint16_t)RFS_PTR_FREE(e.inode_ptr);
            uint64_t target = RFS_PTR_ADDR(e.inode_ptr);

            if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN) {
                bad("dentry leaf inum=%llu blk=%llu entries[%u] name_len=%u 非法", (unsigned long long)owner, (unsigned long long)blk, i, nlen);
                continue;
            }
            std::string name;
            name.resize(nlen);
            uint16_t ncopy = nlen < DENTRY_INLINE_NAME_MAX ? nlen : (uint16_t)DENTRY_INLINE_NAME_MAX;
            memcpy(&name[0], e.name, ncopy);
            bool name_ok = true;
            if (nlen > DENTRY_INLINE_NAME_MAX) {
                if (e.full_char_remain_ptr == PTR_INV || (e.full_char_remain_ptr >> 12) == PTR52) {
                    c_heap_bad++;
                    bad("dentry leaf inum=%llu blk=%llu entries[%u] 长名(name_len=%u)但 remain 无效=%#llx",
                        (unsigned long long)owner, (unsigned long long)blk, i, nlen, (unsigned long long)e.full_char_remain_ptr);
                    name_ok = false;
                } else {
                    uint64_t fblk = e.full_char_remain_ptr >> 12;
                    const HeapRegion *hit = nullptr;
                    for (auto &r : cur_heaps) if (fblk >= r.data_lo && fblk < r.data_hi) { hit = &r; break; }
                    if (!hit) {
                        c_heap_bad++;
                        bad("dentry leaf inum=%llu blk=%llu entries[%u] 长名 remain=%#llx 不落本目录任何激活堆数据区",
                            (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)e.full_char_remain_ptr);
                        name_ok = false;
                    } else if (!read_long_name(e, nlen, &name[0])) {
                        c_heap_bad++;
                        bad("dentry leaf inum=%llu blk=%llu entries[%u] 长名尾部读取失败", (unsigned long long)owner, (unsigned long long)blk, i);
                        name_ok = false;
                    } else {
                        /* 堆位图置位校验（连续单元） */
                        uint16_t tail = (uint16_t)(nlen - DENTRY_INLINE_NAME_MAX);
                        uint16_t units = (tail <= 40) ? (uint16_t)((tail + 7) / 8) : 1;
                        uint64_t sbyte = (fblk - hit->data_lo) * 4096 + (e.full_char_remain_ptr & 0xFFF);
                        uint64_t bit0 = sbyte >> hit->gran_log2;
                        for (uint16_t u = 0; u < units; u++) {
                            if (heap_bit(*hit, bit0 + u) != 1) {
                                c_heap_bad++;
                                bad("dentry leaf inum=%llu blk=%llu entries[%u] 长名堆位图未置位 (bit=%llu)", 
                                    (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)(bit0 + u));
                                break;
                            }
                        }
                    }
                }
            } else {
                if (e.full_char_remain_ptr != PTR_INV)
                    warn("dentry leaf inum=%llu blk=%llu entries[%u] name_len<=40 但 remain 非 INVALID=%#llx（软）",
                         (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)e.full_char_remain_ptr);
            }

            /* hash 重算（XXH64 seed=0） */
            if (name_ok) {
                uint64_t h = (uint64_t)XXH64(name.data(), name.size(), 0);
                if (h != e.hash)
                    bad("dentry leaf inum=%llu blk=%llu entries[%u] hash 不符 stored=%#llx recalc=%#llx name=%s",
                        (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)e.hash, (unsigned long long)h, name.c_str());
            }

            /* 复合序：严格递增 */
            if (!out.empty()) {
                const DentEntry &p = out.back();
                int c = 0;
                if (p.hash != e.hash) c = (p.hash < e.hash) ? -1 : 1;
                else { int sc = p.name.compare(name); c = (sc < 0) ? -1 : (sc > 0 ? 1 : 0); }
                if (c >= 0)
                    bad("dentry leaf inum=%llu blk=%llu entries[%u] 复合序非严格递增（prev=(%#llx,%s) cur=(%#llx,%s)）",
                        (unsigned long long)owner, (unsigned long long)blk, i,
                        (unsigned long long)p.hash, p.name.c_str(), (unsigned long long)e.hash, name.c_str());
            }

            if (target >= inode_count) {
                c_dangling++;
                bad("dentry leaf inum=%llu blk=%llu entries[%u] inode_ptr=%llu 越界", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)target);
            } else if (!ino_lit(target)) {
                c_dangling++;
                bad("dentry leaf inum=%llu blk=%llu entries[%u] → inode %llu 未分配（悬挂）", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)target);
            }

            out.push_back(DentEntry{ name, e.hash, target });
            c_dentry_entries++;
        }
        return out.empty() ? 0 : out.back().hash;
    }

    /* inner */
    const Bptree_dentry_inner_node_t *nd = (const Bptree_dentry_inner_node_t *)nbuf.data();
    uint64_t prev_key = 0, ret = 0;
    bool first = true;
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t child = RFS_PTR_ADDR(nd->hints[i].sub_ptr);
        uint64_t key   = nd->hints[i].hash_max;
        if (!first && key <= prev_key)
            bad("dentry inner inum=%llu blk=%llu hint[%u] hash_max=%#llx 非严格递增", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)key);
        prev_key = key; first = false;

        bool cok = false;
        uint64_t cmax = dentry_node(child, owner, blk, false, depth + 1, out, cok);
        if (cok && cmax != key)
            bad("dentry inner inum=%llu blk=%llu hint[%u] hash_max=%#llx != 子树 max=%#llx", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)key, (unsigned long long)cmax);
        ret = cok ? cmax : key;
    }
    return ret;
}

void Auditor::audit_dentry_tree(uint64_t root, uint64_t owner, std::vector<DentEntry> &out) {
    cur_leaf_links.clear();
    bool ok = false;
    dentry_node(root, owner, PTR_INV, true, 0, out, ok);
}

/* ─────────────────── 叶链校验（v1.2 新增；contract v1.1 §4.1 ptrs[3] 四态）───────────────────
 *   非根叶 ptrs[1]=前驱叶块号  ptrs[2]=后继叶块号（首/尾 = 52 位全 1）
 *   根叶   ptrs[1]='..' inode  ptrs[2]=预留（全 1）
 * 校验：从最左叶沿 ptrs[2] 走链，恰好覆盖全部叶、顺序 = 中序；prev/next 对称；
 *       首叶前驱 invalid、末叶后继 invalid；非根叶 ptrs[1] 指回前驱（非 '..'）。
 * 根叶 ptrs[1] 对 '..' 的一致性：仅对 v1.1-mkfs 残留态（root 的 ptrs[1]=0）放宽为 WARN。
 */
void Auditor::check_leaf_chain(uint64_t inum) {
    const size_t K = cur_leaf_links.size();
    if (K == 0) return;                 /* 树根读取失败/无叶：已由别处报 BAD */

    const uint64_t dotdot_exp = (inum == root_inode) ? root_inode : parent_of[inum];
    const uint64_t bad_before = c_chain_bad;

    for (size_t i = 0; i < K; i++) {
        const LeafLink &L = cur_leaf_links[i];
        if (L.is_root) {
            if (K != 1) {
                c_chain_bad++;
                bad("leaf chain inum=%llu 根叶 blk=%llu 但为多叶树（叶数=%zu）",
                    (unsigned long long)inum, (unsigned long long)L.blk, K);
            }
            if (dotdot_exp != PTR_INV && L.ptr1 != dotdot_exp) {
                if (inum == root_inode && L.ptr1 == 0) {
                    warn("leaf chain inum=%llu 根叶 ptrs[1]='..'=0 != 自身 %llu（v1.1 mkfs 残留）",
                         (unsigned long long)inum, (unsigned long long)dotdot_exp);
                } else {
                    c_chain_bad++;
                    bad("leaf chain inum=%llu 根叶 blk=%llu ptrs[1]='..'=%llu != 期望 %llu",
                        (unsigned long long)inum, (unsigned long long)L.blk,
                        (unsigned long long)L.ptr1, (unsigned long long)dotdot_exp);
                }
            }
            if (L.ptr0 != PTR52) { c_chain_bad++; bad("leaf chain inum=%llu 根叶 blk=%llu ptrs[0]=%llu != 预留(全1)", (unsigned long long)inum, (unsigned long long)L.blk, (unsigned long long)L.ptr0); }
            if (L.ptr2 != PTR52) { c_chain_bad++; bad("leaf chain inum=%llu 根叶 blk=%llu ptrs[2]=%llu != 预留(全1)", (unsigned long long)inum, (unsigned long long)L.blk, (unsigned long long)L.ptr2); }
        } else {
            const uint64_t exp_prev = (i == 0) ? PTR52 : cur_leaf_links[i - 1].blk;
            const uint64_t exp_next = (i + 1 == K) ? PTR52 : cur_leaf_links[i + 1].blk;
            if (L.ptr1 != exp_prev) {
                c_chain_bad++;
                bad("leaf chain inum=%llu 叶 blk=%llu ptrs[1](prev)=%llu != 期望 %llu（%s）",
                    (unsigned long long)inum, (unsigned long long)L.blk,
                    (unsigned long long)L.ptr1, (unsigned long long)exp_prev,
                    (i == 0) ? "首叶应 invalid" : "应指回前驱");
            }
            if (L.ptr2 != exp_next) {
                c_chain_bad++;
                bad("leaf chain inum=%llu 叶 blk=%llu ptrs[2](next)=%llu != 期望 %llu（%s）",
                    (unsigned long long)inum, (unsigned long long)L.blk,
                    (unsigned long long)L.ptr2, (unsigned long long)exp_next,
                    (i + 1 == K) ? "末叶应 invalid" : "应指向后继");
            }
        }
    }

    /* 独立走链：从最左叶沿 next，必须恰好覆盖全部叶且顺序 = 中序 */
    if (!cur_leaf_links[0].is_root) {
        std::vector<uint64_t> walked;
        std::unordered_set<uint64_t> seen;
        uint64_t blk = cur_leaf_links[0].blk;
        bool broken = false;
        for (;;) {
            if (blk == PTR52) break;
            if (blk == PTR_INV || blk >= total_blocks || (blk & 1)) {
                c_chain_bad++;
                bad("leaf chain inum=%llu 沿 next 走到非法块号 %llu", (unsigned long long)inum, (unsigned long long)blk);
                broken = true; break;
            }
            if (seen.count(blk)) {
                c_chain_bad++;
                bad("leaf chain inum=%llu 沿 next 成环（blk=%llu 重入）", (unsigned long long)inum, (unsigned long long)blk);
                broken = true; break;
            }
            seen.insert(blk);
            walked.push_back(blk);
            if (walked.size() > K + 2) {
                c_chain_bad++;
                bad("leaf chain inum=%llu 链长 > 叶数 %zu（悬垂/额外叶）", (unsigned long long)inum, K);
                broken = true; break;
            }
            std::vector<uint8_t> nb(NODE_BYTES);
            if (!read_node(blk, nb.data())) {
                c_chain_bad++;
                bad("leaf chain inum=%llu blk=%llu 读取失败", (unsigned long long)inum, (unsigned long long)blk);
                broken = true; break;
            }
            const Bptree_meta_t *m = (const Bptree_meta_t *)nb.data();
            blk = RFS_PTR_ADDR(m->ptrs[2]);
        }
        if (!broken) {
            if (walked.size() != K) {
                c_chain_bad++;
                bad("leaf chain inum=%llu 沿 next 覆盖 %zu 叶 != 树内叶数 %zu",
                    (unsigned long long)inum, walked.size(), K);
            } else {
                for (size_t i = 0; i < K; i++) {
                    if (walked[i] != cur_leaf_links[i].blk) {
                        c_chain_bad++;
                        bad("leaf chain inum=%llu 沿 next 顺序 != 中序（第 %zu 个：走=%llu 期望=%llu）",
                            (unsigned long long)inum, i,
                            (unsigned long long)walked[i], (unsigned long long)cur_leaf_links[i].blk);
                        break;
                    }
                }
            }
        }
    }

    if (c_chain_bad == bad_before)
        ok("inode %llu leaf chain ✓（%zu 叶，prev/next 双向一致，走链覆盖中序）",
           (unsigned long long)inum, K);
}

/* ─────────────────────── B+tree：extent ─────────────────────── */
uint64_t Auditor::extent_node(uint64_t blk, uint64_t owner, uint64_t expect_parent,
                              bool expect_root, int depth, uint64_t size_bound, bool &ok)
{
    ok = false;
    if (depth > 64) { bad("extent tree inum=%llu depth 超限 blk=%llu", (unsigned long long)owner, (unsigned long long)blk); return 0; }
    if (blk == PTR_INV || blk == PTR52 || blk >= total_blocks || (blk & 1)) {
        bad("extent tree inum=%llu 非法节点块号 %llu", (unsigned long long)owner, (unsigned long long)blk);
        return 0;
    }
    if (visited_node.count(blk)) { bad("extent tree inum=%llu blk=%llu 节点重入", (unsigned long long)owner, (unsigned long long)blk); return 0; }
    visited_node.insert(blk);
    claim_block(blk, owner, "extent_node");
    if (blk + 1 < total_blocks) claim_block(blk + 1, owner, "extent_node");

    std::vector<uint8_t> nbuf(NODE_BYTES);
    if (!read_node(blk, nbuf.data())) { bad("extent tree blk=%llu 读取失败", (unsigned long long)blk); return 0; }
    const Bptree_meta_t *meta = (const Bptree_meta_t *)nbuf.data();
    if (meta->magics[0] != BPTREE_MAGIC1 || meta->magics[1] != BPTREE_MAGIC2 ||
        meta->magics[2] != BPTREE_MAGIC3 || meta->magics[3] != BPTREE_MAGIC4) {
        bad("extent tree inum=%llu blk=%llu magic 不符", (unsigned long long)owner, (unsigned long long)blk);
        return 0;
    }
    c_extent_nodes++;

    uint32_t cnt = RFS_PP_COUNT(meta->parent_ptr);
    uint32_t inner = RFS_PP_IS_INNER(meta->parent_ptr);
    uint32_t isroot = RFS_PP_IS_ROOT(meta->parent_ptr);
    uint64_t ptr = RFS_PP_PTR(meta->parent_ptr);

    if ((bool)isroot != expect_root)
        bad("extent tree inum=%llu blk=%llu is_root=%u 期望 %d", (unsigned long long)owner, (unsigned long long)blk, isroot, (int)expect_root);
    if (expect_root) {
        if (ptr != owner) bad("extent tree inum=%llu root blk=%llu '.' ptr=%llu != 自身", (unsigned long long)owner, (unsigned long long)blk, (unsigned long long)ptr);
    } else {
        if (ptr != expect_parent) bad("extent tree inum=%llu blk=%llu parent_ptr=%llu != 父 %llu", (unsigned long long)owner, (unsigned long long)blk, (unsigned long long)ptr, (unsigned long long)expect_parent);
    }
    if (cnt > EXTENT_ORDER + 1)
        bad("extent tree inum=%llu blk=%llu count=%u > 阶 %u", (unsigned long long)owner, (unsigned long long)blk, cnt, (unsigned)(EXTENT_ORDER + 1));

    ok = true;

    if (!inner) {
        const Bptree_extent_leaf_node_t *leaf = (const Bptree_extent_leaf_node_t *)nbuf.data();
        uint64_t prev_end = 0; bool first = true; uint64_t ret = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            const extent_t &e = leaf->extents[i];
            if (e.length == 0) bad("extent leaf inum=%llu blk=%llu extents[%u] length==0", (unsigned long long)owner, (unsigned long long)blk, i);
            if (!first && prev_end > e.fblkbase)
                bad("extent leaf inum=%llu blk=%llu extents[%u] 重叠 prev_end=%llu > fblkbase=%llu", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)prev_end, (unsigned long long)e.fblkbase);
            if (e.fblkbase + e.length > size_bound)
                bad("extent leaf inum=%llu blk=%llu extents[%u] 逻辑越界 end=%llu > size_bound=%llu", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)(e.fblkbase + e.length), (unsigned long long)size_bound);
            uint64_t pb = RFS_PTR_ADDR(e.block_base);
            if (pb == PTR52 || pb >= total_blocks || pb + e.length > total_blocks) {
                bad("extent leaf inum=%llu blk=%llu extents[%u] 物理指针越界 base=%llu len=%llu", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)pb, (unsigned long long)e.length);
            } else {
                for (uint64_t k = 0; k < e.length; k++) claim_block(pb + k, owner, "extent_data");
            }
            prev_end = e.fblkbase + e.length; first = false; ret = prev_end;
        }
        return ret;
    }

    const Bptree_extent_inner_node_t *nd = (const Bptree_extent_inner_node_t *)nbuf.data();
    uint64_t prev_key = 0, ret = 0; bool first = true;
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t child = RFS_PTR_ADDR(nd->extents[i].sub_node_ptr);
        uint64_t key   = nd->extents[i].key;
        if (!first && key <= prev_key) bad("extent inner inum=%llu blk=%llu hint[%u] key=%llu 非严格递增", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)key);
        prev_key = key; first = false;
        bool cok = false;
        uint64_t cmax = extent_node(child, owner, blk, false, depth + 1, size_bound, cok);
        if (cok && cmax != key) bad("extent inner inum=%llu blk=%llu hint[%u] key=%llu != 子树 max=%llu", (unsigned long long)owner, (unsigned long long)blk, i, (unsigned long long)key, (unsigned long long)cmax);
        ret = cok ? cmax : key;
    }
    return ret;
}

void Auditor::audit_extent_tree(uint64_t root, uint64_t owner, uint64_t size_bound) {
    bool ok = false;
    extent_node(root, owner, PTR_INV, true, 0, size_bound, ok);
}

/* ─────────────────────── xattr 块 ─────────────────────── */
void Auditor::audit_xattr(uint64_t inum, uint64_t xptr) {
    if (xptr == RFS_XATTR_NONE) return;
    c_xattr++;
    if (xptr >= total_blocks) { bad("inode %llu xattr_ptr=%llu 越界", (unsigned long long)inum, (unsigned long long)xptr); return; }
    claim_block(xptr, inum, "xattr");
    std::vector<uint8_t> buf(FS_BLOCK_BYTES);
    if (!read_block(xptr, buf.data())) { bad("inode %llu xattr 块 %llu 读取失败", (unsigned long long)inum, (unsigned long long)xptr); return; }
    const rfs_xattr_block_hdr *h = (const rfs_xattr_block_hdr *)buf.data();
    if (h->h_magic != RFS_XATTR_H_MAGIC) { bad("inode %llu xattr 块 %llu h_magic=%#x 期望 RFSX", (unsigned long long)inum, (unsigned long long)xptr, h->h_magic); return; }
    if (h->h_blocks != 1)   bad("inode %llu xattr 块 %llu h_blocks=%u != 1", (unsigned long long)inum, (unsigned long long)xptr, h->h_blocks);
    if (h->h_refcount != 1) bad("inode %llu xattr 块 %llu h_refcount=%u != 1", (unsigned long long)inum, (unsigned long long)xptr, h->h_refcount);

    uint64_t off = RFS_XATTR_HDR_SIZE;
    uint64_t entry_end = off;
    uint32_t nent = 0;
    std::vector<std::pair<uint64_t, uint64_t>> vranges;   /* (offs, size) */
    while (off + RFS_XATTR_ENTRY_HDR_SIZE <= FS_BLOCK_BYTES) {
        const rfs_xattr_entry *e = (const rfs_xattr_entry *)(buf.data() + off);
        if (RFS_XATTR_IS_LAST(e)) break;                      /* 终止：首个 u32==0（ext4） */
        uint8_t idx = e->e_name_index;
        bool idx_ok = (idx == RFS_XATTR_INDEX_USER || idx == RFS_XATTR_INDEX_POSIX_ACL_ACCESS ||
                       idx == RFS_XATTR_INDEX_POSIX_ACL_DEFAULT || idx == RFS_XATTR_INDEX_TRUSTED ||
                       idx == RFS_XATTR_INDEX_SECURITY || idx == RFS_XATTR_INDEX_SYSTEM);
        if (!idx_ok) bad("inode %llu xattr 条目 %u e_name_index=%u 非法", (unsigned long long)inum, nent, idx);
        if (e->e_name_len == 0) bad("inode %llu xattr 条目 %u e_name_len==0（空名非法）", (unsigned long long)inum, nent);
        uint64_t ent_len = RFS_XATTR_LEN(e->e_name_len);
        if (off + ent_len > FS_BLOCK_BYTES) {
            bad("inode %llu xattr 条目 %u 越界 (off=%llu name_len=%u)", (unsigned long long)inum, nent, (unsigned long long)off, e->e_name_len);
            break;
        }
        if (e->e_value_size) {
            uint64_t vo = e->e_value_offs, vs = e->e_value_size;
            if (vo < RFS_XATTR_HDR_SIZE || vo + vs > FS_BLOCK_BYTES || (vo & 3))
                bad("inode %llu xattr 条目 %u 值越界/未对齐 (offs=%llu size=%llu)", (unsigned long long)inum, nent, (unsigned long long)vo, (unsigned long long)vs);
            else
                vranges.push_back({vo, vs});
        }
        off += ent_len;
        entry_end = off;
        nent++;
    }
    if (h->h_hash != 0)
        warn("inode %llu xattr 块 %llu h_hash=%u != 0（v1.2 应置 0）", (unsigned long long)inum, (unsigned long long)xptr, h->h_hash);
    /* 条目区末端 ≤ 值区最低地址 */
    uint64_t vmin = FS_BLOCK_BYTES;
    for (auto &r : vranges) if (r.first < vmin) vmin = r.first;
    if (!vranges.empty() && entry_end > vmin)
        bad("inode %llu xattr 条目区末端 %llu 越过值区最低 %llu", (unsigned long long)inum, (unsigned long long)entry_end, (unsigned long long)vmin);
    /* 值区间互不重叠 */
    std::sort(vranges.begin(), vranges.end());
    for (size_t i = 1; i < vranges.size(); i++)
        if (vranges[i-1].first + vranges[i-1].second > vranges[i].first)
            bad("inode %llu xattr 值区间重叠 [(%llu,%llu) 与 (%llu,%llu)]", (unsigned long long)inum,
                (unsigned long long)vranges[i-1].first, (unsigned long long)vranges[i-1].second,
                (unsigned long long)vranges[i].first, (unsigned long long)vranges[i].second);
    if (nent == 0) warn("inode %llu xattr 块 %llu 存在但 0 条目", (unsigned long long)inum, (unsigned long long)xptr);
    ok("inode %llu xattr 块 %llu 校验通过（%u 条目）", (unsigned long long)inum, (unsigned long long)xptr, nent);
}

/* ─────────────────────── inode 公共检查 + 分派 ─────────────────────── */
void Auditor::audit_inode_dispatch(uint64_t inum) {
    uint8_t raw[256];
    if (!read_inode_raw(inum, raw)) { bad("inode %llu 读取失败", (unsigned long long)inum); return; }
    uint64_t w1 = rd_le64(raw + 0);
    uint32_t type = RFS_INO1_GET_TYPE(w1);
    if (type == INODE_TYPE_DIR) audit_dir(inum, raw);
    else                        audit_file(inum, raw);
}

void Auditor::audit_dir(uint64_t inum, const uint8_t *raw) {
    raven_dir_Inode_t d;
    memcpy(&d, raw, sizeof(d));

    uint32_t type = RFS_INO1_GET_TYPE(d.word1);
    uint32_t direct = RFS_INO1_GET_DIRECT(d.word1);
    uint32_t flags = 0;
    uint64_t mode = RFS_MODE_GET(d.mode_xattr);
    uint64_t xptr = RFS_XATTRPTR_GET(d.mode_xattr);
    c_dir++;

    ok("inode %llu DIR type=%u size=%llu link_count=%u mode=%04o xattr=%s data_root=%llu",
       (unsigned long long)inum, type, (unsigned long long)d.size, d.link_count, mode,
       (xptr == RFS_XATTR_NONE ? "none" : "yes"), (unsigned long long)d.data_root);

    (void)direct; (void)flags;
    audit_xattr(inum, xptr);

    if (d.link_count != d.size + 2) {
        c_link_mismatch++;
        bad("inode %llu DIR link_count=%u != size+2 (%llu)", (unsigned long long)inum, d.link_count, (unsigned long long)(d.size + 2));
    }
    if (d.data_root == PTR_INV || d.data_root >= total_blocks || (d.data_root & 1)) {
        bad("inode %llu DIR data_root 非法=%#llx", (unsigned long long)inum, (unsigned long long)d.data_root);
        return;
    }

    parse_heaps(inum, &d);

    std::vector<DentEntry> ents;
    audit_dentry_tree(d.data_root, inum, ents);

    if ((uint64_t)ents.size() != d.size)
        bad("inode %llu DIR size=%llu != 实际 dentry 条目 %zu", (unsigned long long)inum, (unsigned long long)d.size, ents.size());
    else
        ok("inode %llu DIR dentry 条目数 %zu == size ✓", (unsigned long long)inum, ents.size());

    check_leaf_chain(inum);   /* v1.2 新增：叶链校验 */

    /* 目录树可视化（pre-order 缩进） */
    {
        int depth = (int)ancestor_stack.size() - 1; if (depth < 0) depth = 0;
        std::string ind(2 * depth, ' ');
        for (const DentEntry &de : ents)
            info("%s+-- \"%s\" -> inode %llu", ind.c_str(), de.name.c_str(), (unsigned long long)de.inode);
    }

    /* '..' 记录（dir 树根 ptrs[1]） —— 由 dentry_node 未直接取出，这里重读根节点 */
    {
        std::vector<uint8_t> nbuf(NODE_BYTES);
        if (read_node(d.data_root, nbuf.data())) {
            const Bptree_meta_t *meta = (const Bptree_meta_t *)nbuf.data();
            uint64_t dd = RFS_PTR_ADDR(meta->ptrs[1]);
            dotdot_of[inum] = dd;
        }
    }

    /* 递归下探 */
    for (const DentEntry &de : ents) {
        uint64_t child = de.inode;
        if (child == PTR52 || child >= inode_count) continue;   /* 越界已在 dentry_node 报过 */
        ref_count[child]++;
        if (!bit_test(reachable, child)) bit_set(reachable, child);
        if (parent_of[child] == PTR_INV) parent_of[child] = inum;
        if (!ino_lit(child)) continue;   /* 悬挂：dentry_node 已报，跳过实际审计 */

        if (visited_inode.count(child)) {
            /* 二次到达：统计引用即可 */
            bool in_anc = std::find(ancestor_stack.begin(), ancestor_stack.end(), child) != ancestor_stack.end();
            if (in_anc) {
                c_dir_cycle++;
                bad("目录环：inode %llu 已在祖先栈 [. . .]，从 inode %llu 再次到达 → 不再下探",
                    (unsigned long long)child, (unsigned long long)inum);
            } else {
                uint8_t craw[256];
                if (read_inode_raw(child, craw)) {
                    uint32_t ct = RFS_INO1_GET_TYPE(rd_le64(craw + 0));
                    if (ct == INODE_TYPE_DIR)
                        warn("DIR inode %llu 被多次引用（目录硬链接/多父），跳过二次遍历", (unsigned long long)child);
                    else
                        info("inode %llu 再次到达（硬链接引用，跳过二次遍历）", (unsigned long long)child);
                }
            }
            continue;
        }
        visited_inode.insert(child);

        /* 先判环再下探 */
        uint8_t craw[256];
        if (!read_inode_raw(child, craw)) { bad("inode %llu 读取失败", (unsigned long long)child); continue; }
        uint32_t ct = RFS_INO1_GET_TYPE(rd_le64(craw + 0));
        if (!type_valid(ct)) {
            bad("inode %llu 非法 type=%u（引用自 DIR %llu 条目 \"%s\"）",
                (unsigned long long)child, ct, (unsigned long long)inum, de.name.c_str());
            continue;
        }
        if (ct == INODE_TYPE_DIR) {
            ancestor_stack.push_back(child);
            audit_dir(child, craw);
            ancestor_stack.pop_back();
        } else {
            audit_file(child, craw);
        }
    }
}

void Auditor::audit_file(uint64_t inum, const uint8_t *raw) {
    raven_Inode_t ino;
    memcpy(&ino, raw, sizeof(ino));

    uint64_t w1 = ino.word1;
    uint32_t type = RFS_INO1_GET_TYPE(w1);
    uint32_t direct = RFS_INO1_GET_DIRECT(w1);
    uint64_t flags = RFS_INO1_GET_FLAGS(w1);
    uint64_t mode = RFS_MODE_GET(ino.mode_xattr);
    uint64_t xptr = RFS_XATTRPTR_GET(ino.mode_xattr);

    if (!type_valid(type)) {
        bad("inode %llu 非法 type=%u（不在 {1,2,4,6,8,10,12}）", (unsigned long long)inum, type);
        return;
    }

    /* i_mode 恒等式（结构上成立：type 与 mode 是 i_mode 的不相交切片；无独立落盘 i_mode） */
    uint16_t imode = RFS_IMODE_BUILD(type, mode);
    if (flags != 0)
        warn("inode %llu i_flags=%#llx != 0（v1.2 不实现 chattr，规范为 0）", (unsigned long long)inum, (unsigned long long)flags);

    audit_xattr(inum, xptr);

    switch (type) {
    case INODE_TYPE_FIFO: c_fifo++; break;
    case INODE_TYPE_SOCK: c_sock++; break;
    case INODE_TYPE_CHR:  c_chr++;  break;
    case INODE_TYPE_BLK:  c_blk++;  break;
    case INODE_TYPE_REGULAR: c_reg++; break;
    case INODE_TYPE_SOFT_SYM: c_lnk++; break;
    default: break;
    }

    /* ── 无数据类型：FIFO/CHR/BLK/SOCK ── */
    if (type == INODE_TYPE_FIFO || type == INODE_TYPE_CHR ||
        type == INODE_TYPE_BLK  || type == INODE_TYPE_SOCK) {
        bool okfields = true;
        if (ino.size != 0) { okfields = false; bad("inode %llu %s size=%llu != 0（无数据类型）", (unsigned long long)inum, type_name(type), (unsigned long long)ino.size); }
        if (ino.data_root != PTR_INV) { okfields = false; bad("inode %llu %s data_root=%#llx != PTR_INVALID（无数据类型）", (unsigned long long)inum, type_name(type), (unsigned long long)ino.data_root); }
        if (direct != 0) { okfields = false; warn("inode %llu %s direct_bit=1（无数据类型应忽略/为 0）", (unsigned long long)inum, type_name(type)); }

        if (type == INODE_TYPE_CHR || type == INODE_TYPE_BLK) {
            uint32_t rdev = rd_le32(ino.inline_data);
            uint32_t major = (rdev >> 8) & 0xfffu;
            uint32_t minor = (rdev & 0xffu) | ((rdev >> 12) & 0xfff00u);
            ok("inode %llu %s rdev=0x%08x (major=%u minor=%u) mode=%04o i_mode=%#o",
               (unsigned long long)inum, type_name(type), rdev, major, minor, mode, imode);
        } else {
            /* FIFO/SOCK：inline_data 应全 0 */
            bool nz = false;
            for (int i = 0; i < 160; i++) if (ino.inline_data[i] != 0) { nz = true; break; }
            if (rd_le32(ino.inline_data) != 0)
                bad("inode %llu %s rdev 区 inline_data[0:4]=%#x != 0（非设备文件应置 0）", (unsigned long long)inum, type_name(type), rd_le32(ino.inline_data));
            else if (nz)
                warn("inode %llu %s inline_data 非全 0（建议置 0）", (unsigned long long)inum, type_name(type));
            else
                ok("inode %llu %s size=0 data_root=PTR_INVALID inline_data=0 mode=%04o i_mode=%#o",
                   (unsigned long long)inum, type_name(type), mode, imode);
        }
        (void)okfields;
        return;
    }

    /* ── REG / LNK：数据三态 ── */
    const uint64_t size = ino.size;
    if (size <= 160) {
        /* 内联（160 阈）*/
        if (ino.data_root != PTR_INV)
            warn("inode %llu %s size=%llu 内联但 data_root 残留=%#llx（不变量 23：黄牌）",
                 (unsigned long long)inum, type_name(type), (unsigned long long)size, (unsigned long long)ino.data_root);
        if (direct != 0)
            warn("inode %llu %s size=%llu 内联但 direct_bit=1（规范置 0）", (unsigned long long)inum, type_name(type), (unsigned long long)size);
        if (type == INODE_TYPE_SOFT_SYM) {
            std::string tgt((const char *)ino.inline_data, (size_t)size);
            ok("inode %llu LNK size=%llu inline → \"%s\" mode=%04o i_mode=%#o",
               (unsigned long long)inum, (unsigned long long)size, tgt.c_str(), mode, imode);
        } else {
            ok("inode %llu REG size=%llu inline mode=%04o i_mode=%#o", (unsigned long long)inum, (unsigned long long)size, mode, imode);
        }
        return;
    }

    uint64_t blocks = (size + FS_BLOCK_BYTES - 1) / FS_BLOCK_BYTES;
    if (direct) {
        uint64_t base = ino.data_root;
        if (base == PTR_INV || base == PTR52 || base >= total_blocks || base + blocks > total_blocks) {
            bad("inode %llu %s direct data_root=%#llx blocks=%llu 越界", (unsigned long long)inum, type_name(type), (unsigned long long)base, (unsigned long long)blocks);
            return;
        }
        for (uint64_t i = 0; i < blocks; i++) claim_block(base + i, inum, "direct_data");
        ok("inode %llu %s size=%llu direct base=%llu blocks=%llu mode=%04o i_mode=%#o",
           (unsigned long long)inum, type_name(type), (unsigned long long)size, (unsigned long long)base, (unsigned long long)blocks, mode, imode);
    } else {
        uint64_t root = ino.data_root;
        if (root == PTR_INV || root == PTR52 || root >= total_blocks || (root & 1)) {
            bad("inode %llu %s extent 树根非法=%#llx", (unsigned long long)inum, type_name(type), (unsigned long long)root);
            return;
        }
        audit_extent_tree(root, inum, blocks);
        ok("inode %llu %s size=%llu extent_root=%llu mode=%04o i_mode=%#o",
           (unsigned long long)inum, type_name(type), (unsigned long long)size, (unsigned long long)root, mode, imode);
    }
}

/* ─────────────────────── 收口：backup 链 ─────────────────────── */
void Auditor::load_backup_blocks() {
    uint64_t blk = sb.backup_next;
    int guard = 0;
    while (blk != PTR_INV && blk != PTR52 && blk < total_blocks && guard < (int)sb.backup_count + 8) {
        backup_blocks.push_back(blk);
        uint8_t buf[FS_BLOCK_BYTES];
        if (!read_block(blk, buf)) break;
        uint64_t nxt = rd_le64(buf + 116 + 8);   /* backup_next @ 116+8 */
        if (nxt == PTR_INV || nxt == PTR52 || nxt == blk || nxt >= total_blocks) break;
        blk = nxt; guard++;
    }
}

void Auditor::reconcile_blocks() {
    for (uint64_t b = 0; b < total_blocks; b++) {
        bool lit = blk_lit(b);
        if (b >= inode_bm_base) {           /* 元数据尾巴：结构块，须常亮 */
            if (!lit) bad("元数据块 %llu 未亮灯（inode bitmap/array/blk bitmap 区）", (unsigned long long)b);
            continue;
        }
        if (std::find(backup_blocks.begin(), backup_blocks.end(), b) != backup_blocks.end()) {
            if (!lit) bad("backup SB 块 %llu 未亮灯", (unsigned long long)b);
            continue;
        }
        bool claimed = claim_test(b);
        if (claimed && !lit) { c_leaked++; bad("block %llu 被认领但 bitmap 未亮", (unsigned long long)b); }
        else if (!claimed && lit) { c_orphan_blk++; info("block %llu 已分配但不可达（orphan）", (unsigned long long)b); }
    }
}

void Auditor::reconcile_inodes() {
    for (uint64_t i = 0; i < inode_count; i++) {
        bool lit = ino_lit(i);
        bool reach = bit_test(reachable, i);
        if (reach && !lit) { bad("inode %llu 被引用但 inode bitmap 未亮", (unsigned long long)i); continue; }
        if (lit && !reach) {
            c_orphan_ino++;
            if (i == root_inode) bad("root inode %llu 已分配但不可达", (unsigned long long)i);
            else info("inode %llu 已分配但不可达（orphan）", (unsigned long long)i);
            continue;
        }
        if (!lit) continue;
        if (i == root_inode) continue;   /* root 无 dentry 引用 */
        if (i >= inode_count) continue;
        uint8_t raw[256];
        if (!read_inode_raw(i, raw)) continue;
        uint32_t type = RFS_INO1_GET_TYPE(rd_le64(raw + 0));
        if (type == INODE_TYPE_DIR) continue;   /* DIR：link_count==size+2 已查 */
        uint32_t lc = rd_le32(raw + 28);
        uint32_t refs = ref_count[i];
        if (lc != refs) {
            c_link_mismatch++;
            bad("inode %llu link_count=%u != dentry 引用数 %u", (unsigned long long)i, lc, refs);
        }
    }

    /* DIR '..' 对账 */
    for (uint64_t i = 0; i < inode_count; i++) {
        if (!ino_lit(i)) continue;
        if (dotdot_of[i] == PTR_INV && !bit_test(reachable, i)) continue;
        if (i == root_inode) {
            uint64_t dd = dotdot_of[i];
            if (dd == 0 && root_inode == 16)
                warn("root DIR '..' = 0 != 自身 %llu（v1.1 mkfs 残留）", (unsigned long long)root_inode);
            else if (dd != root_inode && dotdot_of[i] != PTR_INV)
                bad("root DIR '..' =%llu != 自身 %llu", (unsigned long long)dd, (unsigned long long)root_inode);
        } else {
            uint64_t dd = dotdot_of[i];
            uint64_t p = parent_of[i];
            if (dd != PTR_INV && p != PTR_INV && dd != p)
                bad("DIR inode %llu '..' =%llu != 实际父 %llu", (unsigned long long)i, (unsigned long long)dd, (unsigned long long)p);
        }
    }
}

void Auditor::summary() const {
    puts("─────────────────────────────────────────────────────────────");
    printf("SUMMARY\n");
    printf("  root_inode=%llu total_blocks=%llu inode_count=%llu\n",
           (unsigned long long)root_inode, (unsigned long long)total_blocks, (unsigned long long)inode_count);
    printf("  geo: bitmap=[%llu,+%llu) inode_array=[%llu,+%llu) inode_bm=[%llu,+%llu)\n",
           (unsigned long long)bitmap_start, (unsigned long long)bitmap_blocks,
           (unsigned long long)inode_array_base, (unsigned long long)inode_blocks,
           (unsigned long long)inode_bm_base, (unsigned long long)inode_bm_blocks);
    printf("  dirs=%llu reg=%llu lnk=%llu fifo=%llu sock=%llu chr=%llu blk=%llu\n",
           (unsigned long long)c_dir, (unsigned long long)c_reg, (unsigned long long)c_lnk,
           (unsigned long long)c_fifo, (unsigned long long)c_sock, (unsigned long long)c_chr, (unsigned long long)c_blk);
    printf("  dentry_nodes=%llu dentry_leaves=%llu dentry_entries=%llu extent_nodes=%llu xattr_blocks=%llu\n",
           (unsigned long long)c_dentry_nodes, (unsigned long long)c_dentry_leaves,
           (unsigned long long)c_dentry_entries,
           (unsigned long long)c_extent_nodes, (unsigned long long)c_xattr);
    printf("  double_claim=%llu leaked_blocks=%llu orphan_blocks=%llu orphan_inodes=%llu\n",
           (unsigned long long)c_double_claim, (unsigned long long)c_leaked,
           (unsigned long long)c_orphan_blk, (unsigned long long)c_orphan_ino);
    printf("  dangling_refs=%llu link_mismatch=%llu dir_cycle=%llu heap_bad=%llu chain_bad=%llu\n",
           (unsigned long long)c_dangling, (unsigned long long)c_link_mismatch,
           (unsigned long long)c_dir_cycle, (unsigned long long)c_heap_bad,
           (unsigned long long)c_chain_bad);
    printf("  OK=%llu WARN=%llu INFO=%llu BAD=%llu\n",
           (unsigned long long)n_ok, (unsigned long long)n_warn, (unsigned long long)n_info, (unsigned long long)n_bad);
    printf("  RESULT: %s\n", n_bad == 0 ? "PASS" : "FAIL");
    puts("─────────────────────────────────────────────────────────────");
}

/* ─────────────────────── run ─────────────────────── */
int Auditor::run(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) { fprintf(stderr, "error: stat %s failed\n", path); return 2; }
    fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "error: open %s (O_RDONLY) failed\n", path); return 2; }

    if (S_ISBLK(st.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &dev_bytes) < 0) { fprintf(stderr, "error: BLKGETSIZE64 failed\n"); return 2; }
    } else if (S_ISREG(st.st_mode)) {
        dev_bytes = (uint64_t)st.st_size;
    } else {
        fprintf(stderr, "error: %s is neither regular file nor block device\n", path); return 2;
    }

    /* ── Superblock ── */
    std::vector<uint8_t> sbraw(FS_BLOCK_BYTES);
    if (!pread_full(sbraw.data(), FS_BLOCK_BYTES, 0)) { fprintf(stderr, "error: read superblock failed\n"); return 2; }
    memcpy(&sb, sbraw.data(), sizeof(sb));

    printf("=== RavenFS v1.2 audit (readonly) : %s ===\n", path);
    if (sb.magic != SUPERBLOCK_MAGIC) { bad("SB magic=%#llx != \"RAVN\"(0x5241564E)", (unsigned long long)sb.magic); summary(); return 1; }
    ok("SB magic = \"RAVN\"");
    if (sb.version != RAVENFS_VERSION) { bad("SB version=%u != %u", sb.version, RAVENFS_VERSION); summary(); return 1; }
    ok("SB version = %u (v1.2)", sb.version);

    {
        uint32_t stored = sb.crc32;
        std::vector<uint8_t> tmp = sbraw;
        tmp[112] = tmp[113] = tmp[114] = tmp[115] = 0;   /* crc32 @ offset 112 */
        uint32_t calc = crc32_calc(tmp.data(), tmp.size());
        if (calc != stored) bad("SB crc32=%#08x != 计算 %#08x", stored, calc);
        else ok("SB crc32 = %#08x ✓", stored);
    }

    total_blocks = sb.total_blocks;
    inode_count  = sb.inode_array_count;
    root_inode   = sb.root_inode;

    /* ── 几何（与 mkfs calc_geometry 逐式一致）── */
    uint64_t bb = (total_blocks + 32767) / 32768;
    bitmap_blocks = bb;
    bitmap_start  = total_blocks - bb;
    uint64_t ib = (inode_count * 256 + 4095) / 4096;
    inode_blocks = ib;
    inode_array_base = bitmap_start - ib;
    uint64_t ibb = (inode_count + 32767) / 32768;
    inode_bm_blocks = ibb;
    inode_bm_base = inode_array_base - ibb;

    if (dev_bytes != FS_BLOCK_BYTES * (total_blocks + 1))
        warn("设备字节 %llu != 4096*(total_blocks+1)=%llu（SB total_blocks 与设备尺寸不符）",
             (unsigned long long)dev_bytes, (unsigned long long)(FS_BLOCK_BYTES * (total_blocks + 1)));

    printf("geo: total_blocks=%llu inode_count=%llu root_inode=%llu\n",
           (unsigned long long)total_blocks, (unsigned long long)inode_count, (unsigned long long)root_inode);
    printf("     bitmap[%llu,+%llu) inode_array[%llu,+%llu) inode_bm[%llu,+%llu)\n",
           (unsigned long long)bitmap_start, (unsigned long long)bitmap_blocks,
           (unsigned long long)inode_array_base, (unsigned long long)inode_blocks,
           (unsigned long long)inode_bm_base, (unsigned long long)inode_bm_blocks);

    if (root_inode >= inode_count) { bad("root_inode=%llu >= inode_count", (unsigned long long)root_inode); summary(); return 1; }
    if (inode_bm_base <= 1) { bad("几何非法：inode_bm_base=%llu", (unsigned long long)inode_bm_base); summary(); return 1; }

    /* ── 分配位图 ── */
    claim.assign((total_blocks + 7) / 8, 0);
    track_owner = (total_blocks <= (64ULL << 20));
    if (track_owner) claim_owner.assign(total_blocks, PTR_INV);
    ref_count.assign(inode_count, 0);
    reachable.assign((inode_count + 7) / 8, 0);
    parent_of.assign(inode_count, PTR_INV);
    dotdot_of.assign(inode_count, PTR_INV);

    bmp.assign((size_t)bitmap_blocks * FS_BLOCK_BYTES, 0);
    if (!pread_full(bmp.data(), bmp.size(), blkoff(bitmap_start))) { fprintf(stderr, "error: read block bitmap failed\n"); return 2; }
    ibmp.assign((size_t)inode_bm_blocks * FS_BLOCK_BYTES, 0);
    if (!pread_full(ibmp.data(), ibmp.size(), blkoff(inode_bm_base))) { fprintf(stderr, "error: read inode bitmap failed\n"); return 2; }
    ok("block bitmap / inode bitmap 载入成功");

    load_backup_blocks();
    {
        char b[256]; int off = 0;
        off += snprintf(b + off, sizeof(b) - off, "backup SB 链 (%zu):", backup_blocks.size());
        for (uint64_t x : backup_blocks) off += snprintf(b + off, sizeof(b) - off, " %llu", (unsigned long long)x);
        ok("%s", b);
    }

    /* ── 递归遍历 ── */
    if (!ino_lit(root_inode)) { bad("root inode %llu 未在 inode bitmap 亮灯", (unsigned long long)root_inode); }
    else {
        visited_inode.insert(root_inode);
        bit_set(reachable, root_inode);
        ancestor_stack.push_back(root_inode);
        uint8_t craw[256];
        if (!read_inode_raw(root_inode, craw)) bad("root inode %llu 读取失败", (unsigned long long)root_inode);
        else {
            uint32_t rt = RFS_INO1_GET_TYPE(rd_le64(craw + 0));
            if (rt != INODE_TYPE_DIR) bad("root inode %llu type=%u != DIR", (unsigned long long)root_inode, rt);
            else audit_dir(root_inode, craw);
        }
        ancestor_stack.pop_back();
    }

    /* ── 收口 ── */
    reconcile_inodes();
    reconcile_blocks();
    summary();
    close(fd);
    return n_bad == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-or-device>\n", argv[0]);
        return 2;
    }
    Auditor a;
    return a.run(argv[1]);
}
