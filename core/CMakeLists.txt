cmake_minimum_required(VERSION 3.20)
project(ravenfs-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(ravenfs_core STATIC
    btree.cpp
    bitmap.cpp
    inode.cpp
    dentry.cpp
    cache.cpp
    ravenfs.cpp
)

target_include_directories(ravenfs_core PUBLIC include)
