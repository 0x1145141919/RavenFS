add_executable(mkfs.ravenfs main.cpp mkfs.cpp)
target_include_directories(mkfs.ravenfs PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(mkfs.ravenfs PRIVATE uuid)
