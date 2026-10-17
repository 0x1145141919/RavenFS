# rfs_compact — RavenFS extent 树瘦身/坍缩修复器（host 侧，用户态挂载）
#
# 依赖：已构建的 libravenfs_juvenile.a（god 静态库）+ tests/cpu_id_stub.cpp + 系统 xxhash。
# 若库不存在：先 `cmake --build build_v20 --target ravenfs_juvenile`。

ROOT := ../..
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I$(ROOT)/include -I$(ROOT)/tests
LIB  := $(ROOT)/build_v20/juvenile_god/libravenfs_juvenile.a
STUB := $(ROOT)/tests/cpu_id_stub.cpp
LDLIBS := -lxxhash -lpthread

rfs_compact: rfs_compact.cpp $(LIB)
	$(CXX) $(CXXFLAGS) rfs_compact.cpp $(STUB) $(LIB) -o $@ $(LDLIBS)

clean:
	rm -f rfs_compact

.PHONY: clean
