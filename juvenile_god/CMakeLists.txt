# RavenFS v1.1 幼年期上帝类
#
# ravenfs_juvenile.cpp — 挂载/卸载 + 块/inode I/O + 上帝类编排（Raven 实现）
# bitmap_slices.cpp — 双位图分片分配器独立模块：slice 自治单元 + mgr 几何推导/路由
#                    （2026-09-03 从上帝类顶出；原 bitmap_slice.cpp 退役）
# dentry_tree.cpp      — dentry B+tree 全套（Sparrow 手写）
# extent_tree.cpp      — extent B+tree 轮子区：walk / inner+leaf lookup（Raven 起草）
# root_dir.cpp         — root_dir 顶层业务（Sparrow 手写）
# name_heap.cpp        — 名字堆：长名外置分配/释放/读写（Raven 实现）
# inode_cache.cpp      — inode 缓存三模式（FULL_RAM / CTRL_ARRAY 已实现，RADIX 草稿）
# mdata_piece_cache.cpp — 8K 元数据负载片缓存 self_reliat 双档（CTRL_ARRAY ≤13 / RADIX >13）

add_library(ravenfs_juvenile STATIC
    ravenfs_juvenile.cpp
    bitmap_slices.cpp
    dentry_tree.cpp
    extent_tree.cpp
    root_dir.cpp
    name_heap.cpp
    inode_cache.cpp
    mdata_piece_cache.cpp
    direct_file_ops.cpp
)
