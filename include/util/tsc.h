#pragma once
#include <cstdint>
#include <chrono>
#include <cpuid.h>   /* __cpuid（GCC/Clang 用户态） */

/* ═══════════════════════════════════════════════════════════════════
 * tsc.h — x86_64 TSC 时间戳工具（绑死 x86_64，Invariant TSC）
 *
 * 测量逻辑对齐 SparrowOS kernel tsc.cpp ENV_BARE_METAL 分支：
 *   1. CPUID.80000007H:EDX[8] = Invariant TSC（跨核心/频率锁定的保证），
 *      不可靠则整个 TSC 时钟不可用（返回 reliable=false，禁止用 tick 换算）
 *   2. 频率：首选 CPUID.15H（ecx*ebx/eax），缺失则 steady_clock 定时校准
 *   3. 换算：ttl_ticks = freq_hz × 秒数
 *
 * 使用约束：仅 x86_64；调用前先 tsc_probe 确认 reliable，否则换算无意义。
 * ═══════════════════════════════════════════════════════════════════ */

struct tsc_info_t {
    bool     reliable      = false;   /* Invariant TSC 可用（CPUID.80000007H:EDX[8]） */
    uint64_t freq_hz       = 0;       /* TSC 频率（Hz），0 = 未测出 */
    uint64_t fs_per_cycle  = 0;       /* 每周期飞秒（调试用） */
};

static inline uint64_t tsc_rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 探测 Invariant TSC + 测量频率（对齐 kernel tsc.cpp BARE_METAL 分支） */
static inline bool tsc_probe(tsc_info_t *out) {
    if (!out) return false;
    out->reliable = false; out->freq_hz = 0; out->fs_per_cycle = 0;

    /* 1) Invariant TSC：CPUID.80000007H:EDX[8] */
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid(0x80000007, a, b, c, d);
    if (!(d & (1u << 8))) return false;          /* TSC 不可靠：跨核/变频不同步 */
    out->reliable = true;

    /* 2) 频率：CPUID.15H（TSC/Crystal ratio） */
    __cpuid(0x15, a, b, c, d);
    if (a && b && c) {
        out->freq_hz = (uint64_t)c * b / a;      /* ecx × ebx / eax */
    } else {
        /* fallback：steady_clock 校准 ~10ms 窗口 */
        uint64_t s1 = tsc_rdtsc();
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(10)) {}
        uint64_t s2 = tsc_rdtsc();
        out->freq_hz = (s2 - s1) * 100;          /* 10ms → ×100 = Hz */
    }

    out->fs_per_cycle = out->freq_hz
        ? (uint64_t)((1000000000000ull) / out->freq_hz)   /* 1e12 fs / Hz */
        : 0;
    return true;
}

/* 秒 → tick（freq 已知时）；freq=0 或溢出返回 0（调用方视为「无 TTL 时钟」） */
static inline uint64_t tsc_ticks_of_seconds(uint64_t seconds, const tsc_info_t &info) {
    if (!info.reliable || !info.freq_hz || !seconds) return 0;
    __uint128_t ticks = (__uint128_t)info.freq_hz * seconds;
    if (ticks > ~0ull) return 0;                 /* 溢出防御 */
    return (uint64_t)ticks;
}
