cmake_minimum_required(VERSION 3.10)
project(ravenfs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 全局 include：全项目（含所有子目录目标）统一引用 ${CMAKE_CURRENT_SOURCE_DIR}/include
# 子目录不再各自 target_include_directories（2026-08-21 收敛）
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(mkfs)
add_subdirectory(tests)
add_subdirectory(juvenile_god)
add_subdirectory(tools/rfs_admin)   # 离线镜像运维工具（免 sudo / 免 mount）
add_subdirectory(audit)   # 顶层独立只读校验器（与 juvenile_god/linux 平级；不链 god）
add_subdirectory(audit_leakscan)  # audit/ 的 fork：漏块 8K 单元魔数扫描（目标 modified_audit）
