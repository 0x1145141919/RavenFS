#pragma once

#include <cstdio>
#include <cstring>

/* ═══════════════════════════════════════════════════════════════
 * 共享测试计数器 + CHECK 宏
 *
 * 用法:
 *   #include "test_common.hpp"
 *   ... test function ...
 *   // main() 中最后输出 print_summary()
 * ═══════════════════════════════════════════════════════════════ */

extern int g_tests;
extern int g_failures;

#define CHECK(cond, fmt, ...) do {                                      \
    g_tests++;                                                          \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL [%d] " fmt "\n",                       \
                g_tests, ##__VA_ARGS__);                                \
        g_failures++;                                                   \
    } else {                                                            \
        fprintf(stderr, "  PASS [%d] " fmt "\n",                       \
                g_tests, ##__VA_ARGS__);                                \
    }                                                                   \
} while(0)

/* 成功: 返回 0 */
#define CHECK_OK(expr, fmt, ...) \
    CHECK((expr) == 0, fmt " (rc=0)", ##__VA_ARGS__)

/* 失败: 返回指定 -errno */
#define CHECK_ERR(expr, expected_err, fmt, ...) do {                    \
    int __rc = (expr);                                                  \
    CHECK(__rc == (expected_err),                                       \
          fmt " → got %d, want %d", ##__VA_ARGS__,                      \
          __rc, (expected_err));                                        \
} while(0)

void print_separator(const char *title);
void print_summary();
