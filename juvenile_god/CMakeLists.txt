# RavenFS v1.1 幼年期上帝类
#
# ravenfs_juvenile.cpp — 挂载/卸载 + 双位图分配器（Raven 实现）
# dentry_tree.cpp      — B+tree 全套（Sparrow 手写）
#
# Sparrow 完成 dentry_tree.cpp 后：取消下行注释即可编入
# set(JUVENILE_BTREE_SRCS dentry_tree.cpp)

add_library(ravenfs_juvenile STATIC
    ravenfs_juvenile.cpp
    # ${JUVENILE_BTREE_SRCS}
)
target_include_directories(ravenfs_juvenile PRIVATE ${CMAKE_SOURCE_DIR}/include)
