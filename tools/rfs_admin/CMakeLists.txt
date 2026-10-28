# rfs_admin —— RavenFS 离线镜像运维工具（免 sudo / 免 mount）
#
# 单文件工具，链接上帝类静态库；cpu_id_stub.cpp 提供用户态
# get_processor_id() weak 桩（与 tests/ 下各目标同一外部身份合约）。
add_executable(rfs_admin
    rfs_admin.cpp
    ${CMAKE_SOURCE_DIR}/tests/cpu_id_stub.cpp
)
target_link_libraries(rfs_admin PRIVATE ravenfs_juvenile)
