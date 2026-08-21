add_executable(mkfs.ravenfs main.cpp mkfs.cpp)
target_link_libraries(mkfs.ravenfs PRIVATE uuid)
