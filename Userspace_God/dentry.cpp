#include "../include/ravenfs/ravenfs.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

/* 下溯路径节点类型（内部使用，不暴露到头文件） */
struct RavenFS::DentryPathEntry {
    uint64_t          block;
    Dentry_Bptree_node_t node;
};

static bool entry_is_tombstone(const Dentry_common_t *e) {
    return e->ptr == 0;
}

/* ─── 常量 ─── */
#define DENTRY_ENTRY_NAME_LEN  120
#define MAX_DENTRY_ENTRIES      63
#define DENTRY_LEAF_MIN         32
#define DENTRY_INTERNAL_MIN     32

/* 在节点中查找 name
 * 返回: ≥0 = entry 索引（找到，可能是墓碑）
 *       负值 = -insert_pos - 1（未找到，insert_pos 为插入位置）
 *
 * 注：墓碑也参与匹配——insert 靠此找到同名墓碑来覆写 */
int RavenFS::dentry_node_find(const Dentry_Bptree_node_t *node, const char *name) {
    uint8_t count = node->head.head.fields.entry_count;
    for (int i = 0; i < count; i++) {
        int cmp = strcmp(name, node->entries[i].name);
        if (cmp == 0) return i;
        if (cmp < 0)  return -i - 1;
    }
    return -(int)count - 1;
}

/* 在叶子节点中从 start 位置开始向右/向左移动 entry (dir=1 右移, dir=-1 左移) */
void RavenFS::dentry_node_shift_entries(Dentry_Bptree_node_t *node,
                                        int start, int dir)
{
    uint8_t count = node->head.head.fields.entry_count;
    if (dir > 0) {
        for (int i = count; i > start; i--)
            node->entries[i] = node->entries[i - 1];
    } else {
        for (int i = start; i < count - 1; i++)
            node->entries[i] = node->entries[i + 1];
    }
}

/* ─── 内部：叶子分裂 ───
 *
 * 将满叶 (63 entries) + 新 entry 共 64 条排序，平分两半。
 * 旧叶保留左 32 条，新叶取右 32 条。
 * 返回新叶块号、提升 separator name。
 * ─── */
static bool split_leaf(Dentry_Bptree_node_t *old_node,
                       const char *new_name, uint64_t new_ptr,
                       Dentry_Bptree_node_t *new_node,
                       char *separator, size_t sep_size)
{
    uint8_t old_count = old_node->head.head.fields.entry_count;

    struct TempEntry {
        char     name[DENTRY_ENTRY_NAME_LEN];
        uint64_t ptr;
    };
    TempEntry tmp[64];
    uint8_t tmp_count = 0;

    for (uint8_t i = 0; i < old_count; i++) {
        memcpy(tmp[tmp_count].name, old_node->entries[i].name, DENTRY_ENTRY_NAME_LEN);
        tmp[tmp_count].ptr = old_node->entries[i].ptr;
        tmp_count++;
    }
    memcpy(tmp[tmp_count].name, new_name, DENTRY_ENTRY_NAME_LEN);
    tmp[tmp_count].ptr = new_ptr;
    tmp_count++;

    std::sort(tmp, tmp + tmp_count, [](const TempEntry &a, const TempEntry &b) {
        return strcmp(a.name, b.name) < 0;
    });

    uint8_t split = tmp_count / 2;  // 32

    memset(old_node->entries, 0, sizeof(Dentry_common_t) * MAX_DENTRY_ENTRIES);
    old_node->head.head.fields.entry_count = split;
    for (int i = 0; i < split; i++) {
        old_node->entries[i].ptr = tmp[i].ptr;
        memcpy(old_node->entries[i].name, tmp[i].name, DENTRY_ENTRY_NAME_LEN);
    }

    memset(new_node->entries, 0, sizeof(Dentry_common_t) * MAX_DENTRY_ENTRIES);
    new_node->head.head.fields.entry_count = tmp_count - split;  // 32
    for (int i = split; i < tmp_count; i++) {
        int j = i - split;
        new_node->entries[j].ptr = tmp[i].ptr;
        memcpy(new_node->entries[j].name, tmp[i].name, DENTRY_ENTRY_NAME_LEN);
    }

    memcpy(separator, tmp[split].name, sep_size < DENTRY_ENTRY_NAME_LEN ? sep_size : DENTRY_ENTRY_NAME_LEN);
    separator[sep_size - 1] = '\0';

    return true;
}

/* ─── 内部：内部节点分裂 ───
 *
 * 内部节点满 (63 entries = 64 children)，插入新的 separator+ptr 后共 64 entries / 65 children。
 * 左半 32 entries 留原节点，右半 32 entries 去新节点，
 * 中间 entry (index 32) 提升到父节点。
 *
 * 注：不排序，因为 entries 本身已有序。在正确位置插入再从中点分裂即可。
 * ─── */
static bool split_internal(Dentry_Bptree_node_t *old_node,
                           const char *new_sep, uint64_t new_ptr,
                           Dentry_Bptree_node_t *new_node,
                           char *separator, size_t sep_size)
{
    uint8_t old_count = old_node->head.head.fields.entry_count;

    /* 找到新 separator 的插入位置（entries 已升序） */
    uint8_t ins = 0;
    while (ins < old_count && strcmp(old_node->entries[ins].name, new_sep) < 0)
        ins++;

    /* 插入新 separator：将 entries[ins..old_count-1] 右移 */
    for (uint8_t i = old_count; i > ins; i--)
        old_node->entries[i] = old_node->entries[i - 1];
    memcpy(old_node->entries[ins].name, new_sep, DENTRY_ENTRY_NAME_LEN);
    old_node->entries[ins].ptr = new_ptr;
    old_node->head.head.fields.entry_count = old_count + 1;

    /* 分裂：提升中间 entry (index 32)，左右各保留 32 条 */
    uint8_t split = 32;  // 左 32, 右 32, 提升中间

    memcpy(separator, old_node->entries[split].name,
           sep_size < DENTRY_ENTRY_NAME_LEN ? sep_size : DENTRY_ENTRY_NAME_LEN);
    separator[sep_size - 1] = '\0';

    /* 新节点 = 右 32 条，最左子树 = 提升 entry 的 child */
    memset(new_node->entries, 0, sizeof(Dentry_common_t) * MAX_DENTRY_ENTRIES);
    new_node->head.head.fields.entry_count = old_count - split;  // 32
    new_node->head.ptr1 = old_node->entries[split].ptr;
    for (uint8_t i = split + 1; i <= old_count; i++) {
        int j = i - split - 1;
        new_node->entries[j].ptr = old_node->entries[i].ptr;
        memcpy(new_node->entries[j].name, old_node->entries[i].name, DENTRY_ENTRY_NAME_LEN);
    }

    /* 旧节点保留左 32 条 */
    old_node->head.head.fields.entry_count = split;
    /* ptr1 不变 */

    return true;
}

/* ═══════════════════════════════════════════════════════════════
 * dentry_lookup — 在目录中按名字查找目标 inode
 *
 * 墓碑跳过：entry->ptr == 0 视为不存在
 * 根节点支持 . 和 .. 合成
 * ═══════════════════════════════════════════════════════════════ */
bool RavenFS::dentry_lookup(uint64_t dir_inode, const char *name, uint64_t *out_inum) {
    raven_Inode_t dinode;
    inode_read(dir_inode, &dinode);
    if (dinode.type != INODE_TYPE_DIR)
        return false;

    uint64_t root_fblock = dinode.data_root;
    if (root_fblock == ~0ull)
        return false;

    Dentry_Bptree_node_t node;
    uint64_t cur = root_fblock;

    for (;;) {
        read_btnode(cur, &node, sizeof(node));
        if (!node.head.head.fields.is_internal_node)
            break;

        uint8_t count = node.head.head.fields.entry_count;
        if (count == 0) return false;

        if (strcmp(name, node.entries[0].name) < 0) {
            cur = node.head.ptr1;
            continue;
        }

        int found = -1;
        for (int i = 0; i < count; i++) {
            if (strcmp(name, node.entries[i].name) >= 0)
                found = i;
        }
        cur = found < 0 ? node.head.ptr1 : node.entries[found].ptr;
    }

    if (node.head.head.fields.is_root_node) {
        if (strcmp(name, ".") == 0) {
            *out_inum = node.head.head.fields.parent_ptr;
            return true;
        }
        if (strcmp(name, "..") == 0) {
            *out_inum = node.head.ptr2;
            return true;
        }
    }

    int pos = dentry_node_find(&node, name);
    if (pos < 0) return false;
    if (entry_is_tombstone(&node.entries[pos])) return false;

    *out_inum = node.entries[pos].ptr;
    return true;
}

/* ═══════════════════════════════════════════════════════════════
 * dentry_insert — 插入 dentry，支持 B+tree 分裂
 *
 * 下溯过程中追踪完整路径。如果叶子满则分裂并提升 separator，
 * 父节点满则递归分裂，根满则创建新根。
 * ═══════════════════════════════════════════════════════════════ */
bool RavenFS::dentry_insert(uint64_t dir_inode, const char *name, uint64_t target_inum) {
    raven_Inode_t dinode;
    inode_read(dir_inode, &dinode);
    if (dinode.type != INODE_TYPE_DIR)
        return false;

    uint64_t root_fblock = dinode.data_root;
    if (root_fblock == ~0ull)
        return false;

    /* 路径栈 */
    struct {
        Dentry_Bptree_node_t node;
        uint64_t block;
    } path[64];
    int depth = 0;

    path[0].block = root_fblock;
    read_btnode(path[0].block, &path[0].node, sizeof(Dentry_Bptree_node_t));

    while (path[depth].node.head.head.fields.is_internal_node) {
        Dentry_Bptree_node_t *n = &path[depth].node;
        uint8_t count = n->head.head.fields.entry_count;
        if (count == 0) return false;

        uint64_t child;
        if (strcmp(name, n->entries[0].name) < 0) {
            child = n->head.ptr1;
        } else {
            int found = -1;
            for (int i = 0; i < count; i++) {
                if (strcmp(name, n->entries[i].name) >= 0)
                    found = i;
            }
            child = found < 0 ? n->head.ptr1 : n->entries[found].ptr;
        }

        depth++;
        path[depth].block = child;
        read_btnode(path[depth].block, &path[depth].node, sizeof(Dentry_Bptree_node_t));
    }

    Dentry_Bptree_node_t *leaf = &path[depth].node;

    /* 查重 */
        int dup_pos = dentry_node_find(leaf, name);
    if (dup_pos >= 0 && leaf->entries[dup_pos].ptr != 0) {
        fprintf(stderr, "RavenFS: '%s' already exists\n", name);
        return false;
    }
    /* 如果查到墓碑，可以覆写 */
    if (dup_pos >= 0) {
        leaf->entries[dup_pos].ptr = target_inum;
        memset(leaf->entries[dup_pos].name, 0, DENTRY_ENTRY_NAME_LEN);
        memcpy(leaf->entries[dup_pos].name, name, strlen(name) < DENTRY_ENTRY_NAME_LEN - 1 ? strlen(name) : DENTRY_ENTRY_NAME_LEN - 1);
        write_btnode(path[depth].block, leaf, sizeof(Dentry_Bptree_node_t));
        return true;
    }

    uint8_t count = leaf->head.head.fields.entry_count;

    /* ─── 未满或虽有墓碑但物理有槽：按排序位置插入 ─── */
    if (count < MAX_DENTRY_ENTRIES) {
        int insert_at = -dup_pos - 1;
        if (insert_at < count)
            dentry_node_shift_entries(leaf, insert_at, 1);
        leaf->entries[insert_at].ptr = target_inum;
        memset(leaf->entries[insert_at].name, 0, DENTRY_ENTRY_NAME_LEN);
        memcpy(leaf->entries[insert_at].name, name,
               strlen(name) < DENTRY_ENTRY_NAME_LEN - 1 ? strlen(name) : DENTRY_ENTRY_NAME_LEN - 1);
        leaf->head.head.fields.entry_count = count + 1;
        write_btnode(path[depth].block, leaf, sizeof(Dentry_Bptree_node_t));
        return true;
    }

    /* ─── 叶子满 63：分裂 ─── */
    Dentry_Bptree_node_t new_leaf;
    memset(&new_leaf, 0, sizeof(new_leaf));
    new_leaf.head.head.fields.is_internal_node = 0;
    new_leaf.head.head.fields.is_root_node     = 0;
    new_leaf.head.magic1 = DENTRY_MAGIC1;
    new_leaf.head.magic2 = DENTRY_MAGIC2;
    new_leaf.head.magic3 = DENTRY_MAGIC3;
    new_leaf.head.magic4 = DENTRY_MAGIC4;

    char separator[DENTRY_ENTRY_NAME_LEN] = {};
    split_leaf(leaf, name, target_inum, &new_leaf, separator, sizeof(separator));

    /* 分配新叶子块 */
    uint64_t new_block = bcache->hard_alloc(2);
    if (new_block == (uint64_t)-1) return false;

    /* 更新兄弟指针 */
    uint64_t old_next = leaf->head.ptr2;
    new_leaf.head.ptr1 = path[depth].block;  // prev = old leaf
    new_leaf.head.ptr2 = old_next;            // next = old leaf's next
    leaf->head.ptr2 = new_block;              // old leaf's next = new leaf

    /* 新叶子 parent_ptr = old leaf's parent */
    new_leaf.head.head.fields.parent_ptr = leaf->head.head.fields.parent_ptr;

    /* 写回 */
    write_btnode(path[depth].block, leaf, sizeof(Dentry_Bptree_node_t));
    write_btnode(new_block, &new_leaf, sizeof(Dentry_Bptree_node_t));

    /* 更新 old_next 的 prev 指针（如果存在） */
    if (old_next != 0 && old_next != ~0ull) {
        Dentry_Bptree_node_t next_node;
        read_btnode(old_next, &next_node, sizeof(Dentry_Bptree_node_t));
        next_node.head.ptr1 = new_block;
        write_btnode(old_next, &next_node, sizeof(Dentry_Bptree_node_t));
    }

    /* ─── 向上提升 separator ─── */
    uint64_t promoted_block = new_block;

    auto create_new_root = [&](uint64_t old_root_fblock) -> bool {
        uint64_t new_root_block = bcache->hard_alloc(2);
        if (new_root_block == (uint64_t)-1) return false;

        Dentry_Bptree_node_t old_root_guard;
        read_btnode(old_root_fblock, &old_root_guard, sizeof(Dentry_Bptree_node_t));
        uint64_t dotdot_inode = old_root_guard.head.ptr2;

        Dentry_Bptree_node_t new_root;
        memset(&new_root, 0, sizeof(new_root));
        new_root.head.head.fields.entry_count      = 1;
        new_root.head.head.fields.is_internal_node = 1;
        new_root.head.head.fields.is_root_node     = 1;
        new_root.head.head.fields.parent_ptr       = dir_inode;
        new_root.head.ptr1 = old_root_fblock;
        new_root.head.ptr2 = dotdot_inode;
        memcpy(new_root.entries[0].name, separator, DENTRY_ENTRY_NAME_LEN);
        new_root.entries[0].ptr = promoted_block;

        /* 断言：新根中 entries[0].name > ptr1 子树最大 key */
        {
            Dentry_Bptree_node_t probe;
            uint64_t pb = old_root_fblock;
            while (pb != ~0ull) {
                read_btnode(pb, &probe, sizeof(Dentry_Bptree_node_t));
                if (!probe.head.head.fields.is_internal_node) break;
                uint8_t pec = probe.head.head.fields.entry_count;
                if (pec > 0) pb = probe.entries[pec - 1].ptr;
                else pb = probe.head.ptr1;
            }
            if (pb != ~0ull) {
                uint8_t pec = probe.head.head.fields.entry_count;
                if (pec > 0 && strcmp(probe.entries[pec - 1].name, separator) >= 0) {
                    fprintf(stderr, "RavenFS: INVARIANT FAIL create_root: ptr1 last >= sep\n");
                }
            }
        }

        new_root.head.magic1 = DENTRY_MAGIC1;
        new_root.head.magic2 = DENTRY_MAGIC2;
        new_root.head.magic3 = DENTRY_MAGIC3;
        new_root.head.magic4 = DENTRY_MAGIC4;
        write_btnode(new_root_block, &new_root, sizeof(Dentry_Bptree_node_t));

        Dentry_Bptree_node_t old_root;
        read_btnode(old_root_fblock, &old_root, sizeof(Dentry_Bptree_node_t));
        old_root.head.head.fields.is_root_node = 0;
        old_root.head.head.fields.parent_ptr   = new_root_block;
        write_btnode(old_root_fblock, &old_root, sizeof(Dentry_Bptree_node_t));

        {
            Dentry_Bptree_node_t prom_node;
            read_btnode(promoted_block, &prom_node, sizeof(prom_node));
            prom_node.head.head.fields.parent_ptr = new_root_block;
            write_btnode(promoted_block, &prom_node, sizeof(prom_node));
        }

        dinode.data_root = new_root_block;
        inode_write(dir_inode, &dinode);
        return true;
    };

    while (true) {
        depth--;
        if (depth < 0) {
            if (!create_new_root(root_fblock)) return false;
            break;
        }

        Dentry_Bptree_node_t *parent = &path[depth].node;
        if (parent->head.head.fields.entry_count < MAX_DENTRY_ENTRIES) {
            /* 父节点未满，插入 separator */
            uint8_t pc = parent->head.head.fields.entry_count;
            int ins = 0;
            while (ins < pc && strcmp(parent->entries[ins].name, separator) < 0)
                ins++;

            dentry_node_shift_entries(parent, ins, 1);
            memcpy(parent->entries[ins].name, separator, DENTRY_ENTRY_NAME_LEN);
            parent->entries[ins].ptr = promoted_block;
            parent->head.head.fields.entry_count = pc + 1;

            /* 断言：entries[0].name 必须 > ptr1 子树的最大 key（递归到叶子） */
            if (parent->head.head.fields.entry_count > 0) {
                Dentry_Bptree_node_t probe;
                uint64_t pb = parent->head.ptr1;
                while (pb != ~0ull) {
                    read_btnode(pb, &probe, sizeof(Dentry_Bptree_node_t));
                    if (!probe.head.head.fields.is_internal_node) break;
                    /* internal: 从最右 entry 走到底 */
                    uint8_t pec = probe.head.head.fields.entry_count;
                    if (pec > 0)
                        pb = probe.entries[pec - 1].ptr;
                    else
                        pb = probe.head.ptr1;
                }
                if (pb != ~0ull) {
                    uint8_t pec = probe.head.head.fields.entry_count;
                    if (pec > 0 && strcmp(probe.entries[pec - 1].name,
                                          parent->entries[0].name) >= 0) {
                        fprintf(stderr, "RavenFS: INVARIANT FAIL at insert: block %lu: "
                                "ptr1 last=\"%s\" >= entries[0].name=\"%s\"\n",
                                (unsigned long)path[depth].block,
                                probe.entries[pec - 1].name, parent->entries[0].name);
                    }
                }
            }

            write_btnode(path[depth].block, parent, sizeof(Dentry_Bptree_node_t));

            /* 更新 promoted_block 的 parent_ptr（可能是叶子或内部节点） */
            {
                Dentry_Bptree_node_t prom_node;
                read_btnode(promoted_block, &prom_node, sizeof(prom_node));
                prom_node.head.head.fields.parent_ptr = path[depth].block;
                write_btnode(promoted_block, &prom_node, sizeof(prom_node));
            }

            break;
        } else {
            /* 父节点也满，分裂父节点 */
            Dentry_Bptree_node_t new_internal;
            memset(&new_internal, 0, sizeof(new_internal));
            new_internal.head.head.fields.is_internal_node = 1;
            new_internal.head.head.fields.is_root_node     = 0;
            new_internal.head.magic1 = DENTRY_MAGIC1;
            new_internal.head.magic2 = DENTRY_MAGIC2;
            new_internal.head.magic3 = DENTRY_MAGIC3;
            new_internal.head.magic4 = DENTRY_MAGIC4;

            char next_sep[DENTRY_ENTRY_NAME_LEN] = {};
            split_internal(parent, separator, promoted_block,
                          &new_internal, next_sep, sizeof(next_sep));

            uint64_t new_int_block = bcache->hard_alloc(2);
            if (new_int_block == (uint64_t)-1) return false;

            new_internal.head.head.fields.parent_ptr =
                parent->head.head.fields.parent_ptr;

            write_btnode(path[depth].block, parent, sizeof(Dentry_Bptree_node_t));
            write_btnode(new_int_block, &new_internal, sizeof(Dentry_Bptree_node_t));

            /* 更新子节点 parent_ptr（新内部节点的所有子树） */
            {
                Dentry_Bptree_node_t child_node;
                uint64_t b = new_internal.head.ptr1;
                read_btnode(b, &child_node, sizeof(Dentry_Bptree_node_t));
                child_node.head.head.fields.parent_ptr = new_int_block;
                write_btnode(b, &child_node, sizeof(Dentry_Bptree_node_t));
                for (uint8_t i = 0; i < new_internal.head.head.fields.entry_count; i++) {
                    b = new_internal.entries[i].ptr;
                    read_btnode(b, &child_node, sizeof(Dentry_Bptree_node_t));
                    child_node.head.head.fields.parent_ptr = new_int_block;
                    write_btnode(b, &child_node, sizeof(Dentry_Bptree_node_t));
                }
            }

            memcpy(separator, next_sep, sizeof(separator));
            promoted_block = new_int_block;
            /* 继续向上回溯 */
        }
    }

    return true;
}

static int dentry_find_child_index(const Dentry_Bptree_node_t *parent,
                                    uint64_t child_block) {
    if (parent->head.ptr1 == child_block) return -1;
    for (int i = 0; i < parent->head.head.fields.entry_count; i++) {
        if (parent->entries[i].ptr == child_block) return i;
    }
    return -2;
}

void RavenFS::dentry_internal_remove_separator(
    DentryPathEntry *path,
    int depth, int separator_idx)
{
    Dentry_Bptree_node_t *node = &path[depth].node;
    int count = node->head.head.fields.entry_count;
    if (separator_idx < 0 || separator_idx >= count) return;

    for (int i = separator_idx; i < count - 1; i++)
        node->entries[i] = node->entries[i + 1];
    memset(&node->entries[count - 1], 0, sizeof(Dentry_common_t));
    node->head.head.fields.entry_count = count - 1;
    write_btnode(path[depth].block, node, sizeof(Dentry_Bptree_node_t));

    if (depth == 0) {
        if (node->head.head.fields.entry_count > 0) return;
        uint64_t child_block = node->head.ptr1;
        Dentry_Bptree_node_t child;
        read_btnode(child_block, &child, sizeof(Dentry_Bptree_node_t));
        child.head.head.fields.is_root_node = 1;
        child.head.head.fields.parent_ptr    = node->head.head.fields.parent_ptr;
        if (!child.head.head.fields.is_internal_node)
            child.head.ptr2 = node->head.ptr2;
        write_btnode(child_block, &child, sizeof(Dentry_Bptree_node_t));
        raven_Inode_t dinode;
        inode_read(node->head.head.fields.parent_ptr, &dinode);
        dinode.data_root = child_block;
        inode_write(node->head.head.fields.parent_ptr, &dinode);
        if (bcache) { bcache->free(path[0].block, 1); bcache->free(path[0].block + 1, 1); }
        return;
    }

    if (node->head.head.fields.entry_count >= DENTRY_INTERNAL_MIN)
        return;

    Dentry_Bptree_node_t parent;
    uint64_t parent_block = path[depth - 1].block;
    read_btnode(parent_block, &parent, sizeof(Dentry_Bptree_node_t));

    int my_idx = dentry_find_child_index(&parent, path[depth].block);
    if (my_idx < -1) return;

    bool has_left = (my_idx >= 0);
    bool has_right = (my_idx < parent.head.head.fields.entry_count - 1);
    if (my_idx == -1 && parent.head.head.fields.entry_count > 0)
        has_right = true;
    if (my_idx + 1 >= parent.head.head.fields.entry_count)
        has_right = false;
    if (!has_left && !has_right) return;

    int left_count = 0, right_count = 0;
    uint64_t left_block = 0, right_block = 0;
    Dentry_Bptree_node_t left, right;

    if (has_left) {
        left_block = (my_idx == 0) ? parent.head.ptr1 : parent.entries[my_idx - 1].ptr;
        read_btnode(left_block, &left, sizeof(Dentry_Bptree_node_t));
        left_count = left.head.head.fields.entry_count;
    }
    if (has_right) {
        right_block = (my_idx == -1) ? parent.entries[0].ptr : parent.entries[my_idx + 1].ptr;
        read_btnode(right_block, &right, sizeof(Dentry_Bptree_node_t));
        right_count = right.head.head.fields.entry_count;
    }

    bool use_left;
    if (!has_left)           use_left = false;
    else if (!has_right)     use_left = true;
    else if (left_count > right_count) use_left = true;
    else                     use_left = false;

    int my_count = node->head.head.fields.entry_count;

    auto merge_internal = [&](Dentry_Bptree_node_t *survive, uint64_t survive_block, int survive_count,
                               Dentry_Bptree_node_t *absorbed, uint64_t absorbed_block, int absorbed_count,
                               int sep_to_remove)
    {
        char sep_name[DENTRY_ENTRY_NAME_LEN];
        {
            Dentry_Bptree_node_t first_child;
            read_btnode(absorbed->head.ptr1, &first_child, sizeof(Dentry_Bptree_node_t));
            memcpy(sep_name, first_child.entries[0].name, DENTRY_ENTRY_NAME_LEN);
            first_child.head.head.fields.parent_ptr = survive_block;
            write_btnode(absorbed->head.ptr1, &first_child, sizeof(Dentry_Bptree_node_t));
        }
        for (int i = 0; i < absorbed_count; i++) {
            Dentry_Bptree_node_t child;
            uint64_t b = absorbed->entries[i].ptr;
            read_btnode(b, &child, sizeof(Dentry_Bptree_node_t));
            child.head.head.fields.parent_ptr = survive_block;
            write_btnode(b, &child, sizeof(Dentry_Bptree_node_t));
        }

        int ins = 0;
        while (ins < survive_count && strcmp(survive->entries[ins].name, sep_name) < 0)
            ins++;
        for (int i = survive_count - 1; i >= ins; i--)
            survive->entries[i + 1] = survive->entries[i];
        memset(&survive->entries[ins], 0, sizeof(Dentry_common_t));
        memcpy(survive->entries[ins].name, sep_name, DENTRY_ENTRY_NAME_LEN);
        survive->entries[ins].ptr = absorbed->head.ptr1;
        for (int i = 0; i < absorbed_count; i++)
            survive->entries[survive_count + 1 + i] = absorbed->entries[i];

        if (ins == 0) {
            Dentry_Bptree_node_t ptr1_child;
            read_btnode(survive->head.ptr1, &ptr1_child, sizeof(Dentry_Bptree_node_t));
            if (strcmp(ptr1_child.entries[0].name, sep_name) > 0) {
                uint64_t tmp_ptr = survive->head.ptr1;
                survive->head.ptr1 = survive->entries[0].ptr;
                survive->entries[0].ptr = tmp_ptr;
            }
        }

        survive->head.head.fields.entry_count = survive_count + absorbed_count + 1;
        uint64_t absorbed_next = absorbed->head.ptr2;
        survive->head.ptr2 = absorbed_next;
        write_btnode(survive_block, survive, sizeof(Dentry_Bptree_node_t));
        if (absorbed_next != 0 && absorbed_next != ~0ULL) {
            Dentry_Bptree_node_t next;
            read_btnode(absorbed_next, &next, sizeof(Dentry_Bptree_node_t));
            next.head.ptr1 = survive_block;
            write_btnode(absorbed_next, &next, sizeof(Dentry_Bptree_node_t));
        }
        if (bcache) { bcache->free(absorbed_block, 1); bcache->free(absorbed_block + 1, 1); }
        dentry_internal_remove_separator(path, depth - 1, sep_to_remove);
    };

    if (use_left) {
        int total = my_count + left_count;
        if (total >= 2 * DENTRY_INTERNAL_MIN || total >= MAX_DENTRY_ENTRIES) {
            int target = total / 2;
            int move = left_count - target;
            for (int i = my_count + move - 1; i >= move; i--)
                node->entries[i] = node->entries[i - move];
            for (int i = 0; i < move; i++) {
                int src = left_count - move + i;
                node->entries[i] = left.entries[src];
                memset(&left.entries[src], 0, sizeof(Dentry_common_t));
            }
            for (int i = 0; i < move; i++) {
                Dentry_Bptree_node_t child;
                uint64_t b = node->entries[i].ptr;
                read_btnode(b, &child, sizeof(Dentry_Bptree_node_t));
                child.head.head.fields.parent_ptr = path[depth].block;
                write_btnode(b, &child, sizeof(Dentry_Bptree_node_t));
            }
            left.head.head.fields.entry_count = target;
            node->head.head.fields.entry_count = my_count + move;

            int sep_idx = (my_idx == 0) ? 0 : my_idx;
            memset(parent.entries[sep_idx].name, 0, DENTRY_ENTRY_NAME_LEN);
            memcpy(parent.entries[sep_idx].name, node->entries[0].name, DENTRY_ENTRY_NAME_LEN - 1);

            write_btnode(left_block, &left, sizeof(Dentry_Bptree_node_t));
            write_btnode(path[depth].block, node, sizeof(Dentry_Bptree_node_t));
            write_btnode(parent_block, &parent, sizeof(Dentry_Bptree_node_t));
        } else {
            int sep_to_remove = (my_idx == 0) ? 0 : my_idx;
            merge_internal(&left, left_block, left_count, node, path[depth].block, my_count, sep_to_remove);
        }
    } else {
        int total = my_count + right_count;
        if (total >= 2 * DENTRY_INTERNAL_MIN || total >= MAX_DENTRY_ENTRIES) {
            int target = total / 2;
            int move = right_count - target;
            for (int i = 0; i < move; i++)
                node->entries[my_count + i] = right.entries[i];
            for (int i = 0; i < move; i++) {
                Dentry_Bptree_node_t child;
                uint64_t b = node->entries[my_count + i].ptr;
                read_btnode(b, &child, sizeof(Dentry_Bptree_node_t));
                child.head.head.fields.parent_ptr = path[depth].block;
                write_btnode(b, &child, sizeof(Dentry_Bptree_node_t));
            }
            for (int i = move; i < right_count; i++)
                right.entries[i - move] = right.entries[i];
            for (int i = right_count - move; i < right_count; i++)
                memset(&right.entries[i], 0, sizeof(Dentry_common_t));
            right.head.head.fields.entry_count = target;
            node->head.head.fields.entry_count = my_count + move;

            int sep_idx = (my_idx == -1) ? 0 : my_idx + 1;
            memset(parent.entries[sep_idx].name, 0, DENTRY_ENTRY_NAME_LEN);
            memcpy(parent.entries[sep_idx].name, right.entries[0].name, DENTRY_ENTRY_NAME_LEN - 1);

            write_btnode(right_block, &right, sizeof(Dentry_Bptree_node_t));
            write_btnode(path[depth].block, node, sizeof(Dentry_Bptree_node_t));
            write_btnode(parent_block, &parent, sizeof(Dentry_Bptree_node_t));
        } else {
            int sep_to_remove = (my_idx == -1) ? 0 : my_idx + 1;
            merge_internal(node, path[depth].block, my_count, &right, right_block, right_count, sep_to_remove);
        }
    }
}

void RavenFS::dentry_rebalance_leaf(
    DentryPathEntry *path,
    int depth)
{
    Dentry_Bptree_node_t *leaf = &path[depth].node;
    uint64_t leaf_block = path[depth].block;
    Dentry_Bptree_node_t parent;
    read_btnode(path[depth - 1].block, &parent, sizeof(Dentry_Bptree_node_t));

    int my_idx = dentry_find_child_index(&parent, leaf_block);
    if (my_idx < -1) return;

    bool has_left = (my_idx >= 0);
    bool has_right = (my_idx < parent.head.head.fields.entry_count - 1);
    if (my_idx == -1 && parent.head.head.fields.entry_count > 0)
        has_right = true;
    if (my_idx + 1 >= parent.head.head.fields.entry_count)
        has_right = false;
    if (!has_left && !has_right) return;

    int left_live = 0, right_live = 0;
    uint64_t left_block = 0, right_block = 0;
    Dentry_Bptree_node_t left, right;

    if (has_left) {
        left_block = (my_idx == 0) ? parent.head.ptr1 : parent.entries[my_idx - 1].ptr;
        read_btnode(left_block, &left, sizeof(Dentry_Bptree_node_t));
        left_live = left.head.head.fields.entry_count;
    }
    if (has_right) {
        right_block = (my_idx == -1) ? parent.entries[0].ptr : parent.entries[my_idx + 1].ptr;
        read_btnode(right_block, &right, sizeof(Dentry_Bptree_node_t));
        right_live = right.head.head.fields.entry_count;
    }

    int cur_live = leaf->head.head.fields.entry_count;

    bool take_left;
    if (!has_left)            take_left = false;
    else if (!has_right)      take_left = true;
    else if (left_live > right_live) take_left = true;
    else                      take_left = false;

    auto merge_leaf = [&](Dentry_Bptree_node_t *survive, uint64_t survive_block, int survive_count,
                           Dentry_Bptree_node_t *absorbed, uint64_t absorbed_block, int absorbed_count,
                           int sep_to_move)
    {
        for (int i = 0; i < absorbed_count; i++)
            survive->entries[survive_count + i] = absorbed->entries[i];
        survive->head.head.fields.entry_count = survive_count + absorbed_count;
        uint64_t absorbed_next = absorbed->head.ptr2;
        survive->head.ptr2 = absorbed_next;
        write_btnode(survive_block, survive, sizeof(Dentry_Bptree_node_t));
        if (absorbed_next != 0 && absorbed_next != ~0ULL) {
            Dentry_Bptree_node_t next;
            read_btnode(absorbed_next, &next, sizeof(Dentry_Bptree_node_t));
            next.head.ptr1 = survive_block;
            write_btnode(absorbed_next, &next, sizeof(Dentry_Bptree_node_t));
        }
        if (bcache) { bcache->free(absorbed_block, 1); bcache->free(absorbed_block + 1, 1); }
        dentry_internal_remove_separator(path, depth - 1, sep_to_move);
    };

    if (take_left) {
        int total = cur_live + left_live;
        if (total >= 2 * DENTRY_LEAF_MIN) {
            int target = total / 2;
            int move = left_live - target;
            for (int i = cur_live + move - 1; i >= move; i--)
                leaf->entries[i] = leaf->entries[i - move];
            for (int i = 0; i < move; i++) {
                int src = left_live - move + i;
                leaf->entries[i] = left.entries[src];
                memset(&left.entries[src], 0, sizeof(Dentry_common_t));
            }
            left.head.head.fields.entry_count = target;
            leaf->head.head.fields.entry_count = total - target;

            int sep_idx = (my_idx == 0) ? 0 : my_idx;
            memset(parent.entries[sep_idx].name, 0, DENTRY_ENTRY_NAME_LEN);
            memcpy(parent.entries[sep_idx].name, leaf->entries[0].name, DENTRY_ENTRY_NAME_LEN - 1);

            write_btnode(left_block, &left, sizeof(Dentry_Bptree_node_t));
            write_btnode(leaf_block, leaf, sizeof(Dentry_Bptree_node_t));
            write_btnode(path[depth - 1].block, &parent, sizeof(Dentry_Bptree_node_t));
        } else {
            int sep_to_move = (my_idx == 0) ? 0 : my_idx;
            merge_leaf(&left, left_block, left_live, leaf, leaf_block, cur_live, sep_to_move);
        }
    } else {
        int total = cur_live + right_live;
        if (total >= 2 * DENTRY_LEAF_MIN) {
            int target = total / 2;
            int move = right_live - target;
            for (int i = 0; i < move; i++)
                leaf->entries[cur_live + i] = right.entries[i];
            for (int i = move; i < right_live; i++)
                right.entries[i - move] = right.entries[i];
            for (int i = right_live - move; i < right_live; i++)
                memset(&right.entries[i], 0, sizeof(Dentry_common_t));
            right.head.head.fields.entry_count = target;
            leaf->head.head.fields.entry_count = total - target;

            int sep_idx = (my_idx == -1) ? 0 : my_idx + 1;
            memset(parent.entries[sep_idx].name, 0, DENTRY_ENTRY_NAME_LEN);
            memcpy(parent.entries[sep_idx].name, right.entries[0].name, DENTRY_ENTRY_NAME_LEN - 1);

            write_btnode(right_block, &right, sizeof(Dentry_Bptree_node_t));
            write_btnode(leaf_block, leaf, sizeof(Dentry_Bptree_node_t));
            write_btnode(path[depth - 1].block, &parent, sizeof(Dentry_Bptree_node_t));
        } else {
            int sep_to_move = (my_idx == -1) ? 0 : my_idx + 1;
            merge_leaf(leaf, leaf_block, cur_live, &right, right_block, right_live, sep_to_move);
        }
    }
}

bool RavenFS::dentry_remove(uint64_t dir_inode, const char *name) {
    raven_Inode_t dinode;
    inode_read(dir_inode, &dinode);
    if (dinode.type != INODE_TYPE_DIR)
        return false;

    uint64_t root_fblock = dinode.data_root;
    if (root_fblock == ~0ull)
        return false;

    DentryPathEntry path[64];
    int depth = 0;

    path[0].block = root_fblock;
    read_btnode(path[0].block, &path[0].node, sizeof(Dentry_Bptree_node_t));

    while (path[depth].node.head.head.fields.is_internal_node) {
        Dentry_Bptree_node_t *n = &path[depth].node;
        uint8_t count = n->head.head.fields.entry_count;
        if (count == 0) return false;

        uint64_t child;
        if (strcmp(name, n->entries[0].name) < 0) {
            child = n->head.ptr1;
        } else {
            int found = -1;
            for (int i = 0; i < count; i++) {
                if (strcmp(name, n->entries[i].name) >= 0)
                    found = i;
            }
            child = found < 0 ? n->head.ptr1 : n->entries[found].ptr;
        }

        depth++;
        path[depth].block = child;
        read_btnode(path[depth].block, &path[depth].node, sizeof(Dentry_Bptree_node_t));
    }

    Dentry_Bptree_node_t *leaf = &path[depth].node;
    uint64_t leaf_block = path[depth].block;

    int pos = dentry_node_find(leaf, name);
    if (pos < 0) {
        fprintf(stderr, "RavenFS: '%s' not found\n", name);
        return false;
    }

    uint8_t old_count = leaf->head.head.fields.entry_count;
    for (int i = pos; i < old_count - 1; i++)
        leaf->entries[i] = leaf->entries[i + 1];
    memset(&leaf->entries[old_count - 1], 0, sizeof(Dentry_common_t));
    leaf->head.head.fields.entry_count = old_count - 1;
    int live = old_count - 1;
    write_btnode(leaf_block, leaf, sizeof(Dentry_Bptree_node_t));

    if (depth == 0) return true;

    if (live < DENTRY_LEAF_MIN) {
        dentry_rebalance_leaf(path, depth);
    } else if (pos == 0 && live > 0) {
        /* 第一项被删除，parent 中指向本 leaf 的 separator 需要更新 */
        Dentry_Bptree_node_t parent;
        read_btnode(path[depth - 1].block, &parent, sizeof(Dentry_Bptree_node_t));
        int my_idx = dentry_find_child_index(&parent, leaf_block);
        if (my_idx >= 0) {
            memset(parent.entries[my_idx].name, 0, DENTRY_ENTRY_NAME_LEN);
            memcpy(parent.entries[my_idx].name, leaf->entries[0].name, DENTRY_ENTRY_NAME_LEN - 1);
            write_btnode(path[depth - 1].block, &parent, sizeof(Dentry_Bptree_node_t));
        }
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════
 * dentry_enumerate — 枚举目录所有条目
 *
 * 递归到达最左叶子，然后沿 ptr2 向右遍历同级链表。
 * 支持多级内部节点：反复沿 ptr1 下溯直到叶子。
 * 跳过墓碑 (ptr == 0) 条目。
 * ═══════════════════════════════════════════════════════════════ */
int RavenFS::dentry_enumerate(uint64_t dir_inode,
                              DirEntryInfo *entries, int max_entries)
{
    raven_Inode_t dinode;
    inode_read(dir_inode, &dinode);
    if (dinode.type != INODE_TYPE_DIR)
        return -1;

    uint64_t root_fblock = dinode.data_root;
    if (root_fblock == ~0ull)
        return 0;

    Dentry_Bptree_node_t node;
    read_btnode(root_fblock, &node, sizeof(node));

    int n = 0;

    /* 根节点合成 . 和 .. */
    if (node.head.head.fields.is_root_node) {
        if (n < max_entries) {
            entries[n].inode = node.head.head.fields.parent_ptr;
            strcpy(entries[n].name, ".");
            n++;
        }
        if (n < max_entries) {
            entries[n].inode = node.head.ptr2;
            strcpy(entries[n].name, "..");
            n++;
        }
    }

    /* 递归找到最左叶子块号 */
    uint64_t leftmost = root_fblock;
    {
        Dentry_Bptree_node_t tmp;
        read_btnode(leftmost, &tmp, sizeof(Dentry_Bptree_node_t));
        while (tmp.head.head.fields.is_internal_node) {
            leftmost = tmp.head.ptr1;
            read_btnode(leftmost, &tmp, sizeof(Dentry_Bptree_node_t));
        }
    }

    /* 从最左叶子沿 ptr2 遍历 */
    Dentry_Bptree_node_t leaf;
    uint64_t cur = leftmost;
    while (n < max_entries) {
        read_btnode(cur, &leaf, sizeof(Dentry_Bptree_node_t));
        uint8_t count = leaf.head.head.fields.entry_count;
        for (int i = 0; i < count && n < max_entries; i++) {
            if (entry_is_tombstone(&leaf.entries[i])) continue;
            entries[n].inode = leaf.entries[i].ptr;
            memcpy(entries[n].name, leaf.entries[i].name, DENTRY_ENTRY_NAME_LEN);
            n++;
        }
        uint64_t next = leaf.head.ptr2;
        /* 根叶子（state 2）：ptr2 是 `..` 的 Inode 指针，非 sibling 块指针 */
        if (leaf.head.head.fields.is_root_node || next == 0 || next == ~0ull) break;
        cur = next;
    }

    return n;
}
