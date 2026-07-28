add_executable(ravenfs_test
    test_main.cpp
    test_mkdir_rmdir.cpp
    debug_dump.cpp
)
target_include_directories(ravenfs_test PRIVATE ../include)
target_link_libraries(ravenfs_test PRIVATE ravenfs_god)

add_executable(ravenfs_stress
    stress_mkdir.cpp
)
target_link_libraries(ravenfs_stress PRIVATE ravenfs_god)

add_executable(ravenfs_stress_rev
    stress_mkdir_rev.cpp
)
target_link_libraries(ravenfs_stress_rev PRIVATE ravenfs_god)

add_executable(ravenfs_stress_delete
    stress_delete.cpp
)
target_link_libraries(ravenfs_stress_delete PRIVATE ravenfs_god)

add_executable(ravenfs_replay_delete
    replay_delete.cpp
)
target_link_libraries(ravenfs_replay_delete PRIVATE ravenfs_god)

add_executable(ravenfs_replay_insert
    replay_insert.cpp
)
target_link_libraries(ravenfs_replay_insert PRIVATE ravenfs_god)
