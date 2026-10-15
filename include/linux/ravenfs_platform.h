/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_platform.h —— RavenFS ko 平台适配层（**内核 / libc 双编共用宏**）
 *
 * 定位：ko 里「算法/类」头（如 ravenfs_bitmap_core.h）**不得**出现 `#ifdef __KERNEL__`；
 *       一切平台差异（类型 / 内存 / CPU 身份 / 日志 / 锁）**只在本头收口**，
 *       由它按 __KERNEL__ 选路，对上层只暴露一套统一的宏与类型。
 *
 * 覆盖：
 *   · 基本类型      u8..u64 / bool（内核对 linux/types.h，用户态对 stdint.h）
 *   · 内存          RFS_ZALLOC / RFS_FREE
 *   · CPU 身份      RFS_CPU_ID()
 *   · 日志          RFS_LOG(fmt, ...)
 *   · 锁            rfs_spin_t / rfs_mutex_t / rfs_rwsem_t + RFS_{SPIN,MUTEX,RWSEM}_*
 *   · RAII 守卫     RFS_SPIN_GUARD / RFS_MUTEX_GUARD（cleanup 属性；作用域退出即解锁）
 *
 * RAII 锁守卫（C 语言模拟）：
 *   用 GCC/Clang 的 `__attribute__((cleanup(fn)))`——变量离开作用域时自动调 fn。
 *   与 C++ 的 std::lock_guard 语义等价（**含 return / break / continue / goto 提前退出**），
 *   且内核（gnu11）与用户态（gnu11/gcc、clang）**同一套宏、零分支**。
 *
 *   ⚠️ 用法约束（同内核 scoped_guard）：
 *     1. 守卫宏展开为「声明 + 加锁语句」两条，**必须置于块（`{}`）内**，
 *        不可当单语句 if/for 的裸体：`if (x) RFS_SPIN_GUARD(..);` ✗
 *     2. 同一个块内多个同类守卫会先后解锁（LIFO 顺序，符合 RAII）。
 *     3. 内核侧 spin_lock 可能带 preempt/irq 语义：守卫只做 lock/unlock 配对，
 *        中断/抢占处置仍按内核锁纪律由调用方选择（本层不替它决定）。
 *
 * 纪律：**新增平台差异只扩本头**；不允许在算法/类头里复活 `#ifdef __KERNEL__`。
 */
#ifndef _RAVENFS_PLATFORM_H
#define _RAVENFS_PLATFORM_H

/* ═══════════════════════════ 平台选路 ═══════════════════════════ */
#ifdef __KERNEL__

#  include <linux/types.h>      /* u8..u64 / bool */
#  include <linux/spinlock.h>   /* spinlock_t */
#  include <linux/mutex.h>      /* struct mutex */
#  include <linux/rwsem.h>      /* struct rw_semaphore（rfs_rwsem_t）*/
#  include <linux/string.h>     /* memcpy / memset（算法文件不直接 include 内核头）*/
#  include <linux/slab.h>       /* kcalloc / kfree */
#  include <linux/smp.h>        /* raw_smp_processor_id */
#  include <linux/kernel.h>     /* pr_err */
#  include <linux/minmax.h>     /* min / max */
#  include <linux/math.h>       /* DIV_ROUND_UP_ULL */
#  include <linux/errno.h>      /* -ENOENT / -EIO / -ENOSPC ... */
#  include <linux/bitops.h>     /* hweight64（popcount 不降级为 libgcc __popcountdi2）*/
#  include <linux/timekeeping.h> /* ktime_get_ns（累计耗时类计数器）*/
#  include <linux/delay.h>     /* msleep（打烊排空的时间轮询）*/

#else  /* 用户态：压测 harness / 离线工具 */

/* 需 POSIX/GNU 特性：pthread_spinlock_t、sched_getcpu。
 * 必须位于任何 libc 头之前 —— 本头请放在 TU 最前，或编译期 -D_GNU_SOURCE。*/
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif

#  include <stdint.h>
#  include <stddef.h>
#  include <stdbool.h>
#  include <pthread.h>          /* pthread_{spin,mutex}_*（链接 -lpthread）*/
#  include <sched.h>            /* sched_getcpu */
#  include <stdlib.h>
#  include <stdio.h>
#  include <string.h>
#  include <errno.h>          /* -ENOENT / -EIO / -ENOSPC ... */
#  include <time.h>           /* clock_gettime（累计耗时类计数器）*/

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* 内核算术助手的内核态由 linux/math.h、linux/minmax.h 提供；用户态补等价物 */
#  ifndef DIV_ROUND_UP_ULL
#    define DIV_ROUND_UP_ULL(x, y)  (((u64)(x) + (u64)(y) - 1) / (u64)(y))
#  endif
#  ifndef min
#    define min(a, b)  ((a) < (b) ? (a) : (b))
#  endif
#  ifndef max
#    define max(a, b)  ((a) > (b) ? (a) : (b))
#  endif

#endif /* __KERNEL__ */

/* ═══════════════════════════ 内存 ═══════════════════════════ */
#ifdef __KERNEL__
#  define RFS_ZALLOC(n)   kcalloc(1, (n), GFP_KERNEL)   /* 零初始化数组 */
#  define RFS_FREE(p)     kfree(p)
#else
#  define RFS_ZALLOC(n)   calloc(1, (n))
#  define RFS_FREE(p)     free(p)
#endif

/* 注入式分配器（环境无关；算法/容器/分配器各头共用）——“分配依赖外抛”的唯一落点：
 * 内核绑 kmem_cache / GFP_NOFS，用户态绑 malloc，或绑测试的计数 / 故障注入器。
 * ⚠ 用时纪律：**轮子内零直接分配**（不得出现 kmalloc/malloc 字面），全经此回调。*/
struct rfs_mem_alloc {
	void	*(*alloc)(void *ctx, u64 size);	/* 取 size 字节；失败返 NULL */
	void	 (*free)(void *ctx, void *p);	/* 还（p 由 alloc 取得） */
	void	 *ctx;				/* 宿主上下文（可 NULL） */
};

/* ═══════════════════════════ 时间（单调纳秒） ═══════════════════════════ */
/* 仅供「累计耗时」类计数器用（bm_chunk_switch_ns / bm_scan_ns）；**算法语义不依赖它**
 * （计时只进计数器，绝不参与控制流）。内核走 ktime_get_ns；用户态走 CLOCK_MONOTONIC。*/
#ifdef __KERNEL__
#  define RFS_NOW_NS()   ktime_get_ns()
#else
#  define RFS_NOW_NS()   ({ struct timespec __rfs_ts; \
	clock_gettime(CLOCK_MONOTONIC, &__rfs_ts); \
	(u64)__rfs_ts.tv_sec * 1000000000ULL + (u64)__rfs_ts.tv_nsec; })
#endif

/* ═══════════════════════ 等待 / 自旋（排空用）═══════════════════════
 * 为什么是「时间轮询」而不是 waitqueue/condvar：**环境通用 + 架构通用**。
 *   · waitqueue 是内核专属（栈上对象、调度语义），condvar 是 POSIX 专属；
 *     两者无法用同一份算法代码覆盖内核/用户态/SparrowOS。
 *   · 时间轮询只要求「自旋提示 + 睡 N 毫秒 + 单调时钟」三件事，任何环境/架构都能给，
 *     且**天然无丢失唤醒**（无需处理「入队前最后一个引用已释放」的窗口）。
 * 算法/类文件只用下面三个宏，**不得** #ifdef __KERNEL__；差异只在本头收口。
 *   · RFS_CPU_RELAX()  自旋提示（架构相关，均有通用回退）
 *   · RFS_DELAY_MS(ms) 让出 CPU 睡 ms（进程上下文；毫秒级）
 *   · RFS_NOW_NS()     单调纳秒（已在上文定义）*/
#ifdef __KERNEL__
#  define RFS_CPU_RELAX()   cpu_relax()
#  define RFS_DELAY_MS(ms)  msleep(ms)
#else
#  if defined(__i386__) || defined(__x86_64__)
#    define RFS_CPU_RELAX()  __asm__ __volatile__("pause")
#  else
     /* 通用回退：编译屏障即可（架构无需栈/时钟支持）*/
#    define RFS_CPU_RELAX()  __asm__ __volatile__("" ::: "memory")
#  endif
#  define RFS_DELAY_MS(ms)  do { \
	struct timespec __rfs_d = { (time_t)((ms) / 1000), \
				    (long)(((ms) % 1000) * 1000000L) }; \
	nanosleep(&__rfs_d, NULL); \
} while (0)
#endif

/* ═══════════════════════════ CPU 身份 ═══════════════════════════ */
#ifdef __KERNEL__
#  define RFS_CPU_ID()    raw_smp_processor_id()
#else
#  define RFS_CPU_ID()    ((u32)sched_getcpu())
#endif

/* ═══════════════════════════ 日志 ═══════════════════════════ */
#ifdef __KERNEL__
#  define RFS_LOG(fmt, ...)   pr_err("ravenfs: " fmt, ##__VA_ARGS__)
#else
#  define RFS_LOG(fmt, ...)   fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#endif

/* ═══════════════════════ 位 / 算术助手 ═══════════════════════ */
/* 64 位 popcount：内核用 hweight64，**不得**用 __builtin_popcountll
 * （x86-64 无 -mpopcnt 时编译器降级为 libgcc `__popcountdi2` → modpost undefined）。*/
#ifdef __KERNEL__
#  define RFS_POPCOUNT64(x)   hweight64(x)
#else
#  define RFS_POPCOUNT64(x)   ((u64)__builtin_popcountll((u64)(x)))
#endif

/* 原子标量（relaxed）：用于「可竞争但无害」的共享提示位（如 per-CPU preferred 游标）。
 * 内核/用户态 GCC 均支持 __atomic_* 内建（无需分支）；relaxed 足够——只去 UB/竞争报告，
 * 分配正确性由片锁保证。*/
#define RFS_ATOMIC_LOAD_U32(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define RFS_ATOMIC_STORE_U32(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELAXED)

/* ═══════════════════════════ 锁：类型 + 原语 ═══════════════════════════ */
#ifdef __KERNEL__

typedef spinlock_t     rfs_spin_t;
typedef struct mutex   rfs_mutex_t;
typedef struct rw_semaphore rfs_rwsem_t;

#  define RFS_SPIN_INIT(l)     spin_lock_init(l)
#  define RFS_SPIN_LOCK(l)     spin_lock(l)
#  define RFS_SPIN_UNLOCK(l)   spin_unlock(l)

#  define RFS_MUTEX_INIT(m)    mutex_init(m)
#  define RFS_MUTEX_LOCK(m)    mutex_lock(m)
#  define RFS_MUTEX_UNLOCK(m)  mutex_unlock(m)

#  define RFS_RWSEM_INIT(l)       init_rwsem(l)
#  define RFS_RWSEM_DOWN_READ(l)  down_read(l)
#  define RFS_RWSEM_UP_READ(l)    up_read(l)
#  define RFS_RWSEM_DOWN_WRITE(l) down_write(l)
#  define RFS_RWSEM_UP_WRITE(l)   up_write(l)

static __always_inline void rfs_spin_guard_release(rfs_spin_t **l)
{ if (l && *l) spin_unlock(*l); }
static __always_inline void rfs_mutex_guard_release(rfs_mutex_t **m)
{ if (m && *m) mutex_unlock(*m); }

#else  /* 用户态 */

typedef pthread_spinlock_t  rfs_spin_t;
typedef pthread_mutex_t     rfs_mutex_t;
typedef pthread_rwlock_t    rfs_rwsem_t;

#  define RFS_SPIN_INIT(l)     pthread_spin_init((l), PTHREAD_PROCESS_PRIVATE)
#  define RFS_SPIN_LOCK(l)     pthread_spin_lock(l)
#  define RFS_SPIN_UNLOCK(l)   pthread_spin_unlock(l)

#  define RFS_MUTEX_INIT(m)    pthread_mutex_init((m), NULL)
#  define RFS_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#  define RFS_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)

#  define RFS_RWSEM_INIT(l)       pthread_rwlock_init((l), NULL)
#  define RFS_RWSEM_DOWN_READ(l)  pthread_rwlock_rdlock(l)
#  define RFS_RWSEM_UP_READ(l)    pthread_rwlock_unlock(l)
#  define RFS_RWSEM_DOWN_WRITE(l) pthread_rwlock_wrlock(l)
#  define RFS_RWSEM_UP_WRITE(l)   pthread_rwlock_unlock(l)

static inline void rfs_spin_guard_release(rfs_spin_t **l)
{ if (l && *l) pthread_spin_unlock(*l); }
static inline void rfs_mutex_guard_release(rfs_mutex_t **m)
{ if (m && *m) pthread_mutex_unlock(*m); }

#endif /* __KERNEL__ */

/* ═══════════════════════ RAII 锁守卫（cleanup 属性）═══════════════════════ */

/* 通用：声明「锁指针守卫」变量，离开作用域自动 release。lk 传锁的地址。 */
#define RFS_LOCK_GUARD(release, var, lk)                                  \
	__typeof__(lk) var                                                \
		__attribute__((__cleanup__(release), __unused__)) = (lk)

/* 加锁即守：用后自动解锁（块作用域内使用）*/
#define RFS_SPIN_GUARD(var, lk)                                           \
	RFS_LOCK_GUARD(rfs_spin_guard_release, var, (lk)); RFS_SPIN_LOCK(var)

#define RFS_MUTEX_GUARD(var, lk)                                          \
	RFS_LOCK_GUARD(rfs_mutex_guard_release, var, (lk)); RFS_MUTEX_LOCK(var)

/* 为新锁类型生成守卫（在 include 处展开一次即可；同名重复定义用 include guard 兜）*/
#define RFS_DEFINE_LOCK_GUARD(name, locktype, unlockfn)                   \
	static inline void name##_guard_release(locktype **l)             \
	{ if (l && *l) unlockfn(*l); }

#endif /* _RAVENFS_PLATFORM_H */
