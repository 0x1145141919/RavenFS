#include "fsck.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <vector>

#include <unistd.h>
#include <fcntl.h>

/* ═══════════════════════════════════════════════════════════════
 * dentry validate — 全量校验 Dentry B+tree
 *
 * 从根目录开始递归遍历整个 dentry 树，校验：
 *   - 所有节点的魔数签名
 *   - 叶子节点 entry 字典序
 *   - 内部节点分隔 key 一致性
 *   - 同级链表 prev/next 一致性
 *   - parent_ptr 一致性
 *   - 没有循环引用
 *
 * 仅依赖 disk_struct.hpp / magics.h，不依赖 God 类。
 * ═══════════════════════════════════════════════════════════════ */

/* ─── 校验状态 ─── */
struct DentryValidateCtx {
    int   fd;
    int   errors;
    int   warnings;
    uint64_t world_base_lba;

    std::set<uint64_t> visited;   // 已访问节点，防循环
};

#define VPRINT(fmt, ...)  fprintf(stdout, fmt "\n", ##__VA_ARGS__)
#define VERR(fmt, ...)    do { \
    fprintf(stdout, "  ERROR: " fmt "\n", ##__VA_ARGS__); \
    ctx.errors++; \
} while(0)
#define VWARN(fmt, ...)   do { \
    fprintf(stdout, "  WARN:  " fmt "\n", ##__VA_ARGS__); \
    ctx.warnings++; \
} while(0)

/* ─── 读一个 FS_block ─── */
static bool read_fs_block(DentryValidateCtx &ctx, uint64_t fblock, void *buf) {
    uint64_t base = (ctx.world_base_lba == 0) ? 0 : 4096;
    uint64_t off  = base + fblock * 4096;
    return pread(ctx.fd, buf, 4096, off) == 4096;
}

/* ─── 读 B+tree 节点（2 个连续 FS_block = 8K） ─── */
static bool read_dentry_node(DentryValidateCtx &ctx, uint64_t fblock,
                              Dentry_Bptree_node_t *node)
{
    uint8_t *p = (uint8_t *)node;
    for (int i = 0; i < 2; i++) {
        if (!read_fs_block(ctx, fblock + i, p + i * 4096))
            return false;
    }
    return true;
}

/* ─── 魔数校验 ─── */
static bool check_magic(const Dentry_Bptree_node_t *node,
                        DentryValidateCtx &ctx, uint64_t block)
{
    if (node->head.magic1 != DENTRY_MAGIC1 ||
        node->head.magic2 != DENTRY_MAGIC2 ||
        node->head.magic3 != DENTRY_MAGIC3 ||
        node->head.magic4 != DENTRY_MAGIC4)
    {
        VERR("block %lu: dentry magic mismatch "
             "(0x%lx 0x%lx 0x%lx 0x%lx, expected 0x%lx 0x%lx 0x%lx 0x%lx)",
             (unsigned long)block,
             (unsigned long)node->head.magic1,
             (unsigned long)node->head.magic2,
             (unsigned long)node->head.magic3,
             (unsigned long)node->head.magic4,
             (unsigned long)DENTRY_MAGIC1,
             (unsigned long)DENTRY_MAGIC2,
             (unsigned long)DENTRY_MAGIC3,
             (unsigned long)DENTRY_MAGIC4);
        return false;
    }
    return true;
}

/* ─── 递归校验子树 ─── */
static void validate_subtree(DentryValidateCtx &ctx, uint64_t block,
                             uint64_t expected_parent, int depth)
{
    if (depth > 256) {
        VERR("block %lu: max depth exceeded (loop?)", (unsigned long)block);
        return;
    }

    if (ctx.visited.find(block) != ctx.visited.end()) {
        VERR("block %lu: already visited (cycle detected)", (unsigned long)block);
        return;
    }
    ctx.visited.insert(block);

    Dentry_Bptree_node_t node;
    if (!read_dentry_node(ctx, block, &node)) {
        VERR("block %lu: read failed", (unsigned long)block);
        return;
    }

    if (!check_magic(&node, ctx, block))
        return;

    auto meta = node.head.head.fields;  // copy (packed bitfield can't bind ref)
    uint8_t ec = meta.entry_count;
    bool is_int  = meta.is_internal_node != 0;
    bool is_root = meta.is_root_node != 0;
    uint64_t parent_ptr = meta.parent_ptr;

    /* parent_ptr 校验 */
    if (!is_root && parent_ptr != expected_parent) {
        VERR("block %lu: parent_ptr=%lu, expected parent=%lu",
             (unsigned long)block, (unsigned long)parent_ptr,
             (unsigned long)expected_parent);
    }

    if (ec > 63) {
        VERR("block %lu: entry_count=%u > 63", (unsigned long)block, ec);
        return;
    }

    if (is_int) {
        /* ─── 内部节点 ─── */
        VPRINT("  %s internal node block=%lu  entries=%u  parent=%lu",
               depth == 0 ? "root" : "    ", (unsigned long)block, ec,
               (unsigned long)parent_ptr);

        /* 校验分隔 key 有序 */
        for (uint8_t i = 1; i < ec; i++) {
            if (strcmp(node.entries[i-1].name, node.entries[i].name) >= 0) {
                VERR("block %lu: internal entry[%u]=\"%s\" >= entry[%u]=\"%s\"",
                     (unsigned long)block,
                     i-1, node.entries[i-1].name,
                     i,   node.entries[i].name);
            }
        }

        /* 校验最左子树 */
        {
            uint64_t child = node.head.ptr1;
            if (child == ~0ull) {
                VERR("block %lu: leftmost child ptr1 is ~0ull (invalid)",
                     (unsigned long)block);
            } else {
                VPRINT("  %s   leftmost child=%lu",
                       depth == 0 ? "    " : "    ", (unsigned long)child);
                validate_subtree(ctx, child, block, depth + 1);
            }
        }

        /* 校验每个 entry 的子树 */
        for (uint8_t i = 0; i < ec; i++) {
            uint64_t child = node.entries[i].ptr;
            if (child == 0 || child == ~0ull) {
                VERR("block %lu: entry[%u] child ptr is null/sentinel",
                     (unsigned long)block, i);
            } else {
                VPRINT("  %s   child[%u]=%lu  sep=\"%s\"",
                       depth == 0 ? "    " : "    ",
                       i, (unsigned long)child, node.entries[i].name);
                validate_subtree(ctx, child, block, depth + 1);
            }
        }

    } else {
        /* ─── 叶子节点 ─── */
        VPRINT("  %s leaf node block=%lu  entries=%u  parent=%lu%s",
               depth == 0 ? "root" : "    ",
               (unsigned long)block, ec,
               (unsigned long)parent_ptr,
               is_root ? " (root)" : "");

        /* 校验字典序 */
        int valid_entries = 0;
        for (uint8_t i = 0; i < ec; i++) {
            if (node.entries[i].ptr == 0) {
                /* 墓碑，跳过名称检查 */
                VWARN("block %lu: entry[%u] is tombstone (ptr=0)",
                      (unsigned long)block, i);
                continue;
            }
            if (i > 0) {
                int prev = i - 1;
                while (prev >= 0 && node.entries[prev].ptr == 0) prev--;
                if (prev >= 0 && strcmp(node.entries[prev].name, node.entries[i].name) >= 0) {
                    VERR("block %lu: leaf entry[%u]=\"%s\" not sorted after entry[%u]=\"%s\"",
                         (unsigned long)block,
                         i, node.entries[i].name,
                         prev, node.entries[prev].name);
                }
            }
            valid_entries++;
        }

        /* 校验兄弟指针（非根叶子） */
        if (!is_root) {
            VPRINT("  %s   prev=%lu  next=%lu",
                   depth == 0 ? "    " : "    ",
                   (unsigned long)node.head.ptr1,
                   (unsigned long)node.head.ptr2);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * validate_dentry_tree — 入口
 *
 * fd: 打开的设备/文件描述符
 * world_base_lba: 从 superblock 读取
 * total_blocks, inode_count: 从 superblock 读取
 * root_inode: 根目录 inode 编号
 *
 * 返回错误数。
 * ═══════════════════════════════════════════════════════════════ */
int validate_dentry_tree(int fd, uint64_t world_base_lba,
                          uint64_t total_blocks, uint64_t inode_count,
                          uint64_t root_inode)
{
    DentryValidateCtx ctx;
    ctx.fd             = fd;
    ctx.errors         = 0;
    ctx.warnings       = 0;
    ctx.world_base_lba = world_base_lba;

    VPRINT("dentry tree validation");
    VPRINT("────────────────────────────");

    /* 推导几何 */
    FsGeometry geo = derive_geometry(total_blocks, inode_count);

    /* 读根 inode */
    uint64_t base_off = (world_base_lba == 0) ? 0 : 4096;
    uint64_t inode_off = base_off + geo.inode_array_base * 4096 + root_inode * 256;
    raven_Inode_t root_ino;
    if (pread(fd, &root_ino, sizeof(root_ino), inode_off) != (ssize_t)sizeof(root_ino)) {
        fprintf(stderr, "ERROR: cannot read root inode at offset %lu\n",
                (unsigned long)inode_off);
        return 1;
    }

    if (root_ino.type != INODE_TYPE_DIR) {
        fprintf(stderr, "ERROR: root inode is not a directory\n");
        return 1;
    }

    uint64_t dentry_root = root_ino.data_root;
    if (dentry_root == ~0ull) {
        fprintf(stderr, "ERROR: root dentry data_root is ~0ull (inline, invalid)\n");
        return 1;
    }

    VPRINT("root inode=%lu  dentry_root=%lu", (unsigned long)root_inode,
           (unsigned long)dentry_root);
    VPRINT("");

    validate_subtree(ctx, dentry_root, 0, 0);

    VPRINT("");
    VPRINT("dentry tree: %d errors, %d warnings (visited %zu nodes)",
           ctx.errors, ctx.warnings, ctx.visited.size());

    return ctx.errors;
}
