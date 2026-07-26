#pragma once
#include "../include/ravenfs/ravenfs.hpp"

/* 在测试中每次操作后打印 cache vs 磁盘双视图 */
static inline void debug_dump(const char *label, RavenFS &fs) {
    fs.debug_dump(label);
}
