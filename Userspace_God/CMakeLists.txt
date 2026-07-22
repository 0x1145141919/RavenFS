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
    cache.cpp
    inode_cache_controller.cpp
    validate_dentry.cpp
)
target_include_directories(ravenfs_god PUBLIC ${CMAKE_SOURCE_DIR}/include)
