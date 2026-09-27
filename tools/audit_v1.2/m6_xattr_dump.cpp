/* ═══════════════════════════════════════════════════════════════════════════
 * m6_xattr_dump.cpp — M6（xattr / ACL / capability）**host 侧只读回放/校验器**
 *
 * 独立于 audit_v1.2.cpp（自证独立实现）、独立于 ko：
 *   · list   <image>            ：扫描全部亮灯 inode 的 xattr 块，逐条打印（人类可读）
 *   · verify <image> <xexpect>  ：独立解析镜像（SB/几何/inode/xattr 块），逐 inode 断言
 *                                 == m6_xexpect.h 清单（index / 后缀 / 值长 / 值 XXH64），
 *                                 并校「无 xattr 哨兵 / 有块且终止符存在」。
 *   · dump   <image> <out>      ：把镜像实况写成 xexpect 格式（`X <inode> <inode> ...`），
 *                                 供与 mkstates_x 产出的 .xexpect 人工/机器比对（顺序按 inode）。
 *
 * 纪律：**只读**（O_RDONLY）；不复用 audit_v1.2 代码（自证独立）。
 * 用法： ./m6_xattr_dump {list <image> | verify <image> <xexpect> | dump <image> <out>}
 * 退出码： 0 = 全部一致 / 成功；1 = 有 BAD；2 = 用法 / IO 错误
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "disk_struct_v1.2.h"
#include "magics.h"
#include "m6_xexpect.h"

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
static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }
static inline uint64_t rd_le64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

struct Reader {
    int fd = -1;
    uint64_t total_blocks = 0, inode_count = 0, inode_array_base = 0, inode_bm_base = 0, inode_bm_blocks = 0;
    std::vector<uint8_t> ibmp;
    bool bad = false;

    bool rd(void *buf, size_t n, uint64_t off) {
        uint8_t *p = (uint8_t *)buf; size_t d = 0;
        while (d < n) { ssize_t r = pread(fd, p + d, n - d, (off_t)(off + d)); if (r <= 0) return false; d += (size_t)r; }
        return true;
    }
    bool read_inode(uint64_t idx, uint8_t *raw) { return rd(raw, 256, blkoff(inode_array_base) + idx * 256); }
    bool read_block(uint64_t b, std::vector<uint8_t> &buf) {
        if (b >= total_blocks) return false;
        buf.assign(BLK, 0);
        return rd(buf.data(), BLK, blkoff(b));
    }
    bool ino_lit(uint64_t i) const { return (ibmp[i >> 3] >> (i & 7)) & 1; }
};

/* 解析一个 xattr 块 → 条目列表（index/后缀/值长/值 XXH64）+ 终止符存在性 */
struct PEnt { uint8_t index; std::string name; uint32_t size; uint64_t hash; bool value_ok; };

struct ParseResult {
    std::vector<PEnt> ents;
    bool terminated = false;
    bool magic_ok = false;
    std::string err;
};

static ParseResult parse_xattr_block(const std::vector<uint8_t> &blk) {
    ParseResult r;
    if (blk.size() < RFS_XATTR_HDR_SIZE) { r.err = "块过小"; return r; }
    const rfs_xattr_block_hdr *h = (const rfs_xattr_block_hdr *)blk.data();
    r.magic_ok = (h->h_magic == RFS_XATTR_H_MAGIC);
    uint64_t off = RFS_XATTR_HDR_SIZE;
    while (off + 4 <= blk.size()) {
        const rfs_xattr_entry *e = (const rfs_xattr_entry *)(blk.data() + off);
        if (*(const uint32_t *)(blk.data() + off) == 0u) { r.terminated = true; break; }
        if (off + RFS_XATTR_ENTRY_HDR_SIZE > blk.size()) { r.err = "条目头越界"; break; }
        PEnt pe;
        pe.index = e->e_name_index;
        pe.size  = e->e_value_size;
        uint64_t nl = e->e_name_len;
        uint64_t ent_len = RFS_XATTR_LEN(nl);
        if (off + ent_len > blk.size()) { r.err = "条目越界"; break; }
        if (nl) pe.name.assign((const char *)(blk.data() + off + RFS_XATTR_ENTRY_HDR_SIZE), nl);
        pe.value_ok = true;
        pe.hash = 0;
        if (e->e_value_size) {
            uint64_t vo = e->e_value_offs, vs = e->e_value_size;
            if (vo < RFS_XATTR_HDR_SIZE || vo + vs > blk.size() || (vo & 3)) {
                pe.value_ok = false;
            } else {
                pe.hash = xexp_hash(blk.data() + vo, vs);
            }
        } else {
            pe.hash = xexp_hash("", 0);
        }
        r.ents.push_back(pe);
        off += ent_len;
    }
    return r;
}

static bool load_meta(Reader &r, const char *image) {
    struct stat st;
    if (stat(image, &st) < 0) { fprintf(stderr, "m6_xattr_dump: stat %s 失败\n", image); return false; }
    r.fd = open(image, O_RDONLY);
    if (r.fd < 0) { fprintf(stderr, "m6_xattr_dump: open %s 失败\n", image); return false; }
    RavenSuperblock sb;
    if (!r.rd(&sb, sizeof(sb), 0)) { fprintf(stderr, "m6_xattr_dump: 读 SB 失败\n"); return false; }
    if (sb.magic != SUPERBLOCK_MAGIC || sb.version != RAVENFS_VERSION) {
        fprintf(stderr, "m6_xattr_dump: SB magic/version 不符\n"); return false;
    }
    r.total_blocks = sb.total_blocks;
    r.inode_count  = sb.inode_array_count;
    uint64_t bb  = (r.total_blocks + 32767) / 32768;
    uint64_t ib  = (r.inode_count * 256 + 4095) / 4096;
    r.inode_array_base = (r.total_blocks - bb) - ib;
    uint64_t ibb = (r.inode_count + 32767) / 32768;
    r.inode_bm_blocks = ibb;
    r.inode_bm_base = r.inode_array_base - ibb;
    r.ibmp.assign((size_t)ibb * BLK, 0);
    if (!r.rd(r.ibmp.data(), r.ibmp.size(), blkoff(r.inode_bm_base))) {
        fprintf(stderr, "m6_xattr_dump: 读 inode bitmap 失败\n"); return false;
    }
    return true;
}

static const char *ns_name(uint8_t idx) {
    switch (idx) {
    case RFS_XATTR_INDEX_USER: return "user";
    case RFS_XATTR_INDEX_POSIX_ACL_ACCESS: return "system.posix_acl_access";
    case RFS_XATTR_INDEX_POSIX_ACL_DEFAULT: return "system.posix_acl_default";
    case RFS_XATTR_INDEX_TRUSTED: return "trusted";
    case RFS_XATTR_INDEX_SECURITY: return "security";
    case RFS_XATTR_INDEX_SYSTEM: return "system";
    default: return "?";
    }
}
static std::string full_key(const PEnt &e) {
    std::string k = ns_name(e.index);
    if (!e.name.empty()) k += "." + e.name;
    return k;
}

/* ── list ── */
static int cmd_list(const char *image) {
    Reader r;
    if (!load_meta(r, image)) return 2;
    printf("=== m6_xattr_dump list (host, readonly) : %s ===\n", image);
    uint64_t nblk = 0, nent = 0;
    for (uint64_t i = 0; i < r.inode_count; i++) {
        if (!r.ino_lit(i)) continue;
        uint8_t raw[256];
        if (!r.read_inode(i, raw)) continue;
        uint64_t xptr = RFS_XATTRPTR_GET(rd_le64(raw + 32));
        if (xptr == RFS_XATTR_NONE) continue;
        uint32_t type = RFS_INO1_GET_TYPE(rd_le64(raw + 0));
        std::vector<uint8_t> blk;
        if (!r.read_block(xptr, blk)) { printf("inode %llu xptr=%llu 读块失败\n", (unsigned long long)i, (unsigned long long)xptr); continue; }
        ParseResult pr = parse_xattr_block(blk);
        nblk++;
        printf("inode %-4llu type=%-2u xattr_blk=%-6llu magic=%s term=%s entries=%zu\n",
               (unsigned long long)i, type, (unsigned long long)xptr,
               pr.magic_ok ? "ok" : "BAD", pr.terminated ? "ok" : "MISSING", pr.ents.size());
        for (const PEnt &e : pr.ents) {
            printf("    [%s] size=%-5u xxh64=%016llx%s\n",
                   full_key(e).c_str(), e.size, (unsigned long long)e.hash,
                   e.value_ok ? "" : "  <值偏移非法>");
            nent++;
        }
    }
    printf("m6_xattr_dump: xattr_blocks=%llu entries=%llu\n",
           (unsigned long long)nblk, (unsigned long long)nent);
    close(r.fd);
    return 0;
}

/* ── verify ── */
static int cmd_verify(const char *image, const char *xexpect) {
    std::vector<XExpectIno> exps;
    if (!m6_read_xexpect(xexpect, exps)) { fprintf(stderr, "m6_xattr_dump: 解析 %s 失败\n", xexpect); return 2; }
    Reader r;
    if (!load_meta(r, image)) return 2;
    printf("=== m6_xattr_dump verify (host, readonly) : %s ===\n", image);

    int nbad = 0;
    for (const XExpectIno &x : exps) {
        uint8_t raw[256];
        if (!r.read_inode(x.inode, raw)) {
            printf("[BAD] %-18s 读 inode %llu 失败\n", x.path.c_str(), (unsigned long long)x.inode);
            nbad++; continue;
        }
        uint64_t xptr = RFS_XATTRPTR_GET(rd_le64(raw + 32));
        if (x.type == 0) {
            if (xptr != RFS_XATTR_NONE) {
                printf("[BAD] %-18s 期望无 xattr，但 ptr=%llu\n", x.path.c_str(), (unsigned long long)xptr);
                nbad++;
            } else {
                printf("[OK ] %-18s inode=%-4llu no-xattr（哨兵）\n", x.path.c_str(), (unsigned long long)x.inode);
            }
            continue;
        }
        if (xptr == RFS_XATTR_NONE) {
            printf("[BAD] %-18s 期望有 xattr，但 ptr=哨兵\n", x.path.c_str());
            nbad++; continue;
        }
        std::vector<uint8_t> blk;
        if (!r.read_block(xptr, blk)) {
            printf("[BAD] %-18s 读 xattr 块 %llu 失败\n", x.path.c_str(), (unsigned long long)xptr);
            nbad++; continue;
        }
        ParseResult pr = parse_xattr_block(blk);
        bool okay = pr.magic_ok && pr.terminated && pr.err.empty() && pr.ents.size() == x.ents.size();
        if (!pr.magic_ok)     printf("[BAD] %-18s xattr 块 magic 错\n", x.path.c_str());
        if (!pr.terminated)   printf("[BAD] %-18s xattr 块缺终止符\n", x.path.c_str());
        if (!pr.err.empty())  printf("[BAD] %-18s xattr 块解析: %s\n", x.path.c_str(), pr.err.c_str());
        if (pr.ents.size() != x.ents.size())
            printf("[BAD] %-18s xattr 条目数 盘=%zu 期望=%zu\n", x.path.c_str(), pr.ents.size(), x.ents.size());
        size_t n = std::min(pr.ents.size(), x.ents.size());
        for (size_t i = 0; i < n; i++) {
            const PEnt &g = pr.ents[i]; const XExpectEntry &e = x.ents[i];
            if (g.index != e.index) {
                printf("[BAD] %-18s 条目%zu index 盘=%u 期望=%u\n", x.path.c_str(), i, g.index, e.index); okay = false;
            }
            if (g.name != e.name) {
                printf("[BAD] %-18s 条目%zu 名 盘=\"%s\" 期望=\"%s\"\n", x.path.c_str(), i, g.name.c_str(), e.name.c_str()); okay = false;
            }
            if (g.size != e.size) {
                printf("[BAD] %-18s 条目%zu 值长 盘=%u 期望=%u\n", x.path.c_str(), i, g.size, e.size); okay = false;
            }
            if (!g.value_ok) {
                printf("[BAD] %-18s 条目%zu 值偏移非法\n", x.path.c_str(), i); okay = false;
            } else if (g.hash != e.hash) {
                printf("[BAD] %-18s 条目%zu 值指纹 盘=%016llx 期望=%016llx\n", x.path.c_str(), i,
                       (unsigned long long)g.hash, (unsigned long long)e.hash); okay = false;
            }
        }
        if (!okay) nbad++;
        else {
            printf("[OK ] %-18s inode=%-4llu xattr_blk=%-6llu %zu 条（magic/终止符/指纹 全验）\n",
                   x.path.c_str(), (unsigned long long)x.inode, (unsigned long long)xptr, x.ents.size());
        }
    }
    close(r.fd);
    printf("m6_xattr_dump: RESULT: %s（%zu inode, BAD=%d）\n",
           nbad == 0 ? "PASS" : "FAIL", exps.size(), nbad);
    return nbad == 0 ? 0 : 1;
}

/* ── dump（按 inode 顺序落 xexpect：供与生产者 .xexpect 对比）── */
static int cmd_dump(const char *image, const char *out) {
    Reader r;
    if (!load_meta(r, image)) return 2;
    std::vector<XExpectIno> outv;
    for (uint64_t i = 0; i < r.inode_count; i++) {
        if (!r.ino_lit(i)) continue;
        uint8_t raw[256];
        if (!r.read_inode(i, raw)) continue;
        uint64_t xptr = RFS_XATTRPTR_GET(rd_le64(raw + 32));
        if (xptr == RFS_XATTR_NONE) continue;
        std::vector<uint8_t> blk;
        if (!r.read_block(xptr, blk)) continue;
        ParseResult pr = parse_xattr_block(blk);
        XExpectIno x; x.path = std::to_string(i); x.inode = i; x.type = 1;
        for (const PEnt &e : pr.ents)
            x.ents.push_back(XExpectEntry{ e.index, e.name, e.size, e.hash });
        outv.push_back(x);
    }
    if (!m6_write_xexpect(out, outv)) { fprintf(stderr, "m6_xattr_dump: 写 %s 失败\n", out); return 2; }
    printf("m6_xattr_dump: dump %zu inode with xattr → %s\n", outv.size(), out);
    close(r.fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "list") == 0)   return cmd_list(argv[2]);
    if (argc == 4 && strcmp(argv[1], "verify") == 0) return cmd_verify(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "dump") == 0)   return cmd_dump(argv[2], argv[3]);
    fprintf(stderr,
        "usage:\n"
        "  m6_xattr_dump list   <image>\n"
        "  m6_xattr_dump verify <image> <xexpect>\n"
        "  m6_xattr_dump dump   <image> <out.xexpect>\n");
    return 2;
}
