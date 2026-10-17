# RavenFS audit — 顶层独立只读校验器（与 juvenile_god/、linux/ 平级）
#
# 纪律：audit/DISCIPLINE.md（信任边界 + 引导期五道 fail-fast 硬门 F0–F4）
# 依赖：盘面契约头 disk_struct_v2.0.h + magics.h + xxhash + libc。
#       **不链 libravenfs_juvenile.a**，不碰 fs_allocator / mdata_piece_cache /
#       InodeSession / read_buff（见纪律 §1 信任边界表）。
#
# 公共头统一放 include/（根 CMakeLists 全局 include_directories 约定）：
#   include/audit/audit_types.hpp, include/audit/audit_image.hpp, include/audit/audit.hpp
#
# 两种构建方式都支持：
#   ① 作为子目录（推荐）：根工程 `cmake -S . -B build_v20 && cmake --build build_v20 --target audit`
#   ② 独立构建：`cmake -S audit -B <builddir> && cmake --build <builddir>`（禁止源内 cmake .）

# ② 独立构建：只有本目录就是顶层时才自建 project + 自带 include 路径。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.10)
    project(ravenfs_audit LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(RAVENFS_AUDIT_STANDALONE ON)
endif()

add_library(ravenfs_audit STATIC
    audit_types.cpp
    audit_report.cpp
    audit_image.cpp
    audit_inode_plane.cpp
    audit_dfs.cpp
    audit_entry.cpp
)

add_executable(audit audit_cli.cpp)
target_link_libraries(audit PRIVATE ravenfs_audit xxhash)

if(RAVENFS_AUDIT_STANDALONE)
    # 独立构建时补上仓库统一 include 目录（子目录构建时由根工程提供，不重复加）。
    target_include_directories(ravenfs_audit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
endif()
