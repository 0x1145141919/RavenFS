#include "../include/ravenfs/inode_cache.hpp"

/* ─── select_victim_and_lock — 淘汰 + 持锁返回
 *
 * 优先选空槽（V=0），其次 LRU（accessor==0 T=0 中 stamp 最小）。
 * 成功时返回 way 并**持有该 slot 的锁**，由 load_slot_locked 释放。
 * 失败返回 -1。
 */
int Inode_cache_controller::select_victim_and_lock(controller_set *cs) {
    int best_way = -1;
    uint64_t best_stamp = UINT64_MAX;

    for (int w = 0; w < WAYS_PER_SET; w++) {
        auto &ctrl = cs->controllers[w];

        if (ctrl.num_of_accessor > 0) continue;
        if (ctrl.flags & INODE_CACHE_T) continue;

        if (!(ctrl.flags & INODE_CACHE_V)) {
            if (ctrl.lock.try_lock()) {
                if (ctrl.num_of_accessor == 0 && !(ctrl.flags & INODE_CACHE_T))
                    return w;
                ctrl.lock.unlock();
            }
            continue;
        }

        if (ctrl.last_access_stamp < best_stamp) {
            best_stamp = ctrl.last_access_stamp;
            best_way   = w;
        }
    }

    if (best_way < 0) return -1;

    auto &ctrl = cs->controllers[best_way];
    if (!ctrl.lock.try_lock()) return -1;
    if (ctrl.num_of_accessor > 0 || (ctrl.flags & INODE_CACHE_T)) {
        ctrl.lock.unlock();
        return -1;
    }
    return best_way;
}

/* ─── load_slot_locked — 淘汰加载
 *
 * 进入时 slot 已由 select_victim_and_lock 持有锁。
 * Phase 1: 设 T → 手动解锁（跨函数锁交接结束）
 * Phase 3: guard 管理（不跨函数）
 */
raven_Inode_t *Inode_cache_controller::load_slot_locked(
    controller_set *cs, int way, uint64_t inum, const InumFields &f)
{
    auto &ctrl = cs->controllers[way];
    auto *data = slot_data(f.set_idx, way);

    /* Phase 1: 锁已持 → 捕获旧状态 → T=1 → 手动解锁 */
    uint16_t old_flags = ctrl.flags;
    uint64_t old_tag   = ctrl.tag;
    ctrl.flags |= INODE_CACHE_T;
    ctrl.lock.unlock();

    /* Phase 2: I/O（不持锁） */
    if (old_flags & INODE_CACHE_D) {
        uint64_t old_block = cfg.inode_array_base
            + ((old_tag << cfg.num_of_entries_log2) | f.set_idx);
        cfg.dev->write_block(old_block, data);
    }
    cfg.dev->read_block(inum_to_block(inum), data);

    /* Phase 3: guard → 设新状态 → 自动解锁 */
    {
        spintrylock_spin_guard g(ctrl.lock);
        ctrl.tag   = f.tag;
        ctrl.flags = INODE_CACHE_V;
        ctrl.num_of_accessor   = 1;
        ctrl.last_access_stamp = ++access_stamp_counter;
    }
    return &data->entries[f.row];
}

/* ─── cache_get — 打开 session ─── */
raven_Inode_t *Inode_cache_controller::cache_get(uint64_t inum) {
    if (inum >= cfg.inode_array_count) return nullptr;

    auto f   = decode_inum(inum);
    auto *cs = cfg.all_entries + f.set_idx;

    /* 自旋扫描 8-way：持 guard 后读 flags/tag */
    for (int w = 0; w < WAYS_PER_SET; w++) {
        auto &ctrl = cs->controllers[w];
        spintrylock_spin_guard g(ctrl.lock);
        if ((ctrl.flags & (INODE_CACHE_V | INODE_CACHE_T)) == INODE_CACHE_V
            && ctrl.tag == f.tag) {
            ctrl.num_of_accessor++;
            ctrl.last_access_stamp = ++access_stamp_counter;
            return &slot_data(f.set_idx, w)->entries[f.row];
        }
    }

    int way = select_victim_and_lock(cs);
    if (way < 0) return nullptr;
    return load_slot_locked(cs, way, inum, f);
}

/* ─── cache_put — 关闭 session ─── */
void Inode_cache_controller::cache_put(uint64_t inum) {
    if (inum >= cfg.inode_array_count) return;

    auto f   = decode_inum(inum);
    auto *cs = cfg.all_entries + f.set_idx;

    for (int w = 0; w < WAYS_PER_SET; w++) {
        auto &ctrl = cs->controllers[w];
        spintrylock_spin_guard g(ctrl.lock);
        if ((ctrl.flags & INODE_CACHE_V) && ctrl.tag == f.tag) {
            ctrl.num_of_accessor--;
            return;
        }
    }
}

/* ─── mark_dirty ─── */
void Inode_cache_controller::mark_dirty(uint64_t inum) {
    if (inum >= cfg.inode_array_count) return;

    auto f   = decode_inum(inum);
    auto *cs = cfg.all_entries + f.set_idx;

    for (int w = 0; w < WAYS_PER_SET; w++) {
        auto &ctrl = cs->controllers[w];
        spintrylock_spin_guard g(ctrl.lock);
        if ((ctrl.flags & (INODE_CACHE_V | INODE_CACHE_T)) == INODE_CACHE_V
            && ctrl.tag == f.tag) {
            ctrl.flags |= INODE_CACHE_D;
            return;
        }
    }
}

/* ─── flush_inum — 显式刷脏 ─── */
void Inode_cache_controller::flush_inum(uint64_t inum) {
    if (inum >= cfg.inode_array_count) return;

    auto f   = decode_inum(inum);
    auto *cs = cfg.all_entries + f.set_idx;

    for (int w = 0; w < WAYS_PER_SET; w++) {
        auto &ctrl = cs->controllers[w];
        {
            spintrylock_spin_guard g(ctrl.lock);
            if (!(ctrl.flags & INODE_CACHE_V) || ctrl.tag != f.tag)
                continue;
            if (ctrl.num_of_accessor > 0) return;
            if (!(ctrl.flags & INODE_CACHE_D)) return;
            ctrl.flags = (ctrl.flags & ~INODE_CACHE_D) | INODE_CACHE_T;
        }

        cfg.dev->write_block(inum_to_block(inum), slot_data(f.set_idx, w));

        {
            spintrylock_spin_guard g(ctrl.lock);
            ctrl.flags &= ~INODE_CACHE_T;
        }
        return;
    }
}

/* ─── flush_all — 全量脏回写
 *
 * 每个 slot 自旋拿锁后再操作。宁等锁不跳槽。
 * 清 D 后写盘，若并发又被脏，下次 flush 再写。
 */
void Inode_cache_controller::flush_all() {
    uint64_t n = total_slots();
    for (uint64_t i = 0; i < n; i++) {
        uint64_t set = i / WAYS_PER_SET;
        int      way = i % WAYS_PER_SET;
        auto &ctrl = cfg.all_entries[set].controllers[way];

        uint64_t block;
        {
            spintrylock_spin_guard g(ctrl.lock);
            if (!(ctrl.flags & INODE_CACHE_D)) continue;
            ctrl.flags &= ~INODE_CACHE_D;
            block = cfg.inode_array_base
                + ((ctrl.tag << cfg.num_of_entries_log2) | set);
        }

        cfg.dev->write_block(block, slot_data(set, way));
    }
}