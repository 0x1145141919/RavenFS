cmake_minimum_required(VERSION 3.10)
project(ravenfs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(Userspace_God)
add_subdirectory(mkfs)
add_subdirectory(fsck)
add_subdirectory(tests)
