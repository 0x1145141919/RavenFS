#pragma once
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════
 * bitmap_base.hpp — 位图视图轮子（移植自 Sparrow kernel util/bitmap.h）
 *
 * 语义与 kernel 版 bitmap_base 一致（字节内不反转、64bit 单元存储）：
 *   - enable()  绑定外部原始内存（uint64_t* 数组），不持有所有权
 *   - operator[] 读返回 bool，写返回 bit_proxy（按位赋值）
 *   - 越界/空指针 → bitmap_base_boomer 直接页错误（同 kernel 语义）
 *
 * 本移植仅依赖 <stdint.h>（kernel 版经 lock.h→OS_utils.h 拉内核原语，
 * 用户态编译不可用）；位图内存/生命周期归调用方。
 * ═══════════════════════════════════════════════════════════════════ */
class bitmap_base {
protected:
    uint64_t *bitmap = nullptr;   /* bit_index 访问：bitmap[bit_index>>6] 的 (1<<(bit_index&63)) */
    uint64_t  bitcount = 0;
public:
    struct bit_proxy {
        uint64_t *word;
        uint64_t  mask;
        operator bool() const { return (*word & mask) != 0; }
        bit_proxy &operator=(bool v) {
            if (v) *word |= mask; else *word &= ~mask;
            return *this;
        }
    };
    void enable(uint64_t *bmp, uint64_t bits) { bitmap = bmp; bitcount = bits; }
    uint64_t *get_base() { return bitmap; }
    uint64_t  bit_count() const { return bitcount; }
    void disable() { bitmap = nullptr; bitcount = 0; }
    void bitmap_base_boomer(uint64_t idx) const {
        if (__builtin_expect(idx >= bitcount, 0)) *(volatile uint64_t *)1 = 0;   /* 越界 → 页错误 */
        if (__builtin_expect(!bitmap, 0))         *(volatile uint64_t *)0 = 0;   /* 空指针 → 页错误 */
    }
    bit_proxy operator[](uint64_t idx) {
        bitmap_base_boomer(idx);
        return { bitmap + (idx >> 6), 1ULL << (idx & 63) };
    }
    bool operator[](uint64_t idx) const {
        bitmap_base_boomer(idx);
        return (bitmap[idx >> 6] >> (idx & 63)) & 1ULL;
    }
};
