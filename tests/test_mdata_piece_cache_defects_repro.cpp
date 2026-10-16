/* SPDX-License-Identifier: GPL-2.0 */
/* ═══════════════════════════════════════════════════════════════════════════
 * tests/test_mdata_piece_cache_defects_repro.cpp
 *
 * 确定性复现 mdata_piece_caches_mgr（self_reliat）**尚未修复**的缺陷。
 * 对应文档：Docs/runtime_tech/07_mdata_piece_cache_defects.md。
 *
 *   H2  flush 脏写回无 pin：flush 持页写盘期间活会话改内容 + 置脏，flush 返回时
 *       fetch_and(~DIRTY) 把该脏标清掉 → 页非脏被丢弃 → 静默丢更新。**确定性**。
 *   H3  flush_all ⨯ reap 重叠：reap 释放 flush 正要读的页 → 读已释放内存（UAF）。
 *       **需 ASan 观测**（不加 ASan 通常不崩，仅逻辑复现）。
 *   H7  不平衡 put（无配对 get）→ accessor 下溢为 ~2^32 → 行永不可淘汰（契约依赖）。
 *   H5  mdp_ctrl_acquire 持读锁自旋 / 读锁跨 I/O → 写者饥饿（无确定性复现，见文档）。
 *   H6  get↔reap_expired_strong 重建抖动（无确定性复现，见文档）。
 *
 * 独立编译（不接 tests/CMakeLists.txt，避免与使用者未提交改动纠缠）：
 *   cd /home/PS/PS_git/OS_pj_uefi/ravenfs
 *   g++ -I include -std=gnu++20 -pthread -O2 -g tests/test_mdata_piece_cache_defects_repro.cpp \
 *       tests/cpu_id_stub.cpp build_v20/juvenile_god/libravenfs_juvenile.a -o /tmp/opencode/mdp_defects
 *   /tmp/opencode/mdp_defects
 * ASan（判定 H3 的 UAF 必需）：
 *   g++ -I include -std=gnu++20 -pthread -O1 -g -fsanitize=address \
 *       tests/test_mdata_piece_cache_defects_repro.cpp tests/cpu_id_stub.cpp \
 *       build_v20/juvenile_god/libravenfs_juvenile.a -o /tmp/opencode/mdp_defects_asan
 *   /tmp/opencode/mdp_defects_asan
 *
 * 退出码：0 = 每个"预期复现"的缺陷都复现了（未被误修/回归）；
 *         非 0 = 某预期缺陷未复现（可能已被修复 —— 应同步更新文档）。
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "ravenfs/mdata_piece_caches.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

/* CTRL_ARRAY 档（无片锁 → 会话/flush/reap 天然并发重叠，便于确定性复现）。*/
static constexpr uint64_t PIECES = 8;    /* bits=3 ≤ 13 → MDP_MODE_CTRL_ARRAY */
static constexpr uint64_t BLK    = 0;    /* 目标负载起始块号（偶数、区间内）*/

static std::vector<uint8_t>   g_img;                 /* 后备镜像（blk*4096 起）*/
static std::atomic<uint64_t>  g_reads{0};            /* read_block 调用数 */

static constexpr uint8_t MAGIC1 = 0x11;              /* flush 捕获进盘的内容 */
static constexpr uint8_t MAGIC2 = 0x22;              /* 会话随后改写的内容 */

/* ── 写回钩子模式（用 write_block 回调做确定性同步点）── */
enum { WB_NORMAL = 0, WB_COPY_THEN_BLOCK, WB_BLOCK_THEN_COPY };
static std::atomic<int>  g_wb_mode{WB_NORMAL};
static std::atomic<bool> g_wb_entered{false};
static std::atomic<bool> g_wb_release{false};

static result_code_t img_read(void *, uint64_t blk, uint64_t nblk, void *buf) {
    g_reads.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(buf, g_img.data() + blk * 4096, (size_t)nblk * 4096);
    return RC_OK;
}

static result_code_t img_write(void *, uint64_t blk, uint64_t nblk, const void *buf) {
    int m = g_wb_mode.load(std::memory_order_relaxed);
    if (m == WB_COPY_THEN_BLOCK) {   /* H2：先落盘（捕获旧内容）再卡住，让会话改页 */
        std::memcpy(g_img.data() + blk * 4096, buf, (size_t)nblk * 4096);
        g_wb_mode.store(WB_NORMAL, std::memory_order_relaxed);   /* 一次性 */
        g_wb_entered.store(true, std::memory_order_relaxed);
        while (!g_wb_release.load(std::memory_order_relaxed)) std::this_thread::yield();
        return RC_OK;
    }
    if (m == WB_BLOCK_THEN_COPY) {   /* H3：先卡住（期间被 reap free），恢复后再读 buf → UAF */
        g_wb_mode.store(WB_NORMAL, std::memory_order_relaxed);   /* 一次性 */
        g_wb_entered.store(true, std::memory_order_relaxed);
        while (!g_wb_release.load(std::memory_order_relaxed)) std::this_thread::yield();
        std::memcpy(g_img.data() + blk * 4096, buf, (size_t)nblk * 4096);   /* buf 可能已释放 */
        return RC_OK;
    }
    std::memcpy(g_img.data() + blk * 4096, buf, (size_t)nblk * 4096);
    return RC_OK;
}

static mdata_piece_caches_mgr *make_mgr() {
    mdata_piece_caches_cfg_t cfg{};
    cfg.mode        = cache_work_mode_t::self_reliat_cache;
    cfg.read_block  = img_read;
    cfg.write_block = img_write;
    cfg.io_ctx      = nullptr;
    cfg.Piece_count = PIECES;
    cfg.Piece_base_FS_block_idx = 0;
    result_code_t rc = RC_OK;
    auto *m = new mdata_piece_caches_mgr(cfg, &rc);
    if (rc != RC_OK || !m->is_ready()) { delete m; return nullptr; }
    return m;
}

/* ═══ H2 — flush 脏写回无 pin → 清掉活会话的 DIRTY → 丢更新 ═══ */
static bool case_h2() {
    g_img.assign(PIECES * 8192, 0);
    mdata_piece_caches_mgr *m = make_mgr();
    if (!m) { std::printf("H2: mgr init failed\n"); return false; }

    { mdata_piece_session s(m, BLK); std::memset(s.data, MAGIC1, 8192); s.dirty(); }  /* 造脏 */

    g_wb_release.store(false); g_wb_entered.store(false);
    g_wb_mode.store(WB_COPY_THEN_BLOCK);
    std::thread fl([m] { m->flush_all(); });
    while (!g_wb_entered.load()) std::this_thread::yield();   /* flush 已把 MAGIC1 落盘并卡住 */

    { mdata_piece_session s(m, BLK); std::memset(s.data, MAGIC2, 8192); s.dirty(); }  /* 并发改页 + 置脏 */

    g_wb_release.store(true); fl.join();   /* flush 返回 → fetch_and(~DIRTY) 清掉上面那次置脏 */

    m->self_reliat_reap_expired_strong(MDP_SQUEEZE_PRESSURE);   /* accessor==0 且（被清脏后）非脏 → 丢弃 */

    const bool lost = (g_img[BLK * 4096] == MAGIC1);   /* 盘上仍是旧内容 ⇒ MAGIC2 丢失 */
    delete m;
    std::printf("H2: disk=%#x (会话写入 %#x) -> %s\n",
                g_img[BLK * 4096], MAGIC2, lost ? "REPRODUCED (lost update)" : "clean");
    return lost;
}

/* ═══ H3 — flush_all ⨯ reap 重叠 → 读已释放页（UAF，需 ASan） ═══ */
static bool case_h3() {
    g_img.assign(PIECES * 8192, 0);
    mdata_piece_caches_mgr *m = make_mgr();
    if (!m) { std::printf("H3: mgr init failed\n"); return false; }

    { mdata_piece_session s(m, BLK); std::memset(s.data, MAGIC1, 8192); s.dirty(); }

    g_wb_release.store(false); g_wb_entered.store(false);
    g_wb_mode.store(WB_BLOCK_THEN_COPY);
    std::thread fl([m] { m->flush_all(); });          /* 进入 write_block 并卡住（尚未读 buf）*/
    while (!g_wb_entered.load()) std::this_thread::yield();

    /* 契约上禁止的并发：reap 接管 → 写回（走 NORMAL）→ 释放该页；flush 仍在等 */
    m->self_reliat_reap_expired_strong(MDP_SQUEEZE_PRESSURE);

    g_wb_release.store(true); fl.join();              /* flush 恢复 → memcpy 读已释放的 buf */
    delete m;
    std::printf("H3: flush 的 write_block 恢复后读取被 reap 释放的页 -> "
                "REPRODUCED (ASan 下为 heap-use-after-free；无 ASan 常不崩)\n");
    return true;   /* 逻辑复现；UAF 判定依赖 ASan */
}

/* ═══ H7 — 不平衡 put → accessor 下溢 → 行永不可淘汰 ═══ */
static bool case_h7() {
    g_img.assign(PIECES * 8192, 0);
    mdata_piece_caches_mgr *m = make_mgr();
    if (!m) { std::printf("H7: mgr init failed\n"); return false; }

    { mdata_piece_session s(m, BLK); }                    /* get + put → 驻留、accessor=0 */
    m->self_reliat_put(BLK);                              /* 不平衡 put → accessor 下溢 ~2^32 */
    m->self_reliat_reap_expired_strong(MDP_SQUEEZE_PRESSURE);   /* 要求 accessor==0 → 跳过该行 */

    const uint64_t before = g_reads.load();
    { mdata_piece_session s(m, BLK); }                    /* 若行未被淘汰 → 命中（不再读盘）*/
    const bool not_evicted = (g_reads.load() == before);
    delete m;
    std::printf("H7: 强淘汰后再 get %s -> %s\n",
                not_evicted ? "命中（行不可淘汰）" : "未命中（已淘汰）",
                not_evicted ? "REPRODUCED (accessor underflow)" : "clean");
    return not_evicted;
}

int main() {
    std::printf("=== mdata_piece_caches_mgr 未修复缺陷复现（见 Docs/runtime_tech/07_*.md）===\n");
    const bool h2 = case_h2();
    const bool h3 = case_h3();
    const bool h7 = case_h7();
    std::printf("============== SUMMARY: H2=%s  H3=%s  H7=%s ==============\n",
                h2 ? "REPRODUCED" : "clean",
                h3 ? "REPRODUCED(ASan needed)" : "clean",
                h7 ? "REPRODUCED" : "clean");
    /* H5/H6 无确定性复现（见文档）。全预期复现 → 0。 */
    return (h2 && h3 && h7) ? 0 : 1;
}
