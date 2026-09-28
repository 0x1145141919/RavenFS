# 03 chunk 化位图 — 测试团队进度台账（`Docs/linux/03_test_progress.md`）

> 2026-09-28 · 测试 subagent（Phase A = god 可靠性加压，Phase B = Linux 移植验收 harness）
> 纪律：**每阶段必写本文件**（开工 / Phase A / Phase B 各一条），防失联。
> 写范围：本文件 + `tests/` 新增 + `tests/CMakeLists.txt` 注册 + `vm/`、`tests/linux/` 脚本。
> 参照（只读）：`Docs/runtime_tech/03_chunk_backed_slices.md` §10 十条不变量 = 验收清单；
> god 实现 `juvenile_god/bitmap_slices.cpp` + `include/ravenfs/bitmap_slices.hpp`（禁改）；
> 既有测试 `tests/test_bitmap_slice_chunk.cpp`(125) / `test_bitmap_slice.cpp` / `test_block_occupy.cpp`（断言禁改）。

## 进度日志

### 2026-09-28 19:5x · Phase A 完成（三个新测试全部落地并跑完）

新增测试（`tests/`，已注册 `tests/CMakeLists.txt`）：
| 文件 | 目标 | 结果 |
|---|---|---|
| `test_bitmap_slice_differential.cpp` | Phase A-1 FULL↔CHUNK 对拍 | **156 passed / 61 failed**（failed = bug 复现单；18/18 顺序对拍跑完且终态镜像+计数全等；C1 并发 6/6 几何跨模式 memcmp 全等；C2 各自影子全等） |
| `test_bitmap_slice_churn.cpp` | Phase A-2 并发 churn 影子对账 | **72 passed / 3 failed**（failed = BUG#3 复现单；FULL 控制组全绿；no-occ 配置 3 轮零分歧） |
| `test_bitmap_slice_invariants.cpp` | Phase A-3 §10 缺口专项 | **90 passed / 0 failed · ALL PASS** |
| `chunk_common.hpp` | 三者共享靶场（mock 盘/几何推导/影子/门闸） | — |

**基线回归**：既有 `ravenfs_juv_bitmap_slice_chunk`=125、`bitmap_slice`、`block_occupy`=25 全部复跑全绿（断言零改动）。

### 发现的 god 实现 bug（只登记复现单，未改实现）
| # | 现象 | 根因（代码定位） | 证据 |
|---|---|---|---|
| **BUG#1** | occupy 锚点在「摘要 ALL_USED 且无 record」窗 → **全额虚报占领**（位没置、计数没动）= 双分配方向 | `chunk_soft_occupy` 首窗 `chunk_lock_window→RC_ERR_NO_BLOCK` 时 break **不回填 `*rest_bits`** → 路由 `claimed = remain - 0 = remain` | seq 对拍 46 次；最小复现 FULL=PTR_INVALID/rest=10 vs CHUNK=blkbase/rest=0 |
| **BUG#2** | 离线 record 路径对**已空闲位** free → `free_total` 虚高（§10.6 计数保真违例，statfs 谎报） | `chunk_free_interval` 挂 record 时 `free_count += n` 不数实际 1→0 位 | seq 对拍 3106 次；最小复现 +20 vs FULL +0 |
| **BUG#3** | 并发下 occupy 被片锁忙**直接放弃**（`RC_CHUNK_BUSY` → 路由 break → PTR_INVALID），FULL 自旋强抢永不放弃 | `chunk_soft_occupy→chunk_lock_window→slock_try` 忙即 BUSY；违反头注「free/定点自旋强抢」+ §10.10 | churn：CHUNK mix 646/388/313 次漏报（停止位仍空闲）；FULL 控制组 0 |
| **BUG#4** | 并发 `block_test` 把空闲位**假报为已用**（重试即 0） | `chunk_test_bit` 末段 `(online==c) ? bit_get : true` — 换片竞态下 online≠c → 保守 true（§1.1 test 语义与 FULL 不等 → 调用方漏分配） | 并发对拍 33 次；单窗几何 G-E 不复现（无换片）✓ 自证 |

### 关键测试设计决定（报告里展开）
- **margin 纪律**（`chunk_common.hpp` 头注）：block 组初值/free 保证 0-run 不跨窗 → 多位 alloc 的 FULL↔CHUNK 逐位等价可判（跨窗段延伸是 §8 已知形态差异，不混入 bug 判据）。
- **并发对拍分两配置**：C1=free/test 分区并发（结果与交错无关 → 跨模式终态**强制** memcmp）；C2=+alloc 并发（god try 换片次序使落点非确定——单片几何实测全等、多片必现差异 → 判据降为「各自影子强制全等」+ 落点差记 INFO）。
- **churn 双配置**：mix（occupy 参与 → 登记 BUG#3）+ no-occ（alloc/free/test 3 轮零分歧收口）；FULL 控制组证协议无误伤。
- 中途修过 3 处**测试自身**缺陷（inode 影子漏清位 / INV-6 目标窗选到在线窗 / 占位断言），非实现 bug。

### 阶段状态
| 阶段 | 状态 |
|---|---|
| Phase A-1 FULL↔CHUNK 对拍 | ✅ 完成（156/61，bug 单见上） |
| Phase A-2 并发 churn 影子对账 | ✅ 完成（72/3） |
| Phase A-3 §10 缺口专项 | ✅ 完成（90/0 ALL PASS） |
| Phase B Linux 验收 harness | ⏳ 进行中 |

### 2026-09-28 19:0x · 开工基线
- 仓库：`/home/PS/PS_git/OS_pj_uefi/ravenfs`；`cmake --build build` 零工作（基线已建）。
- **基线复跑全绿**：`ravenfs_juv_bitmap_slice_chunk` = 125 passed / 0 failed；
  `ravenfs_juv_bitmap_slice` = ALL PASS；`ravenfs_juv_block_occupy` = 25 passed / 0 failed。
- 已通读规格 §1–§13（§10 十条 = 验收清单）、god 实现（1634 行）、既有 125 项测试覆盖点；
  既有覆盖：T1 mount 扫描 / T2 换片+在线直改+evict / T3 record→drain 顺序 / T4 池空兜底 /
  T5 摘要跳过+降级 / T6 hint 绕回 / T7 尾块 1 页 / T8 occupy 跨窗续圈 / T9 TTL 三态 /
  T10 契约+三段校验 / T11 影子对账（**仅镜像 memcmp，不查 free_total**）。
- 缺口识别（Phase A-3 补测对象）：① FULL↔CHUNK 双模式对拍（无）② 并发 churn 对账（无）③
  §10.3 「I/O 不进 slice_lock」的并发/时序验证（仅单线程钩子）④ §10.6 计数与驻留无关
  （T11 不查计数）⑤ §10.8 尾块几何只测了 order=1（0/4/8 无）⑥ §10.9 池空兜底的池复用/多段。
- Phase B 侦察：`vm/` 实际位于 **`linux/vm/`**（gitignored：disks/share/run-vm.sh/test-runs）；
  `linux/bitmap.c` **已出现 CHUNK/backing 代码**（2224 行，模块参数 `bitmap_backing`/`bitmap_chunk_order`）
  → 按任务书 Phase B-2 走「构建模块 + 实跑 harness」分支。

### 2026-09-28 20:1x · Phase B 完成（Linux 侧 harness 落地 + 实跑两轮全绿）

**交付**（写范围 `tests/linux/` 新目录）：
| 文件 | 说明 |
|---|---|
| `tests/linux/bitmap_chunk_guest.sh` | guest 工作段（build/load/mkfs/mount/stress/ttl/umount/audit/verify/unload 分相 + `all` 一键） |
| `tests/linux/run-bitmap-chunk-vm.sh` | 宿主编排（起 VM→scp 投递→guest 构建→分 pass 驱动→日志收拢到 `linux/vm/test-runs/<ts>-bitmap-chunk/`） |
| `tests/linux/README.md` | **运行手册**（用法/判据 C0–C6/已知边界） |

**实跑结果**（run `20260928-200739-bitmap-chunk`，ko guest 内编译 srcversion=EECFB48BF924A5EFB2969FE）：
- **CHUNK 轮（bitmap_backing=1 order=4）：PASS=10 FAIL=1**
- **FULL 轮（bitmap_backing=0 控制组）：PASS=10 FAIL=1**
- 关键证据：mount dmesg `backing=CHUNK slices=8 block_bits=8388608(free=8322778)`（mount 扫描计数真值）；
  ENOSPC 往返 free 8321556→2960→**8321556 精确归位**；重挂载 416 文件 md5 全等；
  statfs 三态全等（8321556×3）；`audit_v1.2` 全绿；dmesg `bm_err=0 wb_err=0`。
- **唯一 FAIL = C6（inode statfs 不随会话更新）**：会话内 `%c-%d` 恒冻结（800 建删前后均=1），
  重挂载重算正确（418）；**两模式同现 → 既有 FULL 行为，非 chunk 回归**；根因指针 =
  `linux/super.c ravenfs_statfs()` 走 `ravenfs_bitmap_used(位图盘态)`（buffer/盘态快照），
  inode 位图的落盘刷新时机 ≠ 活跃刷新（块侧因写穿 buffer cache 恰好是活值）→ 归 super.c/刷新专项。
- **观察（非 chunk 归属）**：FULL 轮 fill 在 `f_bfree=1568472`（6.1G 空闲）时 dd 报 ENOSPC
  （CHUNK 轮同一负载填至 2960）→ 疑似碎片化尾部 hard 分配（2 块对齐元数据）先竭，
  建议写路径专项复现；C2/C3 均过 → 位图计数本身无恙。
- 踩坑记录（写进 harness 注释）：9p `/rfssrc` 重启不自动挂 → `ensure_9p`；
  `cp -a /rfssrc/linux` 会连 `linux/vm`（36G qcow2）一起拷爆 guest /tmp → 只拷 `Makefile/*.c/*.h + include/`；
  mkfs 需 `sudo` + `-f`（已有文件系统）；`df --output=f_bfree` 字段不存在 → `stat -f -c %f`；
  raven 无 CAP_SYSLOG → `sudo dmesg`；ravenfs 根目录 root 属主 → 工作目录 `sudo chown`。

### 阶段状态（终）
| 阶段 | 状态 |
|---|---|
| Phase A-1 FULL↔CHUNK 对拍 | ✅ 156/75（75=bug 复现单：KB1×14+KB2×17+BUG4×44 打印条目） |
| Phase A-2 并发 churn 影子对账 | ✅ 72/3（3=BUG#3 复现单；no-occ 3 轮零分歧） |
| Phase A-3 §10 缺口专项 | ✅ 90/0 ALL PASS |
| Phase B Linux harness | ✅ 实跑 CHUNK 10/1、FULL 10/1（唯一 FAIL=C6，两模式同现=既有问题） |
