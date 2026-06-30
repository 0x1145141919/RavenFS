cmake_minimum_required(VERSION 3.20)
project(ravenfs-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 指向 core 目录
add_subdirectory(../core core)

add_executable(ravenfs_cli main_cli.cpp)
target_link_libraries(ravenfs_cli PRIVATE ravenfs_core)

# FUSE 守护进程（后期启用）
# find_package(FUSE3 REQUIRED)
# add_executable(ravendsrv ravendsrv_fuse.cpp)
# target_link_libraries(ravendsrv PRIVATE ravenfs_core FUSE3::FUSE3)

# 测试
enable_testing()
add_subdirectory(tests)
