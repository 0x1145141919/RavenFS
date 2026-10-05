# RavenFS v1.1 幼年期上帝类
#
# ravenfs_juvenile.cpp — 挂载/卸载 + 块/inode I/O + 上帝类编排（Raven 实现）
# bitmap_slices.cpp — 双位图分片分配器独立模块：slice 自治单元 + mgr 几何推导/路由
#                    （2026-09-03 从上帝类顶出；原 bitmap_slice.cpp 退役）
# fs_allocator.cpp  — mgr_slice_t 主从红黑树分片分配器片自治单元（2026-10-03 新建）：
#                    enable_slice / test_interval / free_interval / free_intervals /
#                    all_dump / soft_occupy（红黑树版后端，与位图版并存待切换）
# dentry_tree.cpp      — dentry B+tree 全套（2026-09-07 拆为 4 域文件，Sparrow 手写代码按域分置）：
#     dentry_walk_lookup.cpp — 只读导航：node_max_hash / walk / name_cmp / 两 lookup
#     dentry_insert_delete.cpp — 叶子条目增删 + 下游资产维护（guard 回滚）
#     dentry_split.cpp       — 分裂域 + 节点基建（leaf/inner split / parent_apply / 建根）
#     dentry_balance.cpp     — 欠载平衡域（node_balance / root_shrink）
# extent_tree.cpp      — extent B+tree 轮子区（2026-09-07 拆为 6 域文件，见下）：
#     extent_walk_lookup.cpp — 只读导航：walk / inner+leaf lookup（Raven 起草）
#     extent_iterator.cpp    — 稀疏文件空洞/区间游走器全套
#     extent_split.cpp       — 分裂域 + 节点基建（refresh_key_chain / 分裂三件套）
#     extent_balance.cpp     — 欠载平衡域（node_balance / root_shrink / balance_from）
#     extent_map_unmap.cpp   — map/unmap 原语对（预算池原子，2026-09-07）
#     extent_rebuild.cpp     — 激进重建（全量融合，可并对全并 + 均分重打包）
# root_dir.cpp         — dir_* 顶层业务（任意目录；root_dir_* 薄封装转发）
# name_data_plane.cpp  — 目录名字数据面：长名外置分配/释放/读写（映射树 + fr 树）
# name_data_freeranges.cpp — fr 空闲区间 B+树机械（best-fit / 分裂级联 / 空叶退役）
# inode_cache.cpp      — inode 缓存三模式（FULL_RAM / CTRL_ARRAY 已实现，RADIX 草稿）
# mdata_piece_cache.cpp — 8K 元数据负载片缓存 self_reliat 双档（CTRL_ARRAY ≤13 / RADIX >13）
# audit.cpp         — 静态合法性审计（audit_inode 模块；复用位图+缓存；Docs/audit_inode_design.md）

add_library(ravenfs_juvenile STATIC
    ravenfs_juvenile.cpp
    fs_allocator.cpp
    fs_allocator_router.cpp
    dentry_walk_lookup.cpp
    dentry_insert_delete.cpp
    dentry_split.cpp
    dentry_balance.cpp
    extent_walk_lookup.cpp
    extent_iterator.cpp
    extent_split.cpp
    extent_balance.cpp
    extent_map_unmap.cpp
    extent_rebuild.cpp
    root_dir.cpp
    name_data_plane.cpp
    name_data_freeranges.cpp
    inode_cache.cpp
    mdata_piece_cache.cpp
    direct_file_ops.cpp
    audit.cpp
)
