# rfs_trim — RavenFS 离线裁剪（trim/discard）工具（host 侧，自包含）
# 无外部库依赖：只用 include/ 的 SB 头 + 系统调用（ioctl BLKDISCARD / fallocate punch）。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g
CPPFLAGS  = -I../../include

TARGET    = rfs_trim
SRCS      = rfs_trim.cpp

all: $(TARGET)

$(TARGET): $(SRCS) ../../include/disk_struct_v1.1.hpp ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

check: $(TARGET)
	./$(TARGET) --selftest-plan
	./selftest.sh

clean:
	rm -f $(TARGET)

.PHONY: all clean check
