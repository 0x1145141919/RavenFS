add_executable(ravenfs_test
    test_main.cpp
    test_mkdir_rmdir.cpp
)
target_link_libraries(ravenfs_test PRIVATE ravenfs_god)
