#include "ravenfs/ravenfs.h"

/* ─── B+tree 操作桩 ─── */

bool RavenFS::btree_load_node(uint64_t block, Bptree_node_t* node) {
    return read_block(block, node);
}

bool RavenFS::btree_store_node(uint64_t block, const Bptree_node_t* node) {
    return write_block(block, node);
}

bool RavenFS::btree_lookup(uint64_t root_block, uint64_t target_fblk, uint64_t* pblk) {
    /* TODO: B+tree 遍历。返回叶子层 target_fblk 所在 extent 的 pblkbase */
    (void)root_block; (void)target_fblk; (void)pblk;
    return false;
}
