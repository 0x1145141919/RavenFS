/* ═══════════════════════════════════════════════════════════════════════════
 * audit_dfs.cpp — 相位 B（根 DFS）+ 相位 C（三空间收口）
 *
 * 分解（audit.hpp）：
 *   dfs_descend    —— 唯一递归点（可达性 ino_set + 路径栈 + traversal B 下钻）
 *   inode_audit    —— 核心：访问 + 类型分派，**不递归**
 *     inode_audit_dir 走自己的 ddentry 树（traversal A）
 *     enumerate_dir_children 独立第二遍（traversal B）→ A/B 对拍
 *
 * 单目录名字面瞬态（namedata_ctx_t）由 dfs_descend 栈持有，覆盖 A→B；不进 dfs_ctx_t。
 *
 * 全部访存走 audit_image::fsblk()；不链 god。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "audit/audit.hpp"
#include "leakscan_glue.hpp"

#include <xxhash.h>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace rfs_audit {

/* [FORK/LEAKSCAN] 临时漏块修复胶水的定义（声明见 leakscan_glue.hpp）。 */
bool     g_free_leaks          = false;
int      g_repair_fd           = -1;
uint64_t g_repair_bitmap_start = 0;
uint64_t g_repair_total_blocks = 0;
uint64_t g_leaks_repaired      = 0;
uint64_t g_leaks_repair_failed = 0;

bool leak_repair_clear(uint64_t blk)
{
    if (g_repair_fd < 0 || blk >= g_repair_total_blocks)
        return false;
    /* 位图存储起于 FS_block[bitmap_start]，位 i ↔ FS_block i：
     * 文件字节偏移 = AUDIT_BLK0 + bitmap_start*4096 + (i>>3)。 */
    const uint64_t byte_off = AUDIT_BLK0 + g_repair_bitmap_start * AUDIT_BLOCK + (blk >> 3);
    const uint8_t  mask     = (uint8_t)(1u << (blk & 7));
    uint8_t cur = 0;
    if (pread(g_repair_fd, &cur, 1, (off_t)byte_off) != 1)
        return false;
    if (!(cur & mask))
        return true;                       /* 已经是 0 */
    cur = (uint8_t)(cur & ~mask);
    if (pwrite(g_repair_fd, &cur, 1, (off_t)byte_off) != 1)
        return false;
    return true;
}

namespace {

/* PTR_INVALID 由 rc.hpp 提供（~0ULL）。PTR52 = 52 位无效指针。 */
constexpr uint64_t PTR52 = AUDIT_PTR52;

constexpr uint16_t DENTRY_ORDER = 126;
constexpr uint16_t DENTRY_MINA  = 63;
constexpr uint16_t EXTENT_ORDER = 337;
constexpr uint16_t EXTENT_MINA  = 168;
constexpr uint16_t FR_ORDER     = 337;

inline bool ptr52_bad(uint64_t p) { return p == PTR52; }

inline bool dentry_magic(const Bptree_meta_t *m)
{
    return m->magics[0] == DENTRY_MAGIC1 && m->magics[1] == DENTRY_MAGIC2 &&
           m->magics[2] == DENTRY_MAGIC3 && m->magics[3] == DENTRY_MAGIC4;
}
inline bool file_extent_magic(const Bptree_meta_t *m)
{
    return m->magics[0] == BPTREE_MAGIC1 && m->magics[1] == BPTREE_MAGIC2 &&
           m->magics[2] == BPTREE_MAGIC3 && m->magics[3] == BPTREE_MAGIC4;
}
inline bool nd_magic(const Bptree_meta_t *m)
{
    return m->magics[0] == NAMEDATA_EXTENT_MAGIC1 && m->magics[1] == NAMEDATA_EXTENT_MAGIC2 &&
           m->magics[2] == NAMEDATA_EXTENT_MAGIC3 && m->magics[3] == NAMEDATA_EXTENT_MAGIC4;
}
inline bool fr_magic(const Bptree_meta_t *m)
{
    return m->magics[0] == NAMEDATA_FREERANGES_MAGIC1 && m->magics[1] == NAMEDATA_FREERANGES_MAGIC2 &&
           m->magics[2] == NAMEDATA_FREERANGES_MAGIC3 && m->magics[3] == NAMEDATA_FREERANGES_MAGIC4;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * [FORK/LEAKSCAN] 对「位图亮但不可达」的 8K 单元做魔数扫描 → 判定类型。
 *
 * 8K 单元 = 2 连续 FS_block（块/片恒 8K 对齐；disk_layout_v2.0.md §2.4 / §11.2 / §12.2）。
 * 只读单元首 32B：Bptree_meta_t.magics[4] / fr 快照 hdr.magic[4] / xattr hdr.h_magic。
 * 命中 256-bit 魔数 → 该类型节点；否则归 data 类（文件数据 / 垃圾 / 非节点头）。
 * 判据来源 = include/magics.h + disk_struct_v2.0.h（与 audit 同契约，不猜）。
 * ═══════════════════════════════════════════════════════════════════════════ */
enum class leak_kind_t : uint8_t {
    bptree = 0,          /* 文件区段树节点      BPTREE_MAGIC*               */
    dentry,              /* 目录 dentry 树节点  DENTRY_MAGIC*               */
    namedata_extent,     /* 名字面映射树节点    NAMEDATA_EXTENT_MAGIC*      */
    namedata_fr,         /* 名字面 fr 树节点    NAMEDATA_FREERANGES_MAGIC*  */
    inode_extent,        /* inode 面映射树节点  INODE_DATA_EXTENT_MAGIC*    */
    inode_fr,            /* inode 面扁 fr 快照  INODE_DATA_FR_SNAPSHOT_MAGIC* */
    xattr,               /* xattr 块（1 块）    "RFSX"                        */
    data,                /* 无已知魔数：数据 / 垃圾 / 非 8K 单元头            */
    kind_count
};

inline const char *leak_kind_str(leak_kind_t k)
{
    switch (k) {
    case leak_kind_t::bptree:          return "extent_tree_node";
    case leak_kind_t::dentry:          return "dentry_tree_node";
    case leak_kind_t::namedata_extent: return "namedata_map_node";
    case leak_kind_t::namedata_fr:     return "namedata_fr_node";
    case leak_kind_t::inode_extent:    return "inode_map_node";
    case leak_kind_t::inode_fr:        return "inode_fr_snapshot";
    case leak_kind_t::xattr:           return "xattr_block";
    case leak_kind_t::data:            return "data_or_unknown";
    default:                           return "?";
    }
}

inline leak_kind_t leak_classify(const uint8_t *p)
{
    uint64_t m[4];
    uint32_t u0;
    std::memcpy(m, p, sizeof(m));
    std::memcpy(&u0, p, sizeof(u0));
    if (m[0] == BPTREE_MAGIC1 && m[1] == BPTREE_MAGIC2 && m[2] == BPTREE_MAGIC3 && m[3] == BPTREE_MAGIC4)
        return leak_kind_t::bptree;
    if (m[0] == DENTRY_MAGIC1 && m[1] == DENTRY_MAGIC2 && m[2] == DENTRY_MAGIC3 && m[3] == DENTRY_MAGIC4)
        return leak_kind_t::dentry;
    if (m[0] == NAMEDATA_EXTENT_MAGIC1 && m[1] == NAMEDATA_EXTENT_MAGIC2 && m[2] == NAMEDATA_EXTENT_MAGIC3 && m[3] == NAMEDATA_EXTENT_MAGIC4)
        return leak_kind_t::namedata_extent;
    if (m[0] == NAMEDATA_FREERANGES_MAGIC1 && m[1] == NAMEDATA_FREERANGES_MAGIC2 && m[2] == NAMEDATA_FREERANGES_MAGIC3 && m[3] == NAMEDATA_FREERANGES_MAGIC4)
        return leak_kind_t::namedata_fr;
    if (m[0] == INODE_DATA_EXTENT_MAGIC1 && m[1] == INODE_DATA_EXTENT_MAGIC2 && m[2] == INODE_DATA_EXTENT_MAGIC3 && m[3] == INODE_DATA_EXTENT_MAGIC4)
        return leak_kind_t::inode_extent;
    if (m[0] == INODE_DATA_FR_SNAPSHOT_MAGIC1 && m[1] == INODE_DATA_FR_SNAPSHOT_MAGIC2 && m[2] == INODE_DATA_FR_SNAPSHOT_MAGIC3 && m[3] == INODE_DATA_FR_SNAPSHOT_MAGIC4)
        return leak_kind_t::inode_fr;
    if (u0 == RFS_XATTR_H_MAGIC)
        return leak_kind_t::xattr;
    return leak_kind_t::data;
}

inline uint64_t name_hash(const char *s, size_t len) { return XXH64(s, len, 0); }

inline bool fr_key_gt(uint64_t alen, uint64_t abase, uint64_t blen, uint64_t bbase)
{
    return blen < alen || (blen == alen && bbase < abase);
}

std::string path_str(const dfs_ctx_t &c)
{
    std::string s;
    for (size_t i = 0; i < c.path_name.size(); i++) { if (i) s += '/'; s += c.path_name[i]; }
    return s.empty() ? std::string("/") : s;
}

/* 日志净化：把名字里的非可打印字节转义成 \xHH，保证「一行 = 一条记录」不被换行/控制符
 * 撕开（名字在 Linux 下唯一禁的是 '/' 与 NUL，其余字节皆可出现）。反斜杠自身也转义。 */
std::string sanitize(const char *s, size_t len)
{
    std::string o;
    o.reserve(len);
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch == '\\') o += "\\\\";
        else if (ch >= 0x20 && ch < 0x7f) o += (char)ch;
        else { char b[6]; std::snprintf(b, sizeof(b), "\\x%02x", ch); o += b; }
    }
    return o;
}

/* 形态名（raw 打印用） */
const char *form_name(file_form_t f)
{
    return f == file_form_t::inline_data ? "inline"
         : f == file_form_t::direct      ? "direct" : "extent";
}
const char *type_name(uint32_t t)
{
    switch (t) {
    case INODE_TYPE_FIFO:    return "fifo";
    case INODE_TYPE_CHR:     return "chr";
    case INODE_TYPE_DIR:     return "dir";
    case INODE_TYPE_BLK:     return "blk";
    case INODE_TYPE_REGULAR: return "reg";
    case INODE_TYPE_SOFT_SYM:return "sym";
    case INODE_TYPE_SOCK:    return "sock";
    default:                 return "bad";
    }
}

} /* namespace */

file_form_t form_of(const raven_Inode_t *ino)
{
    if (ino->size <= 160) return file_form_t::inline_data;
    return RFS_INO1_GET_DIRECT(ino->word1) ? file_form_t::direct : file_form_t::extent;
}

audit_session::audit_session(const audit_image &img, audit_reporter &rep)
    : img_(&img), rep_(&rep) {}

void audit_session::bad(scope_t s, const char *check, const char *fmt, ...)
{
    char b[1200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    rep_->error(s, check, "%s", b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 相位 A — inode 数据面
 * ═══════════════════════════════════════════════════════════════════════════ */
bool audit_session::phase_a_inode_plane()
{
    ctx_.img = img_;
    ctx_.rep = rep_;
    ctx_.sum = &sum_;
    ctx_.blk_bits = img_->geo().total_blocks;
    ctx_.shadow_blk.assign((size_t)img_->geo().bitmap_blocks * 4096, 0);
    ctx_.shadow_name.assign(ctx_.shadow_blk.size(), 0);   /* 名字面块占用子集 */
    ctx_.meta_lo = img_->geo().bitmap_start;
    ctx_.meta_hi = img_->geo().total_blocks;
    ctx_.max_depth = 64;

    inode_plane_auditor::result_t r = plane_.audit(*img_, *rep_, sum_, ctx_);
    ctx_.res = &plane_.resolver();
    ctx_.alloc = &plane_.alloc();
    ctx_.ino_bits = plane_.resolver().inode_count();
    ctx_.shadow_ino.assign((size_t)((ctx_.ino_bits + 7) / 8), 0);
    return r.mapping_ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 相位 B — 根 DFS
 * ═══════════════════════════════════════════════════════════════════════════ */
void audit_session::phase_b_dfs()
{
    /* backup SB 链白名单（只读遍历） */
    ctx_.backup_blocks.clear();
    {
        const RavenSuperblock &sb = img_->sb();
        const uint64_t total = img_->geo().total_blocks;
        uint64_t blk = sb.backup_next;
        int guard = 0;
        while (blk != PTR_INVALID && !ptr52_bad(blk) && blk < total &&
               guard < (int)sb.backup_count + 8) {
            ctx_.backup_blocks.push_back(blk);
            const RavenSuperblock *bs = reinterpret_cast<const RavenSuperblock *>(img_->fsblk(blk));
            uint64_t nxt = bs->backup_next;
            if (nxt == PTR_INVALID || ptr52_bad(nxt) || nxt == blk || nxt >= total) break;
            blk = nxt; guard++;
        }
    }
    rep_->info(scope_t::boot,
               "START root=%llu total_blocks=%llu inode_count=%llu | 白名单 meta[%llu,%llu) backup=%zu",
               (unsigned long long)RFS_ROOT_INODE_IDX, (unsigned long long)img_->geo().total_blocks,
               (unsigned long long)ctx_.ino_bits, (unsigned long long)ctx_.meta_lo,
               (unsigned long long)ctx_.meta_hi, ctx_.backup_blocks.size());

    dfs_descend(ctx_, RFS_ROOT_INODE_IDX, PTR_INVALID, nullptr);
}

/* ── 外部下钻驱动（唯一递归点）── */
void audit_session::dfs_descend(dfs_ctx_t &c, uint64_t inum, uint64_t parent_inum, const char *name)
{
    if (c.depth > c.max_depth) {
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "depth", "inum=%llu 递归深度超限 %d（路径 %s）",
                    (unsigned long long)inum, c.depth, path_str(c).c_str());
        return;
    }
    if (inum >= c.ino_bits) {
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "range", "引用越界 inode=%llu（父=%llu）",
                    (unsigned long long)inum, (unsigned long long)parent_inum);
        return;
    }
    if (!c.ino_set(inum)) {
        sum_.multi_ref_ino++;
        rep_->error(scope_t::inode, "MULTI_REF", "inode %llu 二次触达（环/共享/双引用）父=%llu",
                    (unsigned long long)inum, (unsigned long long)parent_inum);
        return;
    }

    /* 路径栈（驱动拥有；inode_audit 只读） */
    const raven_Inode_t *pre = c.res->inode(inum);
    const uint32_t ptype = pre ? RFS_INO1_GET_TYPE(pre->word1) : 0xFFFFFFFFu;
    const bool is_dir = (pre && ptype == INODE_TYPE_DIR);
    c.path_inum.push_back(inum);
    /* 路径用父目录在 traversal B 中给出的**真实条目名**（根为空 → path_str 给 "/"）；
     * 净化控制符，保证日志单行完整。 */
    c.path_name.push_back(name ? sanitize(name, std::strlen(name)) : std::string());
    c.depth++;

    /* 名字数据面瞬态：目录节点作用域，覆盖 traversal A → B；非 DIR 不触碰。 */
    namedata_ctx_t nd;
    inode_audit(c, inum, parent_inum, is_dir ? &nd : nullptr);   /* traversal A（不递归） */

    if (is_dir) {
        const raven_dir_Inode_t *d = reinterpret_cast<const raven_dir_Inode_t *>(pre);
        std::vector<child_ref_t> b;
        enumerate_dir_children(c, d, nd, b);   /* traversal B（刻意冗余） */

        /* A/B 对拍：以 (inum,name) 多重集比较 */
        std::vector<child_ref_t> a = c.dir_entries_audited;
        auto by = [](const child_ref_t &x, const child_ref_t &y) {
            return x.inum < y.inum || (x.inum == y.inum && x.name < y.name);
        };
        std::sort(a.begin(), a.end(), by);
        std::sort(b.begin(), b.end(), by);
        if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin(),
                                                [](const child_ref_t &x, const child_ref_t &y) {
                                                    return x.inum == y.inum && x.name == y.name; })) {
            sum_.dentry_xcheck_mismatch++;
            rep_->error(scope_t::dentry, "xcheck",
                        "DIR inum=%llu 两遍 ddentry 遍历条目集不一致（A=%zu B=%zu）",
                        (unsigned long long)inum, a.size(), b.size());
        }
        for (const child_ref_t &k : b)
            dfs_descend(c, k.inum, inum, k.name.c_str());
    }

    c.depth--;
    c.path_inum.pop_back();
    c.path_name.pop_back();
}

/* ── traversal B：独立极简 ddentry 枚举（只共享裸节点读取）── */
void audit_session::enumerate_dir_children(dfs_ctx_t &c, const raven_dir_Inode_t *d,
                                           const namedata_ctx_t &nd, std::vector<child_ref_t> &out)
{
    const uint64_t total = img_->geo().total_blocks;
    uint64_t blk = d->ddentry_Bptree_root;
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total || (blk & 1)) return;

    std::unordered_set<uint64_t> seen;
    /* 下钻最左叶（只把内部节点记入 seen；叶子留给链循环登记） */
    for (int guard = 0; guard <= c.max_depth; guard++) {
        if (blk >= total || (blk & 1)) return;
        const Bptree_meta_t *m = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(blk));
        if (!dentry_magic(m)) return;
        if (!RFS_PP_IS_INNER(m->parent_ptr)) break;
        if (!seen.insert(blk).second) return;
        const uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
        if (cnt == 0) return;
        const auto *in = reinterpret_cast<const Bptree_ddentry_inner_node_t *>(m);
        blk = RFS_PTR_ADDR(in->hints[0].sub_ptr);
    }

    /* 沿 ptrs[2] 走叶链枚举 */
    uint64_t cur = blk; uint64_t guard = 0;
    while (cur != PTR52 && cur < total && !(cur & 1) && guard++ < total) {
        if (!seen.insert(cur).second) return;
        const Bptree_meta_t *m = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(cur));
        if (!dentry_magic(m)) return;
        const uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
        const uint16_t it  = std::min<uint16_t>(cnt, (uint16_t)(DENTRY_ORDER + 1));
        const auto *leaf = reinterpret_cast<const Bptree_ddentry_leaf_node_t *>(m);
        for (uint16_t i = 0; i < it; i++) {
            const ddentry_t &e = leaf->entries[i];
            const uint16_t name_len = (uint16_t)RFS_PTR_FREE(e.inode_ptr);
            const uint64_t target = RFS_PTR_ADDR(e.inode_ptr);
            child_ref_t kr; kr.inum = target;
            char full[RAVENFS_MAX_NAME_LEN + 1];
            const uint16_t ncopy = name_len < DENTRY_INLINE_NAME_MAX ? name_len : DENTRY_INLINE_NAME_MAX;
            std::memcpy(full, e.name, ncopy);
            if (name_len > DENTRY_INLINE_NAME_MAX) {
                if (e.full_char_remain_offset == PTR_INVALID) { kr.inum = target; out.push_back(kr); continue; }
                (void)name_data_read(nd, e.full_char_remain_offset,
                                     (uint16_t)(name_len - DENTRY_INLINE_NAME_MAX),
                                     full + DENTRY_INLINE_NAME_MAX);
            }
            full[name_len < RAVENFS_MAX_NAME_LEN ? name_len : RAVENFS_MAX_NAME_LEN] = '\0';
            kr.name.assign(full, name_len < RAVENFS_MAX_NAME_LEN ? name_len : RAVENFS_MAX_NAME_LEN);
            out.push_back(kr);
        }
        cur = RFS_PTR_ADDR(m->ptrs[2]);
    }
}

/* ── 核心：单 inode 审计（访问 + 类型分派；不递归）── */
void audit_session::inode_audit(dfs_ctx_t &c, uint64_t inum, uint64_t parent_inum, namedata_ctx_t *nd)
{
    const raven_Inode_t *ino = c.res->inode(inum);
    if (!ino) {
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "access", "inum=%llu 翻译层不可解（父=%llu）",
                    (unsigned long long)inum, (unsigned long long)parent_inum);
        return;
    }
    raven_Inode_t raw = *ino;   /* 拷贝：路径用；坐标稳定 */
    const uint32_t type = RFS_INO1_GET_TYPE(raw.word1);
    const file_form_t form = form_of(&raw);
    sum_.inodes_walked++;
    rep_->walk(scope_t::inode,
               "PATH inum=%llu parent=%llu depth=%d type=%s size=%llu lc=%u form=%s path=%s",
               (unsigned long long)inum, (unsigned long long)parent_inum, c.depth,
               type_name(type), (unsigned long long)raw.size, raw.link_count,
               form_name(form), path_str(c).c_str());
    AUDIT_TRACE("[TRACE][INODE] inum=%llu parent=%llu depth=%d type=%s(%u) form=%s size=%llu lc=%u "
                "mode=0%o xattr_ptr=%#llx uid=%u gid=%u mtime=%llu ctime=%llu atime=%llu "
                "word1=%#llx data_root=%#llx iblk=%llu path=%s\n",
                (unsigned long long)inum, (unsigned long long)parent_inum, c.depth,
                type_name(type), type, form_name(form), (unsigned long long)raw.size,
                raw.link_count, RFS_MODE_GET(raw.mode_xattr),
                (unsigned long long)RFS_XATTRPTR_GET(raw.mode_xattr),
                raw.uid, raw.gid, (unsigned long long)raw.mtime,
                (unsigned long long)raw.ctime, (unsigned long long)raw.atime,
                (unsigned long long)raw.word1, (unsigned long long)raw.data_root,
                (unsigned long long)c.res->phys_block(inum), path_str(c).c_str());
    if (type == INODE_TYPE_DIR) {
        const auto *d = reinterpret_cast<const raven_dir_Inode_t *>(&raw);
        AUDIT_TRACE("[TRACE][NDCTL] inum=%llu ddroot=%#llx nd_ctl=%#llx nd_direct=%u nd_ptr=%llu "
                    "pieces=%llu fr_ctl=%#llx fr_inline=%u fr_ptr=%llu fr_count=%llu path=%s\n",
                    (unsigned long long)inum, (unsigned long long)d->ddentry_Bptree_root,
                    (unsigned long long)d->name_data_ctl, RFS_ND_CTL_IS_DIRECT(d->name_data_ctl),
                    (unsigned long long)RFS_ND_CTL_PTR(d->name_data_ctl),
                    (unsigned long long)RFS_ND_PIECE_COUNT(d->name_data_piece_count),
                    (unsigned long long)d->name_data_free_ranges_ctl,
                    RFS_ND_FR_CTL_IS_INLINE(d->name_data_free_ranges_ctl),
                    (unsigned long long)RFS_ND_FR_CTL_PTR(d->name_data_free_ranges_ctl),
                    (unsigned long long)RFS_ND_FR_COUNT(d->name_data_free_ranges_count),
                    path_str(c).c_str());
    }

    switch (type) {
    case INODE_TYPE_DIR:
        if (!nd) { sum_.inode_bad++; rep_->error(scope_t::inode, "ctx", "DIR inode %llu 缺少名字面上下文", (unsigned long long)inum); break; }
        inode_audit_dir(c, inum, parent_inum, &raw, *nd); break;
    case INODE_TYPE_REGULAR:
    case INODE_TYPE_SOFT_SYM:  inode_audit_reg(c, inum, &raw); break;
    case INODE_TYPE_FIFO:
    case INODE_TYPE_CHR:
    case INODE_TYPE_BLK:
    case INODE_TYPE_SOCK:      inode_audit_special(c, inum, &raw); break;
    default:
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "type", "inode %llu 非法 type=%u", (unsigned long long)inum, type);
        break;
    }
    audit_xattr(c, inum, &raw);
}

void audit_session::inode_audit_special(dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino)
{
    (void)c;
    if (ino->size != 0 || ino->data_root != PTR_INVALID) {
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "special",
                    "无数据类型 inode %llu 违反不变量 26 (size=%llu data_root=%#llx)",
                    (unsigned long long)inum, (unsigned long long)ino->size,
                    (unsigned long long)ino->data_root);
    }
}

void audit_session::inode_audit_reg(dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino)
{
    if (ino->link_count == 0) {
        sum_.link_mismatch++;
        rep_->error(scope_t::inode, "link", "inode %llu link_count==0 但可达", (unsigned long long)inum);
    } else if (ino->link_count > 1) {
        rep_->warn(scope_t::inode, "link", "inode %llu link_count=%u >1（幼年期疑多引用）",
                   (unsigned long long)inum, ino->link_count);
    }

    const uint64_t total = img_->geo().total_blocks;
    const file_form_t form = form_of(ino);
    if (form == file_form_t::inline_data) {
        if (ino->data_root != PTR_INVALID) {
            sum_.inline_residual++;
            rep_->error(scope_t::inode, "inline_residual",
                        "inode %llu size=%llu inline 但 data_root 残留=%#llx",
                        (unsigned long long)inum, (unsigned long long)ino->size,
                        (unsigned long long)ino->data_root);
        }
        AUDIT_TRACE("[TRACE][EXT] form=inline inum=%llu size=%llu\n",
                    (unsigned long long)inum, (unsigned long long)ino->size);
        return;
    }

    const uint64_t blocks = (ino->size + 4095) / 4096;
    if (form == file_form_t::direct) {
        const uint64_t base = ino->data_root;
        AUDIT_TRACE("[TRACE][EXT] form=direct inum=%llu base=%llu blocks=%llu size=%llu\n",
                    (unsigned long long)inum, (unsigned long long)base,
                    (unsigned long long)blocks, (unsigned long long)ino->size);
        if (base == PTR_INVALID || base >= total || blocks > total - base) {
            sum_.inode_bad++;
            rep_->error(scope_t::inode, "direct", "inode %llu direct data_root=%#llx blocks=%llu 越界",
                        (unsigned long long)inum, (unsigned long long)base, (unsigned long long)blocks);
            return;
        }
        for (uint64_t i = 0; i < blocks; i++)
            if (!c.blk_set(base + i)) {
                sum_.multi_claim_blk++;
                rep_->warn(scope_t::block, "MULTI_CLAIM", "direct 块 %llu 二次认领 (inode %llu)",
                           (unsigned long long)(base + i), (unsigned long long)inum);
            }
        return;
    }

    const uint64_t root = ino->data_root;
    if (root == PTR_INVALID || root >= total || (root & 1)) {
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "extent", "inode %llu extent tree root 非法=%#llx",
                    (unsigned long long)inum, (unsigned long long)root);
        return;
    }
    tree_walk_t t; bool ok = true;
    AUDIT_TRACE("[TRACE][EXT] form=extent inum=%llu root=%llu blocks=%llu size=%llu\n",
                (unsigned long long)inum, (unsigned long long)root,
                (unsigned long long)blocks, (unsigned long long)ino->size);
    walk_btree(c, tree_kind_t::extent, nullptr, root, inum, inum, inum, true, blocks, 0, ok, t);
    walk_leaf_chain(c, tree_kind_t::extent, t, root, inum);
}

void audit_session::inode_audit_lnk(dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino)
{
    inode_audit_reg(c, inum, ino);
}

void audit_session::audit_xattr(dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino)
{
    const uint64_t xptr = (ino->mode_xattr >> 12) & PTR52;
    if (xptr == AUDIT_PTR52) return;   /* RFS_XATTR_NONE（52 位全 1） */
    const uint64_t total = img_->geo().total_blocks;
    if (xptr >= total) {
        sum_.xattr_bad++;
        rep_->error(scope_t::xattr, "range", "inode %llu xattr_ptr=%#llx 越界",
                    (unsigned long long)inum, (unsigned long long)xptr);
        return;
    }
    if (!c.blk_set(xptr)) {
        sum_.multi_claim_blk++;
        rep_->warn(scope_t::block, "MULTI_CLAIM", "xattr 块 %llu 二次认领 (inode %llu)",
                   (unsigned long long)xptr, (unsigned long long)inum);
    }
    const uint8_t *p = img_->fsblk(xptr);
    const auto *h = reinterpret_cast<const rfs_xattr_block_hdr *>(p);
    if (h->h_magic != RFS_XATTR_H_MAGIC) {
        sum_.xattr_bad++;
        rep_->error(scope_t::xattr, "magic", "inode %llu xattr 块 %llu magic=%#x 不符",
                    (unsigned long long)inum, (unsigned long long)xptr, h->h_magic);
    }
    if (h->h_refcount != 1 || h->h_blocks != 1) {
        sum_.xattr_bad++;
        rep_->error(scope_t::xattr, "hdr", "inode %llu xattr 块 %llu refcount=%u blocks=%u（应 1/1）",
                    (unsigned long long)inum, (unsigned long long)xptr, h->h_refcount, h->h_blocks);
    }
}

void audit_session::inode_audit_dir(dfs_ctx_t &c, uint64_t inum, uint64_t parent_inum,
                                    const raven_Inode_t *ino, namedata_ctx_t &nd)
{
    const auto *d = reinterpret_cast<const raven_dir_Inode_t *>(ino);
    const uint64_t total = img_->geo().total_blocks;

    /* 先审计名字数据面（映射树 + fr），并建 nd.map/refs 支撑长名重建 */
    walk_name_plane(c, nd, inum, d);

    const uint64_t root = d->ddentry_Bptree_root;
    if (root == PTR_INVALID || root >= total || (root & 1)) {
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "dentry_root", "DIR inode %llu ddentry_Bptree_root 非法=%#llx",
                    (unsigned long long)inum, (unsigned long long)root);
        return;
    }

    c.dir_entries_audited.clear();
    tree_walk_t t; bool ok = true;
    const uint64_t dotdot = (parent_inum == PTR_INVALID) ? RFS_ROOT_INODE_IDX : parent_inum;
    walk_btree(c, tree_kind_t::dentry, &nd, root, inum, inum, dotdot, true, 0, 0, ok, t);
    walk_leaf_chain(c, tree_kind_t::dentry, t, root, inum);
    c.dir_entries_audited = t.entries;   /* traversal A 结果 */

    if (t.records_total != d->size) {
        sum_.inode_bad++;
        rep_->error(scope_t::dentry, "size", "DIR inode %llu size=%llu != 实际 dentry 条目 %llu",
                    (unsigned long long)inum, (unsigned long long)d->size,
                    (unsigned long long)t.records_total);
    }

    /* link_count = 2 + 直属 DIR 子 inode 数 */
    uint64_t k = 0;
    for (const child_ref_t &e : c.dir_entries_audited) {
        const raven_Inode_t *ci = c.res->inode(e.inum);
        if (ci && RFS_INO1_GET_TYPE(ci->word1) == INODE_TYPE_DIR) k++;
    }
    if ((uint64_t)d->link_count != k + 2) {
        sum_.inode_bad++;
        rep_->error(scope_t::inode, "link", "DIR inode %llu link_count=%u 期望=%llu (子目录数 %llu)",
                    (unsigned long long)inum, d->link_count, (unsigned long long)(k + 2),
                    (unsigned long long)k);
    }

    /* 名字面 3-way 咬合对账（本目录） */
    reconcile_namedata_dir(nd, inum);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * B+树 DFS（dentry / file extent 两族）
 * ═══════════════════════════════════════════════════════════════════════════ */
uint64_t audit_session::walk_btree(dfs_ctx_t &c, tree_kind_t kind, namedata_ctx_t *nd, uint64_t blk,
                                   uint64_t owner_inum, uint64_t expect_parent, uint64_t dotdot_expect,
                                   bool expect_root, uint64_t size_bound, int depth, bool &sub_ok,
                                   tree_walk_t &w)
{
    sub_ok = false;
    const bool is_dentry = (kind == tree_kind_t::dentry);
    const char *tree = is_dentry ? "dentry" : "extent";
    const uint16_t order = is_dentry ? DENTRY_ORDER : EXTENT_ORDER;
    const uint16_t mina  = is_dentry ? DENTRY_MINA  : EXTENT_MINA;
    const uint64_t total = img_->geo().total_blocks;

    if (depth > c.max_depth) { sum_.btree_bad++; rep_->error(scope_t::btree, "depth", "tree=%s inum=%llu depth 超限 %d blk=%llu", tree, (unsigned long long)owner_inum, depth, (unsigned long long)blk); return 0; }
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total || (blk & 1)) {
        sum_.btree_bad++; rep_->error(scope_t::btree, "node", "tree=%s inum=%llu blk=%llu 非法节点块号", tree, (unsigned long long)owner_inum, (unsigned long long)blk); return 0;
    }
    if (!c.blk_set(blk)) {
        sum_.multi_claim_blk++;
        rep_->warn(scope_t::block, "MULTI_CLAIM", "tree=%s inum=%llu 节点 blk=%llu 二次认领", tree, (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    if (blk + 1 < total && !c.blk_set(blk + 1)) {
        sum_.multi_claim_blk++;
        rep_->warn(scope_t::block, "MULTI_CLAIM", "tree=%s inum=%llu 节点尾 blk=%llu 二次认领", tree, (unsigned long long)owner_inum, (unsigned long long)(blk + 1));
    }

    const Bptree_meta_t *meta = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(blk));
    const bool magic_ok = is_dentry ? dentry_magic(meta) : file_extent_magic(meta);
    if (!magic_ok) { sum_.btree_bad++; rep_->error(scope_t::btree, "magic", "tree=%s inum=%llu blk=%llu magic 不符", tree, (unsigned long long)owner_inum, (unsigned long long)blk); return 0; }

    sum_.nodes++;
    const uint16_t cnt = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    const uint16_t it  = std::min<uint16_t>(cnt, (uint16_t)(order + 1));
    const bool inner = RFS_PP_IS_INNER(meta->parent_ptr);
    AUDIT_TRACE("[TRACE][TREE] kind=%s inum=%llu depth=%d blk=%llu role=%s inner=%d cnt=%u order=%u\n",
                tree, (unsigned long long)owner_inum, depth, (unsigned long long)blk,
                expect_root ? "root" : (inner ? "inner" : "leaf"), (int)inner, cnt, order);

    if ((bool)RFS_PP_IS_ROOT(meta->parent_ptr) != expect_root)
        { sum_.btree_bad++; rep_->error(scope_t::btree, "role", "tree=%s inum=%llu blk=%llu is_root=%d 期望 %d", tree, (unsigned long long)owner_inum, (unsigned long long)blk, (int)RFS_PP_IS_ROOT(meta->parent_ptr), (int)expect_root); }
    if (expect_root) {
        if (RFS_PP_PTR(meta->parent_ptr) != owner_inum)
            { sum_.btree_bad++; rep_->error(scope_t::btree, "parent", "tree=%s inum=%llu blk=%llu root ptr=%llu != owner", tree, (unsigned long long)owner_inum, (unsigned long long)blk, (unsigned long long)RFS_PP_PTR(meta->parent_ptr)); }
        if (is_dentry) {
            const uint64_t dd = RFS_PTR_ADDR(meta->ptrs[1]);
            if (dd != dotdot_expect) {
                sum_.dotdot_mismatch++;
                rep_->error(scope_t::dentry, "dotdot", "DIR inode %llu '..' =%llu != 实际父 %llu",
                            (unsigned long long)owner_inum, (unsigned long long)dd, (unsigned long long)dotdot_expect);
            }
        }
    } else if (RFS_PP_PTR(meta->parent_ptr) != expect_parent) {
        sum_.btree_bad++; rep_->error(scope_t::btree, "parent", "tree=%s inum=%llu blk=%llu parent_ptr=%llu != 实际父 %llu", tree, (unsigned long long)owner_inum, (unsigned long long)blk, (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)expect_parent);
    }
    if (cnt > order) { sum_.btree_bad++; rep_->error(scope_t::btree, "order", "tree=%s inum=%llu blk=%llu entry_count=%u > 阶 %u", tree, (unsigned long long)owner_inum, (unsigned long long)blk, cnt, order); }

    if (!inner) {
        sum_.leaves++;
        w.leaves.push_back(blk);
        if (!w.leaf_depth_set) { w.leaf_depth = (uint64_t)depth; w.leaf_depth_set = true; }
        else if ((uint64_t)depth != w.leaf_depth) { sum_.btree_bad++; rep_->error(scope_t::btree, "balance", "tree=%s inum=%llu blk=%llu 叶深度 %d != %llu", tree, (unsigned long long)owner_inum, (unsigned long long)blk, depth, (unsigned long long)w.leaf_depth); }
        if (cnt == 0) {
            if (!expect_root) { sum_.btree_bad++; rep_->error(scope_t::btree, "leaf", "tree=%s inum=%llu blk=%llu 非根叶空", tree, (unsigned long long)owner_inum, (unsigned long long)blk); }
            else if (!is_dentry) { sum_.btree_bad++; rep_->error(scope_t::btree, "leaf", "tree=%s inum=%llu 根叶空（extent 空树）", tree, (unsigned long long)owner_inum); }
        } else if (!expect_root && cnt < mina) {
            sum_.btree_bad++; rep_->error(scope_t::btree, "underload", "tree=%s inum=%llu blk=%llu 叶欠载 cnt=%u < %u", tree, (unsigned long long)owner_inum, (unsigned long long)blk, cnt, mina);
        }
        sub_ok = true;

        if (is_dentry) {
            /* is_dentry ⇒ nd 非空（inode_audit_dir 保证） */
            const auto *leaf = reinterpret_cast<const Bptree_ddentry_leaf_node_t *>(meta);
            return audit_dentry_leaf(c, *nd, owner_inum, blk, leaf, it, w);
        }

        /* file extent 叶 */
        const auto *leaf = reinterpret_cast<const Bptree_extent_leaf_node_t *>(meta);
        uint64_t prev_end = 0;
        for (uint16_t i = 0; i < it; i++) {
            const extent_t &e = leaf->extents[i];
            if (e.length == 0) { sum_.btree_bad++; rep_->error(scope_t::extent, "len0", "tree=extent inum=%llu blk=%llu ext[%u] length==0", (unsigned long long)owner_inum, (unsigned long long)blk, i); }
            if (i > 0 && prev_end > e.fblkbase) { sum_.btree_bad++; rep_->error(scope_t::extent, "overlap", "tree=extent inum=%llu blk=%llu ext[%u] 重叠", (unsigned long long)owner_inum, (unsigned long long)blk, i); }
            if (e.fblkbase + e.length > size_bound) { sum_.btree_bad++; rep_->error(scope_t::extent, "bound", "tree=extent inum=%llu blk=%llu ext[%u] 逻辑越界 end=%llu > %llu", (unsigned long long)owner_inum, (unsigned long long)blk, i, (unsigned long long)(e.fblkbase + e.length), (unsigned long long)size_bound); }
            const uint64_t pb = RFS_PTR_ADDR(e.block_base);
            AUDIT_TRACE("[TRACE][EXTENT] inum=%llu blk=%llu idx=%u fblk=%llu pblk=%llu len=%llu end=%llu\n",
                        (unsigned long long)owner_inum, (unsigned long long)blk, i,
                        (unsigned long long)e.fblkbase, (unsigned long long)pb,
                        (unsigned long long)e.length, (unsigned long long)(e.fblkbase + e.length));
            if (pb == PTR52 || pb >= total || e.length > total - pb) {
                sum_.btree_bad++; rep_->error(scope_t::extent, "phys", "tree=extent inum=%llu blk=%llu ext[%u] 物理指针越界", (unsigned long long)owner_inum, (unsigned long long)blk, i);
            } else {
                for (uint64_t k = 0; k < e.length; k++)
                    if (!c.blk_set(pb + k)) {
                        sum_.multi_claim_blk++;
                        rep_->warn(scope_t::block, "MULTI_CLAIM", "extent 块 %llu 二次认领 (inode %llu)", (unsigned long long)(pb + k), (unsigned long long)owner_inum);
                    }
            }
            prev_end = e.fblkbase + e.length;
        }
        w.records += it; w.records_total += it; sum_.records += it;
        return prev_end;
    }

    /* 内部节点 */
    sum_.inner++;
    if (cnt == 0) { sum_.btree_bad++; rep_->error(scope_t::btree, "inner_empty", "tree=%s inum=%llu blk=%llu 内部节点空", tree, (unsigned long long)owner_inum, (unsigned long long)blk); return 0; }
    if (expect_root && cnt == 1) { sum_.btree_bad++; rep_->error(scope_t::btree, "root1", "tree=%s inum=%llu blk=%llu 根内部仅 1 孩子", tree, (unsigned long long)owner_inum, (unsigned long long)blk); }
    if (!expect_root && cnt < mina) { sum_.btree_bad++; rep_->error(scope_t::btree, "underload", "tree=%s inum=%llu blk=%llu 内部欠载 cnt=%u < %u", tree, (unsigned long long)owner_inum, (unsigned long long)blk, cnt, mina); }

    sub_ok = true;
    std::vector<std::pair<uint64_t, uint64_t>> hints(it);
    if (is_dentry) {
        const auto *dn = reinterpret_cast<const Bptree_ddentry_inner_node_t *>(meta);
        for (uint16_t i = 0; i < it; i++) { hints[i].first = RFS_PTR_ADDR(dn->hints[i].sub_ptr); hints[i].second = dn->hints[i].hash_max; }
    } else {
        const auto *en = reinterpret_cast<const Bptree_extent_inner_node_t *>(meta);
        for (uint16_t i = 0; i < it; i++) { hints[i].first = RFS_PTR_ADDR(en->extents[i].sub_node_ptr); hints[i].second = en->extents[i].key; }
    }

    uint64_t prev_key = 0, ret = 0;
    for (uint16_t i = 0; i < it; i++) {
        const uint64_t child = hints[i].first, key = hints[i].second;
        AUDIT_TRACE("[TRACE][TREEIDX] kind=%s inum=%llu blk=%llu depth=%d idx=%u key=%llu child=%llu\n",
                    tree, (unsigned long long)owner_inum, (unsigned long long)blk, depth, i,
                    (unsigned long long)key, (unsigned long long)child);
        if (i > 0 && key <= prev_key) { sum_.btree_bad++; rep_->error(scope_t::btree, "hint", "tree=%s inum=%llu blk=%llu hint[%u] key=%llu 非严格递增", tree, (unsigned long long)owner_inum, (unsigned long long)blk, i, (unsigned long long)key); }
        prev_key = key;
        if (child == PTR52 || child >= total) { sum_.btree_bad++; rep_->error(scope_t::btree, "hint_child", "tree=%s inum=%llu blk=%llu hint[%u] child 非法=%llu", tree, (unsigned long long)owner_inum, (unsigned long long)blk, i, (unsigned long long)child); continue; }
        bool cok = false;
        const uint64_t cmax = walk_btree(c, kind, nd, child, owner_inum, blk, dotdot_expect, false, size_bound, depth + 1, cok, w);
        if (cok && cmax != key) { sum_.btree_bad++; rep_->error(scope_t::btree, "hint_max", "tree=%s inum=%llu blk=%llu hint[%u] key=%llu != 子树 max=%llu", tree, (unsigned long long)owner_inum, (unsigned long long)blk, i, (unsigned long long)key, (unsigned long long)cmax); }
        ret = cok ? cmax : key;
    }
    return ret;
}

/* dentry 叶记录处理：复合序 + 长名画像（nd.refs）+ hash 重算 + 目标越界。
 * 返回本叶末条 hash（供父层 hint_max 校验）。 */
uint64_t audit_session::audit_dentry_leaf(dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t owner_inum,
                                          uint64_t blk, const Bptree_ddentry_leaf_node_t *leaf,
                                          uint16_t it, tree_walk_t &w)
{
    uint64_t prev_hash = 0; char prev_name[DENTRY_INLINE_NAME_MAX]; std::memset(prev_name, 0, sizeof(prev_name));
    for (uint16_t i = 0; i < it; i++) {
        const ddentry_t &e = leaf->entries[i];
        if (i > 0) {
            const bool bad_order = (e.hash < prev_hash) ||
                (e.hash == prev_hash && std::memcmp(e.name, prev_name, DENTRY_INLINE_NAME_MAX) <= 0);
            if (bad_order) { sum_.btree_bad++; rep_->error(scope_t::dentry, "order", "tree=dentry inum=%llu blk=%llu entries[%u] 复合序非严格递增", (unsigned long long)owner_inum, (unsigned long long)blk, i); }
        }
        prev_hash = e.hash; std::memcpy(prev_name, e.name, DENTRY_INLINE_NAME_MAX);

        const uint16_t name_len = (uint16_t)RFS_PTR_FREE(e.inode_ptr);
        const uint64_t target   = RFS_PTR_ADDR(e.inode_ptr);
        if (name_len > RAVENFS_MAX_NAME_LEN) { sum_.btree_bad++; rep_->error(scope_t::dentry, "namelen", "inum=%llu blk=%llu entries[%u] name_len=%u 超上限", (unsigned long long)owner_inum, (unsigned long long)blk, i, name_len); }

        char full[RAVENFS_MAX_NAME_LEN + 1];
        const uint16_t ncopy = name_len < DENTRY_INLINE_NAME_MAX ? name_len : DENTRY_INLINE_NAME_MAX;
        std::memcpy(full, e.name, ncopy);
        bool name_ok = true;
        if (name_len > DENTRY_INLINE_NAME_MAX) {
            if (e.full_char_remain_offset == PTR_INVALID) {
                name_ok = false;
                nd.refs.push_back(name_ref_t{0, 0});   /* 记录异常引用（不参与咬合） */
                sum_.namedata_bad++;
                rep_->error(scope_t::namedata, "longname", "tree=dentry inum=%llu blk=%llu entries[%u] 长名但 offset=PTR_INVALID", (unsigned long long)owner_inum, (unsigned long long)blk, i);
            } else {
                nd.refs.push_back(name_ref_t{e.full_char_remain_offset, (uint64_t)(name_len - DENTRY_INLINE_NAME_MAX)});
                if (!name_data_read(nd, e.full_char_remain_offset, (uint16_t)(name_len - DENTRY_INLINE_NAME_MAX), full + DENTRY_INLINE_NAME_MAX)) {
                    name_ok = false;
                    sum_.namedata_bad++;
                    rep_->error(scope_t::namedata, "longname", "tree=dentry inum=%llu blk=%llu entries[%u] 长名读取失败 offset=%#llx", (unsigned long long)owner_inum, (unsigned long long)blk, i, (unsigned long long)e.full_char_remain_offset);
                }
            }
        } else if (e.full_char_remain_offset != PTR_INVALID) {
            rep_->warn(scope_t::dentry, "stale_offset", "inum=%llu blk=%llu entries[%u] name_len<=40 但 offset 非 INVALID", (unsigned long long)owner_inum, (unsigned long long)blk, i);
        }
        if (name_ok) {
            full[name_len] = '\0';
            const uint64_t h = name_hash(full, name_len);
            if (h != e.hash) { sum_.btree_bad++; rep_->error(scope_t::dentry, "hash", "tree=dentry inum=%llu blk=%llu entries[%u] hash 不符 stored=%#llx recalc=%#llx", (unsigned long long)owner_inum, (unsigned long long)blk, i, (unsigned long long)e.hash, (unsigned long long)h); }
            child_ref_t kr; kr.inum = target; kr.name.assign(full, name_len); w.entries.push_back(kr);
        } else {
            child_ref_t kr; kr.inum = target; w.entries.push_back(kr);
        }

        {
            const std::string nm = name_ok ? sanitize(full, name_len)
                                           : sanitize(full, ncopy) + "<unread>";
            AUDIT_TRACE("[TRACE][DENT] inum=%llu blk=%llu idx=%u hash=%#llx name_len=%u target=%llu rem_off=%#llx name=%s\n",
                        (unsigned long long)owner_inum, (unsigned long long)blk, i,
                        (unsigned long long)e.hash, name_len, (unsigned long long)target,
                        (unsigned long long)e.full_char_remain_offset, nm.c_str());
        }
        if (target == PTR52 || target >= c.ino_bits) {
            sum_.btree_bad++; rep_->error(scope_t::dentry, "target", "tree=dentry inum=%llu blk=%llu entries[%u] inode_ptr=%llu 越界", (unsigned long long)owner_inum, (unsigned long long)blk, i, (unsigned long long)target);
        }
    }
    w.records += it; w.records_total += it; sum_.records += it;
    return (it > 0) ? leaf->entries[it - 1].hash : 0;
}

void audit_session::walk_leaf_chain(dfs_ctx_t &c, tree_kind_t kind, const tree_walk_t &w,
                                    uint64_t root_blk, uint64_t owner_inum)
{
    if (w.leaves.size() <= 1) return;
    const bool is_dentry = (kind == tree_kind_t::dentry);
    const char *tree = is_dentry ? "dentry" : "extent";
    const uint64_t total = img_->geo().total_blocks;
    auto node_ok = [&](uint64_t b) { return !(b == PTR_INVALID || ptr52_bad(b) || b >= total || (b & 1) || b + 1 >= total); };

    uint64_t blk = root_blk;
    for (int d = 0; d <= c.max_depth; d++) {
        if (!node_ok(blk)) return;
        const Bptree_meta_t *m = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(blk));
        if (!RFS_PP_IS_INNER(m->parent_ptr)) break;
        const uint16_t cc = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
        if (cc == 0) { sum_.btree_bad++; rep_->error(scope_t::btree, "chain", "tree=%s inum=%llu 叶链下钻遇空内部 blk=%llu", tree, (unsigned long long)owner_inum, (unsigned long long)blk); return; }
        blk = is_dentry ? RFS_PTR_ADDR(reinterpret_cast<const Bptree_ddentry_inner_node_t *>(m)->hints[0].sub_ptr)
                        : RFS_PTR_ADDR(reinterpret_cast<const Bptree_extent_inner_node_t *>(m)->extents[0].sub_node_ptr);
    }

    size_t idx = 0; uint64_t cur = blk;
    for (size_t guard = 0; guard < w.leaves.size() + 2; guard++) {
        if (cur == PTR52) break;
        if (idx >= w.leaves.size()) { sum_.btree_bad++; rep_->error(scope_t::btree, "chain_extra", "tree=%s inum=%llu 兄弟链多出叶子 blk=%llu", tree, (unsigned long long)owner_inum, (unsigned long long)cur); return; }
        if (cur != w.leaves[idx]) { sum_.btree_bad++; rep_->error(scope_t::btree, "chain_order", "tree=%s inum=%llu 兄弟链序 != DFS 叶序 (cur=%llu expect=%llu)", tree, (unsigned long long)owner_inum, (unsigned long long)cur, (unsigned long long)w.leaves[idx]); }
        const Bptree_meta_t *m = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(cur));
        const uint64_t prev = RFS_PTR_ADDR(m->ptrs[1]);
        const uint64_t next = RFS_PTR_ADDR(m->ptrs[2]);
        if (prev != PTR52) {
            const Bptree_meta_t *pm = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(prev));
            if (RFS_PTR_ADDR(pm->ptrs[2]) != cur) { sum_.btree_bad++; rep_->error(scope_t::btree, "chain_prev", "tree=%s inum=%llu 兄弟链断裂 cur=%llu.prev != blk", tree, (unsigned long long)owner_inum, (unsigned long long)cur); }
        }
        if (next != PTR52) {
            const Bptree_meta_t *nm = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(next));
            if (RFS_PTR_ADDR(nm->ptrs[1]) != cur) { sum_.btree_bad++; rep_->error(scope_t::btree, "chain_next", "tree=%s inum=%llu 兄弟链断裂 cur=%llu.next != blk", tree, (unsigned long long)owner_inum, (unsigned long long)cur); }
        }
        cur = next; idx++;
    }
    if (idx != w.leaves.size()) { sum_.btree_bad++; rep_->error(scope_t::btree, "chain_count", "tree=%s inum=%llu 兄弟链叶数 %zu != DFS 叶数 %zu", tree, (unsigned long long)owner_inum, idx, w.leaves.size()); }
}

/* 前向声明（下面定义） */
/* ═══════════════════════════════════════════════════════════════════════════
 * 名字数据面（映射树 + fr）
 * ═══════════════════════════════════════════════════════════════════════════ */
void audit_session::walk_name_plane(dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t inum,
                                    const raven_dir_Inode_t *d)
{
    const uint64_t total = img_->geo().total_blocks;
    const uint64_t ctl = d->name_data_ctl;
    const uint64_t pieces = RFS_ND_PIECE_COUNT(d->name_data_piece_count);
    const uint64_t ptr = RFS_ND_CTL_PTR(ctl);
    const uint64_t target = pieces * 2;

    walk_name_fr(c, nd, inum, d);   /* fr 先认领，防 XOR 误报 */

    if (ptr == RFS_ND_CTL_PTR_INVALID) {
        if (pieces != 0) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "empty", "DIR inode %llu 空态但 piece_count=%llu", (unsigned long long)inum, (unsigned long long)pieces); }
        return;
    }
    if (pieces == 0) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "ctl", "DIR inode %llu ptr=%llu 但 piece_count=0", (unsigned long long)inum, (unsigned long long)ptr); }

    nd.active = true;
    sum_.namedata_active_dirs++;
    sum_.namedata_pieces += pieces;
    AUDIT_TRACE("[TRACE][ND] inum=%llu direct=%u ptr=%llu pieces=%llu target_blk=%llu fr_count=%llu\n",
                (unsigned long long)inum, RFS_ND_CTL_IS_DIRECT(ctl),
                (unsigned long long)ptr, (unsigned long long)pieces,
                (unsigned long long)target, (unsigned long long)nd.fr_count);

    if (RFS_ND_CTL_IS_DIRECT(ctl)) {
        if (ptr & 1) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "align", "DIR inode %llu direct base=%llu 未 8K 对齐", (unsigned long long)inum, (unsigned long long)ptr); }
        if (ptr >= total || target > total - ptr) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "range", "DIR inode %llu direct 越界 base=%llu", (unsigned long long)inum, (unsigned long long)ptr); return; }
        nd.is_direct = true; nd.ptr = ptr; nd.used_blk = target;
        AUDIT_TRACE("[TRACE][NDMAP] inum=%llu mode=direct base=%llu blk_len=%llu\n",
                    (unsigned long long)inum, (unsigned long long)ptr, (unsigned long long)target);
        for (uint64_t k = 0; k < target; k++) {
            c.name_set(ptr + k);
            if (!c.blk_set(ptr + k)) { sum_.multi_claim_blk++; rep_->warn(scope_t::block, "MULTI_CLAIM", "name_data 块 %llu 二次认领", (unsigned long long)(ptr + k)); }
        }
        sum_.namedata_data_blk += target;
        return;
    }

    if (ptr & 1) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "align", "DIR inode %llu tree 根=%llu 未 8K 对齐", (unsigned long long)inum, (unsigned long long)ptr); }
    bool sub_ok = false; uint64_t tree_blocks = 0, data_blocks = 0;
    const uint64_t end = walk_name_tree(c, nd, inum, ptr, inum, true, 0, target, sub_ok, tree_blocks, data_blocks);
    if (end != target) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "coverage", "DIR inode %llu 逻辑覆盖 end=%llu != pieces*2=%llu", (unsigned long long)inum, (unsigned long long)end, (unsigned long long)target); }
    nd.used_blk = end;
    sum_.namedata_tree_blk += tree_blocks;
    sum_.namedata_data_blk += data_blocks;
}

uint64_t audit_session::walk_name_tree(dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t owner, uint64_t blk,
                                       uint64_t expect_parent, bool expect_root, int depth,
                                       uint64_t limit, bool &sub_ok, uint64_t &tree_blocks,
                                       uint64_t &data_blocks)
{
    sub_ok = false;
    const uint64_t total = img_->geo().total_blocks;
    if (depth > c.max_depth) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "depth", "name tree inum=%llu depth 超限", (unsigned long long)owner); return 0; }
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total || (blk & 1) || blk + 1 >= total) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "node", "name tree blk=%llu 非法", (unsigned long long)blk); return 0; }
    c.name_set(blk); c.name_set(blk + 1);
    if (!c.blk_set(blk)) { sum_.multi_claim_blk++; rep_->warn(scope_t::block, "MULTI_CLAIM", "name tree 节点 blk=%llu 二次认领", (unsigned long long)blk); return 0; }
    tree_blocks += 2;
    if (!c.blk_set(blk + 1)) { sum_.multi_claim_blk++; rep_->warn(scope_t::block, "MULTI_CLAIM", "name tree 节点尾 blk=%llu 二次认领", (unsigned long long)(blk + 1)); }

    const Bptree_meta_t *meta = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(blk));
    if (!nd_magic(meta)) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "magic", "name tree blk=%llu magic 不符", (unsigned long long)blk); return 0; }
    const uint16_t cnt = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    const uint16_t it  = std::min<uint16_t>(cnt, (uint16_t)(EXTENT_ORDER + 1));
    const bool inner = RFS_PP_IS_INNER(meta->parent_ptr);
    AUDIT_TRACE("[TRACE][NDTREE] inum=%llu depth=%d blk=%llu role=%s inner=%d cnt=%u\n",
                (unsigned long long)owner, depth, (unsigned long long)blk,
                expect_root ? "root" : (inner ? "inner" : "leaf"), (int)inner, cnt);
    if (cnt > EXTENT_ORDER) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "order", "name tree blk=%llu cnt>阶", (unsigned long long)blk); }
    if ((bool)RFS_PP_IS_ROOT(meta->parent_ptr) != expect_root) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "role", "name tree blk=%llu is_root 不符", (unsigned long long)blk); }
    if (expect_root) { if (RFS_PP_PTR(meta->parent_ptr) != owner) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "parent", "name tree blk=%llu 根 ptr=%llu != owner=%llu", (unsigned long long)blk, (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)owner); } }
    else if (RFS_PP_PTR(meta->parent_ptr) != expect_parent) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "parent", "name tree blk=%llu parent_ptr=%llu != expect=%llu", (unsigned long long)blk, (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)expect_parent); }

    if (!inner) {
        const auto *leaf = reinterpret_cast<const Bptree_dir_name_data_extent_leaf_node_t *>(meta);
        uint64_t leaf_max = 0;
        for (uint16_t i = 0; i < it; i++) {
            const extent_t &e = leaf->extents[i];
            const uint64_t beg = e.fblkbase, len = e.length, end = beg + len;
            if (len < 2 || (len & 1)) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "len", "name tree blk=%llu ext[%u] length 非偶/过小", (unsigned long long)blk, i); }
            if (beg & 1) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "align", "name tree blk=%llu ext[%u] fblkbase 未 8K 对齐", (unsigned long long)blk, i); }
            if (end > limit) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "bound", "name tree blk=%llu ext[%u] 越界", (unsigned long long)blk, i); }
            const uint64_t pb = RFS_PTR_ADDR(e.block_base);
            AUDIT_TRACE("[TRACE][NDMAP] inum=%llu blk=%llu idx=%u beg=%llu pblk=%llu len=%llu end=%llu\n",
                        (unsigned long long)owner, (unsigned long long)blk, i,
                        (unsigned long long)beg, (unsigned long long)pb,
                        (unsigned long long)len, (unsigned long long)end);
            if (pb == PTR52 || pb >= total || (pb & 1) || len > total - pb) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "phys", "name tree blk=%llu ext[%u] 物理非法", (unsigned long long)blk, i); }
            else {
                for (uint64_t k = 0; k < len; k++) {
                    c.name_set(pb + k);
                    if (!c.blk_set(pb + k)) { sum_.multi_claim_blk++; rep_->warn(scope_t::block, "MULTI_CLAIM", "name_data 块 %llu 二次认领", (unsigned long long)(pb + k)); }
                }
                data_blocks += len;
                nd.map.push_back(name_extent_t{beg, pb, len});
            }
            leaf_max = end;
        }
        sub_ok = true; nd.used_blk = leaf_max;
        return leaf_max;
    }

    if (cnt == 0) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "inner_empty", "name tree blk=%llu 内部空", (unsigned long long)blk); return 0; }
    if (expect_root && cnt == 1) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "root1", "name tree blk=%llu 根仅 1 孩子", (unsigned long long)blk); }
    if (!expect_root && cnt < EXTENT_MINA) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "underload", "name tree blk=%llu 内部欠载", (unsigned long long)blk); }
    sub_ok = true;
    const auto *nin = reinterpret_cast<const Bptree_dir_name_data_extent_inner_node_t *>(meta);
    std::vector<std::pair<uint64_t, uint64_t>> hints(it);
    for (uint16_t i = 0; i < it; i++) { hints[i].first = RFS_PTR_ADDR(nin->extents[i].sub_node_ptr); hints[i].second = nin->extents[i].key; }
    uint64_t prev_key = 0, ret = 0;
    for (uint16_t i = 0; i < it; i++) {
        const uint64_t child = hints[i].first, key = hints[i].second;
        AUDIT_TRACE("[TRACE][NDIDX] inum=%llu blk=%llu depth=%d idx=%u key=%llu child=%llu\n",
                    (unsigned long long)owner, (unsigned long long)blk, depth, i,
                    (unsigned long long)key, (unsigned long long)child);
        if (i > 0 && key <= prev_key) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "hint", "name tree blk=%llu hint 非递增", (unsigned long long)blk); }
        prev_key = key;
        if (child == PTR52 || child >= total) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "hint_child", "name tree child 非法"); continue; }
        bool cok = false;
        const uint64_t cmax = walk_name_tree(c, nd, owner, child, blk, false, depth + 1, limit, cok, tree_blocks, data_blocks);
        if (cok && cmax != key) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "hint_max", "name tree hint != 子树 max"); }
        ret = cok ? cmax : key;
    }
    return ret;
}

void audit_session::walk_name_fr(dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t inum, const raven_dir_Inode_t *d)
{
    const uint64_t total = img_->geo().total_blocks;
    const uint64_t fr_ctl = d->name_data_free_ranges_ctl;
    const uint64_t fr_cnt = RFS_ND_FR_COUNT(d->name_data_free_ranges_count);
    const uint64_t ptr    = RFS_ND_FR_CTL_PTR(fr_ctl);
    const uint64_t pieces = RFS_ND_PIECE_COUNT(d->name_data_piece_count);
    const uint64_t limit  = pieces * 8192;

    nd.fr_count = fr_cnt;
    if (ptr == RFS_ND_FR_CTL_PTR_INVALID) {
        if (fr_cnt != 0) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_empty", "DIR inode %llu fr 空态但 count=%llu", (unsigned long long)inum, (unsigned long long)fr_cnt); }
        nd.fr_ok = true;
        return;
    }
    if (RFS_ND_FR_CTL_IS_INLINE(fr_ctl)) {
        nd.fr_ok = true;
        if (fr_cnt > RFS_ND_FREE_INLINE_MAX) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_inline", "DIR inode %llu fr inline count=%llu > 上限", (unsigned long long)inum, (unsigned long long)fr_cnt); }
        bool have = false; uint64_t pl = 0, pb = 0;
        const uint64_t n = std::min<uint64_t>(fr_cnt, RFS_ND_FREE_INLINE_MAX);
        AUDIT_TRACE("[TRACE][NDFR] inum=%llu mode=inline count=%llu limit=%llu\n",
                    (unsigned long long)inum, (unsigned long long)fr_cnt, (unsigned long long)limit);
        for (uint64_t i = 0; i < n; i++) {
            const free_range_t &e = d->possible_inline_free_ranges[i];
            AUDIT_TRACE("[TRACE][NDFR] inum=%llu idx=%llu base=%llu len=%llu end=%llu\n",
                        (unsigned long long)inum, (unsigned long long)i,
                        (unsigned long long)e.base, (unsigned long long)e.len,
                        (unsigned long long)(e.base + e.len));
            if (e.len == 0) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_len0", "DIR inode %llu fr inline[%llu] len==0", (unsigned long long)inum, (unsigned long long)i); }
            if (e.base > limit || e.len > limit - e.base) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_bound", "DIR inode %llu fr inline[%llu] 越界", (unsigned long long)inum, (unsigned long long)i); }
            if (have && !fr_key_gt(e.len, e.base, pl, pb)) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_order", "DIR inode %llu fr inline 键序非递增", (unsigned long long)inum); }
            pl = e.len; pb = e.base; have = true;
            nd.free.emplace_back(e.base, e.len);
        }
        return;
    }

    if (ptr & 1) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_align", "DIR inode %llu fr 根未 8K 对齐", (unsigned long long)inum); }
    if (ptr >= total || ptr + 1 >= total) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_range", "DIR inode %llu fr 根越界", (unsigned long long)inum); return; }
    uint64_t node_blocks = 0, records = 0;
    AUDIT_TRACE("[TRACE][NDFR] inum=%llu mode=tree root=%llu count=%llu limit=%llu\n",
                (unsigned long long)inum, (unsigned long long)ptr,
                (unsigned long long)fr_cnt, (unsigned long long)limit);
    walk_fr_tree(c, nd, inum, ptr, inum, true, 0, limit, node_blocks, records);
    sum_.namedata_tree_blk += node_blocks;
    if (records != fr_cnt) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_count", "DIR inode %llu fr_count=%llu != 叶条目 %llu", (unsigned long long)inum, (unsigned long long)fr_cnt, (unsigned long long)records); }
    nd.fr_ok = true;   /* tree 态 fr 可读（结构违例已计 bad） */
}

void audit_session::walk_fr_tree(dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t owner, uint64_t blk,
                                 uint64_t expect_parent, bool expect_root, int depth, uint64_t limit,
                                 uint64_t &node_blocks, uint64_t &records)
{
    const uint64_t total = img_->geo().total_blocks;
    if (depth > c.max_depth) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_depth", "fr tree inum=%llu depth 超限", (unsigned long long)owner); return; }
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total || (blk & 1) || blk + 1 >= total) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_node", "fr tree blk=%llu 非法", (unsigned long long)blk); return; }
    c.name_set(blk); c.name_set(blk + 1);
    if (!c.blk_set(blk)) { sum_.multi_claim_blk++; rep_->warn(scope_t::block, "MULTI_CLAIM", "fr 节点 blk=%llu 二次认领", (unsigned long long)blk); return; }
    node_blocks += 2;
    if (!c.blk_set(blk + 1)) { sum_.multi_claim_blk++; rep_->warn(scope_t::block, "MULTI_CLAIM", "fr 节点尾 blk=%llu 二次认领", (unsigned long long)(blk + 1)); }

    const Bptree_meta_t *meta = reinterpret_cast<const Bptree_meta_t *>(img_->fsblk(blk));
    if (!fr_magic(meta)) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_magic", "fr tree blk=%llu magic 不符", (unsigned long long)blk); return; }
    const uint16_t cnt = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    const uint16_t it  = std::min<uint16_t>(cnt, (uint16_t)(FR_ORDER + 1));
    const bool inner = RFS_PP_IS_INNER(meta->parent_ptr);
    AUDIT_TRACE("[TRACE][FRTREE] inum=%llu depth=%d blk=%llu role=%s inner=%d cnt=%u\n",
                (unsigned long long)owner, depth, (unsigned long long)blk,
                expect_root ? "root" : (inner ? "inner" : "leaf"), (int)inner, cnt);
    if (cnt > FR_ORDER) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_order", "fr tree blk=%llu cnt>阶", (unsigned long long)blk); }
    if ((bool)RFS_PP_IS_ROOT(meta->parent_ptr) != expect_root) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_role", "fr tree blk=%llu is_root 不符", (unsigned long long)blk); }
    if (expect_root) { if (RFS_PP_PTR(meta->parent_ptr) != owner) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_parent", "fr tree blk=%llu 根 ptr=%llu != owner=%llu", (unsigned long long)blk, (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)owner); } }
    else if (RFS_PP_PTR(meta->parent_ptr) != expect_parent) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_parent", "fr tree blk=%llu parent_ptr=%llu != expect=%llu", (unsigned long long)blk, (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)expect_parent); }

    if (!inner) {
        const auto *leaf = reinterpret_cast<const Bptree_dir_name_data_free_ranges_leaf_node_t *>(meta);
        for (uint16_t i = 0; i < it; i++) {
            const uint64_t elen = leaf->free_ranges[i].len, ebase = leaf->free_ranges[i].base;
            AUDIT_TRACE("[TRACE][NDFR] inum=%llu blk=%llu idx=%u base=%llu len=%llu end=%llu\n",
                        (unsigned long long)owner, (unsigned long long)blk, i,
                        (unsigned long long)ebase, (unsigned long long)elen,
                        (unsigned long long)(ebase + elen));
            if (elen == 0) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_len0", "fr tree blk=%llu free[%u] len0", (unsigned long long)blk, i); }
            if (ebase > limit || elen > limit - ebase) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_bound", "fr tree blk=%llu free[%u] 越界", (unsigned long long)blk, i); }
            nd.free.emplace_back(ebase, elen);
            records++;
        }
        return;
    }

    if (cnt == 0) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_inner_empty", "fr tree blk=%llu 内部空", (unsigned long long)blk); return; }
    const auto *fin = reinterpret_cast<const Bptree_dir_name_data_free_ranges_inner_node_t *>(meta);
    for (uint16_t i = 0; i < it; i++) {
        const uint64_t child = RFS_PTR_ADDR(fin->hints[i].sub_node_ptr);
        AUDIT_TRACE("[TRACE][FRIDX] inum=%llu blk=%llu depth=%d idx=%u max_len=%llu max_base=%llu child=%llu\n",
                    (unsigned long long)owner, (unsigned long long)blk, depth, i,
                    (unsigned long long)fin->hints[i].max_len,
                    (unsigned long long)fin->hints[i].max_base, (unsigned long long)child);
        if (child == PTR52 || child >= total) { sum_.namedata_bad++; rep_->error(scope_t::name_plane, "fr_child", "fr tree child 非法"); continue; }
        walk_fr_tree(c, nd, owner, child, blk, false, depth + 1, limit, node_blocks, records);
    }
}

/* 名字数据面逻辑读（direct / tree，≤512B，跨至多 2 块） */
bool audit_session::name_data_read(const namedata_ctx_t &nd, uint64_t offset, uint16_t len, char *out)
{
    if (len == 0 || len > 512) return false;
    uint64_t fblk = 0; uint16_t off = 0;
    if (nd.is_direct) {
        fblk = nd.ptr + (offset >> 12);
        off  = (uint16_t)(offset & 0xFFF);
    } else {
        const uint64_t lblock = offset >> 12;
        bool found = false;
        for (const name_extent_t &e : nd.map) {
            if (lblock >= e.lblk && lblock < e.lblk + e.len) { fblk = e.phys + (lblock - e.lblk); off = (uint16_t)(offset & 0xFFF); found = true; break; }
        }
        if (!found) return false;
    }
    const uint64_t total = img_->geo().total_blocks;
    if (fblk >= total) return false;
    const uint8_t *p = img_->fsblk(fblk);
    const uint16_t first = (off + len <= 4096) ? len : (uint16_t)(4096 - off);
    std::memcpy(out, p + off, first);
    if (first < len) {
        if (fblk + 1 >= total) return false;
        std::memcpy(out + first, img_->fsblk(fblk + 1), (size_t)(len - first));
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 相位 C — 三空间收口
 * ═══════════════════════════════════════════════════════════════════════════ */
void audit_session::reconcile_namedata_dir(const namedata_ctx_t &nd, uint64_t inum)
{
    /* 先决：咬合前提 = 数据面已激活。未激活却有长名引用 ⇒ 画像与数据面矛盾。 */
    if (!nd.active) {
        for (const name_ref_t &r : nd.refs) {
            if (r.len) {
                sum_.namedata_bad++;
                rep_->error(scope_t::namedata, "activation",
                            "DIR inum=%llu 数据面未激活但有长名引用 [%llu,+%llu)",
                            (unsigned long long)inum, (unsigned long long)r.off, (unsigned long long)r.len);
                break;
            }
        }
        return;
    }
    AUDIT_TRACE("[TRACE][NDSUM] inum=%llu active=%d direct=%d used_blk=%llu map=%zu refs=%zu free=%zu\n",
                (unsigned long long)inum, (int)nd.active, (int)nd.is_direct,
                (unsigned long long)nd.used_blk, nd.map.size(), nd.refs.size(), nd.free.size());
    if (!nd.fr_ok) { sum_.namedata_xor_unavailable_dirs++; return; }

    /* refs 交叠 */
    std::vector<name_ref_t> refs;
    for (const name_ref_t &r : nd.refs) if (r.len) refs.push_back(r);
    std::sort(refs.begin(), refs.end(), [](const name_ref_t &a, const name_ref_t &b) { return a.off < b.off; });
    for (size_t i = 1; i < refs.size(); i++) {
        if (refs[i].off < refs[i - 1].off + refs[i - 1].len) {
            sum_.namedata.overlap++;
            rep_->error(scope_t::namedata, "ref_overlap", "DIR inum=%llu 长名引用交叠", (unsigned long long)inum);
        }
    }
    /* 野释放：ref ∩ fr 空闲 */
    for (const name_ref_t &r : refs) {
        for (const auto &f : nd.free) {
            const uint64_t fbeg = f.first, fend = f.first + f.second;
            if (r.off < fend && fbeg < r.off + r.len) {
                sum_.namedata.wild_free++;
                rep_->error(scope_t::namedata, "wild_free", "DIR inum=%llu 引用 [%llu,+%llu) 落在 fr 空闲 [%llu,+%llu)",
                            (unsigned long long)inum, (unsigned long long)r.off, (unsigned long long)r.len,
                            (unsigned long long)fbeg, (unsigned long long)f.second);
                break;
            }
        }
    }
    /* 泄漏（WARN）：片级 + fr 感知——某 8K 片**存在已用字节**（fr 未覆盖整片）
     * 且无任何长名引用 ⇒ 已分配未引用（空间未回收）。纯空闲片（fr 覆盖整片）不算。 */
    const uint64_t used_pieces = nd.used_blk / 2;
    if (used_pieces) {
        std::vector<uint8_t> ref_piece((size_t)used_pieces, 0);
        for (const name_ref_t &r : refs) {
            const uint64_t p0 = r.off / 8192, p1 = (r.off + r.len - 1) / 8192;
            for (uint64_t p = p0; p <= p1 && p < used_pieces; p++) ref_piece[(size_t)p] = 1;
        }
        uint64_t shown = 0;
        for (uint64_t p = 0; p < used_pieces; p++) {
            if (ref_piece[(size_t)p]) continue;
            const uint64_t pbeg = p * 8192, pend = pbeg + 8192;
            uint64_t freecov = 0;
            for (const auto &f : nd.free) {
                const uint64_t a = std::max(pbeg, f.first);
                const uint64_t b = std::min(pend, f.first + f.second);
                if (b > a) freecov += b - a;
            }
            if (freecov >= 8192) continue;   /* 整片空闲：未分配，非泄漏 */
            sum_.namedata.leaked++;
            if (shown++ < 64)
                rep_->warn(scope_t::namedata, "leak_piece", "DIR inum=%llu 名字片 %llu 已用但无长名引用（空间未回收）",
                           (unsigned long long)inum, (unsigned long long)p);
        }
    }
}

void audit_session::reconcile_block(dfs_ctx_t &c)
{
    const uint64_t total = img_->geo().total_blocks;
    const uint64_t lim = std::min<uint64_t>(c.meta_lo, total);
    const uint8_t *disk_bm = img_->fsblk(img_->geo().bitmap_start);
    const size_t nbytes = (size_t)((lim + 7) / 8);

    uint64_t owned = 0, allocated = 0;
    for (size_t i = 0; i < nbytes; i++) {
        owned     += (uint64_t)__builtin_popcount(c.shadow_blk[i]);
        allocated += (uint64_t)__builtin_popcount(disk_bm[i]);
    }
    c.sum->block.owned = owned;
    c.sum->block.allocated = allocated;

    /* [FORK/LEAKSCAN] 漏块魔数扫描账本：按 8K 单元（块号 & ~1）去重。 */
    uint64_t ls_units = 0;
    uint64_t ls_kind[(size_t)leak_kind_t::kind_count] = {0};
    uint64_t last_unit = ~0ULL;

    size_t shown = 0;
    for (size_t i = 0; i < nbytes; i++) {
        uint8_t x = (uint8_t)(c.shadow_blk[i] ^ disk_bm[i]);
        while (x) {
            const int b = __builtin_ctz((unsigned)x);
            x = (uint8_t)(x & (x - 1));
            const uint64_t blk = (uint64_t)i * 8 + (uint64_t)b;
            if (blk >= lim) continue;
            if (c.is_backup_blk(blk)) continue;
            const bool reach = c.blk_test(blk);
            if (reach) {
                sum_.block.unprotected++;
                if (shown++ < 64) rep_->error(scope_t::block, "UNPROT", "块 %llu 可达但位图暗（没有被保护）", (unsigned long long)blk);
            } else {
                sum_.block.leaked++;
                if (shown++ < 64) rep_->error(scope_t::block, "LEAK", "块 %llu 位图亮但不可达（疑似泄漏）", (unsigned long long)blk);

                /* [FORK/LEAKSCAN] 每个 8K 单元只扫首块、去重；按魔数判定类型。 */
                const uint64_t unit = blk & ~1ULL;
                if (unit != last_unit) {
                    last_unit = unit;
                    const uint8_t *p = img_->fsblk(unit);
                    const leak_kind_t k = leak_classify(p);
                    uint64_t m0 = 0, m1 = 0;
                    std::memcpy(&m0, p, 8);
                    std::memcpy(&m1, p + 8, 8);
                    ls_units++;
                    ls_kind[(size_t)k]++;
                    AUDIT_TRACE("[TRACE][LEAKSCAN] unit=%llu magic0=%#018llx magic1=%#018llx kind=%s\n",
                                (unsigned long long)unit, (unsigned long long)m0,
                                (unsigned long long)m1, leak_kind_str(k));
                }

                /* [FORK/LEAKREPAIR] 发现一个泄露→修复一个：清掉该块在盘上位图的分配位。
                 * 仅当 --free-leaks 且 repair fd 已就绪；逐块修复（不是逐单元）。 */
                if (g_free_leaks) {
                    if (leak_repair_clear(blk)) {
                        g_leaks_repaired++;
                        AUDIT_TRACE("[TRACE][LEAKSCAN-REPAIR] blk=%llu freed=1\n",
                                    (unsigned long long)blk);
                    } else {
                        g_leaks_repair_failed++;
                        AUDIT_TRACE("[TRACE][LEAKSCAN-REPAIR] blk=%llu freed=0 (write failed)\n",
                                    (unsigned long long)blk);
                    }
                }
            }
        }
    }

    /* [FORK/LEAKSCAN] 汇总（不依赖 --trace）：直接跟在正常 SUM 之后，便于一眼定性。 */
    AUDIT_TRACE("[TRACE][LEAKSCAN-SUM] units=%llu blocks=%llu\n",
                (unsigned long long)ls_units, (unsigned long long)sum_.block.leaked);
    fprintf(rep_->out(),
            "[AUDIT][SUM][LEAKSCAN] units=%llu extent_tree_node=%llu dentry_tree_node=%llu "
            "namedata_map_node=%llu namedata_fr_node=%llu inode_map_node=%llu "
            "inode_fr_snapshot=%llu xattr_block=%llu data_or_unknown=%llu\n",
            (unsigned long long)ls_units,
            (unsigned long long)ls_kind[(size_t)leak_kind_t::bptree],
            (unsigned long long)ls_kind[(size_t)leak_kind_t::dentry],
            (unsigned long long)ls_kind[(size_t)leak_kind_t::namedata_extent],
            (unsigned long long)ls_kind[(size_t)leak_kind_t::namedata_fr],
            (unsigned long long)ls_kind[(size_t)leak_kind_t::inode_extent],
            (unsigned long long)ls_kind[(size_t)leak_kind_t::inode_fr],
            (unsigned long long)ls_kind[(size_t)leak_kind_t::xattr],
            (unsigned long long)ls_kind[(size_t)leak_kind_t::data]);
    if (g_free_leaks)
        fprintf(rep_->out(), "[AUDIT][SUM][LEAKREPAIR] freed=%llu failed=%llu\n",
                (unsigned long long)g_leaks_repaired, (unsigned long long)g_leaks_repair_failed);
}

/* 名字面块占用归因：shadow_name 是 shadow_blk 的子集（walk_name_* 并行打标）。
 * owned = 名字面可达块；allocated = 其中位图亮者；unprotected = owned - allocated。
 * 这些块已被 BLOCK 面 XOR 覆盖，故 namedata.unprotected 不计入 total_error（避免重复计数）；
 * leaked 无法按子空间归因（磁盘位图无名字归属），沿用相位 B 的逻辑片级泄漏。 */
void audit_session::reconcile_namedata(dfs_ctx_t &c)
{
    const uint64_t total = img_->geo().total_blocks;
    const uint64_t lim = std::min<uint64_t>(c.meta_lo, total);
    const uint8_t *disk_bm = img_->fsblk(img_->geo().bitmap_start);
    const size_t nbytes = (size_t)((lim + 7) / 8);

    uint64_t owned = 0, allocated = 0;
    for (size_t i = 0; i < nbytes; i++) {
        owned     += (uint64_t)__builtin_popcount(c.shadow_name[i]);
        allocated += (uint64_t)__builtin_popcount((uint8_t)(c.shadow_name[i] & disk_bm[i]));
    }
    c.sum->namedata.owned = owned;
    c.sum->namedata.allocated = allocated;
    c.sum->namedata.unprotected = owned - allocated;
}

void audit_session::reconcile_inode(dfs_ctx_t &c)
{
    if (!c.alloc || !c.alloc->available()) {
        sum_.inode_xor_available = false;
        return;
    }
    const uint64_t n = c.ino_bits;
    uint64_t owned = 0, allocated = 0, shown = 0;
    for (uint64_t inum = 0; inum < n; inum++) {
        const bool reach = c.ino_test(inum);
        const bool alloc = c.alloc->allocated(inum);
        if (reach) owned++;
        if (alloc) allocated++;
        if (reach == alloc) continue;
        if (reach) {
            sum_.inode.unprotected++;
            if (shown < 64) { shown++; rep_->error(scope_t::inode, "UNPROT_INO", "inode %llu 可达但未分配（没有被保护）", (unsigned long long)inum); }
        } else {
            sum_.inode.leaked++;
            if (shown < 64) { shown++; rep_->info(scope_t::inode, "LEAK_INO inode %llu 已分配但不可达（疑似泄漏）", (unsigned long long)inum); }
        }
    }
    if (sum_.inode.unprotected + sum_.inode.leaked > shown)
        rep_->info(scope_t::inode, "inode XOR 差异 %llu 条，仅打印前 %llu 条",
                   (unsigned long long)(sum_.inode.unprotected + sum_.inode.leaked), (unsigned long long)shown);
    c.sum->inode.owned = owned;
    c.sum->inode.allocated = allocated;
}

void audit_session::phase_c_reconcile()
{
    reconcile_block(ctx_);
    reconcile_namedata(ctx_);
    reconcile_inode(ctx_);
}

} /* namespace rfs_audit */
