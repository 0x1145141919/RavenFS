# v1.1 幼年期上帝类：root_dir_insert_dentry 接口专项测试
add_executable(ravenfs_juv_insert
    test_root_dir_insert.cpp
)
target_link_libraries(ravenfs_juv_insert PRIVATE ravenfs_juvenile)

# v1.1 extent_Bptree_walk 左闭右开语义专项测试（手工铺双层树打边界）
add_executable(ravenfs_juv_extent_walk
    test_extent_walk.cpp
)
target_link_libraries(ravenfs_juv_extent_walk PRIVATE ravenfs_juvenile)

# v1.1 extent_map_range / extent_unmap_range 原语对专项测试（root_dir 建真文件 + 跃迁建树根）
add_executable(ravenfs_juv_extent_ops
    test_extent_ops.cpp
)
target_link_libraries(ravenfs_juv_extent_ops PRIVATE ravenfs_juvenile)

# v1.1 幼年期上帝类：root_dir_rm_dentry 接口专项测试
add_executable(ravenfs_juv_delete
    test_root_dir_delete.cpp
)
target_link_libraries(ravenfs_juv_delete PRIVATE ravenfs_juvenile)

# v1.1 bitmap_slice_t 分片位图自治单元单测（alloc_hard hint 命中/miss +
# alloc_soft 全量 gather；free_count / hint 变化校验）
add_executable(ravenfs_juv_bitmap_slice
    test_bitmap_slice.cpp
)
target_link_libraries(ravenfs_juv_bitmap_slice PRIVATE ravenfs_juvenile)

# v1.1 名字堆专项测试（长名外置：档位覆盖/完整比较/位图一致性/释放/持久化）
add_executable(ravenfs_juv_name_heap
    test_name_heap.cpp
)
target_link_libraries(ravenfs_juv_name_heap PRIVATE ravenfs_juvenile)

# v1.1 名字堆极限压测（全档位灌满/加塞失败/全删/堆回收；--full 大魔王）
add_executable(ravenfs_juv_name_heap_stress
    test_name_heap_stress.cpp
)
target_link_libraries(ravenfs_juv_name_heap_stress PRIVATE ravenfs_juvenile)

# v1.1 Inode 缓存控制器真盘大测（RADIX 模式，按超级块链式推导初始化）
# 用法: ./benchmark_radix [/dev/nvme1n1]
add_executable(ravenfs_bench_radix
    benchmark_radix.cpp
)
target_link_libraries(ravenfs_bench_radix PRIVATE ravenfs_juvenile)

# v1.1 InodeSession 缓存命中 vs 直接 NVMe 读 对比
add_executable(ravenfs_bench_session
    benchmark_session_vs_nvme.cpp
)
target_link_libraries(ravenfs_bench_session PRIVATE ravenfs_juvenile)
