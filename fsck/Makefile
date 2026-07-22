CXX      ?= g++
CXXFLAGS  = -std=c++20 -O2 -g -Wall -Wextra
CPPFLAGS  = -I../include
LDLIBS    = -luuid
LDFLAGS   = -pthread

TARGET    = ravenfs.fsck

# ── 基础 fsck 对象（不依赖 god 类） ──
FSCK_OBJ  = fsck.o

# ── 条件：god 类支持（--tree 模式） ──
GOD_DIR   = ../tests
GOD_OBJ   = $(GOD_DIR)/ravenfs_god.o

USE_GOD   ?= 0
ifeq ($(USE_GOD),1)
  MAIN_OBJ  = main-with-god.o
  MAIN_SRC  = main.cpp
  MAIN_FLAGS = -DUSE_RAVENFS_GOD=1
  LINK_OBJS = $(MAIN_OBJ) $(FSCK_OBJ) $(GOD_OBJ)
else
  MAIN_OBJ  = main.o
  MAIN_SRC  = main.cpp
  MAIN_FLAGS =
  LINK_OBJS = $(MAIN_OBJ) $(FSCK_OBJ)
endif

.PHONY: all clean god_obj

all: $(TARGET)

$(TARGET): $(LINK_OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS) $(LDFLAGS)

fsck.o: fsck.cpp fsck.hpp ../include/disk_struct.hpp ../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

# main.cpp 总是按基础版编译
main.o: main.cpp fsck.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

# 带 god 支持时用 -DUSE_RAVENFS_GOD 重编
main-with-god.o: main.cpp fsck.hpp $(GOD_OBJ)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(MAIN_FLAGS) -c -o $@ $<

god_obj: $(GOD_OBJ)

$(GOD_OBJ): $(GOD_DIR)/ravenfs_god.cpp $(GOD_DIR)/ravenfs_god.hpp
	$(MAKE) -C $(GOD_DIR) ravenfs_god.o

clean:
	rm -f *.o $(TARGET)

.PHONY: all clean god_obj
