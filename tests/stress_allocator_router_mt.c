// SPDX-License-Identifier: GPL-2.0
/*
 * stress_allocator_router_mt.c —— 顶层路由 struct rfs_allocator 多线程变体（TSan）
 *                                 （测试组，独立于实现）
 *
 * 目的：路由是**并发入口**（片级测试无并发语义），本变体用 N 线程并发 alloc/free
 *   （块组跨片 + inode 组）跑 ThreadSanitizer，暴露路由层的 data race。
 *
 * 关键并发面 = per-CPU preferred 游标：头注释与 ravenfs_platform.h 都把 preferred
 *   描述为「原子 relaxed 提示位」（提供 RFS_ATOMIC_LOAD_U32/STORE_U32），但
 *   ravenfs_allocator_router.c 用**普通读写**更新它。本变体把所有线程钉到同一 CPU
 *   且 cpu_slots 使索引恒定 → 所有线程共用同一 preferred 槽 → TSan 必报该竞争。
 *
 * 构建（必须整条链开 -fsanitize=thread）：
 *   gcc -std=gnu11 -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
 *       -I../include/linux -I../include \
 *       stress_allocator_router_mt.c ../linux/ravenfs_allocator_router.c \
 *       ../linux/ravenfs_allocator_slice.c ../linux/ravenfs_rbtree_intrusive.c \
 *       -lpthread -o rfs_router_mt_tsan
 *
 * 用法： ./rfs_router_mt_tsan [threads] [iters]；退出码：0 逻辑通过 / 2 泄露 / 1 逻辑错误。
 */
#include "ravenfs_allocator_top.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MSB RFS_IV_SLICE_MIN_BITS

static struct rfs_allocator g_a;

/* ── 线程安全计数池（原子）── */
struct apool { u64 allocs, frees, live; };
static struct apool g_pool;

static void *apool_alloc(void *ctx, u64 size)
{
	void *m = calloc(1, (size_t)size);

	(void)ctx;
	if (m) {
		__atomic_add_fetch(&g_pool.allocs, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&g_pool.live, 1, __ATOMIC_RELAXED);
	}
	return m;
}

static void apool_free(void *ctx, void *m)
{
	(void)ctx;
	if (m) {
		__atomic_add_fetch(&g_pool.frees, 1, __ATOMIC_RELAXED);
		__atomic_sub_fetch(&g_pool.live, 1, __ATOMIC_RELAXED);
		free(m);
	}
}

static struct rfs_mem_alloc g_mem = { apool_alloc, apool_free, NULL };
static struct rfs_run_alloc g_ra = { apool_alloc, apool_free, NULL };

/* ── 影子位图（mutex 保护；仅测试侧，不与路由调用同一临界区）── */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static u8 *g_bblk, *g_bino;
static u64 g_bn, g_in;
static u64 g_b_free, g_i_free;
static long g_dup;

static int bit_get(const u8 *b, u64 i)
{
	return (b[i >> 3] >> (i & 7)) & 1u;
}

static void bit_set(u8 *b, u64 i)
{
	b[i >> 3] |= (u8)(1u << (i & 7));
}

static void bit_clr(u8 *b, u64 i)
{
	b[i >> 3] &= (u8)~(1u << (i & 7));
}

/* 认领 [base,base+len)（块组）：任一已占 → 重复交付。返回 0 成功。*/
static int claim_blk(u64 base, u64 len)
{
	u64 k;
	int ok = 1;

	pthread_mutex_lock(&g_lock);
	for (k = 0; k < len; k++) {
		if (base + k >= g_bn || bit_get(g_bblk, base + k)) {
			ok = 0;
			if (base + k < g_bn) {
				__atomic_add_fetch(&g_dup, 1, __ATOMIC_RELAXED);
				printf("  DUP block %llu (thread raced)\n",
				       (unsigned long long)(base + k));
			}
			break;
		}
	}
	if (ok) {
		for (k = 0; k < len; k++)
			bit_set(g_bblk, base + k);
		g_b_free -= len;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

static void release_blk(u64 base, u64 len)
{
	u64 k;

	pthread_mutex_lock(&g_lock);
	for (k = 0; k < len; k++) {
		if (base + k < g_bn && bit_get(g_bblk, base + k)) {
			bit_clr(g_bblk, base + k);
			g_b_free++;
		}
	}
	pthread_mutex_unlock(&g_lock);
}

static int claim_ino(u64 inum)
{
	int ok;

	pthread_mutex_lock(&g_lock);
	ok = (inum < g_in && !bit_get(g_bino, inum));
	if (ok) {
		bit_set(g_bino, inum);
		g_i_free--;
	} else {
		__atomic_add_fetch(&g_dup, 1, __ATOMIC_RELAXED);
		printf("  DUP inode %llu\n", (unsigned long long)inum);
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

static void release_ino(u64 inum)
{
	pthread_mutex_lock(&g_lock);
	if (inum < g_in && bit_get(g_bino, inum)) {
		bit_clr(g_bino, inum);
		g_i_free++;
	}
	pthread_mutex_unlock(&g_lock);
}

/* ── 线程 ── */
#define MAXLOC 65536

static u32 g_pin;

static void pin_to(u32 cpu)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET((int)cpu, &s);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}

struct targ {
	int	 id;
	long	 iters;
};

static void *worker(void *p)
{
	struct targ *t = (struct targ *)p;
	u64 seed = 0x1234567ULL * (u64)(t->id + 1) + 0xC0FFEEULL;
	u64 *oblk = (u64 *)malloc((size_t)MAXLOC * 2 * sizeof(u64));	/* base,len 交错 */
	int nb = 0;
	u64 *oino = (u64 *)malloc((size_t)MAXLOC * sizeof(u64));
	int ni = 0;
	long it;

	pin_to(g_pin);
	for (it = 0; it < t->iters; it++) {
		u64 r;
		long m;

		seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
		r = seed;
		m = (long)(r % 100);

		if (m < 45) {
			u64 out = rfs_allocator_block_alloc_hard(&g_a, 1 + (r >> 8) % 48,
								 (u8)(r % 6));
			if (out != RFS_IV_NO_HIT) {
				u64 len = 1 + (r >> 8) % 48;

				if (claim_blk(out, len)) {
					if (nb < MAXLOC) {
						oblk[nb * 2] = out;
						oblk[nb * 2 + 1] = len;
						nb++;
					} else {
						release_blk(out, len);
						rfs_allocator_block_free(&g_a, out, len);
					}
				}
			}
		} else if (m < 60) {
			struct rfs_run_list rl;
			const struct rfs_run_node *nd;

			rfs_run_list_init(&rl, g_ra);
			if (rfs_allocator_block_alloc_soft(&g_a, 1 + (r >> 8) % 64,
							   (u8)(r % 4), &rl) == 0) {
				for (nd = rl.head; nd; nd = nd->next) {
					if (claim_blk(nd->run.base, nd->run.len)) {
						if (nb < MAXLOC) {
							oblk[nb * 2] = nd->run.base;
							oblk[nb * 2 + 1] = nd->run.len;
							nb++;
						} else {
							release_blk(nd->run.base, nd->run.len);
							rfs_allocator_block_free(&g_a, nd->run.base, nd->run.len);
						}
					}
				}
			}
			rfs_run_list_fini(&rl);
		} else if (m < 70) {
			u64 in = 0;

			if (rfs_allocator_inode_alloc(&g_a, &in) == 0) {
				if (claim_ino(in)) {
					if (ni < MAXLOC)
						oino[ni++] = in;
					else {
						release_ino(in);
						rfs_allocator_inode_free(&g_a, in);
					}
				}
			}
		} else if (m < 90) {
			if (nb > 0) {
				int i = (int)(r % (u64)nb);
				u64 base = oblk[i * 2], len = oblk[i * 2 + 1];

				release_blk(base, len);
				rfs_allocator_block_free(&g_a, base, len);
				oblk[i * 2] = oblk[(nb - 1) * 2];
				oblk[i * 2 + 1] = oblk[(nb - 1) * 2 + 1];
				nb--;
			}
		} else if (m < 98) {
			if (ni > 0) {
				int i = (int)(r % (u64)ni);
				u64 in = oino[i];

				release_ino(in);
				rfs_allocator_inode_free(&g_a, in);
				oino[i] = oino[ni - 1];
				ni--;
			}
		} else {
			/* 只读探针（路由内部持片锁，线程安全）*/
			(void)rfs_allocator_block_test(&g_a, r % g_bn);
			(void)rfs_allocator_inode_test(&g_a, r % g_in);
		}
	}

	/* 归还本线程全部持有 */
	while (nb > 0) {
		nb--;
		release_blk(oblk[nb * 2], oblk[nb * 2 + 1]);
		rfs_allocator_block_free(&g_a, oblk[nb * 2], oblk[nb * 2 + 1]);
	}
	while (ni > 0) {
		ni--;
		release_ino(oino[ni]);
		rfs_allocator_inode_free(&g_a, oino[ni]);
	}
	free(oblk);
	free(oino);
	return NULL;
}

static u32 pin_first_cpu(void)
{
	cpu_set_t set;
	int cpu = -1, i;

	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) != 0)
		return 0;
	for (i = 0; i < CPU_SETSIZE; i++)
		if (CPU_ISSET(i, &set)) {
			cpu = i;
			break;
		}
	return (cpu < 0) ? 0 : (u32)cpu;
}

int main(int argc, char **argv)
{
	int nthr = 8;
	long iters = 20000;
	u64 bn = MSB * 4, in = MSB * 4;
	struct rfs_slices_cfg cfg;
	pthread_t th[64];
	struct targ ta[64];
	int i;
	int bad = 0;

	if (argc > 1)
		nthr = atoi(argv[1]);
	if (argc > 2)
		iters = atol(argv[2]);
	if (nthr < 1)
		nthr = 1;
	if (nthr > 64)
		nthr = 64;

	g_bn = bn;
	g_in = in;
	g_bblk = (u8 *)calloc(1, (size_t)((bn + 7) / 8));	/* 全空闲 */
	g_bino = (u8 *)calloc(1, (size_t)((in + 7) / 8));
	g_bino[0] |= 1u;					/* inode 0 = root 占用 */
	g_b_free = bn;
	g_i_free = in - 1;

	{
		u8 *rb = NULL, *ri = NULL;
		size_t bb = (size_t)((bn + 7) / 8), bi = (size_t)((in + 7) / 8);

		if (posix_memalign((void **)&rb, 4096, bb) != 0 ||
		    posix_memalign((void **)&ri, 4096, bi) != 0) {
			printf("FATAL posix_memalign\n");
			return 2;
		}
		memset(rb, 0, bb);
		memset(ri, 0, bi);
		ri[0] |= 1u;
		memset(&cfg, 0, sizeof(cfg));
		cfg.block_bits_raw = rb;
		cfg.block_bits_count = bn;
		cfg.inode_bits_raw = ri;
		cfg.inode_bits_count = in;
		cfg.logical_processor_count = 8;	/* cpu_slots=8；全线程同钉 → 同槽 */
		cfg.alloc = g_mem;
		if (rfs_allocator_init(&g_a, &cfg) != 0) {
			printf("FATAL init\n");
			return 2;
		}
		free(rb);
		free(ri);
	}

	g_pin = pin_first_cpu();
	printf("=== ravenfs_allocator_router MT (TSan) threads=%d iters=%ld pin_cpu=%u ===\n",
	       nthr, iters, g_pin);

	for (i = 0; i < nthr; i++) {
		ta[i].id = i;
		ta[i].iters = iters;
		if (pthread_create(&th[i], NULL, worker, &ta[i]) != 0) {
			printf("FATAL pthread_create\n");
			return 2;
		}
	}
	for (i = 0; i < nthr; i++)
		pthread_join(th[i], NULL);

	/* 单线程终检 */
	printf("--- dup=%ld block_free_total=%llu ledger=%llu | inode_free_total=%llu ledger=%llu\n",
	       __atomic_load_n(&g_dup, __ATOMIC_RELAXED),
	       (unsigned long long)rfs_allocator_block_free_total(&g_a),
	       (unsigned long long)g_b_free,
	       (unsigned long long)rfs_allocator_inode_free_total(&g_a),
	       (unsigned long long)g_i_free);
	if (__atomic_load_n(&g_dup, __ATOMIC_RELAXED) != 0) {
		printf("FAIL: duplicate delivery detected\n");
		bad = 1;
	}
	if (rfs_allocator_block_free_total(&g_a) != g_b_free) {
		printf("FAIL: block free total != ledger\n");
		bad = 1;
	}
	if (rfs_allocator_inode_free_total(&g_a) != g_i_free) {
		printf("FAIL: inode free total != ledger\n");
		bad = 1;
	}
	if (g_b_free != bn) {
		printf("FAIL: block not fully drained (%llu/%llu)\n",
		       (unsigned long long)g_b_free, (unsigned long long)bn);
		bad = 1;
	}

	rfs_allocator_fini(&g_a);
	printf("--- pool: allocs=%llu frees=%llu live=%llu\n",
	       (unsigned long long)g_pool.allocs, (unsigned long long)g_pool.frees,
	       (unsigned long long)g_pool.live);
	if (g_pool.live != 0 || g_pool.allocs != g_pool.frees) {
		printf("FAIL: pool imbalance\n");
		bad = 1;
	}
	free(g_bblk);
	free(g_bino);
	printf("=== %s ===\n", bad ? "LOGIC FAIL" : "logic OK (race report, if any, is above)");
	return bad ? 1 : 0;
}
