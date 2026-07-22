#include "../include/Userspace_God.hpp"
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

    uint64_t dummy;
    if (dentry_lookup(parent_inode, leaf_name, &dummy)) {
        return -EEXIST;
    }

    uint64_t new_inum = ibitmap_alloc_inode();
    if (new_inum == (uint64_t)-1) return -ENOSPC;

    uint64_t dentry_block = bitmap_alloc_blocks(2);
    if (dentry_block == (uint64_t)-1) {
        ibitmap_clear(new_inum);
        return -ENOSPC;
    }

    Dentry_Bptree_node_t dnode;
    memset(&dnode, 0, sizeof(dnode));
    dnode.head.head.fields.entry_count        = 0;
    dnode.head.head.fields.is_internal_node   = 0;
    dnode.head.head.fields.is_root_node       = 1;
    dnode.head.head.fields.parent_ptr         = new_inum;
    dnode.head.ptr1 = 0;
    dnode.head.ptr2 = parent_inode;
    dnode.head.magic1 = DENTRY_MAGIC1;
    dnode.head.magic2 = DENTRY_MAGIC2;
    dnode.head.magic3 = DENTRY_MAGIC3;
    dnode.head.magic4 = DENTRY_MAGIC4;

    write_btnode(dentry_block, &dnode, sizeof(dnode));

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
    ibitmap_set(new_inum);

    if (!dentry_insert(parent_inode, leaf_name, new_inum)) {
        ibitmap_clear(new_inum);
        bitmap_clear(dentry_block);
        bitmap_clear(dentry_block + 1);
        memset(&inode, 0, sizeof(inode));
        inode_write(new_inum, &inode);
        return -ENOSPC;
    }

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

    uint64_t target_inum;
    if (!dentry_lookup(parent_inode, leaf_name, &target_inum))
        return -ENOENT;

    raven_Inode_t inode;
    inode_read(target_inum, &inode);
    if (inode.type != INODE_TYPE_DIR)
        return -ENOTDIR;

    uint64_t root_block = inode.data_root;
    Dentry_Bptree_node_t node;
    read_btnode(root_block, &node, sizeof(node));

    if (node.head.head.fields.entry_count > 0)
        return -ENOTEMPTY;

    if (!dentry_remove(parent_inode, leaf_name))
        return -ENOENT;

    bitmap_clear(root_block);
    bitmap_clear(root_block + 1);

    ibitmap_clear(target_inum);

    memset(&inode, 0, sizeof(inode));
    inode_write(target_inum, &inode);

    return 0;
}

int RavenFS::readdir(const char *path, DirEntryInfo *entries, int max_entries) {
    uint64_t target_inum;
    int rc = path_resolve(path, &target_inum);
    if (rc < 0) return rc;
    return readdir_inum(target_inum, entries, max_entries);
}

int RavenFS::readdir_inum(uint64_t inum, DirEntryInfo *entries, int max_entries) {
    raven_Inode_t inode;
    inode_read(inum, &inode);
    if (inode.type != INODE_TYPE_DIR)
        return -ENOTDIR;
    return dentry_enumerate(inum, entries, max_entries);
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
