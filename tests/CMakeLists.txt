# v1.1 幼年期上帝类：root_dir_insert_dentry 接口专项测试
add_executable(ravenfs_juv_insert
    test_root_dir_insert.cpp
)
target_link_libraries(ravenfs_juv_insert PRIVATE ravenfs_juvenile)

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
