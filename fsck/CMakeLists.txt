add_executable(ravenfs.fsck main.cpp fsck.cpp)
target_include_directories(ravenfs.fsck PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ravenfs.fsck PRIVATE ravenfs_god uuid)
target_compile_definitions(ravenfs.fsck PRIVATE USE_RAVENFS_GOD=1)
