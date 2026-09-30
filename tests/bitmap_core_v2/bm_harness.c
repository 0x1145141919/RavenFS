// SPDX-License-Identifier: GPL-2.0
/*
 * bm_harness.c —— v2 测试夹具实现（fake disk / 独立 MEM oracle / env）
 * 见 bm_harness.h 顶注释。刻意不 include 任何被测 .c；只链接它。
 */
#include "bm_harness.h"

int g_checks, g_fail;

/* ═══════════════════════════ fake disk ═══════════════════════════ */

void fd_init(fake_disk_t *d, u64 nblk, int delay_us)
{
	d->nblk = nblk;
	d->delay = delay_us;
	d->jiffies = 0;
	d->reads = 0;
	d->writes = 0;
	d->mem = calloc((size_t)nblk, (size_t)BM_BLKSZ);
}

void fd_free(fake_disk_t *d)
{
	free(d->mem);
	d->mem = NULL;
}

static void fd_wait(fake_disk_t *d)
{
	u64 c;
	struct timespec ts;

	if (!d->delay)
		return;
	c = __atomic_add_fetch(&d->jiffies, 1, __ATOMIC_RELAXED);
	ts.tv_sec = 0;
	ts.tv_nsec = (long)((c * 2654435761ULL) % (u64)d->delay) * 1000L;
	nanosleep(&ts, NULL);
}

int fd_read(void *ctx, u64 blk, u64 blk_len, void *buf)
{
	fake_disk_t *d = ctx;

	if (!d || !d->mem || blk + blk_len > d->nblk)
		return -EIO;
	fd_wait(d);
	memcpy(buf, d->mem + blk * BM_BLKSZ, (size_t)(blk_len * BM_BLKSZ));
	__atomic_add_fetch(&d->reads, 1, __ATOMIC_RELAXED);
	return 0;
}

int fd_write(void *ctx, u64 blk, u64 blk_len, const void *buf)
{
	fake_disk_t *d = ctx;

	if (!d || !d->mem || blk + blk_len > d->nblk)
		return -EIO;
	fd_wait(d);
	memcpy(d->mem + blk * BM_BLKSZ, buf, (size_t)(blk_len * BM_BLKSZ));
	__atomic_add_fetch(&d->writes, 1, __ATOMIC_RELAXED);
	return 0;
}

/* ═══════════════════════════ oracle ═══════════════════════════ */

static u64 o_align_up(u64 x, u64 a)
{
	return (x + a - 1) & ~(a - 1);
}

static u64 o_slice_lo(const oracle_t *o, u32 idx)
{
	return (u64)idx * o->per_bits;
}

static u64 o_slice_hi(const oracle_t *o, u32 idx)
{
	u64 hi = o_slice_lo(o, idx) + o->per_bits;

	return hi < o->nbits ? hi : o->nbits;
}

/* 首空闲段 [s,e)，从 from 起、上限 hi；max_extend>0 截断段长。镜像 MEM next_free_seg。 */
static bool o_next_free_seg(const oracle_t *o, u64 hi, u64 from, u64 max_extend,
			    u64 *s, u64 *e)
{
	u64 i = from;

	if (i >= hi)
		return false;
	while (i < hi && o->b[i])
		i++;
	if (i >= hi)
		return false;
	*s = i;
	*e = i;
	while (*e < hi && !o->b[*e]) {
		(*e)++;
		if (max_extend && (*e - *s) >= max_extend)
			break;
	}
	return true;
}

/* 找 [cand, cand+count) 全空闲、cand%align==0；镜像 MEM find_free_run。 */
static u64 o_find_free_run(const oracle_t *o, u64 hi, u64 from, u64 count, u64 align)
{
	u64 probe = from, s, e;

	if (count == 0 || count > hi || align == 0)
		return RFS_BM_NO_HIT;
	while (o_next_free_seg(o, hi, probe, count + align, &s, &e)) {
		u64 cand = o_align_up(s, align);

		if (cand < e && cand + count <= e)
			return cand;
		probe = e;
	}
	return RFS_BM_NO_HIT;
}

void o_init(oracle_t *o, u64 nbits, u64 per_bits, u32 n_slices, u32 cpu_slots,
	    const u64 *raw_words)
{
	u32 i;
	u64 j;

	memset(o, 0, sizeof(*o));
	o->nbits = nbits;
	o->per_bits = per_bits;
	o->n_slices = n_slices ? n_slices : 1;
	o->cpu_slots = cpu_slots ? cpu_slots : 1;
	o->b = calloc((size_t)nbits, 1);
	o->free_count = calloc(o->n_slices, sizeof(u64));
	o->hint = calloc(o->n_slices, sizeof(u64));
	o->preferred = calloc(o->cpu_slots, sizeof(u32));
	if (raw_words) {
		for (j = 0; j < nbits; j++)
			o->b[j] = (u8)((raw_words[j >> 6] >> (j & 63)) & 1ULL);
	}
	for (i = 0; i < o->n_slices; i++) {
		u64 lo = o_slice_lo(o, i), hi = o_slice_hi(o, i), n = 0;

		for (j = lo; j < hi; j++)
			if (!o->b[j])
				n++;
		o->free_count[i] = n;
		o->hint[i] = lo;
	}
	for (i = 0; i < o->cpu_slots; i++)
		o->preferred[i] = i % o->n_slices;
}

void o_fini(oracle_t *o)
{
	free(o->b);
	free(o->free_count);
	free(o->hint);
	free(o->preferred);
	memset(o, 0, sizeof(*o));
}

u64 o_alloc_hard(oracle_t *o, u64 count, u64 align_log2)
{
	u64 align; u32 cpu, round;

	if (!count)
		return RFS_BM_NO_HIT;
	align = 1ULL << (align_log2 > 16 ? 16 : align_log2);
	cpu = RFS_CPU_ID() % o->cpu_slots;
	for (round = 0; round < o->n_slices; round++) {
		u32 idx = (u32)((o->preferred[cpu] + round) % o->n_slices);
		u64 lo = o_slice_lo(o, idx), hi = o_slice_hi(o, idx);
		u64 bits = hi > lo ? hi - lo : 0, from, hit, j;

		if (count > bits)
			continue;
		if (o->free_count[idx] < count)
			continue;
		from = o->hint[idx] < lo ? lo : o->hint[idx];
		if (from > hi)
			from = hi;
		hit = o_find_free_run(o, hi, from, count, align);
		if (hit == RFS_BM_NO_HIT)
			hit = o_find_free_run(o, from, lo, count, align);
		if (hit == RFS_BM_NO_HIT)
			continue;
		for (j = 0; j < count; j++)
			o->b[hit + j] = 1;
		o->free_count[idx] -= count;
		o->hint[idx] = hit + count;
		o->preferred[cpu] = idx;
		return hit;
	}
	return RFS_BM_NO_HIT;
}

u64 o_alloc_soft(oracle_t *o, u64 count, u64 *remain)
{
	u32 cpu, round;

	if (remain)
		*remain = count;
	if (!count)
		return RFS_BM_NO_HIT;
	cpu = RFS_CPU_ID() % o->cpu_slots;
	for (round = 0; round < o->n_slices; round++) {
		u32 idx = (u32)((o->preferred[cpu] + round) % o->n_slices);
		u64 lo = o_slice_lo(o, idx), hi = o_slice_hi(o, idx);
		u64 bits = hi > lo ? hi - lo : 0, from, hit = 0, e = 0, got, j;
		u64 s, e2;
		bool ok = false;

		if (count > bits || o->free_count[idx] == 0)
			continue;
		from = o->hint[idx] < lo ? lo : o->hint[idx];
		if (from > hi)
			from = hi;
		if (o_next_free_seg(o, hi, from, count, &s, &e2))
			{ hit = s; e = e2; ok = true; }
		else if (o_next_free_seg(o, from, lo, count, &s, &e2))
			{ hit = s; e = e2; ok = true; }
		if (!ok)
			continue;
		got = e - hit;
		if (got > count)
			got = count;
		for (j = 0; j < got; j++)
			o->b[hit + j] = 1;
		o->free_count[idx] -= got;
		o->hint[idx] = hit + got;
		o->preferred[cpu] = idx;
		if (remain)
			*remain = count - got;
		return hit;
	}
	return RFS_BM_NO_HIT;
}

u64 o_occupy_soft(oracle_t *o, u64 base, u64 count, u64 *rest)
{
	u64 b = base, remain = count;

	if (rest)
		*rest = count;
	if (!count)
		return RFS_BM_NO_HIT;
	while (remain > 0) {
		u32 idx = (u32)(b / o->per_bits);
		u64 lo, hi, cap, k = 0, j;

		if (idx >= o->n_slices)
			break;
		lo = o_slice_lo(o, idx);
		hi = o_slice_hi(o, idx);
		if (b < lo || b >= hi)
			break;
		cap = hi - b;
		if (cap > remain)
			cap = remain;
		while (k < cap && !o->b[b + k])
			k++;
		if (k == 0)
			break;
		for (j = 0; j < k; j++)
			o->b[b + j] = 1;
		o->free_count[idx] -= k;
		if (b + k > o->hint[idx])
			o->hint[idx] = b + k;
		b += k;
		remain -= k;
		if (remain == 0)
			break;
		if (b != hi)
			break;
	}
	if (rest)
		*rest = remain;
	return (count - remain > 0) ? base : RFS_BM_NO_HIT;
}

void o_block_free(oracle_t *o, u64 base, u64 count)
{
	while (count > 0) {
		u32 idx;
		u64 hi, n, cleared = 0, j;

		if (base >= o->nbits)
			return;
		idx = (u32)(base / o->per_bits);
		if (idx >= o->n_slices)
			return;
		hi = o_slice_hi(o, idx);
		n = (base + count <= hi) ? count : hi - base;
		if (!n)
			return;
		for (j = 0; j < n; j++) {
			if (o->b[base + j]) {
				o->b[base + j] = 0;
				cleared++;
			}
		}
		o->free_count[idx] += cleared;
		base += n;
		count -= n;
	}
}

bool o_test(const oracle_t *o, u64 bit)
{
	return bit < o->nbits ? (bool)o->b[bit] : false;
}

u64 o_free_total(const oracle_t *o)
{
	u64 s = 0; u32 i;

	for (i = 0; i < o->n_slices; i++)
		s += o->free_count[i];
	return s;
}

void o_apply_alloc(oracle_t *o, u64 pos, u64 count)
{
	u64 j;

	for (j = 0; j < count && pos + j < o->nbits; j++)
		o->b[pos + j] = 1;
}

void o_apply_free(oracle_t *o, u64 base, u64 count)
{
	u64 j;

	for (j = 0; j < count && base + j < o->nbits; j++)
		o->b[base + j] = 0;
}

u64 o_zeros_total(const oracle_t *o)
{
	u64 n = 0, j;

	for (j = 0; j < o->nbits; j++)
		if (!o->b[j])
			n++;
	return n;
}

u64 o_used_total(const oracle_t *o)
{
	u64 n = 0, j;

	for (j = 0; j < o->nbits; j++)
		if (o->b[j])
			n++;
	return n;
}

/* ═══════════════════════════ env ═══════════════════════════ */

void words_zero(u8 *words, u64 nbits)
{
	memset(words, 0, (size_t)(nbits >> 3));
}

void words_set_range(u8 *words, u64 start, u64 len)
{
	u64 j;

	for (j = 0; j < len; j++) {
		u64 b = start + j;

		words[b >> 3] |= (u8)(1u << (b & 7));
	}
}

static int env_alloc_words(env_t *e)
{
	void *p;

	if (posix_memalign(&p, 4096, (size_t)(e->blk_nbits >> 3)) != 0)
		return -ENOMEM;
	e->blk_words = p;
	words_zero(e->blk_words, e->blk_nbits);
	if (posix_memalign(&p, 4096, (size_t)(e->ino_nbits >> 3)) != 0)
		return -ENOMEM;
	e->ino_words = p;
	words_zero(e->ino_words, e->ino_nbits);
	return 0;
}

int env_prep_mem(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots)
{
	memset(e, 0, sizeof(*e));
	e->blk_nbits = blk_blocks << 15;
	e->ino_nbits = ino_blocks << 15;
	e->cpu_slots = cpu_slots ? cpu_slots : 1;
	e->is_chunk = 0;
	return env_alloc_words(e);
}

int env_prep_chunk(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots,
		   u8 blk_order, u8 ino_order, int delay)
{
	memset(e, 0, sizeof(*e));
	e->blk_nbits = blk_blocks << 15;
	e->ino_nbits = ino_blocks << 15;
	e->cpu_slots = cpu_slots ? cpu_slots : 1;
	e->chunk_order_blk = blk_order;
	e->chunk_order_ino = ino_order;
	e->is_chunk = 1;
	/* 盘：块位图区 [0, blk_blocks) + inode 区 [blk_blocks, +ino_blocks) + 余量 */
	fd_init(&e->disk, blk_blocks + ino_blocks + 8, delay);
	if (env_alloc_words(e) != 0)
		return -ENOMEM;
	/* 盘上初值 = words 序列化（enable 会读；此处零；调用方可 words_set_range
	 * 后由 env_go 重新序列化） */
	return 0;
}

static void env_serialize_chunk(env_t *e)
{
	/* words → 盘（位图区按块落位）。blk 区从块 0，ino 区从块 blk_blocks。 */
	memcpy(e->disk.mem, e->blk_words, (size_t)(e->blk_nbits >> 3));
	memcpy(e->disk.mem + (e->blk_nbits >> 15) * BM_BLKSZ, e->ino_words,
	       (size_t)(e->ino_nbits >> 3));
}

int env_go(env_t *e)
{
	struct rfs_bm_cfg cfg;
	int rc;

	memset(&cfg, 0, sizeof(cfg));
	if (!e->is_chunk) {
		cfg.choose = RFS_BM_MODE_MEM;
		cfg.pure_mem_cfg.block_bits_raw = e->blk_words;
		cfg.pure_mem_cfg.block_bits_count = e->blk_nbits;
		cfg.pure_mem_cfg.inode_bits_raw = e->ino_words;
		cfg.pure_mem_cfg.inode_bits_count = e->ino_nbits;
		cfg.pure_mem_cfg.logical_processor_count = e->cpu_slots;
		cfg.pure_mem_cfg.min_slice_bits = 0;
	} else {
		cfg.choose = RFS_BM_MODE_CHUNK;
		if (!e->keep_disk)
			env_serialize_chunk(e);
		cfg.chunk_cfg.read_block = fd_read;
		cfg.chunk_cfg.write_block = fd_write;
		cfg.chunk_cfg.io_ctx = &e->disk;
		cfg.chunk_cfg.block_bitmap_base_FS_block = 0;
		cfg.chunk_cfg.block_bitmap_blocks = e->blk_nbits >> 15;
		cfg.chunk_cfg.inode_bitmap_base_FS_block = e->blk_nbits >> 15;
		cfg.chunk_cfg.inode_bitmap_blocks = e->ino_nbits >> 15;
		cfg.chunk_cfg.blkbm_chunk_order = e->chunk_order_blk;
		cfg.chunk_cfg.inobm_chunk_order = e->chunk_order_ino;
		cfg.chunk_cfg.processor_count = e->cpu_slots;
	}
	rc = rfs_bm_init(&e->bm, &cfg);
	return rc;
}

int env_build_mem(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots)
{
	int rc = env_prep_mem(e, blk_blocks, ino_blocks, cpu_slots);

	if (rc)
		return rc;
	return env_go(e);
}

int env_build_chunk(env_t *e, u64 blk_blocks, u64 ino_blocks, u32 cpu_slots,
		    u8 blk_order, u8 ino_order, int delay)
{
	int rc = env_prep_chunk(e, blk_blocks, ino_blocks, cpu_slots,
				blk_order, ino_order, delay);

	if (rc)
		return rc;
	e->chunk_order_blk = blk_order;
	e->chunk_order_ino = ino_order;
	return env_go(e);
}

void env_destroy(env_t *e)
{
	if (e->bm.blk.n_slices || e->bm.ino.n_slices)
		rfs_bm_fini(&e->bm);
	if (e->is_chunk)
		fd_free(&e->disk);
	free(e->blk_words);
	free(e->ino_words);
	memset(e, 0, sizeof(*e));
}

static void packed_expand(const u8 *packed, u64 base, u64 bits, u8 *out)
{
	u64 j;

	for (j = 0; j < bits; j++)
		out[base + j] = (u8)((packed[j >> 3] >> (j & 7)) & 1u);
}

void dev_get_bits(const env_t *e, bool ino, u8 *out)
{
	const struct rfs_bm_group *g = ino ? &e->bm.ino : &e->bm.blk;
	u8 *tmp = malloc((size_t)(g->per_bits >> 3));
	u32 i;

	for (i = 0; i < g->n_slices; i++) {
		u64 base = (u64)i * g->per_bits;
		u64 bits = base >= g->nbits ? 0 : min(g->per_bits, g->nbits - base);

		if (!bits)
			continue;
		memset(tmp, 0, (size_t)(bits >> 3));
		rfs_bm_snapshot_slice(&e->bm, ino, i, tmp);
		packed_expand(tmp, base, bits, out);
	}
	free(tmp);
}

u64 dev_free_total(const env_t *e, bool ino)
{
	return ino ? rfs_bm_inode_free_total(&e->bm) : rfs_bm_block_free_total(&e->bm);
}

/* ═══════════════════════════ 杂项 ═══════════════════════════ */

u64 rnd_u64(u64 *state)
{
	u64 x = *state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return x;
}

void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)sched_setaffinity(0, sizeof(set), &set);
}

int first_allowed_cpu(void)
{
	cpu_set_t set;
	int i;

	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) != 0)
		return 0;
	for (i = 0; i < CPU_SETSIZE; i++)
		if (CPU_ISSET(i, &set))
			return i;
	return 0;
}

static int allowed_cpu_nth(int idx)
{
	cpu_set_t set;
	int i, n = 0;

	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) != 0)
		return 0;
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &set)) {
			if (n == idx)
				return i;
			n++;
		}
	}
	return first_allowed_cpu();
}

void pin_to_allowed(int idx)
{
	pin_to_cpu(allowed_cpu_nth(idx));
}

u64 now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}
