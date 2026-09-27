#pragma once
/*
 * ═══════════════════════════════════════════════════════════════════════════
 * m42_fill.h — M4.2 确定性填充字节（**C / C++ 双兼容**；生产/消费两侧唯一实现）
 *
 * 与 `m41_expect.h::m41_fill_byte` **逐字节相同**（同一公式的 C 版副本）。
 *   · m41 版是 C++（带 <string>/<vector>），无法被 guest 侧纯 C 工具（m42_writer.c）包含；
 *   · 故此处给出一份 C 兼容的 `m42_fill_byte`，供：
 *       - mkstates_w.cpp（host，造靶子 + 出期望清单；同时断言两版一致）
 *       - m42_writer.c （host 编译、guest 运行：按 (name, off) 写填充字节）
 *   · 消费端（readback）仍走 m41_expect.h::m41_fill_byte —— 二者必须一致，
 *     mkstates_w 启动即交叉校验（不一致立即退出）。
 *
 * 公式（勿改）：FNV-1a 名字折叠 → 与 off 线性混合 → murmur3 finalizer → 取低 8 位。
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdint.h>

static inline uint8_t m42_fill_byte(const char *name, uint64_t off) {
    uint64_t h = 1469598103934665603ULL;              /* FNV-1a offset basis */
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    uint64_t x = h ^ (off * 1099511628211ULL);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;         /* murmur3 finalizer */
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint8_t)x;
}
