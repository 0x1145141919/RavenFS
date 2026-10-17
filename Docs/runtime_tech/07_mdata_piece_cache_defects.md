# mdata_piece_caches_mgr 未修复缺陷（白盒并发 + 功能审计）

> 对象：`include/ravenfs/mdata_piece_caches.hpp` + `juvenile_god/mdata_piece_cache.cpp`
> （god 用户态 8K 元数据负载片缓存；两档 `MDP_MODE_CTRL_ARRAY`(bits≤13) / `MDP_MODE_RADIX_SHARD`(bits>13)）
> 来源：2026-10-17 两支测试团队（① 功能/冒烟 + 新功能覆盖，② 白盒并发审计 + sanitizer 压测）。
> 本篇只记**尚未修复**的缺陷；已修项见文末 §附录。
> 行号会随改动漂移 —— 以函数名 + 代码片段为准。

---

## 0. 结论快照

| id | 一句话 | 底子 | 严重度 | 确定性复现 |
|----|--------|------|--------|------------|
| **H2** | flush 脏写回无 pin → 清掉活会话的 DIRTY → 静默丢更新（**残留：仅 CTRL_ARRAY**；RADIX 已由 flush 写锁关闭） | 数据完整性 | 中（小盘/契约） | ✅ 是（`test_..._defects_repro` case H2，CTRL_ARRAY 档） |
| **H3** | `flush_all` ⨯ `reap` 重叠 → 读已释放页（UAF）（**残留：仅 CTRL_ARRAY**；RADIX 已由 flush 写锁关闭） | 内存安全 | 高（CTRL_ARRAY 契约门控） | ✅ 是（ASan，case H3，CTRL_ARRAY 档） |
| **H5** | acquire 持读锁自旋 / reap 读锁跨 I/O → 写者饥饿 | 活性/延迟 | 中 | ✖ 无（理论） |
| **H6** | `get` miss 建链 ↔ `reap_expired_strong` 摘表 抖动 | 活性/CPU | 低 | ✖ 无（理论） |
| **H7** | 不平衡 `put` → accessor 下溢 ~2^32 → 行永不可淘汰 | 健壮性（契约依赖） | 低 | ✅ 是（case H7） |

> **H2/H3 的关闭范围（2026-10-17）**：RADIX 档 `flush_all` 改持**片写锁**
> （`unique_lock`），把「卸载时无访问者」的契约落成机制，H2/H3 在该档关闭。
> CTRL_ARRAY 档（bits≤13，小盘、控制器全量常驻、无片锁）按把关意见**不下沉机制**：
> 由上层保证「卸载时为静默点、无 get/put/mark_dirty/reap 并发」，作为安全卸载契约保留。

已由本模块测试确认**成立**的设计不变量（审计未发现漏洞，供参考）：
RADIX 档 `flush_all` 写锁独占（与 get 探测段 / put / mark_dirty / reap 全互斥）；其余读锁不跨会话（`locate_with_rdlock` miss 即释、handle RAII）；无 read→write 自死锁（`get` 仅在 handle 为空时才 `ensure_path_with_wlock`）；单键唯一分片 → 无跨片锁序；`io_ing`/`accessor` 钉住叶表使 `reap_expired_strong` 不误释放在用叶表；CTRL_ARRAY 全程无锁仅靠 `ctrl_word`（静默点契约见上）。

---

## 1. 复现方式

```sh
cd /home/PS/PS_git/OS_pj_uefi/ravenfs

# 定向确定性复现（H2 / H3 / H7；H2/H3 为 CTRL_ARRAY 档残留）
g++ -I include -std=gnu++20 -pthread -O2 -g tests/test_mdata_piece_cache_defects_repro.cpp \
    tests/cpu_id_stub.cpp build_v20/juvenile_god/libravenfs_juvenile.a -o /tmp/opencode/mdp_defects
/tmp/opencode/mdp_defects          # 退出码 0 = 各预期缺陷都复现

# H3 的 UAF 判定需 ASan
g++ -I include -std=gnu++20 -pthread -O1 -g -fsanitize=address \
    tests/test_mdata_piece_cache_defects_repro.cpp tests/cpu_id_stub.cpp \
    build_v20/juvenile_god/libravenfs_juvenile.a -o /tmp/opencode/mdp_defects_asan
/tmp/opencode/mdp_defects_asan     # 期望 heap-use-after-free @ img_write（freed by reap_one）

# 全量白盒并发压测 harness（团队②，含 accessor 账目/持锁不变式断言 + 各 phase）
g++ -I include -std=gnu++20 -pthread -O2 -g tests/test_mdata_piece_cache_concurrency.cpp \
    tests/cpu_id_stub.cpp build_v20/juvenile_god/libravenfs_juvenile.a -o /tmp/opencode/mdp_conc
setarch -R /tmp/opencode/mdp_conc

# 功能/冒烟（团队①，覆盖两档 + 新功能 + 统计钩子）
g++ -I include -std=gnu++20 -pthread -O2 -g tests/test_mdata_piece_cache_smoke.cpp \
    tests/cpu_id_stub.cpp build_v20/juvenile_god/libravenfs_juvenile.a -o /tmp/opencode/mdp_smoke
/tmp/opencode/mdp_smoke

# 上帝类集成（RADIX 档：镜像 ≥128MiB → Piece_count=bitmap_start/2 > 8192）
#   mount → insert/delete → dir_verify_tree 的 flush_all 同步点 → 卸载 flush_all → 重挂载
truncate -s 128M /tmp/opencode/rfs128.img
./build_v20/mkfs/mkfs.ravenfs -f /tmp/opencode/rfs128.img
./build_v20/tests/ravenfs_juv_insert /tmp/opencode/rfs128.img 500 0 selfreliat
./build_v20/tests/ravenfs_juv_delete /tmp/opencode/rfs128.img 400 0 selfreliat
#   打印 "mdata_piece cache: pieces=16383 mode=RADIX_SHARD" 即命中片写锁路径
```

> `build_v20` 为已配置构建目录；亦可 `cmake --build build_v20 --target ravenfs_juvenile` 先出库。

---

## 2. H2 — `mdp_ctrl_flush_one` 脏写回无 pin → 静默丢更新（残留：仅 CTRL_ARRAY）

> **状态（2026-10-17）**：RADIX 档已关闭 —— `slice_radix_tree::flush_all` 改持**片写锁**
> （`unique_lock`，见 §3）。`mark_dirty` 走 `locate_with_rdlock`，被 flush 写锁挡在
> `fetch_and(~DIRTY)` 之后，故新置的 DIRTY 不会被这次写回清掉。
> 以下描述与复现仅针对 **CTRL_ARRAY 档**（bits≤13，小盘，无片锁）。

**定位** `juvenile_god/mdata_piece_cache.cpp` · `mdp_ctrl_flush_one` 脏分支：

```cpp
if ((w >> CTRL_FLAGS_SHIFT) & CTRL_FLAG_DIRTY) {
    rt.write_block(rt.io_ctx, c->blk, 2, c->cache_data);   /* ① 无 pin，持页写盘 */
    c->ctrl_word.fetch_and(~(CTRL_FLAG_DIRTY << CTRL_FLAGS_SHIFT), release);  /* ② 无条件清 DIRTY */
    return false;
}
```

**根因** 该脏分支**不取 accessor、不 CAS 占坑、不清点写入版本**：flush 在 ① 写盘期间，活会话（`get` 命中即占坑 + 直接改同一 `cache_data`）把页改成新内容并 `mark_dirty` 置 DIRTY；② 回来时 `fetch_and(~DIRTY)` **无条件清掉**了这次置脏。此后该行“working 非脏”，`reap`/`flush` 丢弃它时**不回写** → 会话的新内容丢失。头注释“get 并发安全 / 并发双写同页良性”在此**过度承诺**。

**复现** case H2（`WB_COPY_THEN_BLOCK`：write_block 先落盘再卡住，主线程趁机改页置脏。期望 `disk=0x11 (会话写入 0x22) -> REPRODUCED`；fixture `PIECES=8` → CTRL_ARRAY）。TSan 亦在 supported 路径报 `img_write` 竞态（团队②）。

**把关决策（CTRL_ARRAY）** 小盘控制器数组全量常驻、无片锁；不走 copy-out/占坑机制，
**由上层保证「卸载/显式同步是静默点，无 get/put/mark_dirty/reap 并发」**，作为安全卸载
契约保留。若未来 CTRL_ARRAY 也要支持并发同步，再按下列建议之一下沉机制：

- 引入 **copy-out + WB 标志**（⇄ ko 的 `ROWF_WB` / 文档 `03_shared_radix_cache.md` 所述“copy-out 排他”）：写回前快照内容/置 WB 位，写回后**只清自己置的 DIRTY 版本**（或清 WB），保留会话新置的 DIRTY。
- 或退一步：flush 脏写回前 **CAS 占坑（accessor 或专用 pin 位）**，与活会话写入互斥；写回后再释放。

---

## 3. H3 — `flush_all` ⨯ `reap` 重叠 → 页 UAF（残留：仅 CTRL_ARRAY）

> **状态（2026-10-17）**：RADIX 档已关闭 —— `slice_radix_tree::flush_all` 由
> `std::shared_lock` 改为 **`std::unique_lock`**（片写锁），把「卸载时刻独占、无访问者」
> 的契约落成机制。写回 I/O 期间 `get` 探测段 / `put` / `mark_dirty` /
> `reap_expired` / `reap_expired_strong` 全部被挡在片外，页不可能被改写或释放。
> 代价是写锁跨磁盘 I/O：本函数只允许在静默点（卸载/显式同步）调用，禁止放周期路径。
> 以下描述与复现仅针对 **CTRL_ARRAY 档**（`PIECES=8`，无片锁，行级 CAS 是唯一防线）。

**定位** `mdp_ctrl_flush_one`（读 `c->cache_data` 传给 `write_block`）vs `mdp_ctrl_reap_one`：

```cpp
/* reap 接管成功：写回脏行 → 释放页 → 回 idle */
if (dirty) rt.write_block(rt.io_ctx, c->blk, 2, c->cache_data);
mdp_free(c->cache_data);      /* ← flush 可能正持此指针 */
c->cache_data = nullptr;
```

**根因** flush 的脏路径**零内部防御**：它只 load 了 `ctrl_word` 就去做 I/O，行仍是
`working && accessor==0`；reap 可 CAS `working→evicting` 并 `mdp_free` 掉 flush
正要/正在读的页。CTRL_ARRAY 档没有片锁，这是**契约禁止路径**上的真实 UAF。
（RADIX 档此前也不是「读锁不够」：`reap_expired_strong` 本就取写锁、被 flush 的读锁
排除；真正会重叠的是同为读锁的 `reap_expired`。改成写锁后两者一并关闭。）

**复现** case H3：`WB_BLOCK_THEN_COPY`（flush 进 write_block 先卡住）→ 主线程 `reap_expired_strong(PRESSURE)` 接管并释放该页 → 放行 flush → `memcpy` 读已释放页。ASan：

```
ERROR: AddressSanitizer: heap-use-after-free ... in img_write
  #1 ... img_write tests/test_mdata_piece_cache_defects_repro.cpp:74
freed by ... mdp_free <- mdp_ctrl_reap_one
```

**把关决策（CTRL_ARRAY）** 不下沉机制（不加 flush-vs-reap 锁、不改 copy-out）：
小盘控制器数组全量常驻，`flush_all` 只在卸载时刻调用；由上层保证**无并发访问者**
（先停淘汰线程、无在途 session）后再同步/卸载。CTRL_ARRAY 的 H2/H3 因此按
**安全卸载契约**保留，不视为需在本模块内修复的缺陷。

---

## 4. H5 — acquire 持读锁自旋 + reap 读锁跨 I/O → 写者饥饿（中 · 活性）

**定位** `slice_radix_tree::mdp_ctrl_acquire` 的等待分支 + `reap_expired`：

```cpp
cpu_relax();    /* io_ing：他人加载中；evicting：淘汰接管中 —— 仍持调用方读锁 */
```

**根因/分析** RADIX 档下 `get` 的 handle 持读锁进入 `mdp_ctrl_acquire`；遇到 `io_ing`/`evicting` 时**持锁 `cpu_relax` 自旋**。同时周期路径 `reap_expired` 在**读锁内做 `write_block` I/O**（`flush_all` 自 2026-10-17 起改持写锁，只在卸载静默点调用，不再属于周期争用源）。三者互不构成环（io_ing 装载者、evicting 接管者都**不需要写锁**即可推进）⇒ **无死锁**；但持续读锁会**饿死**待写者（`ensure_path_with_wlock` / `reap_expired_strong`），自旋亦烧核（glibc rwlock 读者优先）。**无确定性复现**（未观测到挂起）。

**建议** 自旋/长 I/O 期间**放读锁**（io_ing 已钉住叶表，见设计；放锁后重新 `locate`），或给自旋封顶 + `sched_yield`。属设计取舍，需实测争用后再定。

---

## 5. H6 — `get` 建链 ↔ `reap_expired_strong` 摘表 抖动（低 · 活性）

**定位** `get` miss 段 `ensure_path_with_wlock`（建链、释写锁）↔ `reap_expired_strong`（写锁内摘“空表”）。

**根因** `get` 建好路径即释写锁；若紧接着 `MDP_SQUEEZE_PRESSURE` 在 `get` 重新 `locate` 前把仍是空的表摘掉，`get` 回到循环再建。持续压力下 `get` 重试次数**无上界**（非硬挂起）。**无确定性复现**。

**建议** 可给 `get` 的 miss-重试加上界（或建链后立即在同一读锁内 `locate` 使用），避免持续压力下的无界重建。

---

## 6. H7 — 不平衡 `put` → accessor 下溢（低 · 契约依赖）

**定位** `self_reliat_put` / `slice_radix_tree::put`：`ctrl_word.fetch_sub(CTRL_ACCESSOR_INC, release)`。

**根因** `put` 不做“是否已占坑”校验；无配对 `get`（或重复 `put`）会把 accessor 32 位域下溢到 `~2³²`，该行**永久 `accessor!=0`** → 强/弱淘汰都跳过 → 该行**永不可淘汰**（近似泄漏），且 `reap_expired_strong` 无法回收其叶表。

**复现** case H7：`get+put` 后**多调一次 `self_reliat_put`** → `reap_expired_strong(PRESSURE)` → 再 `get` 仍是**命中**（未淘汰）。

**建议** 契约侧：调用方保证 get/put 配对（当前语义）。机制侧（可选）：`put` 上用 `if ((w>>ACCESSOR_SHIFT)==0) /* 下溢保护，报 MDP_ERR 并跳过 */`。

---

## 附录 A — 已修复项（本轮，供对照）

| id | 缺陷 | 位置 | 修法 | 复验 |
|----|------|------|------|------|
| H2(RADIX) | flush 脏写回期间 `mark_dirty` 可被清掉 → 丢更新 | `slice_radix_tree::flush_all` | 读锁 → **片写锁**（`unique_lock`）：`mark_dirty` 的读锁被挡在清 DIRTY 之后 | 定向 RADIX 探针：会话写被挡到 flush 后，`disk=0x22`；concurrency harness D/D2 通过；上帝类 insert selfreliat 34/34（128MiB→RADIX） |
| H3(RADIX) | `flush_all` ⨯ `reap` 重叠 → 页 UAF | `slice_radix_tree::flush_all` | 同上：写锁独占，`reap_expired`/`reap_expired_strong` 均串行化 | 定向 RADIX 探针（ASan）：强淘汰等待写锁，无 UAF；harness D2 通过；上帝类 delete selfreliat 72/72、flush→重挂载 |
| H1 | `put` 用 `relaxed` 递减 → 无 happens-before，reap free 可能早于会话访问（潜在 UAF） | `self_reliat_put` / `slice::put` | `relaxed → release` | TSan 生产竞态 12→0 |
| H4(=C) | load 路径不盖 `accss_stamp` → 新载入行立即被判冷 | `mdp_ctrl_acquire` 发布段 | 发布时同盖 `accss_stamp` | smoke case 9；并发 `ttl-stamp-on-load=clean` |
| A | CTRL_ARRAY 强淘汰记成弱计数 `MDP_REAP` | `mdp_ctrl_reap_one` | 加 `bool strong` → 记 `MDP_REAP_STRONG` | smoke 5a/5b/5c |
| B | self_reliat ctor 漏「基址偶数」防卫 | `mdata_piece_caches.hpp` self_reliat 分支 | 补 `(piece_base_&1)→RC_ERR_STATE` | smoke 8b |

## 附录 B — 相关测试/文档

- `tests/test_mdata_piece_cache_defects_repro.cpp` — 本篇 H2/H3/H7 的**确定性复现**。
- `tests/test_mdata_piece_cache_concurrency.cpp` — 团队②白盒并发压测（含 H2/H3/H4 探针 + accessor 账目不变式）。
- `tests/test_mdata_piece_cache_smoke.cpp` — 团队①功能/冒烟（两档 + 新功能 + 统计钩子）。
- `tests/test_root_dir_insert.cpp` — **上帝类集成**：`mode=selfreliat` 时走真挂载/卸载；`dir_verify_tree` 的 flush_all 同步点 + 卸载 flush 都会经过片写锁。RADIX 档需 ≥128MiB 镜像（Piece_count>8192）。
- `tests/test_root_dir_delete.cpp` — 上帝类集成（已有 selfreliat 后端矩阵）：插删 + 重挂载持久化；同样经 flush_all 写锁路径。
- `tests/stress_name_data_fr.cpp` — 上帝类 selfreliat 压测：卸载回写 → 重挂 offload audit 全净。
- `Docs/runtime_tech/03_shared_radix_cache.md` — 本缓存的轮子/状态机设计（H2 的 copy-out 修法见其对回写纪律的描述）。
- `include/ravenfs/mdata_piece_caches_stats.hpp` — 计数器/钩子契约。
