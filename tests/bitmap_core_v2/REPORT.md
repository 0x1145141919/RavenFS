# bitma_core_v2 测试报告 —— C 版双位图分片分配器（MEM + CHUNK 双后端）

- 队伍：第二测试队（重做）
- 日期：2026-09-30
- 被测仓：`/home/PS/PS_git/OS_pj_uefi/ravenfs`
- 产物目录：`tests/bitmap_core_v2/`（**本队唯一新增**；未改任何冻结文件，见 §7）
- 被测源（**只读**）：`include/linux/ravenfs_{platform,bitmap,bitmap_backend,bitmap_core}.h`、
  `linux/{bitmap_core,bitmap_mem,bitmap_chunk}.c`
- 语义真值参考（只读）：`juvenile_god/bitmap_slices.cpp`（v1=MEM）、`bitmap_slices_v2.cpp`（v2=CHUNK）

> **本队与上一队的根本差别**：上一队「拿 MEM 的逐步语义去逐拍卡 CHUNK」，把
> 「合成视角」当成「直接事实」。本队严格按冻结头 ★★ 的口径：
> **MEM 逐步严格比对；CHUNK 逐步只比成败/不变量，位态与计数只在合成后（提交态）比**，
> 挂账期漂移按文档化差异单独统计。

---

## 0. 判定口径（硬前提）

| 后端 | 真值模型 | 可采信时机 | 本队比对方式 |
|---|---|---|---|
| MEM | 内存位图 = 一等事实 | 随时（`block_test` / `*_free_total`） | **逐步严格比对**（位态/位置/缺口/计数） |
| CHUNK | 在线窗 + 挂账归还链 + 盘上块，在**合并点**合成的**合成视角** | 合并后（使用该窗 / `chunks_asyn` / `snapshot_slice`） | 逐步比「分配成败语义/无双重分配/对齐/单窗」；位态/计数**只在提交态**比 |

合并点（引冻结头）：① 该窗被使用之前（alloc/occupy 进入先 drain）；② `rfs_bm_chunks_asyn`。

---

## 1. 文件清单 + 真实构建输出

```
tests/bitmap_core_v2/
├── Makefile                 # 只编本目录 + 3 个实现文件为用户态（gcc -Wall -Wextra -pthread）
├── bm_harness.h             # 夹具声明（fake disk / 独立 MEM oracle / env）
├── bm_harness.c             # 夹具实现
├── bm_test.c                # 主测（子命令 quick|full|tsan|san）
├── sim/
│   ├── bitmap_chunk_prefix.c  # [PRE-FIX 模拟] 由 frozen bitmap_chunk.c 机械去掉 BUG-1 守卫
│   └── sim_prefix.c           # BUG-1「修前」对照主程序
└── REPORT.md
```

**`make`（默认构建）完整输出**（`make clean && make`）：

```
cc -Wall -Wextra -std=gnu11 -g -O2 -D_GNU_SOURCE -pthread -I../../include/linux bm_harness.c bm_test.c ../../linux/bitmap_core.c ../../linux/bitmap_mem.c ../../linux/bitmap_chunk.c -o bm_test -pthread
```

> **zero-warning：是**（`grep -c "warning\|error"` = 0）。三个实现文件以 `-Wall -Wextra` 编入亦无警告。
> `make tsan`（`-fsanitize=thread -O1`）/ `make san`（`-fsanitize=address,undefined -O1`）同样 zero-warning。

工具链：`gcc 16.2.1`，`nproc=22`，TSan/ASan+UBSan 可用。

---

## 2. 各套件原始运行输出

### 2.1 `./bm_test quick`（退出码 0）

```
=== bm_test mode=quick (nproc=22) ===
[suite] geometry
[suite] boundary
[suite] invariants
[suite] regression_bug1
[suite] differential_mem
[suite] differential_chunk
  [diff_chunk] C2 文档化 hard=115 soft=0；疑似缺陷 hard=0 soft=0
  [diff_chunk] C2b 文档化 hard=0 soft=0；疑似缺陷 hard=0 soft=0
[suite] persistence
[suite] chunk_windows
[suite] synth_view (CHUNK 合成视角)
  逐步视图：raw盘 bit100=1（未合并仍 1）；提交态 bit100=0；free_total=1048575
  重复释放后 free_total=1048576（挂账虚高）；真值=1048575
  合并后：free_total=1048575（== 真值）；raw盘 bit100=0
[suite] free_count
=== TOTAL: 4287 checks, 0 failures ===
```

### 2.2 `./bm_test full`（退出码 0，约 14.5s）

```
=== bm_test mode=full (nproc=22) ===
[suite] geometry
[suite] boundary
[suite] invariants
[suite] regression_bug1
[suite] differential_mem
[suite] differential_chunk
  [diff_chunk] C2 文档化 hard=821 soft=0；疑似缺陷 hard=0 soft=0
  [diff_chunk] C2b 文档化 hard=249 soft=0；疑似缺陷 hard=0 soft=0
[suite] persistence
[suite] chunk_windows
[suite] synth_view (CHUNK 合成视角)
  逐步视图：raw盘 bit100=1（未合并仍 1）；提交态 bit100=0；free_total=1048575
  重复释放后 free_total=1048576（挂账虚高）；真值=1048575
  合并后：free_total=1048575（== 真值）；raw盘 bit100=0
[suite] free_count
[suite] fuzz
  [fuzz] CHUNK 文档化 hard=4485 soft=0；疑似缺陷 hard=0 soft=0
[suite] concurrency (threads=16)
  MEM（spin 片锁）…
  CHUNK（mutex 片锁 + fake 盘随机延迟）…
=== TOTAL: 38642 checks, 0 failures ===
```

### 2.3 `./bm_test_tsan tsan`（退出码 0；TSan 报告数 = 0）

```
=== bm_test mode=tsan (nproc=22) ===
[suite] concurrency (threads=8)
  MEM（spin 片锁）…
  CHUNK（mutex 片锁 + fake 盘随机延迟）…
[suite] fuzz
  [fuzz] CHUNK 文档化 hard=10 soft=0；疑似缺陷 hard=0 soft=0
=== TOTAL: 2267 checks, 0 failures ===
```
`grep -c "WARNING: ThreadSanitizer" stderr` = **0**（RISK-1 preferred relaxed 原子已修，无竞争报告）。

### 2.4 `./bm_test_san san`（退出码 0；ASan/UBSan 标记数 = 0）

```
=== bm_test mode=san (nproc=22) ===
[suite] geometry / boundary / invariants / regression_bug1
[suite] differential_mem
[suite] differential_chunk
  [diff_chunk] C2 文档化 hard=115 soft=0；疑似缺陷 hard=0 soft=0
  [diff_chunk] C2b 文档化 hard=0 soft=0；疑似缺陷 hard=0 soft=0
[suite] persistence / chunk_windows / synth_view / free_count / fuzz
[suite] concurrency (threads=8)
=== TOTAL: 6547 checks, 0 failures ===
```
`grep -iE "runtime error|AddressSanitizer|LeakSanitizer|SUMMARY:"` = **0**。

### 2.5 BUG-1「修前行为」对照（`make prefix && ./sim/sim_prefix`，退出码 0）

```
MEM hard(1)      = 0
oracle hard(1)   = 0
PREFIX CHUNK     = 128  (pre-fix：hint 被推到锚点 64 → 命中 128)
MEM/occupy rest  = 1 ; PRE-FIX rest = 1 (期望均 1)
```
> `sim/bitmap_chunk_prefix.c` = 由 frozen `bitmap_chunk.c` **机械去掉** `claimed > 0 &&` 守卫
> 得到的对照后端（**非冻结产物、不参与默认构建**）。冻结版（含守卫）在 `regression_bug1`
> 中该同址断言 = `0 == oracle == MEM` **通过**；PRE-FIX 模拟 = `128`，**复现分叉**。

---

## 3. 套件覆盖内容（摘要）

| 套件 | 覆盖 |
|---|---|
| geometry | slice_count/per_bits 推导（单/多槽、多片、末片非空、覆盖==nbits）、初始 free_total、`rfs_bm_ops_of`、MEM `chunks_asyn`→-ENOSYS |
| boundary | count=0/>总/>单片(MEM)/>单窗(CHUNK→NO_HIT)、align 0..15 与 >15（MEM 夹断 16；CHUNK 拒→NO_HIT）、区间测试 0/-ENOENT/-EINVAL、越界/溢出、满载 ENOSPC、末片短片 |
| invariants | 无双重分配、释放守恒、重复释放提交态不虚高、越界 |
| regression_bug1 | 0 认领失败 occupy 后 CHUNK 与 MEM/oracle 同址（BUG-1） |
| differential_mem | 逐步严格差分（单片 & 4 片 cpu_slots=4）：位态/位置/缺口/计数全等 |
| differential_chunk | C1 单窗/片（位置逐步 + 计数提交态全等 MEM）、C2/C2b 多窗（逐步成败+不变量；合成后位态/计数自洽） |
| persistence | MEM `snapshot_slice`==内存视图；CHUNK `chunks_asyn` 后从 fake 盘重 init，位态/free_total 一致 |
| chunk_windows | 换片 evict→load→publish、归还链挂账零 I/O、使用前 drain 不泄漏、摘要免载入跳窗 |
| synth_view | CHUNK 合成视角断言栈（逐步可异、合成后必同）——见 §4 |
| free_count | 逐片对 shadow 真值；挂账期豁免、提交态必须相等 |
| fuzz | 大轮次；cpu_slots 1 与多；关键点合成后全量校验 |
| concurrency | 线程≥8、高竞争混合调度；MEM(spin)/CHUNK(mutex，fake 盘随机延迟)；无双重分配/无丢 free/无死锁 |

---

## 4. 任务 B：测「合成视角」思想是否传达

### 4.1 只读接口头（`ravenfs_bitmap_core.h` / `ravenfs_bitmap_backend.h` / `ravenfs_bitmap.h`）回答

**Q1. MEM 的真值模型是什么？**
> 内存位图**直接在内存里 = 一等事实（first-class truth）**；位态与 `free_count` 即真值，
> `block_test` / `*_free_total` **随时可直接采信**；无「待合成」中间态，逐步可观测 == 真值。
> 引用：`ravenfs_bitmap_core.h:38-41`（`· MEM ：位图直接在内存里 = 一等事实(first-class truth)…逐步可观测 == 真值`）；
> `ravenfs_bitmap_backend.h:23`（`MEM 后端：位图在内存里就是一等事实，ops 的读写/计数即真值`）；
> `ravenfs_bitmap.h:7`（`MEM ：内存里即一等事实——位态/计数随时可直接采信`）。

**Q2. CHUNK 是什么？**
> **合成视角（synthesized view）**：内存里没有全图的直接事实；任何「当前位态/空闲计数」
> 都由「在线窗 `active_chunk_buffer` + 挂账归还链 `rec_head` + 盘上块」在**合并点**合成。
> 引用：`ravenfs_bitmap_core.h:43-46`（`CHUNK：内存里没有全图的直接事实…合成视角…合并点合成`）；
> `ravenfs_bitmap_backend.h:24-25`；`ravenfs_bitmap.h:8-9`。

**Q3. CHUNK 的视图何时才可信？**
> 在**合并点之后**：① 该窗被使用之前（alloc/occupy 进入时先 drain 在线窗记录）；
> ② `rfs_bm_chunks_asyn`（整片读→合并→写 + 计数重扫校准）。
> 引用：`ravenfs_bitmap_core.h:45-46`。

**Q4. CHUNK 比对为什么不能在逐步态做？**
> 挂账期（未合并）读到的 `block_test`/`free_count` **可与真值暂时不一致**（free 挂账即记账 →
> 计数可虚高；被释放的位在合并前仍为 1）；「判定/比对必须以合成后（提交态）视角为准」，
> **不得**拿 CHUNK 逐步视图逐拍对齐 MEM——那是设计误读**不是缺陷**。
> 引用：`ravenfs_bitmap_core.h:47-52`；`ravenfs_bitmap_backend.h:25`。

**结论**：仅凭接口/注释，一个测试者**能够**自洽推出正确比对口径——
「MEM 逐步严格、CHUNK 合成后比对」在**三处头**里被显式、反复、加粗地写明（core ★★ 段 + backend「语义判定前提」段 + bitmap.h 段），且明确写出合并点 ①②。

### 4.2 传达度评分

**4 / 5**（good but not perfect）。

传达到位的点：
- 「两种视角」的模型、可信时机（合并点 ①②）、「不得逐步逐拍比」的红线，三处头交叉声明，指向清晰。
- `snapshot_slice` / `chunks_asyn` 注释给了「合成后一致视图」的操作入口。

仍含糊/缺失的点（逐条：位置 + 现状 + 建议）：

1. **`block_test` 的逐步行为与 §★★ 的“可暂不一致”表述有张力。**
   位置：`ravenfs_bitmap_core.h:47-48`（“挂账期读到的 block_test/free_count 可以与真值暂时不一致”）+
   `ravenfs_bitmap_core.h:259`（“单点测试：直读位图”）。
   现状：**实测** `rfs_bm_block_test`（CHUNK）对任意位**恒为真值**——`cs_test_interval` 会先 drain
   在线的窗、对离线窗先 `ensure_online`（load+drain）再读；摘要 `ALL_USED/ALL_FREE` 免载入判定亦准确。
   即“逐步位态可漂移”在**公开 API 层面实际不可观测**（漂移只存在于 raw buffer/盘上）。
   建议：头注明确区分「**内部**原始/在线窗视图可漂移」与「**公开 `block_test` 已在读时合并、逐步即真值**」；
   或把“block_test 可暂不一致”改为“**raw/未合并的内部视图**可暂不一致”。
   （本队据此把 `block_test` 当作「提交态」读数使用，见 §4.3。）

2. **`free_count` 的“可虚高”未说明触发条件。**
   位置：`ravenfs_bitmap_core.h:48`（“计数可虚高”）、`bitmap_chunk.c:16`。
   现状：**实测**正常分配/释放下 `free_count` 等于真值；只有**释放已空闲区间（重复释放/释放未分配）**
   才因“挂链即记账 +n”而虚高（`synth_view` 已复现：1048576 vs 真值 1048575）。
   建议：在 `chunks_asyn`/`free` 注记里点明“虚高**源于对已空闲位重复记账**”，并说明“提交态 `chunks_asyn`
   校准回真值”。

3. **返回值/边界口径在两后端间的差异未在头里并置。**
   位置：`ravenfs_bitmap_core.h:250-262`（硬/软/锚定/测试 的注记按「全局」写，未分后端列差异）。
   现状：实测两处**后端差异**（详见 §5 的 DIV-1/DIV-2）：软分配 `count>单片` 时 MEM→NO_HIT 而 CHUNK→部分给；
   硬分配 `align_log2>15` 时 MEM→夹断到 16 而 CHUNK→拒（NO_HIT）。
   建议：头里对 `alloc_hard/alloc_soft` 补一行“**v1(MEM) 与 v2(CHUNK) 在此处的既定差异**”，
   免得测试者把它们误判为 bug。

4. **「单窗/跨窗」语义在硬分配注记里点到，但软分配/occupy 的窗口性未点明。**
   位置：`ravenfs_bitmap_core.h:250-251`（仅 hard 注明单窗约束）、`255`（occupy 只写“可跨片续圈，CHUNK 可跨窗”）。
   现状：soft 的“首空闲段”在 CHUNK 里也是**窗内**取段，故跨窗 run 会被切分（DIV-3）。
   建议：在 soft/occupy 注记补“**CHUNK 为窗内取段/窗内前缀**，跨窗连续段可能被切”。

5. **`in_slice_hint` 的行为契约（只进不退 + 仅在真认领时推进）未在公开头`.h`写明。**
   位置：仅 `bitmap_chunk.c:760-765` 的**实现注释**（BUG-1 修复处）。
   现状：BUG-1 把“0 认领也推进 hint”修掉以对齐 MEM；但**公开头**没有“hint 只在 claimed>0 时推进、
   hint 只进不退（含绕回）”的契约——测试者若只读接口头会漏掉这一跨后端对齐要求。
   建议：把该契约提升到 `ravenfs_bitmap_core.h` 的 alloc/occupy 注记里。

### 4.3 判定规则落地为断言用例（`suite_synth_view`，quick/full/san 均跑）

**用例设计**：占 1 位 → 换片（evict → 盘上=1）→ 离线释放（挂账、零 I/O，盘上仍 1）→
再重复释放一次（挂账虚高）→ `chunks_asyn`。

**原始输出**（`full`）：
```
  逐步视图：raw盘 bit100=1（未合并仍 1）；提交态 bit100=0；free_total=1048575
  重复释放后 free_total=1048576（挂账虚高）；真值=1048575
  合并后：free_total=1048575（== 真值）；raw盘 bit100=0
```

**断言栈**（全通过）：
- `raw_bit == 1` —— 挂账期 raw 盘 bit100 仍为 1（**逐步视图可异**）；
- `committed == false` —— 提交态 bit100 已空闲（使用前合并）；
- `ft_infl (1048576) > truth (1048575)` —— 挂账期 free_total **虚高**；
- `ft_commit == truth` —— **合成后必同**；
- `raw_after == 0` —— 合成后盘上该位 == 0。

⇒ 该用例**证明**了“CHUNK 逐步视图允许与 MEM 不同，但合成后必须相同”。
同时它也**印证了 §4.2 缺陷 1**：这里能观察到“逐步可异”的是 **raw 盘**与 **free_count**，而**不是** `block_test`
（`block_test` 一开始就返回提交态真值）——头注若把 `block_test` 也列为“逐步可漂移”会误导。

---

## 5. bug / 差异清单

> 关键结论：**未发现「真缺陷」**。以下 DIV-* 均为**文档化的跨后端/单窗语义差异**或**注释精度问题**，
> 已按口径显式记录，不算 bug。

| 编号 | 位置 | 最小复现 | 期望 | 实际 | 定性 |
|---|---|---|---|---|---|
| DIV-1 | `bitmap_mem.c:mem_alloc_soft`（`count > s->bits → -ENOSPC`） vs `bitmap_chunk.c:cs_win_alloc_soft`（无此检查，取部分段） | 新环境 `rfs_bm_block_alloc_soft(bm, nbits+5, &rem)` | MEM：「单片内取 min(段,count)」语义下，MEM 因先判 `count>片` 而 **NO_HIT**；CHUNK 取部分段 | MEM=NO_HIT；CHUNK=`pos≠NO_HIT`、`rem<count` | **文档化差异**（god v1 vs v2 既定）；头未并置（§4.2-3） |
| DIV-2 | `bitmap_mem.c:mem_alloc_hard`（`align_log2>16→夹断16`） vs `bitmap_chunk.c:cs_alloc_hard`（`align_log2>15→-EINVAL`） | `alloc_hard(count=1, align=20)` | MEM：按 2^16 对齐成功；CHUNK：拒绝 → NO_HIT | MEM 成功且 `pos%65536==0`；CHUNK=NO_HIT | **文档化差异**；边界套件已分别断言 |
| DIV-3 | `bitmap_chunk.c` 单窗语义（`cs_alloc_hard/soft/occupy_soft` 均在窗内取段/前缀） | 多窗环境，目标 run 跨窗；或 run 恰跨 hint | MEM（整片）可命中；CHUNK 单窗下可能 NO_HIT | 见 `diff_chunk` C2/C2b/fuzz 的 `文档化 hard=…`；`疑似缺陷 hard=0` | **文档化差异**（“单窗语义/多窗位置”，头已声明 hard 单窗约束） |
| DIV-4 | `bitmap_chunk.c`/`bitmap_mem.c` 的 hint 起扫+**回绕**（两趟 `[hint,end)` + `[0,hint)`） | 目标 run 恰好**跨过某片 `in_slice_hint`** | 该 run 被 hint 切成两半，两趟各只看半段 → 两后端**都可能** NO_HIT | `diff_chunk` 漏配分类里**显式豁免**“跨 hint 的窗内 run”（读 `rfs_bm_cslice::in_slice_hint` 判定；仅豁免 hint 所在窗、且 run 跨 hint 者） | **两后端共有的搜索假象**，非 CHUNK 缺陷；已在分类器与报告显式记录（避免误报） |
| DOC-1 | `bitmap_chunk.c:760-765`（BUG-1 实现注释） | — | 注释称“修前会**假 ENOSPC**” | 本队由 frozen 代码**只能**复现「修前 hint 被推到锚点 → 与 MEM **位置分叉**」（`sim_prefix`：128 vs 0）；未能从冻结件构造出纯 NO_HIT 场景 | **注释精度**：机制描述更像“位置分叉”，建议改写或补充精确 reproducer |
| DOC-2 | `ravenfs_bitmap_core.h:47-48` | — | “挂账期读到 `block_test` 可暂不一致” | 实测公开 `block_test` **恒为提交态真值**（读时合并）；“逐步位态漂移”仅存在于 raw 盘/在线窗 | **注释精度**（见 §4.2-1） |

**无“真缺陷”的证据**：
- `differential_mem`：MEM 逐步严格差分 **0 失配**（单片 & 4 片）。
- `differential_chunk` C1（单窗/片）：**位置逐步 + 计数提交态全等 MEM**（0 失配）。
- `differential_chunk` C2/C2b/fuzz：**`疑似缺陷` = 0**（所有失败均可用“窗内无对齐 run / 全满 / 跨窗”解释）。
- `invariants`/`free_count`/`persistence`/`chunk_windows`/`synth_view`：全绿。
- `concurrency`：MEM/CHUNK 均**无双重分配、无丢 free、无死锁**（收尾位态全零、free_total==nbits、seen 全零）。
- `tsan`：0 竞争报告；`san`：ASan/UBSan 全绿。

---

## 6. 诚实边界（未覆盖项）

1. **真盘/内核态路径未测**：本队只做**用户态 glibc + fake 盘**；未做 `make` 内核模块构建（红线），
   未上真 nvme，`sb_bread`/`vzalloc` 等内核分支未执行。
2. **`inode` 组**：差分/持久化以 **block 组**为主；inode 组覆盖了 alloc/free/并发混跑与不变量，
   但未做 inode 组的逐步严格差分。
3. **`rfs_bm_inode_alloc` 的“root 预占”**：类不做保留区（无 hint=1），未覆盖 mkfs 预占场景。
4. **内存分配失败注入**：未模拟 `RFS_ZALLOC` 失败（free 挂链 OOM → 泄漏方向、enable/asyn 失败回滚）——
   代码有对应分支（`cs_free_interval` 返 -ENOMEM、`cs_disable` I/O 失败收资源），本队**未**注入验证。
5. **I/O 错误注入**：fake 盘 read/write 恒成功；`cs_ensure_online`/`evict`/`asyn`/`disable` 的
   `-EIO` 路径（写失败保持在线、asyn 幂等重跑）**未**注入验证。
6. **`align` 语义细节**：CHUNK `>=` 与 MEM 夹断的分界（16/17）只做了代表性取值。
7. **TSan/san 为控时收小轮次**（`g_light`）：sanitizer 跑的是 reduced 规模（并发 8 线程/1200 迭代、
   fuzz 1200 轮），非 full 规模；欲全规模可 `./bm_test_tsan tsan`（本机实测跑满会极慢，故收小）。
8. **`sim/bitmap_chunk_prefix.c`** 是 frozen 的机械派生（**仅对照用**），非独立第三实现。

---

## 7. 只读红线核验

- 本队**只新增** `tests/bitmap_core_v2/`（`git status` 中该目录为唯一 `??`）。
- 逐文件核验冻结件未被本队改动：
  - `include/linux/ravenfs_{bitmap_core,bitmap_backend,platform}.h`、`linux/bitmap_{core,mem,chunk}.c`：`git ls-files` 判为 **untracked(new)**（由先前工作新增），本轮**未写**；
  - `include/linux/ravenfs_bitmap.h`：仅显示**先前已存在的** staged 改动（`7 insertions,2 deletions`，非本队）；
  - 其余 `M/R`（`Docs/…`、`linux/Makefile`、`bitmap.c`、`super.c`、`dkms.conf`、头位移）均为**先前已存在**，与本次会话无关。
- 未 `commit`、未 `sudo`、未联网、未删文件、未跑内核构建。

---

## 8. 结论

- **两条路径是否达标**：**达标**。
  - MEM：逐步严格差分与独立 oracle 全等，边界/不变量/持久化/并发全绿。
  - CHUNK：逐步「成败/不变量/对齐/单窗」+ 合成后「位态/计数」全绿；持久化往返、换片/挂账/drain、
    并发（含 fake 盘随机延迟）全绿；`tsan` 0 报告、`san` 全绿。
- **实现有罪/无罪**：**无罪**（未发现真缺陷；DIV-1/2/3 为文档化的 v1/v2 与单窗语义差异；
  DOC-1/2 为注释精度问题）。
- **注释传达是否达标**：**达标（4/5）**。核心「合成视角/合成后比对」口径在**三处头**清晰传达，
  测试者可自洽推出正确口径；扣分项为注释精度（`block_test` 与 raw 视图的措辞、free_count 虚高触发条件、
  两后端边界差异未并置、hint 契约只在 `.c` 里）。

---

### 附：复现命令

```bash
cd tests/bitmap_core_v2
make            # zero-warning
./bm_test quick && ./bm_test full
make tsan && ./bm_test_tsan tsan     # TSan：0 报告
make san  && ./bm_test_san  san      # ASan+UBSan：全绿
make prefix && ./sim/sim_prefix      # BUG-1 修前对照：PREFIX CHUNK=128 vs MEM=0
```
