# v1.1 幼年期上帝类：root_dir_insert_dentry 接口专项测试
add_executable(ravenfs_juv_insert
    test_root_dir_insert.cpp
)
target_include_directories(ravenfs_juv_insert PRIVATE ../include)
target_link_libraries(ravenfs_juv_insert PRIVATE ravenfs_juvenile)

# v1.1 幼年期上帝类：root_dir_rm_dentry 接口专项测试
add_executable(ravenfs_juv_delete
    test_root_dir_delete.cpp
)
target_include_directories(ravenfs_juv_delete PRIVATE ../include)
target_link_libraries(ravenfs_juv_delete PRIVATE ravenfs_juvenile)
