# 用户态 get_processor_id() 宿主桩（weak）：ravenfs_juvenile.cpp 的外部身份合约，
# 所有链接 lib 的 exe 都要带上——缺了 link 报 undefined reference。
# 多线程分片压测自带强定义覆盖（见 cpu_id_stub.cpp 头注释）。
set(CPU_ID_STUB cpu_id_stub.cpp)

find_package(Threads REQUIRED)   # E 组并行压测（test_extent_ops.cpp）用 std::thread

# v1.1 幼年期上帝类：root_dir_insert_dentry 接口专项测试
add_executable(ravenfs_juv_insert
    test_root_dir_insert.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_insert PRIVATE ravenfs_juvenile)

# v1.1 extent_Bptree_walk 左闭右开语义专项测试（手工铺双层树打边界）
add_executable(ravenfs_juv_extent_walk
    test_extent_walk.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_extent_walk PRIVATE ravenfs_juvenile)

# v1.1 extent_map_range / extent_unmap_range 原语对专项测试（root_dir 建真文件 + 跃迁建树根）
add_executable(ravenfs_juv_extent_ops
    test_extent_ops.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_extent_ops PRIVATE ravenfs_juvenile Threads::Threads)

# v1.1 幼年期上帝类：root_dir_rm_dentry 接口专项测试
add_executable(ravenfs_juv_delete
    test_root_dir_delete.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_delete PRIVATE ravenfs_juvenile)

# v1.1 bitmap_slice_t 分片位图自治单元单测（alloc_hard hint 命中/miss +
# alloc_soft 全量 gather；free_count / hint 变化校验；soft_occupy 锚定软占领）
add_executable(ravenfs_juv_bitmap_slice
    test_bitmap_slice.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_bitmap_slice PRIVATE ravenfs_juvenile)

# v1.1 block_occupy_soft 定点占领路由测试（自旋强制抢 + 片尾跨界续圈 + 跨片释放）
add_executable(ravenfs_juv_block_occupy
    test_block_occupy.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_block_occupy PRIVATE ravenfs_juvenile)

# v1.1 名字堆专项测试（长名外置：档位覆盖/完整比较/位图一致性/释放/持久化）
add_executable(ravenfs_juv_name_heap
    test_name_heap.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_name_heap PRIVATE ravenfs_juvenile)

# v1.1 名字堆极限压测（全档位灌满/加塞失败/全删/堆回收；--full 大魔王）
add_executable(ravenfs_juv_name_heap_stress
    test_name_heap_stress.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_juv_name_heap_stress PRIVATE ravenfs_juvenile)

# v1.1 Inode 缓存控制器真盘大测（RADIX 模式，按超级块链式推导初始化）
# 用法: ./benchmark_radix [/dev/nvme1n1]
add_executable(ravenfs_bench_radix
    benchmark_radix.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_bench_radix PRIVATE ravenfs_juvenile)

# v1.1 InodeSession 缓存命中 vs 直接 NVMe 读 对比
add_executable(ravenfs_bench_session
    benchmark_session_vs_nvme.cpp
    ${CPU_ID_STUB}
)
target_link_libraries(ravenfs_bench_session PRIVATE ravenfs_juvenile)
