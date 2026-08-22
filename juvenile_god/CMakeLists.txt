# RavenFS v1.1 幼年期上帝类
#
# ravenfs_juvenile.cpp — 挂载/卸载 + 双位图分配器 + CRC32/name_hash（Raven 实现）
# dentry_tree.cpp      — dentry B+tree 全套（Sparrow 手写）
# root_dir.cpp         — root_dir 顶层业务（Sparrow 手写）
# inode_cache.cpp      — inode 缓存三模式（FULL_RAM / CTRL_ARRAY 已实现，RADIX 草稿）

add_library(ravenfs_juvenile STATIC
    ravenfs_juvenile.cpp
    dentry_tree.cpp
    root_dir.cpp
    inode_cache.cpp
)
