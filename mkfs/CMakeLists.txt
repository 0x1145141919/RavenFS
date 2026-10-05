add_executable(mkfs.ravenfs main.cpp mkfs.cpp mkfs_v20.cpp)
target_link_libraries(mkfs.ravenfs PRIVATE uuid)
