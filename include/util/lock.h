#pragma once

#include <cstdint>
#include <atomic>

/* ═══════════════════════════════════════════════════════════════
 * 用户态自旋锁 / 读写锁 / TryLock
 *
 * 移除了内核版的中断关闭/恢复功能 (cli/sti) 和
 * reentrant_spinlock (依赖 fast_get_processor_id)。
 *
 * 底层锁定走 __atomic 内置函数，纯用户态无 syscall。
 * cpu_relax() = x86 pause 指令，降低 HT 争用功耗。
 * ═══════════════════════════════════════════════════════════════ */

/* ─── cpu_relax ─── */
static inline void cpu_relax() {
    __asm__ __volatile__("pause" ::: "memory");
}

/* ─── rdtsc_timestamp — TSC 时间戳 ─── */
static inline uint64_t rdtsc_timestamp() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ─── 常量 ─── */
static constexpr uint8_t SPIN_LOCKED   = 1;
static constexpr uint8_t SPIN_UNLOCKED = 0;

/* ═══════════════════════════════════════════════════════════════
 * spinlock_cpp_t — 基本自旋锁
 *
 * TAS (test-and-set) + paused-load 等待，非公平。
 * lock()   → 阻塞式获取
 * unlock() → 释放
 * is_locked() → 检查状态 (不保证准确)
 * ═══════════════════════════════════════════════════════════════ */
class spinlock_cpp_t {
    uint8_t status = SPIN_UNLOCKED;
public:
    spinlock_cpp_t() = default;

    void lock() {
        while (__atomic_test_and_set(&status, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&status, __ATOMIC_RELAXED) == SPIN_LOCKED) {
                cpu_relax();
            }
        }
    }

    bool is_locked() {
        return __atomic_load_n(&status, __ATOMIC_RELAXED) == SPIN_LOCKED;
    }

    void unlock() {
        __atomic_clear(&status, __ATOMIC_RELEASE);
    }
};

/* ═══════════════════════════════════════════════════════════════
 * spinlock_guard — spinlock RAII guard
 * ═══════════════════════════════════════════════════════════════ */
class spinlock_guard {
    spinlock_cpp_t &lock_ref;
public:
    explicit spinlock_guard(spinlock_cpp_t &lock) : lock_ref(lock) {
        lock_ref.lock();
    }
    ~spinlock_guard() { lock_ref.unlock(); }
    spinlock_guard(const spinlock_guard &) = delete;
    spinlock_guard &operator=(const spinlock_guard &) = delete;
};

/* ═══════════════════════════════════════════════════════════════
 * spinrwlock_cpp_t — 读写锁 (写优先)
 *
 * 基于双 spinlock 的经典实现：
 *   writelock 控制写者独占
 *   readlock 保护 readers 计数器
 *
 * 读锁：readers 计数，第一个读者取 writelock，最后一个释放
 * 写锁：直接取 writelock
 * ═══════════════════════════════════════════════════════════════ */
class spinrwlock_cpp_t {
    spinlock_cpp_t readlock;
    spinlock_cpp_t writelock;
    uint32_t       readers = 0;
public:
    spinrwlock_cpp_t() = default;

    void read_lock() {
        readlock.lock();
        readers++;
        if (readers == 1)
            writelock.lock();
        readlock.unlock();
    }

    void read_unlock() {
        readlock.lock();
        readers--;
        if (readers == 0)
            writelock.unlock();
        readlock.unlock();
    }

    void write_lock() {
        writelock.lock();
    }

    void write_unlock() {
        writelock.unlock();
    }
};

/* ─── spinrwlock guard ─── */
class spinrwlock_read_guard {
    spinrwlock_cpp_t &lock_ref;
public:
    explicit spinrwlock_read_guard(spinrwlock_cpp_t &lock) : lock_ref(lock) {
        lock_ref.read_lock();
    }
    ~spinrwlock_read_guard() { lock_ref.read_unlock(); }
    spinrwlock_read_guard(const spinrwlock_read_guard &) = delete;
    spinrwlock_read_guard &operator=(const spinrwlock_read_guard &) = delete;
};

class spinrwlock_write_guard {
    spinrwlock_cpp_t &lock_ref;
public:
    explicit spinrwlock_write_guard(spinrwlock_cpp_t &lock) : lock_ref(lock) {
        lock_ref.write_lock();
    }
    ~spinrwlock_write_guard() { lock_ref.write_unlock(); }
    spinrwlock_write_guard(const spinrwlock_write_guard &) = delete;
    spinrwlock_write_guard &operator=(const spinrwlock_write_guard &) = delete;
};

/* ═══════════════════════════════════════════════════════════════
 * trylock_cpp_t — 纯 trylock (不自旋)
 *
 * 适合外部已经有保护判断的场景。
 * try_lock() 返回 true/false，不自旋等待。
 * ═══════════════════════════════════════════════════════════════ */
class trylock_cpp_t {
    uint8_t status = SPIN_UNLOCKED;
public:
    trylock_cpp_t() = default;

    bool try_lock() {
        return !__atomic_test_and_set(&status, __ATOMIC_ACQUIRE);
    }

    void unlock() {
        __atomic_clear(&status, __ATOMIC_RELEASE);
    }
};

/* ═══════════════════════════════════════════════════════════════
 * spintrylock_cpp_t — 自旋锁 + try_lock 二合一
 *
 * lock()        → 阻塞式，自旋等待
 * try_lock()    → 非阻塞，获取失败立即返回 false
 * unlock()      → 释放
 * ═══════════════════════════════════════════════════════════════ */
class spintrylock_cpp_t {
    uint8_t status = SPIN_UNLOCKED;
public:
    spintrylock_cpp_t() = default;

    void lock() {
        while (__atomic_test_and_set(&status, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&status, __ATOMIC_RELAXED) == SPIN_LOCKED) {
                cpu_relax();
            }
        }
    }

    bool try_lock() {
        return !__atomic_test_and_set(&status, __ATOMIC_ACQUIRE);
    }

    void unlock() {
        __atomic_clear(&status, __ATOMIC_RELEASE);
    }
};

/* ─── spintrylock guards ─── */
class spintrylock_spin_guard {
    spintrylock_cpp_t &lock_ref;
public:
    explicit spintrylock_spin_guard(spintrylock_cpp_t &lock)
        : lock_ref(lock) { lock_ref.lock(); }
    ~spintrylock_spin_guard() { lock_ref.unlock(); }
    spintrylock_spin_guard(const spintrylock_spin_guard &) = delete;
    spintrylock_spin_guard &operator=(const spintrylock_spin_guard &) = delete;
};

class spintrylock_try_guard {
    spintrylock_cpp_t *lock_ref = nullptr;
public:
    explicit spintrylock_try_guard(spintrylock_cpp_t *lock) {
        if (lock && lock->try_lock())
            lock_ref = lock;
    }
    bool is_locked() const { return lock_ref != nullptr; }
    ~spintrylock_try_guard() {
        if (lock_ref)
            lock_ref->unlock();
    }
    spintrylock_try_guard(const spintrylock_try_guard &) = delete;
    spintrylock_try_guard &operator=(const spintrylock_try_guard &) = delete;
};

/* ═══════════════════════════════════════════════════════════════
 * rw_try_spinlock_t — 读写尝试自旋锁（读共享 / 写独占，全 try 语义）
 *
 * 拿不到立即返回 false，不阻塞自旋——调用方拿不到锁时自行决定重试或
 * 换片（无公平性保证：try 语义下饥饿由调用方路由策略承担）。
 * 用户态实现用 std::atomic；sparrowos 内核端移植时替换为 arch 原子原语
 * （xchg/cmpxchg），接口语义不变。
 * ═══════════════════════════════════════════════════════════════ */
class rw_try_spinlock_t {
public:
    rw_try_spinlock_t() : state_(0) {}
    rw_try_spinlock_t(const rw_try_spinlock_t &) = delete;
    rw_try_spinlock_t &operator=(const rw_try_spinlock_t &) = delete;

    bool try_lock_read() {
        uint32_t s = state_.load(std::memory_order_relaxed);
        do {
            if (s & WRITE_BIT) return false;        /* 写者持有 → 放弃 */
        } while (!state_.compare_exchange_weak(s, s + 1,
                     std::memory_order_acquire, std::memory_order_relaxed));
        return true;                                /* 读者计数 +1 */
    }
    void unlock_read() {
        state_.fetch_sub(1, std::memory_order_release);
    }
    bool try_lock_write() {
        uint32_t expect = 0;
        return state_.compare_exchange_strong(expect, WRITE_BIT,
                     std::memory_order_acquire, std::memory_order_relaxed);
    }
    void unlock_write() {
        state_.store(0, std::memory_order_release);
    }
private:
    static constexpr uint32_t WRITE_BIT = 0x80000000u;  /* 高 1 位=写锁，低 31 位=读者计数 */
    std::atomic<uint32_t> state_;
};
