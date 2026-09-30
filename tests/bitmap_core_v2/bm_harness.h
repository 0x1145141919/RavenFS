/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bm_harness.h —— 双位图分片分配器 v2 测试「共生工具」
 *
 * 本目录 = 第二测试队产物（tests/bitmap_core_v2/）。只读被测源
 * （include/linux/ravenfs_{platform,bitmap,bitmap_backend,bitmap_core}.h +
 *  linux/{bitmap_core,bitmap_mem,bitmap_chunk}.c），本目录只新增测试夹具。
 *
 * 提供：
 *   · fake_disk_t ：内存字节数组充当盘（块 = 4096B；read/write 回调 + 可注入延迟）；
 *   · oracle_t    ：**独立复刻 MEM 语义**的参照模型（不是抄实现——按 MEM 的
 *                   「内存即一等事实」语义从零写：平坦位 + per-片 hint/free_count +
 *                   per-CPU preferred 路由）。用于 MEM 逐步严格差分，以及 CHUNK 的
 *                   「合成后（提交态）」比对。
 *   · env_t       ：一次装配（MEM 或 CHUNK 后端）的完整环境（设备 + 盘 + 初值）。
 *
 * ★ 语义视角（判定口径，与冻结头 ★★ 一致）：
 *   MEM  逐步可观测 == 真值 → 逐步严格比对。
 *   CHUNK 位态/计数是「在线窗 + 挂账归还链 + 盘上块」在合并点合成的**合成视角**；
 *         逐步只比「分配成败语义 / 无双重分配」，位态/计数须在**合成后（提交态）**
 *         比（先 drain / chunks_asyn / snapshot_slice）。
 */
#ifndef BM_HARNESS_H
#define BM_HARNESS_H

#include "ravenfs_platform.h"
#include "ravenfs_bitmap_core.h"
#include "ravenfs_bitmap_backend.h"

#include <time.h>
#include <sys/resource.h>

#define BM_BLKSZ	4096ULL			/* FS block 字节 */
#define BM_CHUNK_BITS(o) (32768ULL << (o))	/* chunk 位数 */

/* ═══════════════════ fake disk（内存字节数组） ═══════════════════ */
typedef struct {
	u8	*mem;		/* nblk × BM_BLKSZ */
	u64	 nblk;
	int	 delay;		/* 最大随机延迟 us（0 = 关） */
	u64	 jiffies;	/* 延迟序列源 */
	u64	 reads;		/* 原子计数 */
	u64	 writes;
} fake_disk_t;

void fd_init(fake_disk_t *d, u64 nblk, int delay_us);
void fd_free(fake_disk_t *d);
int  fd_read(void *ctx, u64 blk, u64 blk_len, void *buf);	/* soul.read_block ABI */
int  fd_write(void *ctx, u64 blk, u64 blk_len, const void *buf);	/* soul.write_block ABI */

/* ═══════════════════ oracle（独立 MEM 语义参照） ═══════════════════ */
typedef struct {
	u64	 nbits;
	u64	 per_bits;
	u32	 n_slices;
	u32	 cpu_slots;
	u8	*b;		/* 1 字节/位：1 = 占用 */
	u64	*free_count;	/* per 片（增量维护，同 MEM） */
	u64	*hint;		/* per 片（全局位号；只进不退） */
	u32	*preferred;	/* per CPU 槽 */
} oracle_t;

void o_init(oracle_t *o, u64 nbits, u64 per_bits, u32 n_slices, u32 cpu_slots,
	    const u64 *raw_words);
void o_fini(oracle_t *o);

u64  o_alloc_hard(oracle_t *o, u64 count, u64 align_log2);
u64  o_alloc_soft(oracle_t *o, u64 count, u64 *remain);
u64  o_occupy_soft(oracle_t *o, u64 base, u64 count, u64 *rest);
void o_block_free(oracle_t *o, u64 base, u64 count);
bool o_test(const oracle_t *o, u64 bit);
u64  o_free_total(const oracle_t *o);

/* shadow 模式：由「设备实际返回的动作」驱动（CHUNK 多窗差分用） */
void o_apply_alloc(oracle_t *o, u64 pos, u64 count);
void o_apply_free(oracle_t *o, u64 base, u64 count);
u64  o_zeros_total(const oracle_t *o);
u64  o_used_total(const oracle_t *o);

/* ═══════════════════ env（单次装配） ═══════════════════ */
typedef struct {
	struct rfs_bm	  bm;
	fake_disk_t	  disk;
	u8		 *blk_words;	/* MEM 位图内存（也用于序列化 CHUNK 盘上初值） */
	u8		 *ino_words;
	u64		  blk_nbits;
	u64		  ino_nbits;
	u32		  cpu_slots;
	u8		  chunk_order_blk;
	u8		  chunk_order_ino;
	int		  is_chunk;
	int		  keep_disk;	/* 1 = env_go 不再 words→盘序列化（从已有盘重 init 用）*/
} env_t;

/*
 * 两步装配：prep 只铺内存/盘（零初始化，可按需 words_set_range 预置占用），
 * go 才 rfs_bm_init（MEM 扫 words / CHUNK enable 读盘实测）。
 * MEM：blk_blocks/ino_blocks 为位图块数（位 = 块×32768）。
 */
int  env_prep_mem(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots);
int  env_prep_chunk(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots,
		    u8 blk_order, u8 ino_order, int delay);
int  env_go(env_t *e);
void env_destroy(env_t *e);
/* 便捷：prep + go（零初值）。 */
int  env_build_mem(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots);
int  env_build_chunk(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots,
		     u8 blk_order, u8 ino_order, int delay);

/* 设备「提交态」位图（1 字节/位，长度 nbits；经 snapshot_slice 展开）。
 * MEM：内存字直接快照；CHUNK：读盘 + 覆盖在线窗 + 合并归还链（一致视图）。 */
void dev_get_bits(const env_t *e, bool ino, u8 *out);
u64  dev_free_total(const env_t *e, bool ino);

/* 初值：把 ranges 置 1（写进 words；CHUNK 建盘时据此序列化） */
void words_zero(u8 *words, u64 nbits);
void words_set_range(u8 *words, u64 start, u64 len);

/* ═══════════════════ 杂项 ═══════════════════ */
u64  rnd_u64(u64 *state);			/* xorshift64 */
void pin_to_cpu(int cpu);
int  first_allowed_cpu(void);
void pin_to_allowed(int idx);
u64  now_ms(void);

extern int g_checks, g_fail;
#define CHECK(cond, ...) do {						\
	g_checks++;							\
	if (!(cond)) {							\
		g_fail++;						\
		printf("  FAIL @%s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#endif /* BM_HARNESS_H */
