# 用户态 get_processor_id() 宿主桩（weak）：ravenfs_juvenile.cpp 的外部身份合约，
# 所有链接 lib 的 exe 都要带上——缺了 link 报 undefined reference。
# 多线程分片压测自带强定义覆盖（见 cpu_id_stub.cpp 头注释）。
set(CPU_ID_STUB cpu_id_stub.cpp)

# 平台中立红黑树轮子（2026-10-04）：两个纯 C 轮子（本工程默认仅 CXX），
# 且只依赖 platform/intrusive 收口——直接编入可执行，**不链** god/ko 其它任何东西。
#   · intrusive：侵入式 · 零分配（无分配接口；元素内存调用方自管）
#   · owning   ：STL 式 owning（堆回调注入；insert 分配拷贝、erase/fini 释放）
# owning 复用 intrusive 核，故同一可执行需同时编两个 .c。
enable_language(C)
add_executable(ravenfs_test_rbtree_intrusive
    test_rbtree_intrusive.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_test_rbtree_intrusive PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)

# 分片自治单元 struct rfs_slice（⇄ god mgr_slice_t）冒烟单测（2026-10-04）：
# 直接编 ../linux/ravenfs_allocator_slice.c + 侵入 RB 核 + 测试；不链 god/ko 其它东西。
add_executable(ravenfs_test_allocator_slice
    test_allocator_slice.c
    ../linux/ravenfs_allocator_slice.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_test_allocator_slice PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)

# 顶层路由 struct rfs_allocator（⇄ god fs_allocator_t）冒烟单测（2026-10-04）：
# 直接编 ../linux/ravenfs_allocator_router.c + ../linux/ravenfs_allocator_slice.c
# + 侵入 RB 核 + 测试（不链 god/ko 其它东西）。
add_executable(ravenfs_test_allocator_router
    test_allocator_router.c
    ../linux/ravenfs_allocator_router.c
    ../linux/ravenfs_allocator_slice.c
    ../linux/inode_data_plane.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_test_allocator_router PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)

# 全局 inode 数据面 struct rfs_inode_plane **独立单测**：只编 inode_data_plane.c +
# 侵入 RB 核（不编 router/slice/god）；grow 的 block 符号由测试内打桩 → 证明面自足。
add_executable(ravenfs_test_inode_data_plane
    test_inode_data_plane.c
    ../linux/inode_data_plane.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_test_inode_data_plane PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)

# 分配器片 struct rfs_slice 对抗性压力验证（测试组，2026-10-04）：
# 影子账本 + 朴素参考实现差分 + 每步不变量 + 对抗/边界；直接编
# ../linux/ravenfs_allocator_slice.c + 侵入 RB 核 + 本测试（不链 god/ko）。
add_executable(ravenfs_stress_allocator_slice
    stress_allocator_slice.c
    ../linux/ravenfs_allocator_slice.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_stress_allocator_slice PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)

# 分配器顶层路由 struct rfs_allocator 集成对抗性压力验证（测试组，2026-10-04）：
# 全局影子账本 + 朴素全组参考实现差分 + 跨片路由全覆盖 + 每步不变量；直接编
# ../linux/ravenfs_allocator_router.c + ../linux/ravenfs_allocator_slice.c
# + 侵入 RB 核 + 本测试（不链 god/ko）。
add_executable(ravenfs_stress_allocator_router
    stress_allocator_router.c
    ../linux/ravenfs_allocator_router.c
    ../linux/ravenfs_allocator_slice.c
    ../linux/inode_data_plane.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_stress_allocator_router PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)
add_executable(ravenfs_test_rbtree_owning
    test_rbtree_owning.c
    ../linux/ravenfs_rbtree_owning.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_test_rbtree_owning PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)   # E 组并行压测（test_extent_ops.cpp）用 std::thread

# 分配器顶层路由多线程变体（测试组，2026-10-04）：N 线程并发 alloc/free 跨片，供
# ThreadSanitizer 探测路由层 data race（preferred 游标原子性 / 跨片路由）。
add_executable(ravenfs_stress_allocator_router_mt
    stress_allocator_router_mt.c
    ../linux/ravenfs_allocator_router.c
    ../linux/ravenfs_allocator_slice.c
    ../linux/inode_data_plane.c
    ../linux/ravenfs_rbtree_intrusive.c
)
target_include_directories(ravenfs_stress_allocator_router_mt PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ravenfs_stress_allocator_router_mt PRIVATE Threads::Threads)

# v1.3 dir_insert_dentry 接口专项测试（边界/批量分裂/长名插入+lookup/审计/重挂载）；
# 共享装置 dir_tree_check.hpp（树 DFS 校验器 + fixture）
add_executable(ravenfs_juv_insert
    test_root_dir_insert.cpp
    dir_tree_check.hpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_insert PRIVATE ravenfs_juvenile)

# 注：原「v1.1 stale 测试隔离」的一批目标（extent_walk / extent_fusion /
# bitmap_slice / block_occupy / 名字堆 / benchmark_radix / benchmark_session /
# extent_unmap_loose / extent_loose_stress / pwrite_* / pread_smoke /
# punch_truncate_smoke 等）已随 v1.x 盘面清理**整体删除**（2026-10-16）。
# 现役测试见下方（v1.3 / v2.0）。

# v1.1 extent_map_range / extent_unmap_range 原语对专项测试（root_dir 建真文件 + 跃迁建树根）
add_executable(ravenfs_juv_extent_ops
    test_extent_ops.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_extent_ops PRIVATE ravenfs_juvenile Threads::Threads)


# v1.3 dir_rm_dentry 接口专项测试（长名 split/merge/复用 + 正反向批量删+收缩 +
# 审计 + 重挂载；offload/nocache/selfreliat 三后端）；共享 dir_tree_check.hpp
add_executable(ravenfs_juv_delete
    test_root_dir_delete.cpp
    dir_tree_check.hpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_delete PRIVATE ravenfs_juvenile)

# v1.1 名字堆（name heap）/ 分片位图（bitmap_slices）/ block_occupy 专项测试
# —— **已随 v1.x 清理整体删除**（2026-10-16）。v2.0 仅存名字数据面（见下）。

# v1.1 mgr_slice_t 主从红黑树分片分配器单测（2026-10-03）：
# enable/test_interval/free_interval/free_intervals/all_dump/soft_occupy
add_executable(ravenfs_juv_fs_allocator
    test_fs_allocator.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_fs_allocator PRIVATE ravenfs_juvenile)

# v1.1 fs_allocator_t 路由层单测（2026-10-03）：分片推导 / hard / soft 回滚 /
# occupy 跨片续圈 / 跨片 free / inode / 审计 / all_syn 缺口
add_executable(ravenfs_juv_fs_allocator_router
    test_fs_allocator_router.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_fs_allocator_router PRIVATE ravenfs_juvenile)

# v2.0 全局 inode 数据面专项（2026-10-06，P1）：独立无状态初始化（校验负数例）+
# locate 回环 + alloc/free 融合 + 用尽 grow（硬分配/物理尾合并）+ 随机压测 + 小并发。
add_executable(ravenfs_juv_inode_plane
    test_fs_allocator_inode_plane.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_inode_plane PRIVATE ravenfs_juvenile Threads::Threads)

# 2026-10-04 新树版分片分配器原型可靠性压测（只增测试，不改生产代码）：
#   slice 级：mgr_slice_t 单线程随机+对抗（影子账本全量对账，ops ≥ 2e5）
add_executable(ravenfs_stress_fs_allocator_slice
    stress_fs_allocator_slice.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_stress_fs_allocator_slice PRIVATE ravenfs_juvenile)

#   mt 级：fs_allocator_t 8 线程集成压力（并发分配/归还 + 无重复交付影子账本；
#          自带强定义 get_processor_id，逐线程独立伪 CPU）
add_executable(ravenfs_stress_fs_allocator_mt
    stress_fs_allocator_mt.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_stress_fs_allocator_mt PRIVATE ravenfs_juvenile Threads::Threads)

# 可达性审计（新模型，2026-09-30）：root 先序 DFS + 两张影子图 + XOR 对账
add_executable(ravenfs_juv_audit_dfs
    test_audit_dfs.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_audit_dfs PRIVATE ravenfs_juvenile)

# v1.3 目录名字数据面专项：direct 分配 / 物理碎裂 direct→tree 升级 /
# 映射解析读写回环 / release 全量回收 + audit 全净。
add_executable(ravenfs_juv_name_data
    test_name_data_plane.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_name_data PRIVATE ravenfs_juvenile)

# v1.3 fr 空闲区间树专项：337 满叶 / best-fit >255 索引回归 / 叶分裂 + 内节点 /
# 复合键序 / release。把原先只在临时程序里验证过的 fr 结构路径纳入入库回归。
add_executable(ravenfs_juv_name_data_fr
    test_name_data_freeranges.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_name_data_fr PRIVATE ravenfs_juvenile)

# v1.3 名字数据面离线规整（下行迁移）：name_data_rebuild（tree→direct 压实）+
# name_data_fr_compact（dump+相邻合并，≤9 降级 inline）。供离线工具使用。
add_executable(ravenfs_juv_name_data_offline
    test_name_data_offline.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_name_data_offline PRIVATE ravenfs_juvenile)

# fr 空闲区间树百万级压测（开缓存：offload / selfreliat）。手动运行，不入 ctest 默认。
add_executable(ravenfs_stress_name_data_fr
    stress_name_data_fr.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_stress_name_data_fr PRIVATE ravenfs_juvenile)

# disk_struct_v2.0.h 自校验 TU：让 v2.0 盘面头进入
# compile_commands.json（真实 -I include / -std=gnu++20），VSCode 不再
# 退回带 -D__KERNEL__ 的兜底配置而刷假错；同时钉死 SB 32B magic +
# 全局 inode 数据面四字段的尺寸/offset。无运行时符号。
add_executable(ravenfs_header_selfcheck_v20
    header_selfcheck_v20.cpp
)

# v1.3 fr 空闲区间表平台中立 C 模块（⇄ god name_data_freeranges.cpp）宿主单测：
# 直接编 test_name_data_freeranges.c + ../linux/name_data_freeranges.c
# （测试内置假宿主块存储 + bump 节点分配器；**不链** god/ko 其它任何东西）。
add_executable(ravenfs_test_name_data_freeranges
    test_name_data_freeranges.c
    ../linux/name_data_freeranges.c
)
target_include_directories(ravenfs_test_name_data_freeranges PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)

# mdata 节点缓存平台中立核心（多分片静态基数树 + CTRL_ARRAY）单测（2026-10-17）：
# 直接编 test_mdata_cache_core.c + ../linux/mdata_cache_core.c（+ 兼容空 TU），
# 不链 god/ko 其它任何东西；覆盖几何/statey、get-put-read、wb、drop、reap、
# flush、故障注入与多线程 get/put/reap/drop 竞争。
add_executable(ravenfs_test_mdata_cache_core
    test_mdata_cache_core.c
    ../linux/mdata_cache_core.c
    ../linux/mdata_cache_self_contained.c
)
target_include_directories(ravenfs_test_mdata_cache_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include/linux
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ravenfs_test_mdata_cache_core PRIVATE Threads::Threads)
