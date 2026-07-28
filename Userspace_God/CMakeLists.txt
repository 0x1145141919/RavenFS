find_package(Threads REQUIRED)

add_library(ravenfs_god STATIC
    crc32.cpp
    io.cpp
    mount.cpp
    validate.cpp
    bitmap.cpp
    inode.cpp
    dentry.cpp
    path.cpp
    dir.cpp
    inode_cache_controller.cpp
    validate_dentry.cpp
    bitmap_cache_controller.cpp
    inode_bitmap_controller.cpp
    vfs_cache.cpp
)
target_include_directories(ravenfs_god PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ravenfs_god PUBLIC Threads::Threads)
