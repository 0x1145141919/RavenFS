#include "../include/ravenfs/ravenfs.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>

int RavenFS::mkdir(const char *path) {
    PathCursor cur(*this, path);
    if (!cur.ok())
        return -ENOENT;
    if (!cur.walk_parent())
        return cur.err();

    uint64_t parent_inode = cur.inode();
    const char *leaf_name = cur.leaf();

    LogicalInode *parent_li = logical_inode_get(parent_inode);
    {
        std::lock_guard<std::mutex> lock(parent_li->mtx);

        if (dentry_cache_lookup(parent_inode, leaf_name) != ~0ULL) {
            logical_inode_put(parent_li);
            return -EEXIST;
        }

        uint64_t new_inum = ibcache ? ibcache->alloc_inode() : (uint64_t)-1;
        if (new_inum == (uint64_t)-1) {
            logical_inode_put(parent_li);
            return -ENOSPC;
        }

        uint64_t dentry_block = bcache ? bcache->hard_alloc(2) : (uint64_t)-1;
        if (dentry_block == (uint64_t)-1) {
            if (ibcache) ibcache->free_inode(new_inum);
            logical_inode_put(parent_li);
            return -ENOSPC;
        }

        Dentry_Bptree_node_t *dnode = new Dentry_Bptree_node_t;
        memset(dnode, 0, sizeof(*dnode));
        dnode->head.head.fields.entry_count        = 0;
        dnode->head.head.fields.is_internal_node   = 0;
        dnode->head.head.fields.is_root_node       = 1;
        dnode->head.head.fields.parent_ptr         = new_inum;
        dnode->head.ptr1 = 0;
        dnode->head.ptr2 = parent_inode;
        dnode->head.magic1 = DENTRY_MAGIC1;
        dnode->head.magic2 = DENTRY_MAGIC2;
        dnode->head.magic3 = DENTRY_MAGIC3;
        dnode->head.magic4 = DENTRY_MAGIC4;

        write_btnode(dentry_block, dnode, sizeof(*dnode));
        delete dnode;

        {
            InodeSession s(this, new_inum);
            if (s.ok()) {
                memset(s.inode, 0, sizeof(raven_Inode_t));
                s.inode->type                    = INODE_TYPE_DIR;
                s.inode->is_data_root_direct_bit = 0;
                s.inode->uid                     = 0;
                s.inode->gid                     = 0;
                s.inode->permission_bits         = 0755;
                s.inode->size                    = 0;
                s.inode->data_root               = dentry_block;
                s.inode->link_count              = 2;
                s.dirty();
            } else {
                raven_Inode_t inode;
                memset(&inode, 0, sizeof(inode));
                inode.type                    = INODE_TYPE_DIR;
                inode.is_data_root_direct_bit = 0;
                inode.uid                     = 0;
                inode.gid                     = 0;
                inode.permission_bits         = 0755;
                inode.size                    = 0;
                inode.data_root               = dentry_block;
                inode.link_count              = 2;
                inode_write(new_inum, &inode);
            }
        }

        if (!dentry_cache_insert(parent_inode, leaf_name, new_inum)) {
            if (ibcache) ibcache->free_inode(new_inum);
            if (bcache) { bcache->free(dentry_block, 1); bcache->free(dentry_block + 1, 1); }
            {
                InodeSession s(this, new_inum);
                if (s.ok()) {
                    memset(s.inode, 0, sizeof(raven_Inode_t));
                    s.dirty();
                } else {
                    raven_Inode_t zero_inode;
                    memset(&zero_inode, 0, sizeof(zero_inode));
                    inode_write(new_inum, &zero_inode);
                }
            }
            logical_inode_put(parent_li);
            return -ENOSPC;
        }
    }
    logical_inode_put(parent_li);
    return 0;
}

int RavenFS::rmdir(const char *path) {
    PathCursor cur(*this, path);
    if (!cur.ok())
        return -ENOENT;
    if (!cur.walk_parent())
        return cur.err();

    uint64_t parent_inode = cur.inode();
    const char *leaf_name = cur.leaf();

    LogicalInode *parent_li = logical_inode_get(parent_inode);
    {
        std::lock_guard<std::mutex> parent_lock(parent_li->mtx);

        uint64_t target_inum = dentry_cache_lookup(parent_inode, leaf_name);
        if (target_inum == ~0ULL) {
            logical_inode_put(parent_li);
            return -ENOENT;
        }

        LogicalInode *target_li = logical_inode_get(target_inum);
        {
            std::lock_guard<std::mutex> target_lock(target_li->mtx);

            InodeSession s(this, target_inum);
            if (!s.ok()) {
                logical_inode_put(target_li);
                logical_inode_put(parent_li);
                return -EIO;
            }
            if (s.inode->type != INODE_TYPE_DIR) {
                logical_inode_put(target_li);
                logical_inode_put(parent_li);
                return -ENOTDIR;
            }

            uint64_t root_block = s.inode->data_root;
            Dentry_Bptree_node_t *node = new Dentry_Bptree_node_t;
            read_btnode(root_block, node, sizeof(*node));

            if (node->head.head.fields.entry_count > 0) {
                delete node;
                logical_inode_put(target_li);
                logical_inode_put(parent_li);
                return -ENOTEMPTY;
            }
            delete node;

            if (!dentry_cache_remove(parent_inode, leaf_name)) {
                logical_inode_put(target_li);
                logical_inode_put(parent_li);
                return -ENOENT;
            }

            if (bcache) { bcache->free(root_block, 1); bcache->free(root_block + 1, 1); }
            if (ibcache) ibcache->free_inode(target_inum);

            memset(s.inode, 0, sizeof(raven_Inode_t));
            s.dirty();

            logical_inode_put(target_li);
        }
    }
    logical_inode_put(parent_li);
    return 0;
}

int RavenFS::readdir(const char *path, DirEntryInfo *entries, int max_entries) {
    uint64_t target_inum;
    int rc = path_resolve(path, &target_inum);
    if (rc < 0) return rc;
    return readdir_inum(target_inum, entries, max_entries);
}

int RavenFS::readdir_inum(uint64_t inum, DirEntryInfo *entries, int max_entries) {
    LogicalInode *li = logical_inode_get(inum);
    {
        std::lock_guard<std::mutex> lock(li->mtx);

        raven_Inode_t inode;
        inode_read(inum, &inode);
        if (inode.type != INODE_TYPE_DIR) {
            logical_inode_put(li);
            return -ENOTDIR;
        }

        int n = dentry_enumerate(inum, entries, max_entries);

        /* 预热 dentry cache — 后续路径解析可命中 */
        for (int i = 0; i < n; i++) {
            const char *name = entries[i].name;
            /* 跳过 . 和 ..（元数据，不需要缓存） */
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;
            dentry_cache_warm(inum, name, entries[i].inode);
        }

        logical_inode_put(li);
        return n;
    }
}

int RavenFS::inode_type(uint64_t inum) {
    if (inum >= sb.inode_array_count)
        return -ENOENT;
    raven_Inode_t inode;
    inode_read(inum, &inode);
    if (inode.type > 2)
        return -EIO;
    return (int)inode.type;
}

void RavenFS::dump_info() const {
    fprintf(stderr,
        "RavenFS info:\n"
        "  block size:    4096\n"
        "  total blocks:  %lu\n"
        "  inode count:   %lu\n"
        "  root inode:    %lu\n"
        "  bitmap:        [%lu, %lu) (%lu blocks)\n"
        "  inode array:   [%lu, %lu) (%lu blocks)\n"
        "  inode bitmap:  [%lu, %lu) (%lu blocks)\n"
        "  lba_size:      %lu\n"
        "  multiplier:    %lu\n"
        "  world_base:    %lu\n",
        (unsigned long)geo.total_blocks,
        (unsigned long)sb.inode_array_count,
        (unsigned long)root_inum,
        (unsigned long)geo.bitmap_start,
        (unsigned long)geo.total_blocks,
        (unsigned long)geo.bitmap_blocks,
        (unsigned long)geo.inode_array_base,
        (unsigned long)geo.bitmap_start,
        (unsigned long)geo.inode_blocks,
        (unsigned long)geo.inode_array_bitmap_base,
        (unsigned long)geo.inode_array_base,
        (unsigned long)geo.inode_bitmap_blocks,
        (unsigned long)bdev.lba_size,
        (unsigned long)bdev.multiplier,
        (unsigned long)sb.world_base_lba
    );
}


