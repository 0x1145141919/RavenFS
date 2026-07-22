add_executable(ravenfs_test
    test_main.cpp
    test_mkdir_rmdir.cpp
)
target_link_libraries(ravenfs_test PRIVATE ravenfs_god)

add_executable(ravenfs_stress
    stress_mkdir.cpp
)
target_link_libraries(ravenfs_stress PRIVATE ravenfs_god)
