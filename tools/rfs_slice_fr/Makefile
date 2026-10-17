# rfs_slice_fr — RavenFS v2.0 分片空闲区间 trace dump（只读，自包含）
# 无外部库依赖：只用 include/ 的 v2.0 SB 头 + 系统调用。
# 分片几何依据见 rfs_slice_fr.cpp 顶部（与 god fs_allocator_router.cpp / ko ravenfs_allocator_router.c 逐式一致）。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g
CPPFLAGS  = -I../../include

TARGET    = rfs_slice_fr
SRCS      = rfs_slice_fr.cpp

all: $(TARGET)

$(TARGET): $(SRCS) ../../include/disk_struct_v2.0.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
