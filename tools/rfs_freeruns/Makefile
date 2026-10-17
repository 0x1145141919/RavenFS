# rfs_freeruns — RavenFS v2.0 空闲区间（free-run）dump（只读，自包含）
# 无外部库依赖：只用 include/ 的 v2.0 SB 头 + 系统调用。
# 契约依据见 rfs_freeruns.cpp 顶部（与 mkfs_v20.cpp / audit 同源）。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g
CPPFLAGS  = -I../../include

TARGET    = rfs_freeruns
SRCS      = rfs_freeruns.cpp

all: $(TARGET)

$(TARGET): $(SRCS) ../../include/disk_struct_v2.0.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
