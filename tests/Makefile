CXX      ?= g++
CXXFLAGS  = -std=c++20 -O2 -g -Wall -Wextra -I../include
LDFLAGS   = -pthread
TARGET    = ravenfs_test

SRCS      = ravenfs_god.cpp ravenfs_test_main.cpp
OBJS      = ravenfs_god.o ravenfs_test_main.o

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp ravenfs_god.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 快速跑: 裸盘
test: $(TARGET)
	@echo "=== Running on /dev/nvme1n1 ==="
	./$(TARGET) /dev/nvme1n1 || true

# 用文件映像测试（无需真盘）
test_img: $(TARGET)
	@echo "=== Creating 256MB test image ==="
	dd if=/dev/zero of=/tmp/ravenfs_test.img bs=1M count=256 2>/dev/null
	@echo "=== Running mkfs.ravenfs ==="
	../mkfs/mkfs.ravenfs --force --verbose /tmp/ravenfs_test.img
	@echo "=== Running tests ==="
	./$(TARGET) /tmp/ravenfs_test.img
	@rm -f /tmp/ravenfs_test.img

clean:
	rm -f $(OBJS) $(TARGET)
