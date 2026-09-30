# SPDX-License-Identifier: GPL-2.0
#
# tests/bitmap_core_v2/Makefile —— 双位图分片分配器 v2 用户态测（C，glibc）
#
# 只读被测源（不修改、不参与内核构建）；本 Makefile 只编本目录测试 +
# 直接编 3 个实现文件（core / mem / chunk）成用户态（__KERNEL__ 未定义 → 平台层走 libc）。
#
#   make            默认构建 bm_test（-Wall -Wextra -pthread，应 zero-warning）
#   make tsan       构建 bm_test_tsan（-fsanitize=thread）
#   make san        构建 bm_test_san （-fsanitize=address,undefined）
#   make quick|full 跑对应套件
#   make clean

CC      ?= gcc
ROOT    := ../..
INC     := -I$(ROOT)/include/linux
IMPL    := $(ROOT)/linux/bitmap_core.c $(ROOT)/linux/bitmap_mem.c $(ROOT)/linux/bitmap_chunk.c
LOCAL   := bm_harness.c bm_test.c
WARN    := -Wall -Wextra
BASE    := -std=gnu11 -g -O2 -D_GNU_SOURCE -pthread $(INC)
LDLIBS  := -pthread

BIN       := bm_test
BIN_TSAN  := bm_test_tsan
BIN_SAN   := bm_test_san
BIN_PREFIX:= sim/sim_prefix

.PHONY: all tsan san quick full prefix clean

all: $(BIN)

$(BIN): $(LOCAL) $(IMPL)
	$(CC) $(WARN) $(BASE) $(LOCAL) $(IMPL) -o $@ $(LDLIBS)

tsan: $(BIN_TSAN)

$(BIN_TSAN): $(LOCAL) $(IMPL)
	$(CC) $(WARN) $(BASE) -fsanitize=thread -O1 $(LOCAL) $(IMPL) -o $@ $(LDLIBS)

san: $(BIN_SAN)

$(BIN_SAN): $(LOCAL) $(IMPL)
	$(CC) $(WARN) $(BASE) -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
		$(LOCAL) $(IMPL) -o $@ $(LDLIBS)

quick: $(BIN)
	./$(BIN) quick

full: $(BIN)
	./$(BIN) full

# BUG-1 修前对照（PRE-FIX 模拟后端；见 sim/ 注释）
prefix: $(BIN_PREFIX)

$(BIN_PREFIX): sim/sim_prefix.c sim/bitmap_chunk_prefix.c bm_harness.c \
		$(ROOT)/linux/bitmap_core.c $(ROOT)/linux/bitmap_mem.c
	$(CC) $(WARN) $(BASE) -I. sim/sim_prefix.c sim/bitmap_chunk_prefix.c \
		bm_harness.c $(ROOT)/linux/bitmap_core.c $(ROOT)/linux/bitmap_mem.c \
		-o $@ $(LDLIBS)

clean:
	rm -f $(BIN) $(BIN_TSAN) $(BIN_SAN) $(BIN_PREFIX)
