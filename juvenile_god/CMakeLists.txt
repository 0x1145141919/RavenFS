# RavenFS v1.1 幼年期上帝类
#
# ravenfs_juvenile.cpp — 挂载/卸载 + 双位图分配器 + CRC32/name_hash（Raven 实现）
# bitmap_slice.cpp    — bitmap_slice_t 分片位图自治单元（Raven 实现）
# dentry_tree.cpp      — dentry B+tree 全套（Sparrow 手写）
# root_dir.cpp         — root_dir 顶层业务（Sparrow 手写）
# name_heap.cpp        — 名字堆：长名外置分配/释放/读写（Raven 实现）
# inode_cache.cpp      — inode 缓存三模式（FULL_RAM / CTRL_ARRAY 已实现，RADIX 草稿）
# mdata_piece_cache.cpp — 8K 元数据负载片缓存 self_reliat 双档（CTRL_ARRAY ≤13 / RADIX >13）

add_library(ravenfs_juvenile STATIC
    ravenfs_juvenile.cpp
    bitmap_slice.cpp
    dentry_tree.cpp
    root_dir.cpp
    name_heap.cpp
    inode_cache.cpp
    mdata_piece_cache.cpp
)
