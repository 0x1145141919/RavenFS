# RavenFS audit_leakscan — audit/ 的 fork：漏块 8K 单元魔数扫描器
#
# 与 audit/ 的唯一差异：相位 C 收口（reconcile_block）在报每个「位图亮但不可达」
# 块时，额外对该块所属 8K 单元做魔数扫描，判定它是哪一类节点，并汇总。
#   · 逐单元明细 → [TRACE][LEAKSCAN] unit=.. magic0=.. magic1=.. kind=..（需 --trace）
#   · 汇总       → [AUDIT][SUM][LEAKSCAN] units=.. extent_tree_node=.. ...（恒打印）
# 其余（引导五门 / 相位 A / B / 其它收口）与 audit/ 逐字节同源。
#
# 依赖同 audit/：盘面契约头 disk_struct_v2.0.h + magics.h + xxhash + libc；
# 不链 libravenfs_juvenile.a。公共头仍复用 include/audit/*.hpp（不改契约）。

# ② 独立构建：只有本目录就是顶层时才自建 project + 自带 include 路径。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.10)
    project(ravenfs_audit_leakscan LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(RAVENFS_AUDIT_LEAKSCAN_STANDALONE ON)
endif()

add_library(ravenfs_audit_leakscan STATIC
    audit_types.cpp
    audit_report.cpp
    audit_image.cpp
    audit_inode_plane.cpp
    audit_dfs.cpp
    audit_entry.cpp
)

add_executable(modified_audit audit_cli.cpp)
target_link_libraries(modified_audit PRIVATE ravenfs_audit_leakscan xxhash)

if(RAVENFS_AUDIT_LEAKSCAN_STANDALONE)
    # 独立构建时补上仓库统一 include 目录（子目录构建时由根工程提供，不重复加）。
    target_include_directories(ravenfs_audit_leakscan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
endif()
