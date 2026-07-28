#include "../include/ravenfs/inode_bitmap_cache.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <utility>

/* ═══════════════════════════════════════════════════════════════
 * ── Decode / Encode ──
 * ═══════════════════════════════════════════════════════════════ */

BitmapFields InodeBitmapCacheController::decode(uint64_t inum) const {
    return decode_bitmap_idx(inum, N);
}

uint64_t InodeBitmapCacheController::encode(uint64_t cid, uint64_t bucket_pos) const {
    return (cid << (N + 15)) + bucket_pos;
}

/* ═══════════════════════════════════════════════════════════════
 * ── 位图运算（裸 buffer）──
 * ═══════════════════════════════════════════════════════════════ */

uint64_t InodeBitmapCacheController::scan_run(
    const uint8_t *buf, uint64_t buf_bits, uint64_t start)
{
    for (uint64_t i = 0; i < buf_bits * 2; i++) {
        uint64_t pos = (start + i) % buf_bits;
        uint64_t byte_idx = pos >> 3;
        uint8_t  bit_mask = 1 << (pos & 7);
        if ((buf[byte_idx] & bit_mask) == 0)
            return pos;
    }
    return ~0ULL;
}

void InodeBitmapCacheController::set_bits(uint8_t *buf, uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t pos = start + i;
        buf[pos >> 3] |= (uint8_t)(1 << (pos & 7));
    }
}

void InodeBitmapCacheController::clear_bits(uint8_t *buf, uint64_t start, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t pos = start + i;
        buf[pos >> 3] &= (uint8_t)~(1 << (pos & 7));
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── Bucket I/O ──
 * ═══════════════════════════════════════════════════════════════ */

void InodeBitmapCacheController::load_bucket(uint64_t cid) {
    uint64_t base_fblock = bitmap_base + cid * (1ULL << N);
    uint64_t blocks = 1ULL << N;
    for (uint64_t i = 0; i < blocks; i++)
        dev->read_block(base_fblock + i, bucket + i * 4096);
}

void InodeBitmapCacheController::flush_bucket(uint64_t cid) {
    uint64_t base_fblock = bitmap_base + cid * (1ULL << N);
    uint64_t blocks = 1ULL << N;
    for (uint64_t i = 0; i < blocks; i++)
        dev->write_block(base_fblock + i, bucket + i * 4096);
}

/* ═══════════════════════════════════════════════════════════════
 * ── consume_head ──
 * ═══════════════════════════════════════════════════════════════ */

void InodeBitmapCacheController::consume_head(BitmapBlockController *ctrl) {
    bitmap_return_record_sll *cur = ctrl->head;
    ctrl->head = nullptr;
    while (cur) {
        bitmap_return_record_sll *next = cur->next;
        uint64_t rel_start = cur->base & (ctrl_seg_bits - 1);
        clear_bits(bucket, rel_start, cur->entry_count);
        ::free(cur);
        cur = next;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── try_alloc — 在当前桶中尝试分配 ──
 *
 * 入口条件：alloc_lock 已持有。
 * 内部握 controller.lock 仅更新 free_count / scan_hint（极短）。
 * ═══════════════════════════════════════════════════════════════ */

uint64_t InodeBitmapCacheController::try_alloc() {
    BitmapBlockController *ctrl = &all_controller[proc.current_cid];

    uint64_t pos = scan_run(bucket, bucket_bits, ctrl->scan_hint);
    if (pos != ~0ULL)
        goto alloc;

    /* 空仓 → consume head 重试一次 */
    {
        spinlock_guard g(ctrl->lock);
        consume_head(ctrl);
    }

    pos = scan_run(bucket, bucket_bits, 0);
    if (pos == ~0ULL)
        return ~0ULL;

alloc:
    set_bits(bucket, pos, 1);
    {
        spinlock_guard g(ctrl->lock);
        ctrl->free_count -= 1;
        ctrl->scan_hint = pos;
    }
    return encode(proc.current_cid, pos);
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

void InodeBitmapCacheController::switch_to(uint64_t new_cid) {
    uint64_t old_cid = proc.current_cid;

    /* ── Phase 1 ── */
    spinlock_guard alloc_guard(proc.alloc_lock);

    if (old_cid != ~0ULL) {
        spinlock_guard ctrl_guard(all_controller[old_cid].lock);
        all_controller[old_cid].state = BITMAP_CTRL_OFFLINE;
    }

    {
        spinlock_guard ctrl_guard(all_controller[new_cid].lock);
        all_controller[new_cid].state = BITMAP_CTRL_ONLINE;
    }

    proc.current_cid = ~0ULL;
    alloc_guard.~spinlock_guard();

    /* ── Phase 2 (无锁) ── */
    if (old_cid != ~0ULL)
        flush_bucket(old_cid);

    load_bucket(new_cid);

    /* ── Phase 3 ── */
    proc.alloc_lock.lock();

    {
        spinlock_guard ctrl_guard(all_controller[new_cid].lock);
        consume_head(&all_controller[new_cid]);
    }

    proc.current_cid  = new_cid;
    proc.last_hint_cid = new_cid;
    proc.alloc_lock.unlock();
}

/* ═══════════════════════════════════════════════════════════════
 * ── alloc_inode: 两层分配 ──
 *
 * 1: 本地桶 + consume_head 重试
 * 2: 切换到一个 OFFLINE 的 controller
 *
 * ret = inum，~0ULL = 满
 * ═══════════════════════════════════════════════════════════════ */

uint64_t InodeBitmapCacheController::alloc_inode() {
    if (!all_controller) return ~0ULL;
    uint64_t C = ctrl_count;

    uint8_t *mark = (uint8_t *)alloca(C);
    memset(mark, 0, C);

    uint64_t cid;

    /* ═══════ Tier 1: 本地桶 ═══════ */
    proc.alloc_lock.lock();
    cid = proc.current_cid;
    if (cid != ~0ULL) {
        uint64_t block = try_alloc();
        if (block != ~0ULL) {
            proc.alloc_lock.unlock();
            return block;
        }
        mark[cid] = 1;
    }
    proc.alloc_lock.unlock();

    /* ═══════ Tier 2: 找 OFFLINE 的 controller 切换 ═══════ */
    cid = proc.last_hint_cid;
    for (uint64_t i = 0; i < C; i++) {
        uint64_t t = (cid + 1 + i) % C;
        if (mark[t]) continue;

        BitmapBlockController *ctrl = &all_controller[t];
        if (ctrl->state != BITMAP_CTRL_OFFLINE) continue;
        if (ctrl->free_count == 0) { mark[t] = 1; continue; }

        uint64_t first_inum = t * ctrl_seg_bits;
        if (first_inum >= inode_array_upper) { mark[t] = 1; continue; }

        switch_to(t);
        cid = t;

        proc.alloc_lock.lock();
        uint64_t block = try_alloc();
        if (block != ~0ULL) {
            proc.alloc_lock.unlock();
            return block;
        }
        mark[cid] = 1;
        proc.alloc_lock.unlock();
    }

    return ~0ULL;
}

/* ═══════════════════════════════════════════════════════════════
 * ── free_inode ──
 *
 * 本桶管 → 直清 bit
 * 跨桶管 → 挂 record
 * ═══════════════════════════════════════════════════════════════ */

void InodeBitmapCacheController::free_inode(uint64_t inum) {
    if (inum >= inode_array_upper) return;

    BitmapFields f = decode(inum);

    /* ── 归本桶管 ── */
    if (f.controller_id == proc.current_cid) {
        spinlock_guard alloc_guard(proc.alloc_lock);
        uint64_t rel_start = inum & (ctrl_seg_bits - 1);
        clear_bits(bucket, rel_start, 1);
        {
            spinlock_guard ctrl_guard(all_controller[f.controller_id].lock);
            all_controller[f.controller_id].free_count += 1;
        }
        return;
    }

    /* ── 跨桶：挂 record ── */
    bitmap_return_record_sll *rec = (bitmap_return_record_sll *)malloc(sizeof(*rec));
    rec->base        = inum;
    rec->entry_count = 1;
    rec->next        = nullptr;

    BitmapBlockController *ctrl = &all_controller[f.controller_id];
    {
        spinlock_guard g(ctrl->lock);
        rec->next = ctrl->head;
        ctrl->head = rec;
        ctrl->free_count += 1;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── test_inode ──
 *
 * 如果目标在桶内走桶，否则直读单块。
 * ═══════════════════════════════════════════════════════════════ */

bool InodeBitmapCacheController::test_inode(uint64_t inum) {
    if (inum >= inode_array_upper) return true;

    BitmapFields f = decode(inum);

    if (f.controller_id == proc.current_cid) {
        uint64_t bucket_byte = (f.block_idx * 4096) + (f.bit_in_block >> 3);
        return (bucket[bucket_byte] >> (f.bit_in_block & 7)) & 1;
    }

    uint64_t byte_off   = f.bit_in_block >> 3;
    uint8_t  bit_mask   = 1 << (f.bit_in_block & 7);
    uint8_t  buf[4096];
    uint64_t abs_block  = bitmap_base + f.controller_id * (1ULL << N) + f.block_idx;
    dev->read_block(abs_block, buf);
    return (buf[byte_off] & bit_mask) != 0;
}

/* ═══════════════════════════════════════════════════════════════
 * ── shutdown_all ──
 * ═══════════════════════════════════════════════════════════════ */

void InodeBitmapCacheController::shutdown_all() {
    /* 1. 刷当前桶 */
    if (proc.current_cid != ~0ULL) {
        spinlock_guard alloc_guard(proc.alloc_lock);
        {
            spinlock_guard ctrl_guard(all_controller[proc.current_cid].lock);
            all_controller[proc.current_cid].state = BITMAP_CTRL_OFFLINE;
        }
        uint64_t cid = proc.current_cid;
        proc.current_cid = ~0ULL;
        alloc_guard.~spinlock_guard();

        flush_bucket(cid);
    }

    /* 2. 逐 controller 清偿 head 链表 */
    uint8_t tmp[4096];
    for (uint64_t cid = 0; cid < ctrl_count; cid++) {
        BitmapBlockController *ctrl = &all_controller[cid];

        bitmap_return_record_sll *head;
        {
            spinlock_guard g(ctrl->lock);
            head = ctrl->head;
            ctrl->head = nullptr;
            ctrl->state = BITMAP_CTRL_OFFLINE;
        }
        if (!head) continue;

        for (bitmap_return_record_sll *r = head; r; r = r->next) {
            uint64_t f = r->base;
            uint64_t end = r->base + r->entry_count;
            while (f < end) {
                uint64_t bm_block = bitmap_base + (f / 32768);
                uint64_t bit_off  = f % 32768;
                uint64_t batch = 32768 - bit_off;
                if (end - f < batch) batch = end - f;
                dev->read_block(bm_block, tmp);
                for (uint64_t i = 0; i < batch; i++) {
                    uint64_t pos = bit_off + i;
                    tmp[pos >> 3] &= (uint8_t)~(1 << (pos & 7));
                }
                dev->write_block(bm_block, tmp);
                f += batch;
            }
        }

        bitmap_return_record_sll *r = head;
        while (r) {
            bitmap_return_record_sll *next = r->next;
            ::free(r);
            r = next;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ── 构造 / 析构 ──
 * ═══════════════════════════════════════════════════════════════ */

InodeBitmapCacheController::InodeBitmapCacheController(const InodeBitmapCacheConfig &cfg)
    : dev(cfg.dev)
    , bitmap_base(cfg.bitmap_base)
    , bitmap_block_count(cfg.bitmap_block_count)
    , inode_array_upper(cfg.inode_array_count)
    , N(cfg.num_of_blocks_per_bucket_log2)
    , ctrl_count(0)
    , bucket_sz(0)
    , bucket_bits(0)
    , ctrl_seg_bits(0)
    , all_controller(nullptr)
    , bucket(nullptr)
{
    proc.current_cid   = ~0ULL;
    proc.last_hint_cid = 0;

    ctrl_count    = (bitmap_block_count + (1ULL << N) - 1) >> N;
    bucket_sz     = (1ULL << N) * 4096;
    bucket_bits   = (1ULL << N) * 32768;
    ctrl_seg_bits = 1ULL << (N + 15);

    fprintf(stderr, "InodeBitmapCache: controller_count=%lu  bucket_sz=%lu\n",
            (unsigned long)ctrl_count, (unsigned long)bucket_sz);

    all_controller = new BitmapBlockController[ctrl_count]();

    uint8_t *tmp  = new uint8_t[4096];
    uint64_t best_free = 0;
    uint64_t best_cid  = ~0ULL;

    for (uint64_t cid = 0; cid < ctrl_count; cid++) {
        BitmapBlockController *ctrl = &all_controller[cid];
        ctrl->state     = BITMAP_CTRL_OFFLINE;
        ctrl->free_count = 0;
        ctrl->scan_hint  = 0;
        ctrl->head       = nullptr;
        ctrl->flags      = 0;

        uint64_t first_inum = cid * ctrl_seg_bits;
        if (first_inum >= inode_array_upper) continue;

        uint64_t last_inum = first_inum + ctrl_seg_bits;
        if (last_inum > inode_array_upper)
            last_inum = inode_array_upper;

        uint32_t free_count = 0;

        for (uint64_t inum = first_inum; inum < last_inum; ) {
            uint64_t block_idx = inum / 32768;
            uint64_t bit_off   = inum % 32768;
            uint64_t abs_block = bitmap_base + block_idx;

            dev->read_block(abs_block, tmp);

            uint64_t batch = 32768 - bit_off;
            if (inum + batch > last_inum)
                batch = last_inum - inum;

            for (uint64_t i = 0; i < batch; i++) {
                uint64_t pos = bit_off + i;
                if (!(tmp[pos >> 3] & (1 << (pos & 7))))
                    free_count++;
            }

            inum += batch;
        }

        ctrl->free_count = free_count;

        if (free_count > best_free) {
            best_free = free_count;
            best_cid  = cid;
        }

    }

    delete[] tmp;

    bucket = new uint8_t[bucket_sz]();

    if (best_cid != ~0ULL) {
        load_bucket(best_cid);
        proc.current_cid   = best_cid;
        proc.last_hint_cid = best_cid;
        all_controller[best_cid].state = BITMAP_CTRL_ONLINE;
    }

    fprintf(stderr, "InodeBitmapCache: ready\n");
}

InodeBitmapCacheController::~InodeBitmapCacheController() {
    if (all_controller) {
        shutdown_all();
        delete[] bucket;
        delete[] all_controller;
    }
}
