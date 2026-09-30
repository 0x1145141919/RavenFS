// SPDX-License-Identifier: GPL-2.0
/*
 * sim/bitmap_chunk_prefix.c —— [PRE-FIX 模拟，非冻结产物！]
 *
 * 由 ../../linux/bitmap_chunk.c 机械变换得到：仅去掉 BUG-1 修复处的 claimed>0 守卫
 * （原： if (claimed > 0 && anchor > cs->in_slice_hint) / 模拟：if (anchor > cs->in_slice_hint)）。
 * 目的 = 为 regression_bug1 提供「修前行为」对照证据；**不参与默认构建，不修改冻结源**。
 */
// SPDX-License-Identifier: GPL-2.0
/*
 * bitmap_chunk.c —— RavenFS 双位图分片分配器 · **CHUNK 后端**（= god v2 窗口版）
 *
 * 对应 god `juvenile_god/bitmap_slices_v2.cpp`：`chunk_slice_mgr_t`（窗口机全部）+
 * `bitmap_slices_mgr_t_v2` 与 v2 相关的部分。位图**懒加载窗口（CHUNK）**：类自持
 * 每片窗口缓冲 + 归还记录链；I/O 经赋能式回调（read_block/write_block + io_ctx）。
 *
 * 坐标：**片内局部（0 起）**——片内算法/入参出参均为片内偏移；全局基址外化为
 * `in_global_base_idx`（enable 写入，片内不消费）。global↔local 换算**一律**由路由层
 * （bitmap_core.c）承担，本文件不碰全局位号（"片与片编译期零引用"）。
 *
 * ★ 视角（**本后端的位态/计数不是内存里的直接事实**）：它是「在线窗 active_chunk_buffer
 *   + 挂账归还链 rec_head + 盘上块」在**合并点**（使用该窗前 drain / chunks_asyn）
 *   **合成（synthesized）**出来的视角。挂账期读数可与真值暂不一致：free 挂账即记账
 *   （计数可虚高）、被释放的位在合并前仍为 1。判定/比对须按**合成后（提交态）**视角
 *   （见 ravenfs_bitmap_core.h ★★ 语义视角）。—— 与 MEM 后端（内存即一等事实）勿混。
 *
 * ── 与 god 的**刻意差异**（文档化，非语义差异）──
 *   1. 锁：god v2 分配走 `std::mutex::try_lock`（拿不到 = 本片忙 → 返 RC_ERR_BUSY，
 *      mgr 换片）；ko 口径改为**片内阻塞 `rfs_mutex_t`**（可睡，允许持锁等盘 I/O，
 *      与 linux/bitmap.c 的阻塞纪律一致，2026-09-28 用户裁）⇒ 无 BUSY 分支。
 *   2. 内存：god 用 `posix_memalign`（buffer/临时缓冲）；ko 统一走 `RFS_ZALLOC`
 *      （内核 kcalloc / 用户态 calloc）——窗口缓冲尺寸 = 4096<<order，页对齐足够
 *      位运算；临时整片缓冲尺寸 = slice_bits/8（大，走同一宏，内核侧 kcalloc）。
 *   3. 返回码：RC_* → 0 / 负 -errno；换片状态机内部 `-1` → `-EIO`。
 *   4. chunks_asyn 的 `free_count_calib_t` 出参：冻结公开 API 无此出参 ⇒ ko 侧以
 *      RFS_LOG 上报 [旧, 新] 计数漂移（见 cs_chunks_asyn）。
 *   5. 归还节点分配：god `new (std::nothrow)`；ko `RFS_ZALLOC`（calloc/kcalloc）后
 *      赋字段（等价，零初始化）。
 *   其余语义（唯一在线窗 / 免费一律挂链 / 合并点在使用前 / 单窗语义 / align ≤15
 *   夹断 / 摘要免载入 / 尾块公式 / 记账 [init 实测·alloc 扣减·free 挂账·asyn 校准]）
 *   逐条照 god v2。
 */
#include "ravenfs_bitmap_backend.h"

/* win_* 私有返回码（仅本文件内部；god 头未枚举） */
#define CS_WIN_OK	0	/* 认领 ≥1 位（soft_occupy） */
#define CS_WIN_BAD	(-1)	/* 参数非法 */
#define CS_WIN_EMPTY	(-2)	/* 本窗无货（认领 0 位；≠ 片完） */

/* ═══════════════════════════ 位运算轮子（static）═══════════════════════════ */

static inline bool chunk_bit_get(const u64 *w, u64 i)
{
	return (w[i >> 6] >> (i & 63)) & 1ULL;
}

static inline void chunk_bit_set(u64 *w, u64 i)
{
	w[i >> 6] |= 1ULL << (i & 63);
}

/* 置 1 计数：chunk 窗口位数恒 64 倍数 ⇒ 整字 popcount，零残字分支 */
static u64 chunk_count_ones(const u64 *words, u64 nbits)
{
	u64 ones = 0, nwords = nbits >> 6, w;

	for (w = 0; w < nwords; w++)
		ones += RFS_POPCOUNT64(words[w]);
	return ones;
}

static u64 chunk_count_zeros(const u64 *words, u64 nbits)
{
	return nbits - chunk_count_ones(words, nbits);
}

/*
 * 从 from 起找下一个空闲段 [*seg_s, *seg_e)（字粒度，整字占用跳过；末字按 nbits
 * 截位）。max_extend > 0 段长够即截断（防稀疏大位图 O(n²) 热点，v1 注记保留）。
 * 移植自 god bitmap_slices_v2.cpp next_free_seg。
 */
static bool chunk_next_free_seg(const u64 *words, u64 nbits, u64 from,
				u64 max_extend, u64 *seg_s, u64 *seg_e)
{
	u64 nwords = (nbits + 63) >> 6;
	u64 w = from >> 6;
	u64 word, start, end;

	if (w >= nwords)
		return false;
	word = ~words[w];
	if (w == nwords - 1 && (nbits & 63))
		word &= (1ULL << (nbits & 63)) - 1;
	word &= ~0ULL << (from & 63);
	if (word == 0) {
		for (w++; w < nwords; w++) {
			word = ~words[w];
			if (w == nwords - 1 && (nbits & 63))
				word &= (1ULL << (nbits & 63)) - 1;
			if (word)
				break;
		}
		if (w >= nwords)
			return false;
	}
	start = (w << 6) + (u64)__builtin_ctzll(word);

	end = start;
	for (;;) {
		u64 ew, eword, free_mask, run;

		if (end >= nbits) {
			end = nbits;
			break;
		}
		if (max_extend && (end - start) >= max_extend)
			break;
		ew = end >> 6;
		eword = ~words[ew];
		if (ew == nwords - 1 && (nbits & 63))
			eword &= (1ULL << (nbits & 63)) - 1;
		free_mask = eword >> (end & 63);
		if ((free_mask & 1) == 0)
			break;
		run = (free_mask == ~0ULL) ? 64 : (u64)__builtin_ctzll(~free_mask);
		end += run;
		if (end & 63)
			break;
	}
	if (end > nbits)
		end = nbits;
	*seg_s = start;
	*seg_e = end;
	return true;
}

/* 从 from 起找 count 连续空闲位、起始 align 对齐；段粒度取对齐候选（避免 O(n×count) */
static u64 chunk_find_free_run(const u64 *words, u64 nbits, u64 from, u64 count, u64 align)
{
	u64 cap, probe, seg_s, seg_e;

	if (count == 0 || count > nbits || align == 0)
		return RFS_BM_NO_HIT;
	cap = count + align;
	probe = from;
	while (chunk_next_free_seg(words, nbits, probe, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e && cand + count <= seg_e)
			return cand;
		probe = seg_e;
	}
	return RFS_BM_NO_HIT;
}

/* 从 off 起的连续空闲前缀长 k（≤ cap；撞占用位停）——v1 soft_occupy 镜像。
 * 窗口位数恒 64 倍数且 off+cap ≤ 窗内 ⇒ 读字下标恒在窗内。 */
static u64 chunk_greedy_free_prefix(const u64 *words, u64 off, u64 cap)
{
	u64 k = 0;

	while (k < cap) {
		u64 pos = (off + k) & 63;
		u64 free_mask = (~words[(off + k) >> 6]) >> pos;	/* 1 = 空闲 */
		u64 run;

		if ((free_mask & 1) == 0)
			break;
		run = (free_mask == ~0ULL) ? (64 - pos) : (u64)__builtin_ctzll(~free_mask);
		k += run;
		if (k > cap)
			k = cap;
	}
	return k;
}

/* ═══════════════════════════ 几何（片内；enable 派生）═══════════════════════════ */

static u64 cs_chunk_bits(const struct rfs_bm_cslice *cs)
{
	return 32768ULL << cs->soul.chunk_order;
}

static u32 cs_shift(const struct rfs_bm_cslice *cs)
{
	return (u32)15 + cs->soul.chunk_order;
}

static u32 cs_ctl_of(const struct rfs_bm_cslice *cs, u64 slice_off)
{
	return (u32)(slice_off >> cs_shift(cs));
}

static u64 cs_limit_of(const struct rfs_bm_cslice *cs, u32 ctl)
{
	return (ctl + 1u == cs->chunks_per_slice) ? cs->tail_bits : cs_chunk_bits(cs);
}

static u64 cs_chunk_disk_blk(const struct rfs_bm_cslice *cs, u32 ctl)
{
	return cs->soul.chunk_base_blk + ((u64)ctl << cs->soul.chunk_order);
}

/* ═══════════════════════════ 记录 / 摘要 / 资源 ═══════════════════════════ */

/*
 * 2bit 摘要重算（03 §7）：链上有记录 → 恒 MIXED（防「盘态 ALL_USED 其实有待还空闲」
 * 被免载入跳过 → 错误 ENOSPC）；否则按 src（下标 = 窗首起）统计全 1/全 0/混合。
 * 同步 HAS_RECS 位与链实况一致。
 */
static void chunk_summarize(struct rfs_bm_cmgr *m, const void *src, u64 nbits)
{
	u64 sum, cw;

	if (m->rec_head) {
		sum = RFS_BM_CHUNK_SUM_MIXED;
	} else {
		u64 ones = chunk_count_ones((const u64 *)src, nbits);

		sum = (ones == nbits) ? RFS_BM_CHUNK_SUM_ALL_USED
		    : (ones == 0) ? RFS_BM_CHUNK_SUM_ALL_FREE
		    : RFS_BM_CHUNK_SUM_MIXED;
	}
	cw = m->compact_word & ~(RFS_BM_CHUNK_SUM_MASK | RFS_BM_CHUNK_FLAG_HAS_RECS);
	cw |= sum;
	if (m->rec_head)
		cw |= RFS_BM_CHUNK_FLAG_HAS_RECS;
	m->compact_word = cw;
}

/*
 * 归还链清位（**不摘节点**）：words 以 idx_base 定位——整片暂存缓冲传 0（片内坐标
 * 直接当字下标），在线窗缓冲传 wlo（chunk 内局部 = 片内 − 窗首）。节点摘除由
 * chunk_free_recs 负责（asyn/shutdown 先清位、写回成功才摘账）。
 */
static void chunk_apply_recs(const struct rfs_bm_rec *head, u64 *words, u64 idx_base)
{
	const struct rfs_bm_rec *n;

	for (n = head; n; n = n->next) {
		u64 i;

		for (i = 0; i < n->len; i++) {
			u64 idx = n->off + i - idx_base;

			words[idx >> 6] &= ~(1ULL << (idx & 63));
		}
	}
}

/* 摘链 + 释放全部节点 + 同步 HAS_RECS（链操作全在片锁内） */
static void chunk_free_recs(struct rfs_bm_cmgr *m)
{
	struct rfs_bm_rec *p = m->rec_head;

	while (p) {
		struct rfs_bm_rec *nx = p->next;

		RFS_FREE(p);
		p = nx;
	}
	m->rec_head = NULL;
	m->compact_word &= ~RFS_BM_CHUNK_FLAG_HAS_RECS;
}

/* 收资源（enable 失败回滚 / disable 共用）：全片链节点 + mgrs[] + buffer */
static void chunk_release_resources(struct rfs_bm_cmgr *mgrs, u32 nwin, u8 *buf)
{
	if (mgrs) {
		u32 i;

		for (i = 0; i < nwin; i++)
			chunk_free_recs(&mgrs[i]);
		RFS_FREE(mgrs);
	}
	RFS_FREE(buf);
}

/* ═══════════════════════════ 换片状态机（须持片锁）═══════════════════════════ */

/* drain_chunk：合并本 chunk 归还链（该窗内容须已在 active_chunk_buffer）。
 * 逐节点清位 + 释放节点；free_count 挂链时已记账，此处不动（★ 记账）。 */
static void cs_drain_chunk(struct rfs_bm_cslice *cs, u32 ctl)
{
	u64 wlo;

	if (!cs->mgrs || !cs->active_chunk_buffer || ctl >= cs->chunks_per_slice)
		return;
	if (!cs->mgrs[ctl].rec_head)
		return;
	wlo = (u64)ctl << cs_shift(cs);
	chunk_apply_recs(cs->mgrs[ctl].rec_head, (u64 *)cs->active_chunk_buffer, wlo);
	chunk_free_recs(&cs->mgrs[ctl]);
	/* 清位 = 变更：必须置脏，否则该窗若此前已被清脏，evict 会按 dirty==0 跳过回写 →
	 * 盘上陈旧 → asyn 重扫把 free_count 校准偏小 + test 把空闲位误判占用（泄漏）。*/
	cs->mgrs[ctl].compact_word |= RFS_BM_CHUNK_FLAG_DIRTY;
}

/* evict_online：下线当前在线窗（脏则快照写盘 → 重算摘要 → online=NONE）。写盘失败
 * 保持在线 + 脏标不落（buffer 仍是真相，状态零损坏）。不 drain 归还链（链随 chunk
 * 留存，下次上线 publish 前再还）。 */
static void cs_evict_online(struct rfs_bm_cslice *cs)
{
	u32 old = cs->online_idx;
	struct rfs_bm_cmgr *cm;
	u64 nbits;

	if (old == RFS_BM_CHUNK_NONE || !cs->mgrs || !cs->active_chunk_buffer)
		return;
	cm = &cs->mgrs[old];
	if (cm->compact_word & RFS_BM_CHUNK_FLAG_DIRTY) {
		nbits = cs_limit_of(cs, old);
		if (cs->soul.write_block(cs->soul.io_ctx, cs_chunk_disk_blk(cs, old),
					 nbits >> 15, cs->active_chunk_buffer) != 0)
			return;			/* 写失败：保持在线 + 保留脏 */
		cm->compact_word &= ~RFS_BM_CHUNK_FLAG_DIRTY;
	}
	chunk_summarize(cm, cs->active_chunk_buffer, cs_limit_of(cs, old));
	cs->online_idx = RFS_BM_CHUNK_NONE;
	cs->gen++;
}

/*
 * ensure_online：窗不在线时的慢路径全序列（03 §5/§6 铁律顺序）：
 *   evict 旧窗 → load 目标窗（摘要 ALL_FREE 且链空可省读 memset 0）→ ★ drain 归还链
 *   （必须在 publish 之前，否则 alloc 把盘上仍为 1 的位判成已用 → 永久泄漏）→
 *   publish(online=ctl, gen++) → 按 drained 置 dirty。
 * 返回 0 = 已在线；-EIO = 失败（状态零损坏，可重试）。
 */
static int cs_ensure_online(struct rfs_bm_cslice *cs, u32 ctl)
{
	u64 nbits, cw, nblocks;
	bool drained;

	if (!cs->mgrs || !cs->active_chunk_buffer || ctl >= cs->chunks_per_slice)
		return -EIO;
	if (cs->online_idx == ctl)
		return 0;			/* 复检：已在位 */
	if (cs->online_idx != RFS_BM_CHUNK_NONE) {
		cs_evict_online(cs);
		if (cs->online_idx != RFS_BM_CHUNK_NONE)
			return -EIO;		/* 旧窗写回失败：保持在线 */
	}
	nbits = cs_limit_of(cs, ctl);
	cw = cs->mgrs[ctl].compact_word;
	if ((cw & RFS_BM_CHUNK_SUM_MASK) == RFS_BM_CHUNK_SUM_ALL_FREE &&
	    !cs->mgrs[ctl].rec_head) {
		memset(cs->active_chunk_buffer, 0, (size_t)(nbits >> 3));
	} else {
		nblocks = nbits >> 15;	/* nbits 恒 32768 倍数 = 整页 */
		if (cs->soul.read_block(cs->soul.io_ctx, cs_chunk_disk_blk(cs, ctl),
					nblocks, cs->active_chunk_buffer) != 0)
			return -EIO;	/* 在线仍为 NONE，可重试 */
	}
	drained = (cs->mgrs[ctl].rec_head != NULL);
	cs_drain_chunk(cs, ctl);
	cs->online_idx = ctl;
	cs->gen++;				/* 换片代号（发布递增） */
	if (drained)
		cs->mgrs[ctl].compact_word |= RFS_BM_CHUNK_FLAG_DIRTY;
	return 0;
}

/* ═══════════════════════════ 单窗原语（须持锁且窗在线）═══════════════════════════ */

/* 硬分配单窗 [from,to)（chunk 内局部）：找 count 连续空闲、起始 2^align 对齐；不部分分配 */
static bool cs_win_alloc_hard(struct rfs_bm_cslice *cs, u64 from, u64 to,
			      u8 align_log2, u64 count, u64 *out)
{
	u64 align, hit, i;
	u64 *words;

	if (!out || !cs->active_chunk_buffer)
		return false;
	if (from >= to || count == 0 || count > to - from)
		return false;
	align = 1ULL << align_log2;		/* 入口已限 0..15 */
	words = (u64 *)cs->active_chunk_buffer;
	hit = chunk_find_free_run(words, to, from, count, align);
	if (hit == RFS_BM_NO_HIT)
		return false;
	for (i = 0; i < count; i++)
		chunk_bit_set(words, hit + i);
	cs->free_count -= count;
	*out = hit;
	return true;
}

/* 软分配单窗：首空闲段取对齐候选，实际 = min(段长, count)；缺口 *left 回传 */
static bool cs_win_alloc_soft(struct rfs_bm_cslice *cs, u64 from, u64 to,
			      u8 align_log2, u64 count, u64 *out, u64 *left)
{
	u64 align, cap, seg_s = 0, seg_e = 0, cand, got, i;
	u64 *words;

	if (!out || !left || !cs->active_chunk_buffer)
		return false;
	*left = count;
	if (from >= to || count == 0)
		return false;
	align = 1ULL << align_log2;
	words = (u64 *)cs->active_chunk_buffer;
	cap = (count > align) ? count : align;
	if (!chunk_next_free_seg(words, to, from, cap, &seg_s, &seg_e))
		return false;
	cand = (seg_s + align - 1) & ~(align - 1);
	if (cand >= seg_e)
		return false;
	got = seg_e - cand;
	if (got > count)
		got = count;
	for (i = 0; i < got; i++)
		chunk_bit_set(words, cand + i);
	cs->free_count -= got;
	*out = cand;
	*left = count - got;
	return true;
}

/*
 * 锚定占领单窗（wlo/whi/base = 片内偏移——窗界与锚点须同坐标系）：从 base 起贪心
 * 认领连续空闲前缀（≤ min(窗尾, 片尾, len−*rest)），撞占用/窗尾停。*rest 口径同 v1。
 * 返回 CS_WIN_OK / CS_WIN_EMPTY / CS_WIN_BAD。
 */
static int cs_win_soft_occupy(struct rfs_bm_cslice *cs, u64 wlo, u64 whi, u64 base,
			      u64 len, u64 *rest)
{
	u64 real, whi_eff, cap, off, k, i;
	u64 *words;

	if (!rest || !cs->active_chunk_buffer)
		return CS_WIN_BAD;
	if (*rest > len)
		return CS_WIN_BAD;
	if (len == 0)
		return CS_WIN_OK;
	real = len - *rest;
	if (real == 0)
		return CS_WIN_OK;
	if (base < wlo || base >= whi || base >= cs->slice_bits)
		return CS_WIN_BAD;
	whi_eff = (whi < cs->slice_bits) ? whi : cs->slice_bits;
	cap = whi_eff - base;			/* 窗尾夹断 */
	if (cap > real)
		cap = real;
	if (cap == 0) {
		*rest = len;
		return CS_WIN_EMPTY;
	}
	words = (u64 *)cs->active_chunk_buffer;
	off = base - wlo;			/* → chunk 内局部 */
	k = chunk_greedy_free_prefix(words, off, cap);
	if (k == 0) {
		*rest = len;
		return CS_WIN_EMPTY;
	}
	for (i = 0; i < k; i++)
		chunk_bit_set(words, off + i);
	cs->free_count -= k;
	*rest = len - k;
	return CS_WIN_OK;
}

/* ═══════════════════════════ 片级原语（仅 WORKABLE；自持片锁）═══════════════════════════ */

/* alloc_hard：窗口迭代两趟（hint 窗→片尾 → 绕回 [0,hint)）；命中 → 0 且 *out = 片内偏移 */
static int cs_alloc_hard(struct rfs_bm_cslice *cs, u8 align_log2, u64 bitscount, u64 *out)
{
	u32 sh, hctl, pass, c;
	u64 hint;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE)
		return -EINVAL;
	if (align_log2 > 15)
		return -EINVAL;			/* 对齐仅 0..15：越界拒，勿静默截断 */
	if (!cs->mgrs || !cs->active_chunk_buffer)
		return -EINVAL;
	if (bitscount == 0 || bitscount > cs->slice_bits)
		return -EINVAL;
	if (bitscount > cs->free_count)
		return -ENOSPC;			/* free_count gate 保真 */

	sh = cs_shift(cs);
	hint = cs->in_slice_hint;
	hctl = (hint >= cs->slice_bits) ? cs->chunks_per_slice - 1u : (u32)(hint >> sh);

	for (pass = 0; pass < 2; pass++) {
		u32 c_begin = (pass == 0) ? hctl : 0;
		u32 c_end = (pass == 0) ? cs->chunks_per_slice : hctl + 1u;

		for (c = c_begin; c < c_end; c++) {
			u64 wlo = (u64)c << sh;
			u64 whi = wlo + cs_limit_of(cs, c);
			u64 from = wlo, to = whi, hit_c = 0;

			if (pass == 0 && c == hctl && hint > wlo)
				from = hint;
			if (pass == 1 && c == hctl)
				to = (hint < whi) ? hint : whi;
			if (from >= to)
				continue;

			if (cs->online_idx == c) {
				if (cs->mgrs[c].rec_head)
					cs_drain_chunk(cs, c);
			} else {
				u64 cw = cs->mgrs[c].compact_word;

				if (!cs->mgrs[c].rec_head &&
				    (cw & RFS_BM_CHUNK_SUM_MASK) == RFS_BM_CHUNK_SUM_ALL_USED)
					continue;	/* 免载入跳窗 */
				if (cs_ensure_online(cs, c) != 0)
					return -EIO;
			}
			if (!cs_win_alloc_hard(cs, from - wlo, to - wlo, align_log2,
					       bitscount, &hit_c))
				continue;
			cs->mgrs[c].compact_word |= RFS_BM_CHUNK_FLAG_DIRTY;
			{
				u64 hit = wlo + hit_c;

				cs->in_slice_hint = hit + bitscount;
				*out = hit;
			}
			return 0;
		}
	}
	return -ENOSPC;
}

/* alloc_soft：窗口迭代同 alloc_hard；命中 → 0 且 *out = 片内偏移、*left = 缺口 */
static int cs_alloc_soft(struct rfs_bm_cslice *cs, u8 align_log2, u64 bitscount,
			 u64 *out, u64 *left)
{
	u32 sh, hctl, pass, c;
	u64 hint;

	*left = bitscount;
	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE)
		return -EINVAL;
	if (align_log2 > 15)
		return -EINVAL;
	if (!cs->mgrs || !cs->active_chunk_buffer)
		return -EINVAL;
	if (bitscount == 0)
		return -EINVAL;
	if (cs->free_count == 0)
		return -ENOSPC;

	sh = cs_shift(cs);
	hint = cs->in_slice_hint;
	hctl = (hint >= cs->slice_bits) ? cs->chunks_per_slice - 1u : (u32)(hint >> sh);

	for (pass = 0; pass < 2; pass++) {
		u32 c_begin = (pass == 0) ? hctl : 0;
		u32 c_end = (pass == 0) ? cs->chunks_per_slice : hctl + 1u;

		for (c = c_begin; c < c_end; c++) {
			u64 wlo = (u64)c << sh;
			u64 whi = wlo + cs_limit_of(cs, c);
			u64 from = wlo, to = whi, hit_c = 0, lv = bitscount;

			if (pass == 0 && c == hctl && hint > wlo)
				from = hint;
			if (pass == 1 && c == hctl)
				to = (hint < whi) ? hint : whi;
			if (from >= to)
				continue;

			if (cs->online_idx == c) {
				if (cs->mgrs[c].rec_head)
					cs_drain_chunk(cs, c);
			} else {
				u64 cw = cs->mgrs[c].compact_word;

				if (!cs->mgrs[c].rec_head &&
				    (cw & RFS_BM_CHUNK_SUM_MASK) == RFS_BM_CHUNK_SUM_ALL_USED)
					continue;
				if (cs_ensure_online(cs, c) != 0)
					return -EIO;
			}
			if (!cs_win_alloc_soft(cs, from - wlo, to - wlo, align_log2,
					       bitscount, &hit_c, &lv))
				continue;
			cs->mgrs[c].compact_word |= RFS_BM_CHUNK_FLAG_DIRTY;
			{
				u64 got = bitscount - lv;
				u64 hit = wlo + hit_c;

				cs->in_slice_hint = hit + got;
				*out = hit;
				*left = lv;
			}
			return 0;
		}
	}
	return -ENOSPC;
}

/* test_interval：快/慢路径分派（在线先 drain 再直读；离线摘要裁决 → 换片上线查）。
 * 异位 → -ENOENT；换片失败 → -EIO。 */
static int cs_test_interval(struct rfs_bm_cslice *cs, bool val, u64 slice_base, u64 len)
{
	u32 sh;
	u64 end, pos;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE || !cs->mgrs || !cs->active_chunk_buffer)
		return -EINVAL;
	if (slice_base > cs->slice_bits || len > cs->slice_bits - slice_base)
		return -EINVAL;
	if (len == 0)
		return 0;

	sh = cs_shift(cs);
	end = slice_base + len;
	pos = slice_base;
	while (pos < end) {
		u32 c = cs_ctl_of(cs, pos);
		u64 wlo = (u64)c << sh;
		u64 whi = wlo + cs_limit_of(cs, c);
		u64 n = ((end <= whi) ? end : whi) - pos;

		if (cs->online_idx == c) {
			if (cs->mgrs[c].rec_head)
				cs_drain_chunk(cs, c);
		} else {
			u64 cw = cs->mgrs[c].compact_word;

			if (!cs->mgrs[c].rec_head) {
				u64 sum = cw & RFS_BM_CHUNK_SUM_MASK;

				if (sum == RFS_BM_CHUNK_SUM_ALL_USED ||
				    sum == RFS_BM_CHUNK_SUM_ALL_FREE) {
					bool all_ones = (sum == RFS_BM_CHUNK_SUM_ALL_USED);

					if (all_ones != val)
						return -ENOENT;
					pos += n;	/* 摘要命 val：免载入整窗跳过 */
					continue;
				}
			}
			if (cs_ensure_online(cs, c) != 0)
				return -EIO;
		}
		{
			const u64 *words = (const u64 *)cs->active_chunk_buffer;
			u64 lo = pos - wlo, i;

			for (i = 0; i < n; i++)
				if (chunk_bit_get(words, lo + i) != val)
					return -ENOENT;
		}
		pos += n;
	}
	return 0;
}

/* free_interval：统一挂链（在线与否一律前插该 chunk 归还链，O(1)，零 I/O）；
 * 摘要降级 MIXED + 置 HAS_RECS；free_count 挂链即记账 +n。越界 → -EINVAL。 */
static int cs_free_interval(struct rfs_bm_cslice *cs, u64 slice_base, u64 len)
{
	u32 sh;
	u64 end, pos;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE || !cs->mgrs)
		return -EINVAL;
	if (slice_base > cs->slice_bits || len > cs->slice_bits - slice_base)
		return -EINVAL;
	if (len == 0)
		return 0;

	sh = cs_shift(cs);
	end = slice_base + len;
	pos = slice_base;
	while (pos < end) {
		u32 c = cs_ctl_of(cs, pos);
		u64 wlo = (u64)c << sh;
		u64 whi = wlo + cs_limit_of(cs, c);
		u64 n = (end <= whi) ? end - pos : whi - pos;	/* chunk 边界切段 */
		struct rfs_bm_rec *node = RFS_ZALLOC(sizeof(*node));

		if (!node)
			return -ENOMEM;		/* 堆失败：本段未挂账未记账（泄漏方向，自洽）*/
		node->off = pos;
		node->len = n;
		node->next = cs->mgrs[c].rec_head;
		cs->mgrs[c].rec_head = node;
		cs->mgrs[c].compact_word =
			(cs->mgrs[c].compact_word &
			 ~(RFS_BM_CHUNK_SUM_MASK | RFS_BM_CHUNK_FLAG_HAS_RECS)) |
			RFS_BM_CHUNK_SUM_MIXED | RFS_BM_CHUNK_FLAG_HAS_RECS;
		cs->free_count += n;		/* 挂链即记账（★ 记账） */
		pos += n;
	}
	return 0;
}

/* soft_occupy：锚点窗起逐窗贪心，顶窗尾续下一窗（跨 chunk 续圈 = 原跨片续圈同构）；
 * 撞占用 / 摘要 ALL_USED 如实停手；认领 ≥1 → 0；0 → -ENOSPC（*rest = len）。 */
static int cs_occupy_soft(struct rfs_bm_cslice *cs, u64 slice_base, u64 len, u64 *rest)
{
	u32 sh, c;
	u64 real, anchor, gap, claimed;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE || !cs->mgrs || !cs->active_chunk_buffer)
		return -EINVAL;
	if (!rest || *rest > len)
		return -EINVAL;
	if (len == 0)
		return 0;
	if (slice_base >= cs->slice_bits)
		return -EINVAL;
	if (*rest == len)
		return 0;			/* real == 0：已全满足 */
	real = len - *rest;

	sh = cs_shift(cs);
	anchor = slice_base;
	gap = real;
	claimed = 0;
	c = cs_ctl_of(cs, anchor);
	while (c < cs->chunks_per_slice) {
		u64 wlo = (u64)c << sh;
		u64 whi = wlo + cs_limit_of(cs, c);
		u64 wr = 0, k;
		int rc;

		if (anchor >= whi) {
			c++;
			continue;
		}
		if (cs->online_idx == c) {
			if (cs->mgrs[c].rec_head)
				cs_drain_chunk(cs, c);
		} else {
			u64 cw = cs->mgrs[c].compact_word;

			if (!cs->mgrs[c].rec_head &&
			    (cw & RFS_BM_CHUNK_SUM_MASK) == RFS_BM_CHUNK_SUM_ALL_USED)
				break;		/* 锚点窗无货 → 前缀断（免载入） */
			if (cs_ensure_online(cs, c) != 0) {
				*rest = len - claimed;
				return (claimed > 0) ? 0 : -EIO;
			}
		}
		rc = cs_win_soft_occupy(cs, wlo, whi, anchor, gap, &wr);
		k = gap - wr;
		if (rc == CS_WIN_BAD) {
			*rest = len - claimed;
			return -EINVAL;
		}
		if (k > 0) {
			cs->mgrs[c].compact_word |= RFS_BM_CHUNK_FLAG_DIRTY;
			claimed += k;
			gap = wr;
			anchor += k;
		}
		if (k == 0 || gap == 0)
			break;			/* 锚点即占用 / 全额认领 */
		if (anchor < whi)
			break;			/* 撞占用停：不跨窗硬续 */
		c++;				/* 顶到窗尾 → 下一窗续圈 */
	}
	/* 只进不退；**仅在真正认领 (claimed>0) 时推进** —— 与 MEM 后端 / god v1 对齐。
	 * 修复（2026-09-30）：god v2 原语义连「0 认领的失败 occupy」也把 hint 推到锚点，
	 * 致后续 hard/soft 的回绕区间被截断 → 明明有合法单窗 run 却假 ENOSPC。
	 * ko 以 MEM 为行为基线，故加 claimed>0 守卫（god v2 需同步修）。*/
	if (anchor > cs->in_slice_hint) /* PRE-FIX SIM */
		cs->in_slice_hint = anchor;
	*rest = len - claimed;
	return (claimed > 0) ? 0 : -ENOSPC;
}

/* chunks_asyn：持片锁一整轮全量（读整片 → 在线窗先还账进缓冲 → 覆盖暂存 → 合并
 * 全部归还链 → 写回 → 成功才摘账 + 重算摘要 + 整片重扫校准 free_count + 清在线脏标）。
 * 失败（读/写）链原样保留、状态不动，重跑幂等。校准漂移经 RFS_LOG 上报。 */
static int cs_chunks_asyn(struct rfs_bm_cslice *cs)
{
	u8 *tmp;
	u32 sh, c, oc;
	u64 calib_old;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE || !cs->mgrs || !cs->active_chunk_buffer)
		return -EINVAL;
	tmp = RFS_ZALLOC((size_t)(cs->slice_bits >> 3));
	if (!tmp)
		return -ENOMEM;
	if (cs->soul.read_block(cs->soul.io_ctx, cs->soul.chunk_base_blk,
				cs->soul.chunk_blk_length, tmp) != 0) {
		RFS_FREE(tmp);
		return -EIO;
	}
	sh = cs_shift(cs);
	oc = cs->online_idx;
	if (oc != RFS_BM_CHUNK_NONE) {
		u64 wlo = (u64)oc << sh;

		if (cs->mgrs[oc].rec_head) {
			chunk_apply_recs(cs->mgrs[oc].rec_head,
					 (u64 *)cs->active_chunk_buffer, wlo);
			cs->mgrs[oc].compact_word |= RFS_BM_CHUNK_FLAG_DIRTY;
		}
		memcpy(tmp + (wlo >> 3), cs->active_chunk_buffer,
		       (size_t)(cs_limit_of(cs, oc) >> 3));
	}
	for (c = 0; c < cs->chunks_per_slice; c++)
		if (cs->mgrs[c].rec_head)
			chunk_apply_recs(cs->mgrs[c].rec_head, (u64 *)tmp, 0);
	if (cs->soul.write_block(cs->soul.io_ctx, cs->soul.chunk_base_blk,
				 cs->soul.chunk_blk_length, tmp) != 0) {
		RFS_FREE(tmp);
		return -EIO;			/* 记录仍在链上 → 下次重跑（幂等） */
	}
	for (c = 0; c < cs->chunks_per_slice; c++) {
		chunk_free_recs(&cs->mgrs[c]);
		chunk_summarize(&cs->mgrs[c], tmp + (((u64)c << sh) >> 3), cs_limit_of(cs, c));
	}
	if (oc != RFS_BM_CHUNK_NONE)
		cs->mgrs[oc].compact_word &= ~RFS_BM_CHUNK_FLAG_DIRTY;
	calib_old = cs->free_count;
	cs->free_count = chunk_count_zeros((const u64 *)tmp, cs->slice_bits);
	if (calib_old != cs->free_count)
		RFS_LOG("ravenfs: bm chunk free_count calib %llu -> %llu\n",
			(unsigned long long)calib_old, (unsigned long long)cs->free_count);
	RFS_FREE(tmp);
	return 0;
}

/* ═══════════════════════════ enable / disable ═══════════════════════════ */

/*
 * enable：UNWORKABLE → WORKABLE（阻塞锁）。拷 soul → 派生几何（slice_bits =
 * chunk_blk_length<<15 ⇒ 尾块公式恒成立）→ 建 mgrs[] / window buffer → 一次整片全量
 * 读（堆上临时缓冲）实测 free_count + 填每 chunk 2bit 摘要 → WORKABLE。
 * 任一步失败：收资源、几何归零、保持 UNWORKABLE。
 */
static int cs_enable(struct rfs_bm_cslice *cs, const struct rfs_bm_soul *soul, u64 base_idx)
{
	u64 nblocks, per, nwin, sh;
	u8 *tmp;
	u32 i;

	if (!soul || !soul->read_block || !soul->write_block)
		return -EINVAL;
	if (soul->chunk_blk_length == 0 || soul->chunk_blk_length >= (1ULL << 49))
		return -EINVAL;			/* 移位/体量上界：防 slice_bits 溢出 */
	if (soul->chunk_order > 16)
		return -EINVAL;			/* 缓冲 4096<<order ≤ 256MB；防移位 UB */

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state == RFS_BM_SLICE_WORKABLE)
		return -EINVAL;			/* 双 enable 拒 */

	cs->soul = *soul;
	cs->in_global_base_idx = base_idx;	/* 只存不消费（换算在路由层） */

	nblocks = soul->chunk_blk_length;
	cs->slice_bits = nblocks << 15;
	per = 1ULL << soul->chunk_order;
	nwin = (nblocks + per - 1) >> soul->chunk_order;
	if (nwin == 0 || nwin > 0xFFFFFFFFULL)
		return -EINVAL;
	cs->chunks_per_slice = (u32)nwin;
	cs->tail_bits = cs->slice_bits -
			(u64)(cs->chunks_per_slice - 1) * (32768ULL << soul->chunk_order);

	cs->mgrs = RFS_ZALLOC(sizeof(*cs->mgrs) * cs->chunks_per_slice);
	if (!cs->mgrs) {
		cs->mgrs = NULL;
		return -ENOMEM;
	}
	/* 默认 compact_word = MIXED（ZALLOC 全 0 = ALL_USED，非法，须显式改） */
	for (i = 0; i < cs->chunks_per_slice; i++)
		cs->mgrs[i].compact_word = RFS_BM_CHUNK_SUM_MIXED;

	cs->active_chunk_buffer = RFS_ZALLOC((size_t)((u64)4096 << soul->chunk_order));
	if (!cs->active_chunk_buffer) {
		chunk_release_resources(cs->mgrs, cs->chunks_per_slice, NULL);
		cs->mgrs = NULL;
		return -ENOMEM;
	}
	tmp = RFS_ZALLOC((size_t)(cs->slice_bits >> 3));
	if (!tmp) {
		chunk_release_resources(cs->mgrs, cs->chunks_per_slice, cs->active_chunk_buffer);
		cs->mgrs = NULL;
		cs->active_chunk_buffer = NULL;
		return -ENOMEM;
	}
	if (cs->soul.read_block(cs->soul.io_ctx, soul->chunk_base_blk, nblocks, tmp) != 0) {
		RFS_FREE(tmp);
		chunk_release_resources(cs->mgrs, cs->chunks_per_slice, cs->active_chunk_buffer);
		cs->mgrs = NULL;
		cs->active_chunk_buffer = NULL;
		return -EIO;
	}
	cs->free_count = chunk_count_zeros((const u64 *)tmp, cs->slice_bits);
	sh = (u64)15 + soul->chunk_order;
	for (i = 0; i < cs->chunks_per_slice; i++)
		chunk_summarize(&cs->mgrs[i], tmp + (((u64)i << sh) >> 3), cs_limit_of(cs, i));
	RFS_FREE(tmp);

	cs->online_idx = RFS_BM_CHUNK_NONE;
	cs->gen = 0;
	cs->in_slice_hint = 0;
	cs->state = RFS_BM_SLICE_WORKABLE;	/* 最后一步才开门 */
	return 0;
}

/*
 * disable（shutdown）：须 WORKABLE →（锁内跑完一轮工作）→ UNWORKABLE。
 * 一轮 = 在线脏窗写回 + 全部归还链合并落盘 → 释放 buffer/mgrs/残留链 → 几何归零。
 * I/O 失败仍完成收资源与状态迁移（记录丢失 = 泄漏方向）返 -EIO。
 */
static int cs_disable(struct rfs_bm_cslice *cs)
{
	int rc = 0;
	bool need_io = false;
	u32 c;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE)
		return -EINVAL;
	for (c = 0; c < cs->chunks_per_slice; c++)
		if (cs->mgrs[c].rec_head || (cs->mgrs[c].compact_word & RFS_BM_CHUNK_FLAG_DIRTY))
			need_io = true;

	if (need_io) {
		u8 *tmp = RFS_ZALLOC((size_t)(cs->slice_bits >> 3));
		bool ok = false;

		if (tmp && cs->soul.read_block(cs->soul.io_ctx, cs->soul.chunk_base_blk,
					       cs->soul.chunk_blk_length, tmp) == 0) {
			u32 sh = cs_shift(cs);

			if (cs->online_idx != RFS_BM_CHUNK_NONE) {
				u32 oc = cs->online_idx;
				u64 wlo = (u64)oc << sh;

				if (cs->mgrs[oc].rec_head) {
					chunk_apply_recs(cs->mgrs[oc].rec_head,
							 (u64 *)cs->active_chunk_buffer, wlo);
					cs->mgrs[oc].compact_word |= RFS_BM_CHUNK_FLAG_DIRTY;
				}
				memcpy(tmp + (wlo >> 3), cs->active_chunk_buffer,
				       (size_t)(cs_limit_of(cs, oc) >> 3));
			}
			for (c = 0; c < cs->chunks_per_slice; c++)
				if (cs->mgrs[c].rec_head)
					chunk_apply_recs(cs->mgrs[c].rec_head, (u64 *)tmp, 0);
			ok = cs->soul.write_block(cs->soul.io_ctx, cs->soul.chunk_base_blk,
						  cs->soul.chunk_blk_length, tmp) == 0;
		}
		RFS_FREE(tmp);
		if (!ok)
			rc = -EIO;		/* 记录未落盘：泄漏方向，照收资源 */
	}

	chunk_release_resources(cs->mgrs, cs->chunks_per_slice, cs->active_chunk_buffer);
	cs->mgrs = NULL;
	cs->active_chunk_buffer = NULL;
	cs->online_idx = RFS_BM_CHUNK_NONE;
	cs->gen = 0;
	cs->slice_bits = 0;
	cs->chunks_per_slice = 0;
	cs->tail_bits = 0;
	cs->free_count = 0;
	cs->in_slice_hint = 0;
	cs->state = RFS_BM_SLICE_UNWORKABLE;
	return rc;
}

/* ═══════════════════════════ ops 包装（片自然坐标）═══════════════════════════ */

static int chunk_group_setup(struct rfs_bm_group *g, const struct rfs_bm_cfg *cfg)
{
	const chunk_slices_cfg_t *cc = &cfg->chunk_cfg;
	u64 per_blocks = g->per_bits >> 15;	/* 每片盘上占块数（per_bits 恒 32768 倍数） */
	u32 i;

	g->cslices = RFS_ZALLOC(sizeof(*g->cslices) * g->n_slices);
	if (!g->cslices)
		return -ENOMEM;
	/* 先统一初始化全部片锁（fini 时逐片 disable 才可安全取锁） */
	for (i = 0; i < g->n_slices; i++) {
		RFS_MUTEX_INIT(&g->cslices[i].lock);
		g->cslices[i].state = RFS_BM_SLICE_UNWORKABLE;
	}
	for (i = 0; i < g->n_slices; i++) {
		struct rfs_bm_cslice *cs = &g->cslices[i];
		struct rfs_bm_soul soul;
		u64 begin_blk = (u64)i * per_blocks;
		u64 nblk;
		int rc;

		if (begin_blk >= g->disk_blocks)
			return -EINVAL;		/* 防御（末片非空迭代已保证） */
		nblk = g->disk_blocks - begin_blk;
		if (nblk > per_blocks)
			nblk = per_blocks;	/* 末片收窄 */

		memset(&soul, 0, sizeof(soul));
		soul.read_block = cc->read_block;
		soul.write_block = cc->write_block;
		soul.io_ctx = cc->io_ctx;
		soul.chunk_base_blk = g->disk_base + begin_blk;
		soul.chunk_blk_length = nblk;
		soul.chunk_order = g->chunk_order;
		rc = cs_enable(cs, &soul, (u64)i * g->per_bits);
		if (rc)
			return rc;		/* 部分成功 → core 统一 fini 收 */
	}
	return 0;
}

static void chunk_group_fini(struct rfs_bm_group *g)
{
	u32 i;

	if (!g->cslices)
		return;
	for (i = 0; i < g->n_slices; i++)
		(void)cs_disable(&g->cslices[i]);	/* 未 enable 的片返 -EINVAL，无害 */
	RFS_FREE(g->cslices);
	g->cslices = NULL;
}

static int chunk_alloc_hard(struct rfs_bm_group *g, u32 idx, u64 count,
			    u8 align_log2, u64 *out)
{
	return cs_alloc_hard(&g->cslices[idx], align_log2, count, out);
}

static int chunk_alloc_soft(struct rfs_bm_group *g, u32 idx, u64 count,
			    u8 align_log2, u64 *out, u64 *left)
{
	return cs_alloc_soft(&g->cslices[idx], align_log2, count, out, left);
}

static int chunk_occupy_soft(struct rfs_bm_group *g, u32 idx, u64 base, u64 len, u64 *rest)
{
	return cs_occupy_soft(&g->cslices[idx], base, len, rest);
}

static int chunk_free_interval(struct rfs_bm_group *g, u32 idx, u64 base, u64 len)
{
	return cs_free_interval(&g->cslices[idx], base, len);
}

static int chunk_test_interval(struct rfs_bm_group *g, u32 idx, bool val, u64 base, u64 len)
{
	return cs_test_interval(&g->cslices[idx], val, base, len);
}

static bool chunk_test_bit(struct rfs_bm_group *g, u32 idx, u64 bit)
{
	return cs_test_interval(&g->cslices[idx], true, bit, 1) == 0;
}

static u64 chunk_free_count(const struct rfs_bm_group *g, u32 idx)
{
	struct rfs_bm_cslice *cs = (struct rfs_bm_cslice *)&g->cslices[idx];
	u64 v;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	v = cs->free_count;
	return v;
}

/* 整片一致视图快照：读盘 → 覆盖在线窗（真相）→ 合并全部归还链清位。 */
static void chunk_snapshot_slice(const struct rfs_bm_group *g, u32 idx, void *dst)
{
	struct rfs_bm_cslice *cs = (struct rfs_bm_cslice *)&g->cslices[idx];
	u32 c, sh;

	RFS_MUTEX_GUARD(lk, &cs->lock);
	if (cs->state != RFS_BM_SLICE_WORKABLE || !cs->mgrs)
		return;
	if (cs->soul.read_block(cs->soul.io_ctx, cs->soul.chunk_base_blk,
				cs->soul.chunk_blk_length, dst) != 0)
		return;
	sh = cs_shift(cs);
	if (cs->online_idx != RFS_BM_CHUNK_NONE) {
		u32 oc = cs->online_idx;
		u64 wlo = (u64)oc << sh;

		memcpy((u8 *)dst + (wlo >> 3), cs->active_chunk_buffer,
		       (size_t)(cs_limit_of(cs, oc) >> 3));
	}
	for (c = 0; c < cs->chunks_per_slice; c++)
		if (cs->mgrs[c].rec_head)
			chunk_apply_recs(cs->mgrs[c].rec_head, (u64 *)dst, 0);
}

static int chunk_slice_sync(struct rfs_bm_group *g, u32 idx)
{
	return cs_chunks_asyn(&g->cslices[idx]);
}

/* ═══════════════════════════ ops 表 ═══════════════════════════ */

const struct rfs_bm_ops rfs_bm_chunk_ops = {
	.name			= "chunk",
	.group_setup		= chunk_group_setup,
	.group_fini		= chunk_group_fini,
	.group_rescan_free	= NULL,		/* CHUNK：enable 已实测，no-op */
	.alloc_hard		= chunk_alloc_hard,
	.alloc_soft		= chunk_alloc_soft,
	.occupy_soft		= chunk_occupy_soft,
	.free_interval		= chunk_free_interval,
	.test_interval		= chunk_test_interval,
	.bit_test		= chunk_test_bit,
	.free_count		= chunk_free_count,
	.snapshot_slice		= chunk_snapshot_slice,
	.slice_sync		= chunk_slice_sync,
};
