// SPDX-License-Identifier: GPL-2.0
/*
 * bm_test.c —— tests/bitmap_core_v2 主测（C，glibc；MEM + CHUNK 双后端）
 *
 * 判定口径（★ 硬前提，见 bm_harness.h 与冻结头 ravenfs_bitmap_core.h ★★）：
 *   · MEM 后端逐步可观测 == 真值 ⇒ 逐步严格比对（位态 / 位置 / 缺口 / 计数）。
 *   · CHUNK 后端位态/计数是「在线窗 + 挂账归还链 + 盘上块」合成的**合成视角**：
 *       逐步只比「分配成败语义 / 无双重分配」；位态 / 计数须在**合成后（提交态）**
 *       比（chunks_asyn / snapshot_slice）。挂账期漂移按**文档化差异**显式记录。
 *
 * 子命令： quick | full | tsan | san   （选择跑哪些套件；tsan/san 走并发/内存面）。
 */
#include "bm_harness.h"
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

static int g_full;			/* full：加大轮次 */
static int g_light;			/* tsan/san：收小轮次（sanitizer 开销大）*/
static int g_verbose;

/* ═══════════════════════════ 小工具 ═══════════════════════════ */

static int bits_diff(const u8 *a, const u8 *b, u64 n, const char *tag, int maxprint)
{
	u64 i, bad = 0;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			if ((int)bad < maxprint)
				printf("    [%s] bit %llu dev=%u ref=%u\n", tag,
				       (unsigned long long)i, a[i], b[i]);
			bad++;
		}
	}
	return (int)bad;
}

static const struct rfs_bm_ops *dev_ops(u8 mode)
{
	return rfs_bm_ops_of(mode);
}

/* 片内 per-片 free_count（经后端 ops；公开 API 只给总和） */
static u64 dev_slice_free(const env_t *e, bool ino, u32 i)
{
	const struct rfs_bm_group *g = ino ? &e->bm.ino : &e->bm.blk;

	return dev_ops(e->bm.mode)->free_count(g, i);
}

static void on_alarm(int s)
{
	(void)s;
	printf("\n[TIMEOUT] alarm fired —— 疑似死锁/挂起\n");
	fflush(stdout);
	_exit(99);
}

/* ═══════════════════════════ 几何套件 ═══════════════════════════ */

static void suite_geometry(void)
{
	static const struct { u64 bb, ib; u32 cpu; } cfgs[] = {
		{ 8, 8, 1 }, { 10, 10, 4 }, { 4, 7, 2 }, { 16, 16, 8 }, { 1, 1, 1 },
	};
	unsigned c;

	printf("[suite] geometry\n");
	for (c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
		env_t e;
		u32 i, sc;
		int rc = env_build_mem(&e, cfgs[c].bb, cfgs[c].ib, cfgs[c].cpu);
		u64 covered = 0;

		CHECK(rc == 0, "mem init rc=%d (bb=%llu)", rc,
		      (unsigned long long)cfgs[c].bb);
		if (rc)
			continue;
		sc = rfs_bm_slice_count(&e.bm);
		CHECK(sc == e.bm.blk.n_slices && sc == e.bm.ino.n_slices,
		      "slice_count 一致 sc=%u blk=%u ino=%u", sc,
		      e.bm.blk.n_slices, e.bm.ino.n_slices);
		CHECK(sc >= 1 && (sc & (sc - 1)) == 0, "slice_count 为 2^n：%u", sc);
		CHECK(e.bm.blk.per_bits % RFS_BM_SLICE_MIN_BITS == 0,
		      "per_bits 32768 对齐：%llu", (unsigned long long)e.bm.blk.per_bits);
		for (i = 0; i < sc; i++) {
			u64 base = (u64)i * e.bm.blk.per_bits;
			u64 bits = base >= e.bm.blk.nbits ? 0
				 : min(e.bm.blk.per_bits, e.bm.blk.nbits - base);

			CHECK(bits > 0 || i == 0, "片 %u 非空", i);
			covered += bits;
		}
		CHECK(covered == e.bm.blk.nbits, "片覆盖 == nbits（%llu vs %llu）",
		      (unsigned long long)covered, (unsigned long long)e.bm.blk.nbits);
		CHECK((u64)sc * e.bm.blk.per_bits >= e.bm.blk.nbits,
		      "末片非空：n*per >= nbits");
		CHECK(dev_free_total(&e, false) == e.bm.blk.nbits,
		      "初始 block_free_total == nbits");
		CHECK(dev_free_total(&e, true) == e.bm.ino.nbits,
		      "初始 inode_free_total == nbits");
		CHECK(rfs_bm_chunks_asyn(&e.bm) == -ENOSYS, "MEM chunks_asyn → -ENOSYS");
		env_destroy(&e);
	}
	/* CHUNK 几何（cpu_slots=1 时 per_bits=nbits ⇒ per_blocks 可预期） */
	{
		env_t e;
		int rc = env_build_chunk(&e, 8, 8, 1, 3, 3, 0);

		CHECK(rc == 0, "chunk init rc=%d", rc);
		if (!rc) {
			CHECK(rfs_bm_slice_count(&e.bm) == 1, "chunk sc=1");
			CHECK(dev_free_total(&e, false) == e.bm.blk.nbits, "chunk 初始全空闲");
			CHECK(rfs_bm_chunks_asyn(&e.bm) == 0, "chunk chunks_asyn → 0");
			env_destroy(&e);
		}
	}
}

/* ═══════════════════════════ 边界套件 ═══════════════════════════ */

static void check_align_pos(u64 pos, u64 align_log2, const char *tag)
{
	u64 a = 1ULL << (align_log2 > 63 ? 63 : align_log2);

	CHECK(pos % a == 0, "%s: pos=%llu 未 %llu 对齐", tag,
	      (unsigned long long)pos, (unsigned long long)a);
}

static void suite_boundary(void)
{
	printf("[suite] boundary\n");
	/* MEM：单组 8 块（262144 位），单片 */
	{
		env_t e;
		int rc = env_build_mem(&e, 8, 8, 1);
		u64 nbits = e.blk_nbits;

		CHECK(rc == 0, "mem init");
		if (!rc) {
			CHECK(rfs_bm_block_alloc_hard(&e.bm, 0, 0) == RFS_BM_NO_HIT,
			      "count=0 → NO_HIT");
			CHECK(rfs_bm_block_alloc_hard(&e.bm, nbits + 1, 0) == RFS_BM_NO_HIT,
			      "count>total → NO_HIT");
			CHECK(rfs_bm_block_alloc_soft(&e.bm, 0, NULL) == RFS_BM_NO_HIT,
			      "soft count=0 → NO_HIT");
			/* 整片一次分配 */
			{
				u64 p = rfs_bm_block_alloc_hard(&e.bm, nbits, 0);

				CHECK(p == 0, "整片分配 pos=0（得 %llu）",
				      (unsigned long long)p);
				CHECK(dev_free_total(&e, false) == 0, "满载 free_total=0");
				CHECK(rfs_bm_block_alloc_hard(&e.bm, 1, 0) == RFS_BM_NO_HIT,
				      "满载再分配 → NO_HIT（ENOSPC）");
				rfs_bm_block_free(&e.bm, 0, nbits);
				CHECK(dev_free_total(&e, false) == nbits, "整片释放恢复");
			}
			/* align 0..15 合规；>15（MEM 夹断到 16） */
			{
				u64 al;

				for (al = 0; al <= 15; al++) {
					u64 p = rfs_bm_block_alloc_hard(&e.bm, 1, al);

					CHECK(p != RFS_BM_NO_HIT, "align=%llu 分配成功",
					      (unsigned long long)al);
					if (p != RFS_BM_NO_HIT) {
						check_align_pos(p, al, "align0..15");
						rfs_bm_block_free(&e.bm, p, 1);
					}
				}
				{
					u64 p = rfs_bm_block_alloc_hard(&e.bm, 1, 20);

					CHECK(p != RFS_BM_NO_HIT, "align=20（MEM 夹断 16）");
					if (p != RFS_BM_NO_HIT) {
						check_align_pos(p, 16, "align夹断");
						rfs_bm_block_free(&e.bm, p, 1);
					}
				}
			}
			/* 区间测试语义 */
			CHECK(rfs_bm_block_test_interval(&e.bm, 0, 100, false) == 0,
			      "test_interval 全空闲 val=0");
			CHECK(rfs_bm_block_test_interval(&e.bm, 0, 100, true) == -ENOENT,
			      "test_interval val=1 异位 → -ENOENT");
			CHECK(rfs_bm_block_test_interval(&e.bm, nbits, 1, false) == -EINVAL,
			      "越界 → -EINVAL");
			CHECK(rfs_bm_block_test_interval(&e.bm, ~0ULL, 2, false) == -EINVAL,
			      "溢出 → -EINVAL");
			/* 释放不改变计数（未释放） */
			rfs_bm_block_occupy_soft(&e.bm, 10, 5, NULL);
			CHECK(rfs_bm_block_test(&e.bm, 12), "occupy 后 bit12 占用");
			rfs_bm_block_free(&e.bm, 10, 5);
			CHECK(!rfs_bm_block_test(&e.bm, 12), "free 后 bit12 空闲");
			env_destroy(&e);
		}
	}
	/* CHUNK：单窗语义（> 单窗 → NO_HIT 即便整片够）；align>15 拒 */
	{
		env_t e;
		int rc = env_build_chunk(&e, 8, 8, 1, 0, 0, 0);	/* 8 窗，窗 = 32768 位 */
		u64 cw = BM_CHUNK_BITS(0);

		CHECK(rc == 0, "chunk init");
		if (!rc) {
			CHECK(rfs_bm_block_alloc_hard(&e.bm, cw + 1, 0) == RFS_BM_NO_HIT,
			      "count>单窗 → NO_HIT（单窗语义）");
			CHECK(rfs_bm_block_alloc_hard(&e.bm, cw, 0) == 0,
			      "count==单窗 → pos=0");
			{
				u64 p;

				rfs_bm_block_free(&e.bm, 0, cw);
				p = rfs_bm_block_alloc_hard(&e.bm, 1, 20);
				CHECK(p == RFS_BM_NO_HIT, "chunk align=20 → NO_HIT（拒）");
			}
			/* 单窗内对齐 */
			{
				u64 al;

				for (al = 0; al <= 15; al += 5) {
					u64 p = rfs_bm_block_alloc_hard(&e.bm, 1, al);

					CHECK(p != RFS_BM_NO_HIT, "chunk align=%llu",
					      (unsigned long long)al);
					if (p != RFS_BM_NO_HIT) {
						check_align_pos(p, al, "chunk align");
						rfs_bm_block_free(&e.bm, p, 1);
					}
				}
			}
			env_destroy(&e);
		}
	}
	/* 末片短片 + 片间空洞（MEM 多片） */
	{
		env_t e;
		int rc = env_build_mem(&e, 10, 10, 4);

		CHECK(rc == 0, "mem 10块 4cpu init");
		if (!rc) {
			u32 sc = rfs_bm_slice_count(&e.bm);
			u64 lb = (u64)(sc - 1) * e.bm.blk.per_bits;
			u64 lbits = e.bm.blk.nbits - lb;

			CHECK(lbits > 0 && lbits <= e.bm.blk.per_bits,
			      "末片位数合法：%llu", (unsigned long long)lbits);
			/* 末片可整片分配（片内）；> 末片 → 走别片或 NO_HIT */
			{
				u64 p = rfs_bm_block_alloc_hard(&e.bm, lbits, 0);

				CHECK(p != RFS_BM_NO_HIT, "末片整片分配成功");
			}
			/* 片边界续圈 occupy：从 slice1 末尾起跨边界 */
			env_destroy(&e);
		}
	}
}

/* ═══════════════════════════ 不变量套件 ═══════════════════════════ */

static void suite_invariants(void)
{
	u32 mode;

	printf("[suite] invariants\n");
	for (mode = 0; mode < 2; mode++) {
		env_t e;
		int rc = mode == 0 ? env_build_mem(&e, 8, 8, 1)
				   : env_build_chunk(&e, 8, 8, 1, 0, 0, 0);
		u64 nbits = e.blk_nbits, p;
		int i;

		CHECK(rc == 0, "init mode=%u", mode);
		if (rc)
			continue;
		/* 无双重分配 + 释放守恒 */
		{
			u64 pos[64];
			u64 cnt[64];
			int n = 0;
			u64 used_before = nbits - dev_free_total(&e, false);

			for (i = 0; i < 40; i++) {
				u64 want = 1 + (u64)(i % 7);
				p = rfs_bm_block_alloc_hard(&e.bm, want, i % 4);

				if (p == RFS_BM_NO_HIT)
					continue;
				CHECK(p + want <= nbits, "分配不越界");
				if (mode == 1) {
					/* 提交态可校验：先合并再查区间 */
					rfs_bm_chunks_asyn(&e.bm);
				}
				CHECK(rfs_bm_block_test_interval(&e.bm, p, want, true) == 0,
				      "mode%u 分配区间全占用", mode);
				pos[n] = p;
				cnt[n] = want;
				n++;
			}
			CHECK(n > 0, "至少一次分配");
			(void)used_before;
			for (i = 0; i < n; i++) {
				rfs_bm_block_free(&e.bm, pos[i], cnt[i]);
				/* 重复释放（对已释放区间）——提交态不得虚高 */
				rfs_bm_block_free(&e.bm, pos[i], cnt[i]);
			}
			if (mode == 1)
				rfs_bm_chunks_asyn(&e.bm);
			CHECK(dev_free_total(&e, false) == nbits,
			      "mode%u 全释放后 free_total == nbits（重复释放不虚高）",
			      mode);
			{
				u8 *dev = malloc(nbits);
				u64 z = 0, j;

				dev_get_bits(&e, false, dev);
				for (j = 0; j < nbits; j++)
					z += dev[j];
				CHECK(z == 0, "mode%u 提交态全零", mode);
				free(dev);
			}
		}
		/* 越界/溢出 */
		/* 越界/溢出（软分配 v1/v2 差异：MEM count>片 → NO_HIT；CHUNK 取部分段）*/
		if (mode == 0) {
			CHECK(rfs_bm_block_alloc_soft(&e.bm, nbits + 5, NULL) == RFS_BM_NO_HIT,
			      "MEM soft count>总 → NO_HIT");
		} else {
			u64 rem = 0;
			u64 p = rfs_bm_block_alloc_soft(&e.bm, nbits + 5, &rem);

			CHECK(p != RFS_BM_NO_HIT && rem < nbits + 5,
			      "CHUNK soft count>总 → 部分给(文档化) pos=%llu rem=%llu",
			      (unsigned long long)p, (unsigned long long)rem);
			if (p != RFS_BM_NO_HIT)
				rfs_bm_block_free(&e.bm, p, nbits + 5 - rem);
		}
		CHECK(rfs_bm_block_test_interval(&e.bm, nbits - 1, 2, false) == -EINVAL,
		      "mode%u test_interval 越界 EINVAL", mode);
		env_destroy(&e);
	}
}

/* ═══════════════════════════ BUG-1 回归 ═══════════════════════════ */

/*
 * 0-认领的失败 occupy 之后，后续 alloc 仍须与 MEM 同址（BUG-1：修前 CHUNK 会把
 * in_slice_hint 推到锚点 → 与 MEM 分叉；本用例验证已修）。
 * 场景：预置 [64,128) 占用；occupy(base=64)（锚点占用）→ NO_HIT；随后 hard(1)：
 *   MEM → 0（hint 未动）；修前 CHUNK → 128（hint 被推到 64）。修后 CHUNK → 0。
 */
static void suite_regression_bug1(void)
{
	env_t em, ec;
	oracle_t om;
	u64 pm, pc;

	printf("[suite] regression_bug1\n");
	if (env_prep_mem(&em, 8, 8, 1) != 0) { CHECK(0, "prep mem"); return; }
	if (env_prep_chunk(&ec, 8, 8, 1, 0, 0, 0) != 0) { CHECK(0, "prep chunk"); return; }
	words_set_range(em.blk_words, 64, 64);
	words_set_range(ec.blk_words, 64, 64);
	if (env_go(&em) != 0 || env_go(&ec) != 0) {
		CHECK(0, "go");
		env_destroy(&em); env_destroy(&ec);
		return;
	}
	o_init(&om, em.bm.blk.nbits, em.bm.blk.per_bits, em.bm.blk.n_slices,
	       em.cpu_slots, (const u64 *)em.blk_words);

	/* 失败 occupy（锚点 64 占用，0 认领） */
	{
		u64 rm = 99, rc2 = 99;
		u64 dm = rfs_bm_block_occupy_soft(&em.bm, 64, 1, &rm);
		u64 dc = rfs_bm_block_occupy_soft(&ec.bm, 64, 1, &rc2);

		CHECK(dm == RFS_BM_NO_HIT && rm == 1, "MEM 失败 occupy → NO_HIT rest=1");
		CHECK(dc == RFS_BM_NO_HIT && rc2 == 1, "CHUNK 失败 occupy → NO_HIT rest=1");
	}
	/* 后续 hard(1)：三者须同址 */
	pm = rfs_bm_block_alloc_hard(&em.bm, 1, 0);
	pc = rfs_bm_block_alloc_hard(&ec.bm, 1, 0);
	{
		u64 po = o_alloc_hard(&om, 1, 0);

		CHECK(pm == po, "MEM 与 oracle 同址（%llu vs %llu）",
		      (unsigned long long)pm, (unsigned long long)po);
		CHECK(pc == po, "CHUNK 与 MEM/oracle 同址（%llu vs %llu）——BUG-1 回归",
		      (unsigned long long)pc, (unsigned long long)po);
	}
	o_fini(&om);
	env_destroy(&em);
	env_destroy(&ec);
}

/* ═══════════════════════════ 差分驱动 ═══════════════════════════ */

enum { OP_HARD, OP_SOFT, OP_OCC, OP_FREE };

typedef struct { int kind; u64 a, b; } op_t;	/* HARD:(cnt,align) SOFT:(cnt,_) OCC:(base,cnt) FREE:(base,cnt) */

typedef struct {
	u64 pos[512];
	u64 len[512];
	int n;
} allocring_t;

static void ring_push(allocring_t *r, u64 pos, u64 len)
{
	if (r->n < 512) {
		r->pos[r->n] = pos; r->len[r->n] = len; r->n++;
	} else {
		memmove(&r->pos[0], &r->pos[1], sizeof(u64) * 511);
		memmove(&r->len[0], &r->len[1], sizeof(u64) * 511);
		r->pos[511] = pos; r->len[511] = len;
	}
}

static op_t gen_op(u64 *st, u64 nbits)
{
	op_t op;
	u64 r = rnd_u64(st) % 100;

	if (r < 38) {
		op.kind = OP_HARD;
		op.a = 1 + rnd_u64(st) % 1500;		/* count */
		op.b = rnd_u64(st) % 6;			/* align_log2 0..5 */
	} else if (r < 62) {
		op.kind = OP_SOFT;
		op.a = 1 + rnd_u64(st) % 1500;
		op.b = 0;
	} else if (r < 80) {
		op.kind = OP_OCC;
		op.a = rnd_u64(st) % nbits;		/* base */
		op.b = 1 + rnd_u64(st) % 800;		/* count */
	} else {
		op.kind = OP_FREE;
		op.a = rnd_u64(st) % nbits;
		op.b = 1 + rnd_u64(st) % 800;
	}
	return op;
}

/*
 * MEM 逐步严格差分：设备 vs oracle 全等（位态 / 位置 / 缺口 / 计数）。
 * merge_counts=1 时每步先 chunks_asyn 再比（CHUNK 用：位置仍逐步比，计数/位态按提交态）。
 */
static int diff_mem(env_t *e, oracle_t *o, int nops, u64 seed, const char *tag,
		    int merge_counts)
{
	u64 st = seed;
	u8 *dev = malloc(o->nbits);
	allocring_t ring = { {0}, {0}, 0 };
	int i, fails = 0;

	for (i = 0; i < nops && fails < 8; i++) {
		op_t op = gen_op(&st, o->nbits);

		switch (op.kind) {
		case OP_HARD: {
			u64 dp = rfs_bm_block_alloc_hard(&e->bm, op.a, op.b);
			u64 op2 = o_alloc_hard(o, op.a, op.b);

			if (dp != op2) {
				CHECK(0, "%s step%d HARD cnt=%llu align=%llu dev=%llu ref=%llu",
				      tag, i, (unsigned long long)op.a,
				      (unsigned long long)op.b, (unsigned long long)dp,
				      (unsigned long long)op2);
				fails++;
			}
			if (dp != RFS_BM_NO_HIT)
				ring_push(&ring, dp, op.a);
			break;
		}
		case OP_SOFT: {
			u64 drem = 0, orem = 0;
			u64 dp = rfs_bm_block_alloc_soft(&e->bm, op.a, &drem);
			u64 op2 = o_alloc_soft(o, op.a, &orem);

			if (dp != op2 || drem != orem) {
				CHECK(0, "%s step%d SOFT cnt=%llu dev=(%llu,%llu) ref=(%llu,%llu)",
				      tag, i, (unsigned long long)op.a,
				      (unsigned long long)dp, (unsigned long long)drem,
				      (unsigned long long)op2, (unsigned long long)orem);
				fails++;
			}
			if (dp != RFS_BM_NO_HIT)
				ring_push(&ring, dp, op.a - drem);
			break;
		}
		case OP_OCC: {
			u64 drest = 0, orest = 0;
			u64 dp = rfs_bm_block_occupy_soft(&e->bm, op.a, op.b, &drest);
			u64 op2 = o_occupy_soft(o, op.a, op.b, &orest);

			if (dp != op2 || drest != orest) {
				CHECK(0, "%s step%d OCC base=%llu cnt=%llu dev=(%llu,%llu) ref=(%llu,%llu)",
				      tag, i, (unsigned long long)op.a,
				      (unsigned long long)op.b, (unsigned long long)dp,
				      (unsigned long long)drest, (unsigned long long)op2,
				      (unsigned long long)orest);
				fails++;
			}
			if (dp != RFS_BM_NO_HIT)
				ring_push(&ring, dp, op.b - drest);
			break;
		}
		case OP_FREE: {
			u64 base = op.a, cnt = op.b;

			if (ring.n && (rnd_u64(&st) % 2)) {
				int k = (int)(rnd_u64(&st) % (u64)ring.n);

				base = ring.pos[k];
				cnt = ring.len[k];
			}
			rfs_bm_block_free(&e->bm, base, cnt);
			o_block_free(o, base, cnt);
			break;
		}
		}
		/* 逐步全量比对（CHUNK 先合并再比，实现「合成后视角」口径） */
		if (merge_counts)
			rfs_bm_chunks_asyn(&e->bm);
		dev_get_bits(e, false, dev);
		if (bits_diff(dev, o->b, o->nbits, tag, 4) != 0) {
			CHECK(0, "%s step%d 位态不等", tag, i);
			fails++;
		}
		if (dev_free_total(e, false) != o_free_total(o)) {
			CHECK(0, "%s step%d free_total dev=%llu ref=%llu", tag, i,
			      (unsigned long long)dev_free_total(e, false),
			      (unsigned long long)o_free_total(o));
			fails++;
		}
	}
	free(dev);
	return fails;
}

/* CHUNK divergence 统计（文档化差异分门别类；bug_* 应为 0）。 */
typedef struct {
	int doc_hard, doc_soft;	/* 文档化：单窗/跨窗/对齐/全满 */
	int bug_hard, bug_soft;	/* 疑似真缺陷 */
} divstat_t;

/* 提交态校验：合并后位态/free_total 必须等于 shadow（设备动作真值）。返回非零=状态失配。 */
static int commit_verify(env_t *e, oracle_t *sh, u8 *dev, const char *tag, int step)
{
	int bad = 0;

	CHECK(rfs_bm_chunks_asyn(&e->bm) == 0, "%s asyn", tag);
	dev_get_bits(e, false, dev);
	if (bits_diff(dev, sh->b, sh->nbits, tag, 4)) {
		CHECK(0, "%s step%d 提交态位态 != shadow", tag, step);
		bad = 1;
	}
	if (dev_free_total(e, false) != o_zeros_total(sh)) {
		CHECK(0, "%s step%d 提交态 free_total != 真值", tag, step);
		bad = 1;
	}
	return bad;
}

/* CHUNK 片内 hint（公开结构体 rfs_bm_cslice::in_slice_hint；片内局部 0 起）。 */
static u64 chunk_slice_hint(const env_t *e, bool ino, u32 s)
{
	const struct rfs_bm_cslice *cs = ino ? e->bm.ino.cslices : e->bm.blk.cslices;

	return cs[s].in_slice_hint;
}

/*
 * 是否存在「某窗内、起始对齐、完整含于该窗」的 count 连续空闲 run——**但不跨越该片
 * 的 in_slice_hint**（CHUNK/MEM 同款 hint 起扫 + 回绕会在 hint 处把 run 切成两半，
 * 那是两者共有的搜索假象，不是 CHUNK 缺陷）。返回 true ⇒ CHUNK hard 本当成功。
 */
static bool shadow_has_window_run(const env_t *e, const oracle_t *sh, u64 cw,
				  u64 count, u64 align)
{
	u64 c;

	for (c = 0; c + 1 <= sh->nbits; c += cw) {
		u64 whi = c + cw < sh->nbits ? c + cw : sh->nbits;
		u64 p;

		for (p = c; p + count <= whi; ) {
			u64 q = (p + align - 1) & ~(align - 1), j;
			u32 s = (u32)(q / sh->per_bits);
			u64 hint_g = (u64)s * sh->per_bits + chunk_slice_hint(e, false, s);

			if (q + count > whi)
				break;
			if (q < hint_g && hint_g < q + count) {	/* 跨 hint：两后端共有假象 */
				p = q + 1;
				continue;
			}
			for (j = 0; j < count; j++)
				if (sh->b[q + j])
					break;
			if (j == count)
				return true;
			p = q + 1;
		}
	}
	return false;
}

/*
 * CHUNK 多窗：逐步只比「分配成败语义 / 无双重分配 / 对齐 / 单窗」+ occupy 决定性，
 * 位态/计数在**合成后（提交态）**比。漏配判定以 shadow（设备实际动作）为真值：
 *   · hard 漏配：合成后窗内若存在对齐可配 run → 疑似真缺陷；否则文档化（单窗/对齐）。
 *   · soft 漏配：合成后仍有空闲 → 疑似真缺陷；否则文档化。
 */
static int diff_chunk_commit(env_t *e, oracle_t *shadow, int nops, u64 seed,
			     const char *tag, divstat_t *ds)
{
	u64 st = seed;
	u64 cw = BM_CHUNK_BITS(e->chunk_order_blk);
	u8 *dev = malloc(shadow->nbits);
	allocring_t ring = { {0}, {0}, 0 };
	int i, fails = 0;

	for (i = 0; i < nops && fails < 8; i++) {
		op_t op = gen_op(&st, shadow->nbits);

		switch (op.kind) {
		case OP_HARD: {
			u64 dp = rfs_bm_block_alloc_hard(&e->bm, op.a, op.b);

			if (dp != RFS_BM_NO_HIT) {
				u64 align = 1ULL << (op.b > 16 ? 16 : op.b);
				u64 j, dbl = 0;

				for (j = 0; j < op.a; j++)
					dbl += shadow->b[dp + j];
				CHECK(dbl == 0, "%s 双重分配 hard pos=%llu len=%llu",
				      tag, (unsigned long long)dp, (unsigned long long)op.a);
				CHECK(dp % align == 0, "%s hard 未对齐 pos=%llu",
				      tag, (unsigned long long)dp);
				CHECK(dp / cw == (dp + op.a - 1) / cw, "%s hard 跨窗",
				      tag);
				o_apply_alloc(shadow, dp, op.a);
				ring_push(&ring, dp, op.a);
			} else {
				u64 align = 1ULL << (op.b > 16 ? 16 : op.b);

				if (commit_verify(e, shadow, dev, tag, i) ||
				    shadow_has_window_run(e, shadow, cw, op.a, align)) {
					ds->bug_hard++;
					CHECK(0, "%s 疑似真缺陷：窗内存在对齐可配 run 却漏配（cnt=%llu align=%llu）",
					      tag, (unsigned long long)op.a,
					      (unsigned long long)op.b);
					fails++;
				} else {
					ds->doc_hard++;
				}
			}
			break;
		}
		case OP_SOFT: {
			u64 drem = 0;
			u64 dp = rfs_bm_block_alloc_soft(&e->bm, op.a, &drem);

			if (dp != RFS_BM_NO_HIT) {
				u64 got = op.a - drem, j, dbl = 0;

				for (j = 0; j < got; j++)
					dbl += shadow->b[dp + j];
				CHECK(dbl == 0, "%s soft 双重分配 pos=%llu got=%llu",
				      tag, (unsigned long long)dp, (unsigned long long)got);
				o_apply_alloc(shadow, dp, got);
				if (got)
					ring_push(&ring, dp, got);
			} else {
				if (commit_verify(e, shadow, dev, tag, i) ||
				    o_zeros_total(shadow) > 0) {
					ds->bug_soft++;
					CHECK(0, "%s 疑似真缺陷：尚有 %llu 空闲却 soft 漏配",
					      tag, (unsigned long long)o_zeros_total(shadow));
					fails++;
				} else {
					ds->doc_soft++;
				}
			}
			break;
		}
		case OP_OCC: {
			u64 drest = 0;
			u64 run = 0, exp_claimed;
			u64 dp = rfs_bm_block_occupy_soft(&e->bm, op.a, op.b, &drest);

			/* 期望 = 从 base 起的连续空闲前缀（≤count）——occupy 决定性 */
			while (run < op.b && op.a + run < shadow->nbits &&
			       !shadow->b[op.a + run])
				run++;
			exp_claimed = run;
			CHECK(dp == (exp_claimed > 0 ? op.a : RFS_BM_NO_HIT),
			      "%s occupy 锚点/成败（dev=%llu exp=%llu）", tag,
			      (unsigned long long)dp, (unsigned long long)op.a);
			CHECK(drest == op.b - exp_claimed,
			      "%s occupy 缺口 rest（dev=%llu exp=%llu）", tag,
			      (unsigned long long)drest,
			      (unsigned long long)(op.b - exp_claimed));
			if (exp_claimed > 0) {
				u64 j, dbl = 0;

				for (j = 0; j < exp_claimed; j++)
					dbl += shadow->b[op.a + j];
				CHECK(dbl == 0, "%s occupy 双重分配", tag);
				o_apply_alloc(shadow, op.a, exp_claimed);
				ring_push(&ring, op.a, exp_claimed);
			}
			break;
		}
		case OP_FREE: {
			u64 base = op.a, cnt = op.b;

			if (ring.n && (rnd_u64(&st) % 2)) {
				int k = (int)(rnd_u64(&st) % (u64)ring.n);

				base = ring.pos[k];
				cnt = ring.len[k];
			}
			rfs_bm_block_free(&e->bm, base, cnt);
			o_apply_free(shadow, base, cnt);
			break;
		}
		}
		if ((i % 137) == 0) {		/* 合成点：全量校验 */
			CHECK(rfs_bm_chunks_asyn(&e->bm) == 0, "%s chunks_asyn", tag);
			dev_get_bits(e, false, dev);
			if (bits_diff(dev, shadow->b, shadow->nbits, tag, 4) != 0) {
				CHECK(0, "%s step%d 合成后位态 != shadow", tag, i);
				fails++;
			}
			if (dev_free_total(e, false) != o_zeros_total(shadow)) {
				CHECK(0, "%s step%d 合成后 free_total != shadow 真值", tag, i);
				fails++;
			}
		}
	}
	/* 收尾：合并后校验 */
	CHECK(rfs_bm_chunks_asyn(&e->bm) == 0, "%s 收尾 chunks_asyn", tag);
	dev_get_bits(e, false, dev);
	if (bits_diff(dev, shadow->b, shadow->nbits, tag, 6) != 0) {
		CHECK(0, "%s 收尾 合成后位态 != shadow", tag);
		fails++;
	}
	if (dev_free_total(e, false) != o_zeros_total(shadow)) {
		CHECK(0, "%s 收尾 free_total=%llu 真值=%llu", tag,
		      (unsigned long long)dev_free_total(e, false),
		      (unsigned long long)o_zeros_total(shadow));
		fails++;
	}
	free(dev);
	return fails;
}

/* ═══════════════════════════ 差分套件 ═══════════════════════════ */

static void suite_diff_mem(void)
{
	env_t e;
	oracle_t o;
	int n = g_full ? 4000 : 900;
	int f;

	printf("[suite] differential_mem\n");
	if (env_build_mem(&e, 4, 4, 1) != 0) { CHECK(0, "init"); return; }
	o_init(&o, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
	       e.cpu_slots, (const u64 *)e.blk_words);
	f = diff_mem(&e, &o, n, 0xC0FFEEULL, "mem1sl", 0);
	CHECK(f == 0, "MEM 单片逐步严格差分应 0 失配（得 %d）", f);
	o_fini(&o);
	env_destroy(&e);

	/* 多片（cpu_slots=4）——路由/preferred 参与 */
	if (env_build_mem(&e, 10, 10, 4) != 0) { CHECK(0, "init2"); return; }
	o_init(&o, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
	       e.cpu_slots, (const u64 *)e.blk_words);
	f = diff_mem(&e, &o, n, 0xBEEFULL, "mem4sl", 0);
	CHECK(f == 0, "MEM 4 片逐步严格差分应 0 失配（得 %d）", f);
	o_fini(&o);
	env_destroy(&e);
}

static void suite_diff_chunk(void)
{
	int f;

	printf("[suite] differential_chunk\n");
	/* C1：单窗/片（= MEM 语义）→ 逐步亦应全等 */
	{
		env_t e;
		oracle_t o;
		int n = g_full ? 3000 : 800;

		if (env_build_chunk(&e, 8, 8, 1, 3, 3, 0) != 0) { CHECK(0, "C1 init"); return; }
		o_init(&o, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
		       e.cpu_slots, (const u64 *)e.blk_words);
		f = diff_mem(&e, &o, n, 0x1234ULL, "chunkC1", 1);
		CHECK(f == 0, "CHUNK 单窗/片 位置逐步 + 计数提交态应全等 MEM（得 %d）", f);
		o_fini(&o);
		env_destroy(&e);
	}
	/* C2：多窗（单片 8 窗）→ 逐步比成败 + 合成后位态/计数 */
	{
		env_t e;
		oracle_t sh;
		divstat_t ds = { 0, 0, 0, 0 };
		int n = g_full ? 4000 : 1200;

		if (env_build_chunk(&e, 8, 8, 1, 0, 0, 0) != 0) { CHECK(0, "C2 init"); return; }
		o_init(&sh, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
		       e.cpu_slots, (const u64 *)e.blk_words);
		f = diff_chunk_commit(&e, &sh, n, 0x5678ULL, "chunkC2", &ds);
		CHECK(f == 0, "CHUNK 多窗 合成后应自洽（得 %d 失配）", f);
		CHECK(ds.bug_hard + ds.bug_soft == 0, "CHUNK 多窗 无「疑似真缺陷」（得 %d）",
		      ds.bug_hard + ds.bug_soft);
		printf("  [diff_chunk] C2 文档化 hard=%d soft=%d；疑似缺陷 hard=%d soft=%d\n",
		       ds.doc_hard, ds.doc_soft, ds.bug_hard, ds.bug_soft);
		o_fini(&sh);
		env_destroy(&e);
	}
	/* C2b：单片 32 窗，更快换片 */
	{
		env_t e;
		oracle_t sh;
		divstat_t ds = { 0, 0, 0, 0 };
		int n = g_full ? 4000 : 1000;

		if (env_build_chunk(&e, 32, 32, 1, 0, 0, 0) != 0) { CHECK(0, "C2b init"); return; }
		o_init(&sh, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
		       e.cpu_slots, (const u64 *)e.blk_words);
		f = diff_chunk_commit(&e, &sh, n, 0x9ABCULL, "chunkC2b", &ds);
		CHECK(f == 0, "CHUNK 32窗 合成后自洽（得 %d）", f);
		CHECK(ds.bug_hard + ds.bug_soft == 0, "CHUNK 32窗 无疑似缺陷（得 %d）",
		      ds.bug_hard + ds.bug_soft);
		printf("  [diff_chunk] C2b 文档化 hard=%d soft=%d；疑似缺陷 hard=%d soft=%d\n",
		       ds.doc_hard, ds.doc_soft, ds.bug_hard, ds.bug_soft);
		o_fini(&sh);
		env_destroy(&e);
	}
}

/* ═══════════════════════════ 持久化套件 ═══════════════════════════ */

static void suite_persistence(void)
{
	printf("[suite] persistence\n");
	/* MEM：snapshot_slice == 内存视图 */
	{
		env_t e;
		oracle_t o;
		int i;

		if (env_build_mem(&e, 8, 8, 1) != 0) { CHECK(0, "init"); return; }
		o_init(&o, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
		       e.cpu_slots, (const u64 *)e.blk_words);
		for (i = 0; i < 30; i++) {
			u64 p = rfs_bm_block_alloc_hard(&e.bm, 1 + i % 10, i % 3);

			o_alloc_hard(&o, 1 + i % 10, i % 3);
			(void)p;
		}
		{
			u8 *dev = malloc(e.blk_nbits);

			dev_get_bits(&e, false, dev);
			CHECK(bits_diff(dev, o.b, e.blk_nbits, "mem-persist", 4) == 0,
			      "MEM snapshot == 内存视图");
			free(dev);
		}
		o_fini(&o);
		env_destroy(&e);
	}
	/* CHUNK：asyn 后从 fake 盘重 init，free_total/位态一致 */
	{
		env_t e;
		u64 nbits, nalloc = 0;
		u8 *dev;
		u64 j;

		if (env_build_chunk(&e, 8, 8, 1, 0, 0, 0) != 0) { CHECK(0, "init"); return; }
		nbits = e.blk_nbits;
		for (j = 0; j < 40; j++) {
			u64 p = rfs_bm_block_alloc_hard(&e.bm, 1 + j % 9, j % 3);

			if (p != RFS_BM_NO_HIT)
				nalloc += 1 + j % 9;
		}
		CHECK(rfs_bm_chunks_asyn(&e.bm) == 0, "asyn 落盘");
		dev = malloc(nbits);
		dev_get_bits(&e, false, dev);
		CHECK(dev_free_total(&e, false) == nbits - nalloc,
		      "asyn 后 free_total == nbits-used");
		/* 复制盘 → 新 env 重 init */
		{
			env_t e2;
			u8 *dev2;
			u64 d, totsz = (u64)e.disk.nblk * BM_BLKSZ;

			if (env_prep_chunk(&e2, 8, 8, 1, 0, 0, 0) != 0) { CHECK(0, "prep2"); return; }
			memcpy(e2.disk.mem, e.disk.mem, totsz);
			e2.keep_disk = 1;	/* 从已有盘重 init，不再序列化 words */
			CHECK(env_go(&e2) == 0, "从盘重 init");
			dev2 = malloc(nbits);
			dev_get_bits(&e2, false, dev2);
			d = 0;
			for (j = 0; j < nbits; j++)
				d += dev[j] != dev2[j];
			CHECK(d == 0, "重 init 位态一致（%llu 位差）", (unsigned long long)d);
			CHECK(dev_free_total(&e2, false) == dev_free_total(&e, false),
			      "重 init free_total 一致");
			free(dev2);
			env_destroy(&e2);
		}
		free(dev);
		env_destroy(&e);
	}
}

/* ═══════════════════════════ 窗口机制套件 ═══════════════════════════ */

static void suite_chunk_windows(void)
{
	u64 cw = BM_CHUNK_BITS(0);
	env_t e;

	printf("[suite] chunk_windows\n");
	if (env_build_chunk(&e, 32, 32, 1, 0, 0, 0) != 0) { CHECK(0, "init"); return; }

	/* 1) 换片：w0 分配 → 远窗分配（evict w0→盘）→ 回 w0（load 盘） */
	{
		u64 h0 = rfs_bm_block_occupy_soft(&e.bm, 0, 2, NULL);

		CHECK(h0 == 0, "w0 occupy 2 位（得 %llu）", (unsigned long long)h0);
		{
			u64 hf = rfs_bm_block_occupy_soft(&e.bm, 25 * cw, 8, NULL);

			CHECK(hf == 25 * cw, "远窗 occupy 命中 25*cw（得 %llu）",
			      (unsigned long long)hf);
		}
		CHECK(rfs_bm_block_test(&e.bm, 0), "换片后 w0 bit0 仍在（load 盘正确）");
		CHECK(rfs_bm_block_test(&e.bm, 1), "换片后 w0 bit1 仍在");
		CHECK(!rfs_bm_block_test(&e.bm, 2), "换片后 w0 bit2 空闲");
		CHECK(rfs_bm_block_test(&e.bm, 25 * cw), "远窗位占用");

		/* 2) 归还链挂账零 I/O → 使用前合并不泄漏 */
		{
			u64 rd0 = e.disk.reads, wr0 = e.disk.writes;

			rfs_bm_block_free(&e.bm, 0, 2);	/* w0 离线 → 挂链，零 I/O */
			CHECK(e.disk.reads == rd0 && e.disk.writes == wr0,
			      "离线窗 free 零 I/O（挂账）");
			CHECK(!rfs_bm_block_test(&e.bm, 0),
			      "free 后提交态 bit0 空闲（使用前合并）");
			{
				u64 q = rfs_bm_block_occupy_soft(&e.bm, 0, 2, NULL);

				CHECK(q == 0, "drain 后 bit0/1 可再分配（得 %llu）",
				      (unsigned long long)q);
			}
		}
		/* 重置 w0..w4 便于整窗填满 */
		rfs_bm_block_free(&e.bm, 0, 5 * cw);
	}
	/* 3) 摘要免载入跳窗：填满 w0..w4，再在 w6 分配 */
	{
		u64 h = rfs_bm_block_occupy_soft(&e.bm, 0, 5 * cw, NULL);

		CHECK(h == 0, "占用 w0..w4 全部（得 %llu）", (unsigned long long)h);
		{
			u64 h2 = rfs_bm_block_occupy_soft(&e.bm, 6 * cw, 10, NULL);

			CHECK(h2 == 6 * cw, "w6 分配（得 %llu）", (unsigned long long)h2);
		}
		CHECK(rfs_bm_block_test(&e.bm, 3 * cw + 5), "w3 满窗位占用");
		rfs_bm_block_free(&e.bm, 0, 5 * cw);
		CHECK(rfs_bm_chunks_asyn(&e.bm) == 0, "asyn 校准");
		CHECK(!rfs_bm_block_test(&e.bm, 3 * cw + 5), "释放+合并后空闲");
	}
	env_destroy(&e);
}

/* ═══════════════════ 合成视角套件（Task B：证明逐步可异、合成后必同）═══════════════════ */

/*
 * 设计思想落地用例：CHUNK 的「挂账期视图」允许与真值不同（挂账计费 → free_total
 * 可虚高；释放的位在盘上/未合并前仍为 1），但**合并后（提交态）**必须与真值一致。
 * 驱动：占 1 位 → 换片（evict → 盘上 =1）→ 离线释放（挂账、零 I/O，盘上仍 =1）→
 * 再（重复）释放一次制造虚高 → chunks_asyn 校准。逐步对比三样本（raw 盘 / 提交态 /
 * 真值）并断言「逐步可异、合成后必同」。
 */
static void suite_synth_view(void)
{
	u64 cw = BM_CHUNK_BITS(0);
	env_t e;
	oracle_t sh;
	u64 p, ft_freed, ft_infl, ft_commit, truth;
	bool committed;
	u8 raw_bit, raw_after;

	printf("[suite] synth_view (CHUNK 合成视角)\n");
	if (env_build_chunk(&e, 32, 32, 1, 0, 0, 0) != 0) { CHECK(0, "init"); return; }
	o_init(&sh, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
	       e.cpu_slots, (const u64 *)e.blk_words);

	p = rfs_bm_block_occupy_soft(&e.bm, 100, 1, NULL);	/* w0 bit100 */
	CHECK(p == 100, "占 w0 bit100");
	o_apply_alloc(&sh, 100, 1);
	rfs_bm_block_occupy_soft(&e.bm, 20 * cw, 1, NULL);	/* 换片：evict w0 → 盘 */
	o_apply_alloc(&sh, 20 * cw, 1);

	rfs_bm_block_free(&e.bm, 100, 1);	/* w0 离线：挂账，零 I/O */
	o_apply_free(&sh, 100, 1);
	ft_freed = dev_free_total(&e, false);
	committed = rfs_bm_block_test(&e.bm, 100);	/* 提交态（内部合并） */
	raw_bit = (u8)((e.disk.mem[100 >> 3] >> (100 & 7)) & 1u);	/* 未合并：盘上仍 1 */

	rfs_bm_block_free(&e.bm, 100, 1);	/* 重复释放：挂账计费 → 虚高 */
	ft_infl = dev_free_total(&e, false);
	truth = o_zeros_total(&sh);

	CHECK(rfs_bm_chunks_asyn(&e.bm) == 0, "chunks_asyn");
	ft_commit = dev_free_total(&e, false);
	raw_after = (u8)((e.disk.mem[100 >> 3] >> (100 & 7)) & 1u);

	printf("  逐步视图：raw盘 bit100=%u（未合并仍 1）；提交态 bit100=%u；"
	       "free_total=%llu\n", raw_bit, (unsigned)committed,
	       (unsigned long long)ft_freed);
	printf("  重复释放后 free_total=%llu（挂账虚高）；真值=%llu\n",
	       (unsigned long long)ft_infl, (unsigned long long)truth);
	printf("  合并后：free_total=%llu（== 真值）；raw盘 bit100=%u\n",
	       (unsigned long long)ft_commit, raw_after);

	/* ★ 断言栈：逐步可异（raw 仍 1 / 计数虚高），合成后必同（位态 0 / 计数==真值） */
	CHECK(raw_bit == 1, "挂账期 raw 盘 bit100 仍为 1（逐步视图可异）");
	CHECK(committed == false, "提交态 bit100 已空闲（使用前合并）");
	CHECK(ft_infl > truth, "挂账期 free_total 虚高（%llu > %llu）",
	      (unsigned long long)ft_infl, (unsigned long long)truth);
	CHECK(ft_commit == truth, "合成后 free_total == 真值（%llu）",
	      (unsigned long long)truth);
	CHECK(raw_after == 0, "合成后 raw 盘 bit100 == 0");

	o_fini(&sh);
	env_destroy(&e);
}

/* ═══════════════════════════ free_count 套件 ═══════════════════════════ */
static u64 o_slice_zeros(const oracle_t *o, u32 j)
{
	u64 lo = (u64)j * o->per_bits, hi = lo + o->per_bits, z = 0, i;

	if (hi > o->nbits)
		hi = o->nbits;
	for (i = lo; i < hi; i++)
		if (!o->b[i])
			z++;
	return z;
}

static void suite_free_count(void)
{
	u32 mode;

	printf("[suite] free_count\n");
	for (mode = 0; mode < 2; mode++) {
		env_t e;
		oracle_t sh;
		u64 st = 0xDEADULL;
		int step, j, bad = 0;
		int n = g_full ? 1500 : 600;

		if (mode == 0) {
			if (env_build_mem(&e, 8, 8, 1) != 0) { CHECK(0, "init"); continue; }
		} else {
			if (env_build_chunk(&e, 8, 8, 1, 0, 0, 0) != 0) { CHECK(0, "init"); continue; }
		}
		o_init(&sh, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
		       e.cpu_slots, (const u64 *)e.blk_words);
		for (step = 0; step < n; step++) {
			u64 cnt = 1 + rnd_u64(&st) % 500;
			u64 b = rnd_u64(&st) % e.blk_nbits;

			if (rnd_u64(&st) % 2) {
				u64 p = rfs_bm_block_alloc_hard(&e.bm, cnt, rnd_u64(&st) % 4);

				if (p != RFS_BM_NO_HIT)
					o_apply_alloc(&sh, p, cnt);	/* 动作驱动 shadow */
			} else {
				rfs_bm_block_free(&e.bm, b, cnt);
				o_apply_free(&sh, b, cnt);
			}
			if (mode == 1 && (step % 53) == 0)
				rfs_bm_chunks_asyn(&e.bm);	/* 提交态 */
			if ((step % 53) == 0) {
				for (j = 0; j < (int)e.bm.blk.n_slices; j++) {
					u64 df = dev_slice_free(&e, false, (u32)j);
					u64 of = o_slice_zeros(&sh, (u32)j);

					if (df != of) {
						bad++;
						if (bad <= 3)
							CHECK(0, "mode%u step%d 片%d free_count dev=%llu 真值=%llu",
							      mode, step, j,
							      (unsigned long long)df,
							      (unsigned long long)of);
					}
				}
			}
		}
		if (mode == 1)
			rfs_bm_chunks_asyn(&e.bm);
		CHECK(dev_free_total(&e, false) == o_zeros_total(&sh),
		      "mode%u 收尾 total == 真值", mode);
		o_fini(&sh);
		env_destroy(&e);
	}
}

/* ═══════════════════════════ 随机 fuzz ═══════════════════════════ */

static void fuzz_mem_invariants(env_t *e, oracle_t *sh, int nops, u64 seed)
{
	u64 st = seed;
	u8 *dev = malloc(sh->nbits);
	int i, fails = 0;

	for (i = 0; i < nops && fails < 5; i++) {
		u64 r = rnd_u64(&st) % 100;

		if (r < 45) {
			u64 cnt = 1 + rnd_u64(&st) % 700;
			u64 p = rfs_bm_block_alloc_hard(&e->bm, cnt, rnd_u64(&st) % 5);

			if (p != RFS_BM_NO_HIT) {
				u64 j, dbl = 0;

				for (j = 0; j < cnt; j++)
					dbl += sh->b[p + j];
				if (dbl) { CHECK(0, "fuzz 双重分配 p=%llu n=%llu",
						 (unsigned long long)p, (unsigned long long)cnt); fails++; }
				o_apply_alloc(sh, p, cnt);
			}
		} else if (r < 70) {
			u64 want = 1 + rnd_u64(&st) % 700, rem = 0;
			u64 p = rfs_bm_block_alloc_soft(&e->bm, want, &rem);

			if (p != RFS_BM_NO_HIT) {
				u64 got = want - rem, j, dbl = 0;

				for (j = 0; j < got; j++)
					dbl += sh->b[p + j];
				if (dbl) { CHECK(0, "fuzz soft 双重分配"); fails++; }
				o_apply_alloc(sh, p, got);
			}
		} else {
			u64 b = rnd_u64(&st) % sh->nbits, cnt = 1 + rnd_u64(&st) % 400;

			rfs_bm_block_free(&e->bm, b, cnt);
			o_apply_free(sh, b, cnt);
		}
	}
	dev_get_bits(e, false, dev);
	if (bits_diff(dev, sh->b, sh->nbits, "fuzzmem", 4) != 0) fails++;
	free(dev);
	CHECK(fails == 0, "fuzz MEM 自洽");
}

static void suite_fuzz(void)
{
	int n = g_full ? 20000 : (g_light ? 1200 : 3000);
	u32 cfgs[2] = { 1, 8 };
	unsigned ci;

	printf("[suite] fuzz\n");
	for (ci = 0; ci < 2; ci++) {
		env_t e;
		oracle_t sh;

		if (env_build_mem(&e, 16, 16, cfgs[ci]) != 0) { CHECK(0, "mem init"); continue; }
		if (cfgs[ci] == 1 && !g_light) {
			oracle_t o;
			int f;

			o_init(&o, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
			       e.cpu_slots, (const u64 *)e.blk_words);
			f = diff_mem(&e, &o, n, 0xF00DULL, "fuzzmem1", 0);
			CHECK(f == 0, "fuzz MEM cpu_slots=1 严格差分（得 %d）", f);
			o_fini(&o);
		} else {
			o_init(&sh, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
			       e.cpu_slots, (const u64 *)e.blk_words);
			fuzz_mem_invariants(&e, &sh, n, 0xB00BULL);
			o_fini(&sh);
		}
		env_destroy(&e);
	}
	/* CHUNK 多窗 fuzz（提交态校验） */
	{
		env_t e;
		oracle_t sh;
		divstat_t ds = { 0, 0, 0, 0 };
		int f;

		if (env_build_chunk(&e, 16, 16, 4, 0, 0, 0) != 0) { CHECK(0, "chunk init"); return; }
		o_init(&sh, e.bm.blk.nbits, e.bm.blk.per_bits, e.bm.blk.n_slices,
		       e.cpu_slots, (const u64 *)e.blk_words);
		f = diff_chunk_commit(&e, &sh, n, 0xCAFEULL, "fuzzchunk", &ds);
		CHECK(f == 0, "fuzz CHUNK 合成后自洽（得 %d）", f);
		CHECK(ds.bug_hard + ds.bug_soft == 0, "fuzz CHUNK 无疑似缺陷（得 %d）",
		      ds.bug_hard + ds.bug_soft);
		printf("  [fuzz] CHUNK 文档化 hard=%d soft=%d；疑似缺陷 hard=%d soft=%d\n",
		       ds.doc_hard, ds.doc_soft, ds.bug_hard, ds.bug_soft);
		o_fini(&sh);
		env_destroy(&e);
	}
}

/* ═══════════════════════════ 并发套件 ═══════════════════════════ */

#define MAX_THREADS 16
#define MAX_HELD 8192

typedef struct {
	env_t	*e;
	oracle_t *shadow;		/* 仅用于统计用（本线程不并发写） */
	u8	*seen;			/* 全局位「当前已被分配」原子标记 */
	u64	 nbits;
	int	 tid;
	int	 iters;
	int	 delay;
	u64	 seed;
	/* 统计 */
	u64	 alloc_ok, free_ok, alloc_fail;
	int	 errs;
	struct { u64 pos, len; int ino; } held[MAX_HELD];
	int	 nheld;
} worker_t;

static volatile int g_double_alloc;

static void w_free(worker_t *w);

static void w_alloc(worker_t *w, int ino)
{
	u64 cnt = 1 + rnd_u64(&w->seed) % 64;
	u64 align = rnd_u64(&w->seed) % 4;
	u64 p, base;

	if (w->nheld >= MAX_HELD) {	/* 容量兜底：先腾位，绝不“未记录即泄漏” */
		w_free(w);
		return;
	}
	if (ino)
		p = rfs_bm_inode_alloc(&w->e->bm);
	else
		p = rfs_bm_block_alloc_hard(&w->e->bm, cnt, align);
	if (p == RFS_BM_NO_HIT) {
		w->alloc_fail++;
		return;
	}
	if (ino)
		cnt = 1;
	/* block/inode 两套命名空间：seen 用 inode 偏移错开（否则假双重分配） */
	base = ino ? w->nbits : 0;
	{
		u64 j;

		for (j = 0; j < cnt; j++) {
			u8 old = __atomic_exchange_n(&w->seen[base + p + j], 1,
						     __ATOMIC_SEQ_CST);

			if (old) {
				__atomic_store_n(&g_double_alloc, 1, __ATOMIC_SEQ_CST);
				w->errs++;
			}
		}
	}
	w->held[w->nheld].pos = p;
	w->held[w->nheld].len = cnt;
	w->held[w->nheld].ino = ino;
	w->nheld++;
	w->alloc_ok++;
}

static void w_free(worker_t *w)
{
	int k;

	if (!w->nheld)
		return;
	k = (int)(rnd_u64(&w->seed) % (u64)w->nheld);
	{
		u64 p = w->held[k].pos, len = w->held[k].len;
		int ino = w->held[k].ino, j;
		u64 base = ino ? w->nbits : 0;

		/* 先清 seen（防竞态误报），再设备释放 */
		for (j = 0; j < (int)len; j++)
			__atomic_store_n(&w->seen[base + p + j], 0, __ATOMIC_SEQ_CST);
		if (ino)
			rfs_bm_inode_free(&w->e->bm, p);
		else
			rfs_bm_block_free(&w->e->bm, p, len);
		w->free_ok++;
	}
	w->held[k] = w->held[w->nheld - 1];
	w->nheld--;
}

static void *worker_main(void *arg)
{
	worker_t *w = arg;
	int i;

	pin_to_allowed(w->tid);
	for (i = 0; i < w->iters; i++) {
		u64 r = rnd_u64(&w->seed) % 100;

		if (r < 50)
			w_alloc(w, (r % 7) == 0);
		else
			w_free(w);
	}
	/* 收尾释放全部持有 */
	while (w->nheld)
		w_free(w);
	return NULL;
}

static int run_concurrency(int modechunk, u32 nthr, int iters, int delay, u64 seed)
{
	env_t e;
	worker_t *w;
	pthread_t th[MAX_THREADS];
	u8 *seen;
	int rc, i, errs = 0;

	if (modechunk) {
		rc = env_build_chunk(&e, 16, 16, nthr, 0, 0, delay);
	} else {
		rc = env_build_mem(&e, 16, 16, nthr);
	}
	CHECK(rc == 0, "并发 init mode=%d", modechunk);
	if (rc)
		return 1;

	seen = calloc((size_t)(e.blk_nbits + e.ino_nbits), 1);
	w = calloc(nthr, sizeof(*w));
	g_double_alloc = 0;
	for (i = 0; i < (int)nthr; i++) {
		w[i].e = &e;
		w[i].seen = seen;
		w[i].nbits = e.blk_nbits;
		w[i].tid = i;
		w[i].iters = iters;
		w[i].delay = delay;
		w[i].seed = seed + (u64)i * 2654435761ULL + 1;
	}
	for (i = 0; i < (int)nthr; i++)
		pthread_create(&th[i], NULL, worker_main, &w[i]);
	for (i = 0; i < (int)nthr; i++)
		pthread_join(th[i], NULL);

	for (i = 0; i < (int)nthr; i++) {
		CHECK(w[i].errs == 0, "thread %d 无（双重分配）错误（errs=%d）", i, w[i].errs);
		CHECK(w[i].nheld == 0, "thread %d 收尾已释放全部", i);
		errs += w[i].errs;
	}
	CHECK(!g_double_alloc, "并发无双重分配（全局）");
	if (modechunk)
		CHECK(rfs_bm_chunks_asyn(&e.bm) == 0, "并发后 asyn");
	/* 收尾：全部释放后应全空闲 */
	CHECK(dev_free_total(&e, false) == e.blk_nbits,
	      "并发收尾 block 全空闲（%llu/%llu）",
	      (unsigned long long)dev_free_total(&e, false),
	      (unsigned long long)e.blk_nbits);
	{
		u8 *dev = malloc(e.blk_nbits);
		u64 z = 0, j;

		dev_get_bits(&e, false, dev);
		for (j = 0; j < e.blk_nbits; j++)
			z += dev[j];
		CHECK(z == 0, "并发收尾 block 位态全零（%llu 占用残留）",
		      (unsigned long long)z);
		z = 0;
		for (j = 0; j < e.blk_nbits + e.ino_nbits; j++)
			z += seen[j];
		CHECK(z == 0, "并发收尾 seen 全零（含 inode）");
		free(dev);
	}
	free(seen);
	free(w);
	env_destroy(&e);
	return errs;
}

static void suite_concurrency(void)
{
	u32 maxth = g_light ? 8u : (u32)MAX_THREADS;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	u32 nthr = (u32)(ncpu > (long)maxth ? (long)maxth : ncpu);
	int iters = g_full ? 8000 : (g_light ? 1200 : 3000);

	printf("[suite] concurrency (threads=%u)\n", nthr);
	printf("  MEM（spin 片锁）…\n");
	run_concurrency(0, nthr, iters, 0, 0x1111ULL);
	printf("  CHUNK（mutex 片锁 + fake 盘随机延迟）…\n");
	run_concurrency(1, nthr, iters, 25, 0x2222ULL);
}

/* ═══════════════════════════ main ═══════════════════════════ */

static void banner(const char *mode)
{
	printf("=== bm_test mode=%s (nproc=%ld) ===\n", mode, sysconf(_SC_NPROCESSORS_ONLN));
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "quick";
	const char *v = getenv("BM_VERBOSE");

	g_verbose = v && v[0] == '1';
	pin_to_cpu(first_allowed_cpu());
	signal(SIGALRM, on_alarm);
	banner(mode);
	g_checks = g_fail = 0;

	if (!strcmp(mode, "quick")) {
		suite_geometry();
		suite_boundary();
		suite_invariants();
		suite_regression_bug1();
		suite_diff_mem();
		suite_diff_chunk();
		suite_persistence();
		suite_chunk_windows();
		suite_synth_view();
		suite_free_count();
	} else if (!strcmp(mode, "full")) {
		g_full = 1;
		alarm(600);
		suite_geometry();
		suite_boundary();
		suite_invariants();
		suite_regression_bug1();
		suite_diff_mem();
		suite_diff_chunk();
		suite_persistence();
		suite_chunk_windows();
		suite_synth_view();
		suite_free_count();
		suite_fuzz();
		suite_concurrency();
	} else if (!strcmp(mode, "tsan")) {
		g_light = 1;
		alarm(900);
		suite_concurrency();
		suite_fuzz();
	} else if (!strcmp(mode, "san")) {
		g_light = 1;
		alarm(900);
		suite_geometry();
		suite_boundary();
		suite_invariants();
		suite_regression_bug1();
		suite_diff_mem();
		suite_diff_chunk();
		suite_persistence();
		suite_chunk_windows();
		suite_synth_view();
		suite_free_count();
		suite_fuzz();
		suite_concurrency();
	} else {
		fprintf(stderr, "usage: %s quick|full|tsan|san\n", argv[0]);
		return 2;
	}

	printf("=== TOTAL: %d checks, %d failures ===\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
