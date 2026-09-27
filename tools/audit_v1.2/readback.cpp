/* ═══════════════════════════════════════════════════════════════════════════
 * readback.cpp — M4.1 只读回放/校验器（**host 侧，独立于 ko / 独立于 juvenile_god**）
 *
 * 给定 mkstates 造的镜像 + 其 `.expect` 清单：
 *   · dump   <expect> <outdir>  ：把每文件「应有内容」落成 ground-truth 文件（供 guest `cmp`）
 *   · verify <image> <expect>   ：host 侧独立解析镜像（SB/几何/inode/三态/区段树），
 *                                 逐字节断言 == 应有内容，并校 XXH64。
 *   · list   <expect>           ：打印清单（名字 / inode / size / 段 / xxh64）
 *
 * 纪律：**只读**（O_RDONLY）；不复用 audit_v1.2 代码（自证独立）；不依赖 ko。
 *
 * 用法： ./readback {dump <expect> <outdir> | verify <image> <expect> | list <expect>}
 * 退出码： 0 = 全部一致 / 成功；1 = 有 BAD；2 = 用法 / IO 错误
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "disk_struct_v1.2.h"
#include "magics.h"
#include "m41_expect.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static constexpr uint64_t BLK  = 4096;
static constexpr uint64_t NODE = 8192;
static constexpr uint64_t PTR52 = (1ULL << 52) - 1;
static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

/* ─────────────────────── 只读镜像读器（自证独立实现） ─────────────────────── */
struct Reader {
    int fd = -1;
    uint64_t total_blocks = 0, inode_count = 0, root_inode = 0, inode_array_base = 0;
    bool bad = false;

    bool rd(void *buf, size_t n, uint64_t off) {
        uint8_t *p = (uint8_t *)buf; size_t d = 0;
        while (d < n) { ssize_t r = pread(fd, p + d, n - d, (off_t)(off + d)); if (r <= 0) return false; d += (size_t)r; }
        return true;
    }
    bool read_inode(uint64_t idx, raven_Inode_t &ino) {
        return rd(&ino, 256, blkoff(inode_array_base) + idx * 256);
    }
    bool read_node(uint64_t blk, std::vector<uint8_t> &buf) {
        if (blk >= total_blocks) return false;
        buf.assign(NODE, 0);
        return rd(buf.data(), NODE, blkoff(blk));
    }

    /* 区段树 → 逻辑块 b → 物理块 phys 的映射（洞不入表） */
    void collect_extents(uint64_t blk, std::unordered_map<uint64_t, uint64_t> &map, int depth) {
        if (depth > 64) { fprintf(stderr, "readback: extent 深度超限 blk=%llu\n", (unsigned long long)blk); bad = true; return; }
        std::vector<uint8_t> nb;
        if (!read_node(blk, nb)) { fprintf(stderr, "readback: 读节点失败 blk=%llu\n", (unsigned long long)blk); bad = true; return; }
        const Bptree_meta_t *meta = (const Bptree_meta_t *)nb.data();
        uint32_t cnt = RFS_PP_COUNT(meta->parent_ptr);
        if (!RFS_PP_IS_INNER(meta->parent_ptr)) {
            const Bptree_extent_leaf_node_t *leaf = (const Bptree_extent_leaf_node_t *)nb.data();
            for (uint32_t i = 0; i < cnt; i++) {
                uint64_t f = leaf->extents[i].fblkbase, l = leaf->extents[i].length;
                uint64_t p = RFS_PTR_ADDR(leaf->extents[i].block_base);
                for (uint64_t k = 0; k < l; k++) map[f + k] = p + k;
            }
        } else {
            const Bptree_extent_inner_node_t *nd = (const Bptree_extent_inner_node_t *)nb.data();
            for (uint32_t i = 0; i < cnt; i++)
                collect_extents(RFS_PTR_ADDR(nd->extents[i].sub_node_ptr), map, depth + 1);
        }
    }

    /* 按三态抽取 inode 文件内容 [0,size) */
    bool extract(const raven_Inode_t &ino, std::vector<uint8_t> &out) {
        uint32_t direct = RFS_INO1_GET_DIRECT(ino.word1);
        uint64_t size = ino.size;
        out.assign((size_t)size, 0);
        if (size <= 160) {                       /* inline */
            memcpy(out.data(), ino.inline_data, (size_t)size);
            return true;
        }
        if (direct) {                            /* direct：连续 run */
            for (uint64_t o = 0; o < size; o += BLK) {
                std::vector<uint8_t> b(BLK, 0);
                if (!rd(b.data(), BLK, blkoff(ino.data_root + o / BLK))) { bad = true; return false; }
                uint64_t n = std::min(BLK, size - o);
                memcpy(out.data() + o, b.data(), (size_t)n);
            }
            return true;
        }
        /* extent：树 → 映射；未覆盖块留 0（洞） */
        std::unordered_map<uint64_t, uint64_t> map;
        collect_extents(ino.data_root, map, 0);
        if (bad) return false;
        for (uint64_t o = 0; o < size; o += BLK) {
            uint64_t lb = o / BLK;
            auto it = map.find(lb);
            if (it == map.end()) continue;       /* 洞 → 0 */
            std::vector<uint8_t> b(BLK, 0);
            if (!rd(b.data(), BLK, blkoff(it->second))) { bad = true; return false; }
            uint64_t n = std::min(BLK, size - o);
            memcpy(out.data() + o, b.data(), (size_t)n);
        }
        return true;
    }
};

/* ─────────────────────── mkdir -p 帮助 ─────────────────────── */
static void mkdir_p(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
    }
}

/* ─────────────────────── 命令 ─────────────────────── */
static int cmd_list(const char *expect) {
    std::vector<M41File> files;
    if (!m41_read_expect(expect, files)) { fprintf(stderr, "readback: 解析 expect 失败 %s\n", expect); return 2; }
    for (const M41File &f : files) {
        printf("%-16s inode=%-4llu size=%-8llu xxh64=%016llx segs=%zu\n",
               f.name.c_str(), (unsigned long long)f.inode, (unsigned long long)f.size,
               (unsigned long long)m41_hash(f), f.segs.size());
        for (const M41Seg &s : f.segs)
            printf("    S off=%-8llu len=%-8llu %c\n",
                   (unsigned long long)s.off, (unsigned long long)s.len, s.kind);
    }
    printf("readback: %zu 文件\n", files.size());
    return 0;
}

static int cmd_dump(const char *expect, const char *outdir) {
    std::vector<M41File> files;
    if (!m41_read_expect(expect, files)) { fprintf(stderr, "readback: 解析 expect 失败 %s\n", expect); return 2; }
    mkdir_p(outdir);
    for (const M41File &f : files) {
        std::vector<uint8_t> b;
        m41_build(f, b);
        std::string path = std::string(outdir) + "/" + f.name;
        mkdir_p(path);
        FILE *fp = fopen(path.c_str(), "wb");
        if (!fp) { fprintf(stderr, "readback: 写 %s 失败\n", path.c_str()); return 2; }
        if (!b.empty()) fwrite(b.data(), 1, b.size(), fp);
        fclose(fp);
        printf("DUMP %-16s size=%-8llu xxh64=%016llx -> %s\n",
               f.name.c_str(), (unsigned long long)f.size, (unsigned long long)m41_hash(f), path.c_str());
    }
    printf("readback: dumped %zu 文件 → %s\n", files.size(), outdir);
    return 0;
}

static int cmd_verify(const char *image, const char *expect) {
    std::vector<M41File> files;
    if (!m41_read_expect(expect, files)) { fprintf(stderr, "readback: 解析 expect 失败 %s\n", expect); return 2; }

    struct stat st;
    if (stat(image, &st) < 0) { fprintf(stderr, "readback: stat %s 失败\n", image); return 2; }
    Reader r; r.fd = open(image, O_RDONLY);
    if (r.fd < 0) { fprintf(stderr, "readback: open %s 失败\n", image); return 2; }

    RavenSuperblock sb;
    if (!r.rd(&sb, sizeof(sb), 0)) { fprintf(stderr, "readback: 读 SB 失败\n"); return 2; }
    if (sb.magic != SUPERBLOCK_MAGIC || sb.version != RAVENFS_VERSION) {
        fprintf(stderr, "readback: SB magic/version 不符（magic=%#llx ver=%u）\n",
                (unsigned long long)sb.magic, sb.version);
        return 2;
    }
    r.total_blocks = sb.total_blocks;
    r.inode_count  = sb.inode_array_count;
    r.root_inode   = sb.root_inode;
    uint64_t bb = (r.total_blocks + 32767) / 32768;
    uint64_t ib = (r.inode_count * 256 + 4095) / 4096;
    r.inode_array_base = (r.total_blocks - bb) - ib;

    printf("=== readback verify (host, readonly) : %s ===\n", image);
    printf("geo total_blocks=%llu inode_count=%llu inode_array_base=%llu\n",
           (unsigned long long)r.total_blocks, (unsigned long long)r.inode_count,
           (unsigned long long)r.inode_array_base);

    int nbad = 0;
    for (const M41File &f : files) {
        raven_Inode_t ino;
        if (!r.read_inode(f.inode, ino)) {
            printf("[BAD] %-16s 读 inode %llu 失败\n", f.name.c_str(), (unsigned long long)f.inode);
            nbad++; continue;
        }
        uint32_t type = RFS_INO1_GET_TYPE(ino.word1);
        if (type != INODE_TYPE_REGULAR) {
            printf("[BAD] %-16s inode %llu type=%u != REG\n", f.name.c_str(), (unsigned long long)f.inode, type);
            nbad++; continue;
        }
        if (ino.size != f.size) {
            printf("[BAD] %-16s size 盘=%llu 期望=%llu\n", f.name.c_str(),
                   (unsigned long long)ino.size, (unsigned long long)f.size);
            nbad++; continue;
        }
        std::vector<uint8_t> got;
        if (!r.extract(ino, got)) {
            printf("[BAD] %-16s 抽取失败（引擎错误）\n", f.name.c_str());
            nbad++; continue;
        }
        std::vector<uint8_t> exp;
        m41_build(f, exp);

        int diff = -1;
        for (size_t i = 0; i < exp.size(); i++)
            if (got[i] != exp[i]) { diff = (int)i; break; }
        uint64_t hg = (uint64_t)XXH64(got.data(), got.size(), 0);
        uint64_t he = m41_hash(f);

        if (diff >= 0) {
            printf("[BAD] %-16s 字节不符 @off=%d 盘=%02x 期望=%02x（xxh64 盘=%016llx 期望=%016llx）\n",
                   f.name.c_str(), diff, got[diff], exp[diff],
                   (unsigned long long)hg, (unsigned long long)he);
            nbad++;
        } else if (hg != he) {
            printf("[BAD] %-16s xxh64 不符 盘=%016llx 期望=%016llx\n",
                   f.name.c_str(), (unsigned long long)hg, (unsigned long long)he);
            nbad++;
        } else {
            const char *state = f.size <= 160 ? "inline" : (RFS_INO1_GET_DIRECT(ino.word1) ? "direct" : "extent");
            printf("[OK ] %-16s inode=%-4llu size=%-8llu state=%-6s xxh64=%016llx\n",
                   f.name.c_str(), (unsigned long long)f.inode, (unsigned long long)f.size, state,
                   (unsigned long long)hg);
        }
    }
    close(r.fd);
    printf("readback: RESULT: %s（%zu 文件, BAD=%d）\n", nbad == 0 ? "PASS" : "FAIL", files.size(), nbad);
    return nbad == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "list") == 0)   return cmd_list(argv[2]);
    if (argc == 3 && strcmp(argv[1], "verify") == 0)  { fprintf(stderr, "usage: readback verify <image> <expect>\n"); return 2; }
    if (argc == 4 && strcmp(argv[1], "dump") == 0)   return cmd_dump(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "verify") == 0) return cmd_verify(argv[2], argv[3]);
    fprintf(stderr,
        "usage:\n"
        "  readback dump   <expect> <outdir>\n"
        "  readback verify <image>  <expect>\n"
        "  readback list   <expect>\n");
    return 2;
}
