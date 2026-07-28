#include "../include/ravenfs/bitmap_cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <utility>
#include <cassert>

/* ═══════════════════════════════════════════════════════════════
 * ── Decode / Encode ──
 * ═══════════════════════════════════════════════════════════════ */

BitmapFields BitmapCacheController::decode(uint64_t bit_idx) const {
    return decode_bitmap_idx(bit_idx, N);
}

uint64_t BitmapCacheController::encode(uint64_t cid, uint64_t bucket_pos) const {
    return (cid << (N + 15)) + bucket_pos;
}

/* ═══════════════════════════════════════════════════════════════
 * ── 位图运算（裸 buffer）──
 *
 * 所有函数操作连续 bits，bits = [0, buf_bits)。
 * ═══════════════════════════════════════════════════════════════ */

/* ─── scan_run: 从 start 开始找 ≥ need 个连续零位，卷绕 ───
 *   ret = bit position, ~0ULL = not found
 */
uint64_t BitmapCacheController::scan_run(
    const uint8_t *buf, uint64_t buf_bits,
    uint64_t start, uint64_t need)
{
    if (need == 0 || need > buf_bits) return ~0ULL;

    uint64_t run_start = ~0ULL;
    uint64_t run_len = 0;

    for (uint64_t i = 0; i < buf_bits * 2; i++) {
        uint64_t pos = (start + i) % buf_bits;
        uint64_t byte_idx = pos >> 3;
        uint8_t  bit_mask = 1 << (pos & 7);

        if ((buf[byte_idx] & bit_mask) == 0) {
            if (run_len == 0) run_start = pos;
            run_len++;
            if (run_len >= need)
                return run_start;
        } else {
            run_len = 0;
            run_start = ~0ULL;
        }
    }
    return ~0ULL;
}

/* ─── find_longest: 找最长 ≤ max_count 的连续零位 ───
 *   ret = length, *out_pos = 起始位置
 *   0 = 无空闲
 */
uint64_t BitmapCacheController::find_longest(
    const uint8_t *buf, uint64_t buf_bits,
    uint64_t max_count)
{
    uint64_t best_start = ~0ULL;
    uint64_t best_len = 0;
    uint64_t run_start = ~0ULL;
    uint64_t run_len = 0;

    for (uint64_t pos = 0; pos < buf_bits; pos++) {
        uint64_t byte_idx = pos >> 3;
        uint8_t  bit_mask = 1 << (pos & 7);

        if ((buf[byte_idx] & bit_mask) == 0) {
            if (run_len == 0) run_start = pos;
            run_len++;
        } else {
            if (run_len > best_len && run_len <= max_count) {
                best_len = run_len;
                best_start = run_start;
            }
            run_len = 0;
        }
    }
    /* 扫到尾部，收尾 */
    if (run_len > best_len && run_len <= max_count) {
        best_len = run_len;
        best_start = run_start;
    }

    return best_len;
}

/* ─── set_bits / clear_bits — 批量置位/清位 ─── */
void BitmapCacheController::set_bits(uint8_t *buf, uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t pos = start + i;
        buf[pos >> 3] |= (uint8_t)(1 << (pos & 7));
    }
}

void BitmapCacheController::clear_bits(uint8_t *buf, uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t pos = start + i;
        buf[pos >> 3] &= (uint8_t)~(1 << (pos & 7));
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── Bucket I/O ──
 * ═══════════════════════════════════════════════════════════════ */

/* 从磁盘加载整个 controller 段的 bitmap 数据到 bucket */
void BitmapCacheController::load_bucket(uint8_t *bucket, uint64_t cid) const {
    uint64_t base_fblock = bitmap_base + cid * (1ULL << N);
    uint64_t blocks = 1ULL << N;

    for (uint64_t i = 0; i < blocks; i++) {
        dev->read_block(base_fblock + i, bucket + i * 4096);
    }
}

/* 将 bucket 中整个 controller 段的 bitmap 数据写回磁盘 */
void BitmapCacheController::flush_bucket(const uint8_t *bucket, uint64_t cid) const {
    uint64_t base_fblock = bitmap_base + cid * (1ULL << N);
    uint64_t blocks = 1ULL << N;

    for (uint64_t i = 0; i < blocks; i++) {
        dev->write_block(base_fblock + i, bucket + i * 4096);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── consume_head — 消费控制器链表中的返还记录 ──
 *
 * 把 head 链表中的记录回写到 bucket 内。
 * 每次在 controller.lock 被持有时调用，调用前和调用后 lock 状态不变。
 * 调用时 alloc_lock 可持可不持（sweeper 场景不持，热路径场景持）。
 * ═══════════════════════════════════════════════════════════════ */
void BitmapCacheController::consume_head(uint8_t *bucket, BitmapBlockController *ctrl) {
    bitmap_return_record_sll *cur = ctrl->head;
    ctrl->head = nullptr;

    while (cur) {
        bitmap_return_record_sll *next = cur->next;

        /* 确认记录属于本 controller */
        uint64_t rel_start = cur->base & (ctrl_seg_bits - 1);
        clear_bits(bucket, rel_start, cur->entry_count);

        ::free(cur);
        cur = next;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── try_alloc / try_alloc_soft — 在当前桶中尝试分配 ──
 *
 * 入口条件：alloc_lock 已持有。
 * 内部：扫描 bucket → 空则 consume_head 重试一次 → 还空返回 ~0ULL
 * 内部握 controller.lock 仅更新 free_count / scan_hint（极短）。
 *
 * try_alloc:   必须 ≥ count 个连续块
 * try_alloc_soft: 贪心取最长 ≤ max_count 的连续块
 * ═══════════════════════════════════════════════════════════════ */

uint64_t BitmapCacheController::try_alloc(
    ProcessorBitmapState *ps, uint64_t cid, uint64_t count)
{
    uint8_t *bucket = ps->bucket;
    BitmapBlockController *ctrl = &all_controller[cid];

    /* ── 第一次扫 ── */
    uint64_t pos = scan_run(bucket, bucket_bits, ctrl->scan_hint, count);
    if (pos != ~0ULL)
        goto alloc;

    /* ── 空仓 → consume head 重试一次 ── */
    {
        spinlock_guard g(ctrl->lock);
        consume_head(bucket, ctrl);
    }

    pos = scan_run(bucket, bucket_bits, 0, count);
    if (pos == ~0ULL)
        return ~0ULL;   /* 真没了 */

alloc:
    set_bits(bucket, pos, count);
    {
        spinlock_guard g(ctrl->lock);
        ctrl->free_count -= count;
        ctrl->scan_hint = pos + count - 1;
    }
    return encode(cid, pos);
}

uint64_t BitmapCacheController::try_alloc_soft(
    ProcessorBitmapState *ps, uint64_t cid, uint64_t max_count)
{
    uint8_t *bucket = ps->bucket;
    BitmapBlockController *ctrl = &all_controller[cid];

    /* ── 第一次扫 ── */
    uint64_t pos;
    uint64_t got = find_longest(bucket, bucket_bits, max_count);
    if (got > 0) {
        pos = scan_run(bucket, bucket_bits, 0, got);
        if (pos != ~0ULL)
            goto alloc;
    }

    /* ── 空仓 → consume head 重试一次 ── */
    {
        spinlock_guard g(ctrl->lock);
        consume_head(bucket, ctrl);
    }

    got = find_longest(bucket, bucket_bits, max_count);
    if (got == 0)
        return ~0ULL;

    pos = scan_run(bucket, bucket_bits, 0, got);
    if (pos == ~0ULL)
        return ~0ULL;

alloc:
    set_bits(bucket, pos, got);
    {
        spinlock_guard g(ctrl->lock);
        ctrl->free_count -= got;
        ctrl->scan_hint = pos + got - 1;
    }
    return encode(cid, pos);
}

/* ═══════════════════════════════════════════════════════════════
 * ── switch_to — 切换控制器 ──
 *
 * Phase 1: 持锁下线旧 cid + 上线新 cid，无 I/O
 * Phase 2: 无锁 I/O（flush old bucket + load new bucket）
 * Phase 3: 持锁消费 head + 挂载新 cid
 *
 * old_cid = ~0ULL 表示无旧桶（首次加载场景）
 * ═══════════════════════════════════════════════════════════════ */
void BitmapCacheController::switch_to(uint64_t old_cid, uint64_t new_cid) {
    ProcessorBitmapState *ps = &all_proc_state[0];  /* 单核时简化，后改为 my_processor */
    uint8_t *bucket = ps->bucket;

    /* ── Phase 1 ── */
    spinlock_guard alloc_guard(ps->alloc_lock);

    if (old_cid != ~0ULL) {
        spinlock_guard ctrl_guard(all_controller[old_cid].lock);
        all_controller[old_cid].state = BITMAP_CTRL_OFFLINE;
    }

    {
        spinlock_guard ctrl_guard(all_controller[new_cid].lock);
        all_controller[new_cid].state = BITMAP_CTRL_ONLINE;
    }

    ps->current_cid = ~0ULL;           /* 下野标记 */
    alloc_guard.~spinlock_guard();     /* 释放锁，开始 Phase 2 */
    /* 手动析构 guard，因为下面还要重取 */

    /* ── Phase 2 (无锁) ── */
    if (old_cid != ~0ULL)
        flush_bucket(bucket, old_cid);

    load_bucket(bucket, new_cid);

    /* ── Phase 3 ── */
    ps->alloc_lock.lock();             /* 重取锁 */

    {
        spinlock_guard ctrl_guard(all_controller[new_cid].lock);
        consume_head(bucket, &all_controller[new_cid]);
    }

    ps->current_cid = new_cid;
    ps->last_hint_cid = new_cid;
    ps->alloc_lock.unlock();
}

/* ═══════════════════════════════════════════════════════════════
 * ── hard_alloc: 三层硬性连续分配 ──
 *
 * 1: 本地桶 + consume_head 重试
 * 1.5: 借用其它处理器的桶（跳过本地）
 * 2: 切换到一个 OFFLINE 的 controller
 *
 * ret = 起始 FS_block 号，~0ULL = 全盘满
 * ═══════════════════════════════════════════════════════════════ */

uint64_t BitmapCacheController::hard_alloc(uint64_t count) {
    if (count == 0) return ~0ULL;
    ProcessorBitmapState *ps = &all_proc_state[0];  /* 单核简化 */
    uint64_t C = ctrl_count;

    /* mark 数组：跟踪每个 cid 的分配尝试状态 */
    uint8_t *mark = (uint8_t *)alloca(C);  /* 栈分配 */
    memset(mark, 0, C);

    uint64_t cid, block;

    /* ═══════ Tier 1: 本地桶 ═══════ */
    ps->alloc_lock.lock();
    cid = ps->current_cid;
    if (cid != ~0ULL) {
        block = try_alloc(ps, cid, count);
        if (block != ~0ULL) {
            ps->alloc_lock.unlock();
            return block;
        }
        mark[cid] = 1;  /* 标记已试过 */
    }
    ps->alloc_lock.unlock();

    /* ═══════ Tier 1.5: 借别人的桶 ═══════ */
    for (uint64_t q = 0; q < processor_max; q++) {
        ProcessorBitmapState *peer = &all_proc_state[q];

        peer->alloc_lock.lock();
        uint64_t peer_cid = peer->current_cid;
        if (peer_cid == ~0ULL || peer_cid == cid || mark[peer_cid]) {
            peer->alloc_lock.unlock();
            continue;
        }

        BitmapBlockController *ctrl = &all_controller[peer_cid];
        if (ctrl->state != BITMAP_CTRL_ONLINE) {
            peer->alloc_lock.unlock();
            continue;
        }

        block = try_alloc(peer, peer_cid, count);
        peer->alloc_lock.unlock();

        if (block != ~0ULL)
            return block;

        mark[peer_cid] = 1;
    }

    /* ═══════ Tier 2: 找 OFFLINE 的 controller 切换 ═══════ */
    cid = ps->last_hint_cid;
    for (uint64_t i = 0; i < C; i++) {
        uint64_t t = (cid + 1 + i) % C;
        if (mark[t]) continue;

        BitmapBlockController *ctrl = &all_controller[t];
        if (ctrl->state != BITMAP_CTRL_OFFLINE) continue;
        if (ctrl->free_count < count) {
            mark[t] = 1;
            continue;
        }

        switch_to(ps->current_cid, t);
        cid = t;

        ps->alloc_lock.lock();
        block = try_alloc(ps, cid, count);
        if (block != ~0ULL) {
            ps->last_hint_cid = cid;
            ps->alloc_lock.unlock();
            return block;
        }
        mark[cid] = 1;
        ps->alloc_lock.unlock();
    }

    return ~0ULL;  /* 全盘满 */
}

/* ═══════════════════════════════════════════════════════════════
 * ── soft_alloc: 尽力分配（贪心）──
 *
 * 返回实际拿到的连续块数（1 ~ max_count）。
 * ~0ULL = 全盘满
 * ═══════════════════════════════════════════════════════════════ */

uint64_t BitmapCacheController::soft_alloc(uint64_t max_count) {
    if (max_count == 0) return ~0ULL;
    ProcessorBitmapState *ps = &all_proc_state[0];
    uint64_t C = ctrl_count;

    uint8_t *mark = (uint8_t *)alloca(C);
    memset(mark, 0, C);

    uint64_t cid, block;

    /* ═══════ Tier 1: 本地桶 ═══════ */
    ps->alloc_lock.lock();
    cid = ps->current_cid;
    if (cid != ~0ULL) {
        block = try_alloc_soft(ps, cid, max_count);
        if (block != ~0ULL) {
            ps->alloc_lock.unlock();
            return block;
        }
        mark[cid] = 1;
    }
    ps->alloc_lock.unlock();

    /* ═══════ Tier 1.5: 借别人的桶 ═══════ */
    uint64_t best_block = ~0ULL;
    uint64_t best_len = 0;

    for (uint64_t q = 0; q < processor_max; q++) {
        ProcessorBitmapState *peer = &all_proc_state[q];

        peer->alloc_lock.lock();
        uint64_t peer_cid = peer->current_cid;
        if (peer_cid == ~0ULL || peer_cid == cid || mark[peer_cid]) {
            peer->alloc_lock.unlock();
            continue;
        }

        BitmapBlockController *ctrl = &all_controller[peer_cid];
        if (ctrl->state != BITMAP_CTRL_ONLINE) {
            peer->alloc_lock.unlock();
            continue;
        }

        block = try_alloc_soft(peer, peer_cid, max_count);
        peer->alloc_lock.unlock();

        if (block != ~0ULL) {
            /* 看看拿了多少块 */
            uint64_t got = (block & (ctrl_seg_bits - 1)) + 1;  /* 粗略 */
            if (got > best_len) {
                best_block = block;
                best_len = got;
            }
        }

        mark[peer_cid] = 1;
    }

    if (best_block != ~0ULL)
        return best_block;

    /* ═══════ Tier 2: OFFLINE → 切过去看看 ═══════ */
    cid = ps->last_hint_cid;
    for (uint64_t i = 0; i < C; i++) {
        uint64_t t = (cid + 1 + i) % C;
        if (mark[t]) continue;

        BitmapBlockController *ctrl = &all_controller[t];
        if (ctrl->state != BITMAP_CTRL_OFFLINE) continue;
        if (ctrl->free_count == 0) {
            mark[t] = 1;
            continue;
        }

        switch_to(ps->current_cid, t);
        cid = t;

        ps->alloc_lock.lock();
        block = try_alloc_soft(ps, cid, max_count);
        if (block != ~0ULL) {
            ps->last_hint_cid = cid;
            ps->alloc_lock.unlock();
            return block;
        }
        mark[cid] = 1;
        ps->alloc_lock.unlock();
    }

    return ~0ULL;  /* 全盘满 */
}

/* ═══════════════════════════════════════════════════════════════
 * ── free: 返还 FS_block 区间 ──
 *
 * 本桶管 → 直清 bit
 * 跨桶管 → 挂 record 到目标 controller 的 head 链表
 * ═══════════════════════════════════════════════════════════════ */

void BitmapCacheController::free(uint64_t fblock, uint64_t count) {
    if (count == 0) return;
    ProcessorBitmapState *ps = &all_proc_state[0];
    BitmapFields f = decode(fblock);

    /* ── 归本桶管 ── */
    if (ps->current_cid == f.controller_id) {
        spinlock_guard alloc_guard(ps->alloc_lock);

        uint64_t rel_start = fblock & (ctrl_seg_bits - 1);
        clear_bits(ps->bucket, rel_start, count);

        {
            spinlock_guard ctrl_guard(all_controller[f.controller_id].lock);
            all_controller[f.controller_id].free_count += count;
        }
        return;
    }

    /* ── 跨桶：挂 record ── */
    bitmap_return_record_sll *rec = (bitmap_return_record_sll *)malloc(
        sizeof(bitmap_return_record_sll));
    rec->base        = fblock;
    rec->entry_count = count;
    rec->next        = nullptr;

    BitmapBlockController *ctrl = &all_controller[f.controller_id];
    {
        spinlock_guard g(ctrl->lock);
        rec->next = ctrl->head;
        ctrl->head = rec;
        ctrl->free_count += count;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── Flush — 析构时按序刷下盘 ──
 * ═══════════════════════════════════════════════════════════════ */

/* flush_processor: 下野 + 刷桶 */
void BitmapCacheController::flush_processor(uint64_t pid) {
    ProcessorBitmapState *ps = &all_proc_state[pid];
    uint64_t cid = ps->current_cid;
    if (cid == ~0ULL) return;

    /* 下野 */
    ps->alloc_lock.lock();
    {
        spinlock_guard g(all_controller[cid].lock);
        all_controller[cid].state = BITMAP_CTRL_OFFLINE;
    }
    ps->current_cid = ~0ULL;
    ps->alloc_lock.unlock();

    /* 刷桶 */
    flush_bucket(ps->bucket, cid);
}

/* flush_controller: 读盘 + 清偿 head 链表 */
void BitmapCacheController::flush_controller(uint64_t cid) {
    BitmapBlockController *ctrl = &all_controller[cid];

    /* 摘下 head */
    bitmap_return_record_sll *head;
    {
        spinlock_guard g(ctrl->lock);
        head = ctrl->head;
        ctrl->head = nullptr;
        ctrl->state = BITMAP_CTRL_OFFLINE;
    }

    if (head == nullptr) return;

    /* 按 bitmap FS_block 分组处理 */
    uint8_t tmp[4096];

    for (bitmap_return_record_sll *r = head; r; r = r->next) {
        uint64_t f = r->base;
        uint64_t end = r->base + r->entry_count;

        while (f < end) {
            uint64_t bm_block = bitmap_base + (f / 32768);
            uint64_t bit_off  = f % 32768;
            uint64_t batch = 32768 - bit_off;
            if (end - f < batch) batch = end - f;

            dev->read_block(bm_block, tmp);

            /* clear bits in range [bit_off, bit_off + batch) */
            for (uint64_t i = 0; i < batch; i++) {
                uint64_t pos = bit_off + i;
                tmp[pos >> 3] &= (uint8_t)~(1 << (pos & 7));
            }

            dev->write_block(bm_block, tmp);
            f += batch;
        }
    }

    /* 释放 record 链表 */
    bitmap_return_record_sll *r = head;
    while (r) {
        bitmap_return_record_sll *next = r->next;
        ::free(r);
        r = next;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── Sweeper ──
 * ═══════════════════════════════════════════════════════════════ */

void BitmapCacheController::sweeper_loop() {
    uint64_t C = ctrl_count;

    while (!sweeper_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        for (uint64_t cid = 0; cid < C; cid++) {
            if (sweeper_stop.load(std::memory_order_relaxed)) return;

            BitmapBlockController *ctrl = &all_controller[cid];

            /* 快速跳过：不需要处理 */
            if (ctrl->head == nullptr) continue;
            if (ctrl->state != BITMAP_CTRL_OFFLINE) continue;

            /* ── 临界区①: 标记 INTEGRATING，防止被上盘 ── */
            {
                spinlock_guard g(ctrl->lock);
                if (ctrl->state != BITMAP_CTRL_OFFLINE || ctrl->head == nullptr)
                    continue;
                ctrl->state = BITMAP_CTRL_INTEGRATING;
                consume_head(sweeper_bucket, ctrl);
            }

            /* ── 临界区外①: 读盘到专职 buffer ── */
            load_bucket(sweeper_bucket, cid);

            /* ── 临界区②: 消费 head（在线期间积压的） ── */
            {
                spinlock_guard g(ctrl->lock);
                if (ctrl->head != nullptr)
                    consume_head(sweeper_bucket, ctrl);
            }

            /* ── 临界区外②: 写盘 ── */
            flush_bucket(sweeper_bucket, cid);

            /* ── 临界区③: 恢复 OFFLINE ── */
            {
                spinlock_guard g(ctrl->lock);
                ctrl->state = BITMAP_CTRL_OFFLINE;
            }
        }
    }
}

void BitmapCacheController::stop_sweeper() {
    sweeper_stop.store(true, std::memory_order_release);
    if (sweeper.joinable())
        sweeper.join();
}

/* ═══════════════════════════════════════════════════════════════
 * ── shutdown_all: 上帝类驱动的整体关闭 ──
 * ═══════════════════════════════════════════════════════════════ */

void BitmapCacheController::shutdown_all() {
    /* 1. 停 sweeper */
    stop_sweeper();

    /* 2. 逐核下野刷桶 */
    for (uint64_t p = 0; p < processor_max; p++)
        flush_processor(p);

    /* 3. 逐 controller 清偿 head 链表 */
    for (uint64_t cid = 0; cid < ctrl_count; cid++)
        flush_controller(cid);
}

/* ═══════════════════════════════════════════════════════════════
 * ── 构造 / 析构 ──
 * ═══════════════════════════════════════════════════════════════ */

BitmapCacheController::BitmapCacheController(const BitmapCacheConfig &cfg)
    : dev(cfg.dev)
    , bitmap_base(cfg.bitmap_base)
    , bitmap_block_count(cfg.bitmap_block_count)
    , processor_max(cfg.processor_max)
    , N(cfg.num_of_blocks_per_bucket_log2)
    , ctrl_count(0)
    , bucket_sz(0)
    , bucket_bits(0)
    , ctrl_seg_bits(0)
    , all_controller(nullptr)
    , all_proc_state(nullptr)
    , raw_buckets(nullptr)
    , sweeper_bucket(nullptr)
{
    /* ── 推导常量 ── */
    ctrl_count     = (bitmap_block_count + (1ULL << N) - 1) >> N;
    bucket_sz      = (1ULL << N) * 4096;
    bucket_bits    = (1ULL << N) * 32768;
    ctrl_seg_bits  = 1ULL << (N + 15);

    fprintf(stderr, "BitmapCache: controller_count=%lu  bucket_sz=%lu\n",
            (unsigned long)ctrl_count, (unsigned long)bucket_sz);

    /* ── 分配 all_controller[] ── */
    all_controller = new BitmapBlockController[ctrl_count]();

    /* ── 校准 free_count：遍历每个 controller 的所有 bitmap 块 ── */
    uint8_t *tmp = new uint8_t[4096];

    for (uint64_t cid = 0; cid < ctrl_count; cid++) {
        BitmapBlockController *ctrl = &all_controller[cid];
        ctrl->state     = BITMAP_CTRL_OFFLINE;
        ctrl->free_count = 0;
        ctrl->scan_hint  = 0;
        ctrl->head       = nullptr;
        ctrl->flags      = 0;

        uint64_t blk_start = bitmap_base + cid * (1ULL << N);
        uint64_t blk_end   = blk_start + (1ULL << N);
        if (blk_end > bitmap_base + bitmap_block_count)
            blk_end = bitmap_base + bitmap_block_count;

        for (uint64_t b = blk_start; b < blk_end; b++) {
            dev->read_block(b, tmp);
            /* 数零位的数量 */
            uint32_t zeros = 0;
            for (uint32_t j = 0; j < 4096; j++) {
                uint8_t x = tmp[j];
                for (int k = 0; k < 8; k++) {
                    if (!(x & (1 << k)))
                        zeros++;
                }
            }
            ctrl->free_count += zeros;
        }

    }

    delete[] tmp;

    /* ── 根据 free_count 降序排序 cid，选取最富的预填充处理器 ── */
    typedef std::pair<uint64_t, uint32_t> CidFree;
    CidFree *sorted = new CidFree[ctrl_count];
    for (uint64_t cid = 0; cid < ctrl_count; cid++)
        sorted[cid] = std::make_pair(cid, all_controller[cid].free_count);

    std::sort(sorted, sorted + ctrl_count,
              [](const CidFree &a, const CidFree &b) {
                  return a.second > b.second;
              });

    /* ── 分配处理器状态和桶 ── */
    this->all_proc_state = new ProcessorBitmapState[processor_max]();
    this->raw_buckets    = new uint8_t[processor_max * bucket_sz]();

    /* ── sweeper 专职 buffer ── */
    this->sweeper_bucket = new uint8_t[bucket_sz]();

    /* ── 预填充：每个处理器分配一个最富的 controller ── */
    uint64_t assign_cnt = processor_max;
    if (assign_cnt > ctrl_count) assign_cnt = ctrl_count;

    for (uint64_t p = 0; p < assign_cnt; p++) {
        ProcessorBitmapState *ps = &all_proc_state[p];
        uint64_t cid = sorted[p].first;
        ps->bucket       = raw_buckets + p * bucket_sz;
        ps->current_cid  = cid;
        ps->last_hint_cid = cid;

        load_bucket(ps->bucket, cid);
        all_controller[cid].state = BITMAP_CTRL_ONLINE;
    }

    /* 未分配到处理器的 processor 保持 current_cid = ~0ULL */
    for (uint64_t p = assign_cnt; p < processor_max; p++) {
        all_proc_state[p].bucket       = raw_buckets + p * bucket_sz;
        all_proc_state[p].current_cid  = ~0ULL;
        all_proc_state[p].last_hint_cid = 0;
    }

    delete[] sorted;

    /* ── 启动 sweeper 线程 ── */
    sweeper = std::thread(&BitmapCacheController::sweeper_loop, this);

    fprintf(stderr, "BitmapCache: ready (%lu processors online)\n",
            (unsigned long)assign_cnt);
}

BitmapCacheController::~BitmapCacheController() {
    if (all_controller != nullptr) {
        shutdown_all();

        delete[] sweeper_bucket;
        delete[] raw_buckets;
        delete[] all_proc_state;
        delete[] all_controller;
    }
}
