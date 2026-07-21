#include "../include/Userspace_God.hpp"
#include <cstdio>
#include <cstring>

/* ═══════════════════════════════════════════════════════════════
 * validate_dentry_tree — 从根目录递归校验 dentry B+tree
 *
 * 遍历期间所有 dentry 节点 pin 在 DRAM（map），不反复读盘。
 * ═══════════════════════════════════════════════════════════════ */

#define VPRINT(fmt, ...)  fprintf(stdout, fmt "\n", ##__VA_ARGS__)
#define VERR(fmt, ...)    do { \
    fprintf(stdout, "  ERROR: " fmt "\n", ##__VA_ARGS__); \
    g_err++; \
} while(0)
#define VWARN(fmt, ...)   do { \
    fprintf(stdout, "  WARN:  " fmt "\n", ##__VA_ARGS__); \
} while(0)

static int g_err = 0;

/* ─── pin map 取节点：在则返回，不在则读盘插入 ─── */
#define GET_PINNED(block_var, node_ptr, pin_map)                        \
    do {                                                                \
        auto _it = (pin_map).find(block_var);                           \
        if (_it != (pin_map).end()) {                                   \
            node_ptr = &_it->second;                                    \
        } else {                                                        \
            Dentry_Bptree_node_t _n;                                    \
            read_btnode(block_var, &_n, sizeof(_n));                    \
            auto _r = (pin_map).insert({block_var, _n});                \
            node_ptr = &_r.first->second;                               \
        }                                                               \
    } while(0)

void RavenFS::validate_subtree(uint64_t block, uint64_t expected_parent,
                                int depth, std::set<uint64_t> &visited,
                                std::map<uint64_t, Dentry_Bptree_node_t> &pin)
{
    if (depth > 256) {
        VERR("block %lu: max depth exceeded", (unsigned long)block);
        return;
    }
    if (visited.find(block) != visited.end()) {
        VERR("block %lu: cycle detected", (unsigned long)block);
        return;
    }
    visited.insert(block);

    const Dentry_Bptree_node_t *node = nullptr;
    GET_PINNED(block, node, pin);

    dentry_node_meta_t meta_packed = node->head.head;
    auto meta = meta_packed.fields;
    uint8_t ec = meta.entry_count;
    bool is_int  = meta.is_internal_node != 0;
    bool is_root = meta.is_root_node != 0;
    uint64_t parent_ptr = meta.parent_ptr;

    if (meta_packed.raw == 0) {
        VERR("block %lu: zero metadata", (unsigned long)block);
        return;
    }
    if (ec > 63) {
        VERR("block %lu: entry_count=%u > 63", (unsigned long)block, ec);
        return;
    }

    VPRINT("  %s block=%lu  ec=%u  %s  parent=%lu",
           depth == 0 ? "root" : "    ",
           (unsigned long)block, ec,
           is_int ? "internal" : "leaf",
           (unsigned long)parent_ptr);

    if (!is_root && parent_ptr != expected_parent) {
        VERR("block %lu: parent_ptr=%lu != expected %lu",
             (unsigned long)block, (unsigned long)parent_ptr,
             (unsigned long)expected_parent);
    }

    if (is_int) {
        if (node->head.ptr1 == ~0ull) {
            VERR("block %lu: ptr1 is ~0ull", (unsigned long)block);
        } else {
            validate_subtree(node->head.ptr1, block, depth + 1, visited, pin);
        }
        for (uint8_t i = 0; i < ec; i++) {
            if (i > 0 && strcmp(node->entries[i-1].name, node->entries[i].name) >= 0) {
                VERR("block %lu: internal key[%u]=\"%s\" >= key[%u]=\"%s\"",
                     (unsigned long)block, i-1, node->entries[i-1].name,
                     i, node->entries[i].name);
            }
            if (node->entries[i].ptr != ~0ull)
                validate_subtree(node->entries[i].ptr, block, depth + 1, visited, pin);
        }
    } else {
        int last_valid = -1;
        for (uint8_t i = 0; i < ec; i++) {
            if (node->entries[i].ptr == 0) {
                VWARN("block %lu: entry[%u] tombstone", (unsigned long)block, i);
                continue;
            }
            if (last_valid >= 0) {
                if (strcmp(node->entries[last_valid].name, node->entries[i].name) >= 0) {
                    VERR("block %lu: entry[%u]=\"%s\" >= entry[%u]=\"%s\"",
                         (unsigned long)block, last_valid, node->entries[last_valid].name,
                         i, node->entries[i].name);
                }
            }
            last_valid = i;

            /* 递归校验子目录的 dentry 树 */
            raven_Inode_t child_ino;
            inode_read(node->entries[i].ptr, &child_ino);
            if (child_ino.type == INODE_TYPE_DIR &&
                child_ino.data_root != block &&        /* 不是自引用 */
                child_ino.data_root != ~0ull)
            {
                VPRINT("  %s   -> subdir \"%s\" inode=%lu  dentry_root=%lu",
                       depth == 0 ? "    " : "    ",
                       node->entries[i].name,
                       (unsigned long)node->entries[i].ptr,
                       (unsigned long)child_ino.data_root);
                std::set<uint64_t> sub_visited;
                validate_subtree(child_ino.data_root, block, 0, sub_visited, pin);
                visited.insert(sub_visited.begin(), sub_visited.end());
            }
        }
        if (!is_root) {
            VPRINT("  %s   prev=%lu  next=%lu",
                   depth == 0 ? "    " : "    ",
                   (unsigned long)node->head.ptr1,
                   (unsigned long)node->head.ptr2);
        }
    }
}

int RavenFS::validate_dentry_tree() {
    g_err = 0;

    VPRINT("dentry tree validation (pinned DRAM)");
    VPRINT("────────────────────────────────────────");

    raven_Inode_t root_ino;
    inode_read(root_inum, &root_ino);

    if (root_ino.type != INODE_TYPE_DIR) {
        fprintf(stderr, "ERROR: root inode %lu is not a directory\n",
                (unsigned long)root_inum);
        return 1;
    }

    uint64_t dentry_root = root_ino.data_root;
    if (dentry_root == ~0ull) {
        fprintf(stderr, "ERROR: root dentry data_root is ~0ull\n");
        return 1;
    }

    VPRINT("root inode=%lu  dentry_root=%lu", (unsigned long)root_inum,
           (unsigned long)dentry_root);
    VPRINT("");

    std::set<uint64_t> visited;
    std::map<uint64_t, Dentry_Bptree_node_t> pin;

    validate_subtree(dentry_root, 0, 0, visited, pin);

    VPRINT("");
    VPRINT("dentry tree: %d errors  visited %zu nodes  pinned %zu blocks",
           g_err, visited.size(), pin.size());
    return g_err;
}
