# 03 chunk 化懒加载改制 — 施工进度（防失落盘）

> 规格：`Docs/runtime_tech/03_chunk_backed_slices.md`（权威）
> 日期：2026-09-28 ｜ 施工：03 §13 四刀 ｜ 红线：FULL 逐位逐性能不变、盘 I/O 永不进 slice_lock
> 本文件**每完成一刀必写**（不依赖会话记忆）；改一个文件记一条。

## 状态总览（四刀全部完成）

| 刀 | 内容 | 状态 | 证据 |
|---|---|---|---|
| ① | backing 开关 + cfg/io 契约 + FULL 分叉 + 换片状态机 | ✅ | 构建零错误零新警告；FULL 双测 ALL PASS / 25-0 |
| ② | record 节点池 + 上线前 drain + 摘要降级 | ✅ | 同上（增量构建后重跑） |
| ③ | mount 顺序扫描 + TTL 三态分派 | ✅ | 同上（增量构建后重跑） |
| ④ | chunk 专项测试收口 | ✅ | `ravenfs_juv_bitmap_slice_chunk` **125 passed, 0 failed** |

最终验证（2026-09-28）：
- `cmake --build build` → exit 0；`bitmap_slices.cpp` / `test_bitmap_slice_chunk.cpp`
  重编译**零警告**（既有警告仅 direct_file_ops.cpp / test_pwrite_phase1.cpp，基线即有）。
- `./build/tests/ravenfs_juv_bitmap_slice` → `RESULT: ALL PASS`
- `./build/tests/ravenfs_juv_block_occupy` → `25 passed, 0 failed`
- `./build/tests/ravenfs_juv_bitmap_slice_chunk` → `125 passed, 0 failed` / `RESULT: ALL PASS`
- 其余无需设备的测试全绿；需块设备的 9 个（extent/name_heap/pwrite 等）报 usage
  （需 `<device>` 参数，非回归）。
- 红线结构证据：`git diff juvenile_god/bitmap_slices.cpp` **仅删 2 行**（ctor 裸指针
  校验拆成双模判别），头文件**零删除**；现有两个 bitmap 测试**零改动**；
  `linux/`、`mkfs/`、其它模块**零触碰**。

## 关键裁决（三个待裁点）

1. **CHUNK 模式 cfg 允许 `*_bits_raw` 为空**：✅ 允许。ctor 校验拆成
   `(!chunked && (!raw || …))` → 仅 FULL 必须非空。CHUNK 下 `desc.bitmap_raw`
   不参与任何路径（`block_test/inode_test` 先按 `backing_mode` 分叉，永不落进
   raw 直读分支；`dump_info`/`bitmaps_flush` 是 god 侧 FULL-only 关注点）。
   测试 T10(a)(b) 双向验证：FULL+raw空 → 拒装；FULL+合法 raw → 原样直读。
2. **CHUNK `block_test/inode_test` 语义**：**组合式**——在线窗直读（零 I/O）→
   离线先查 record 覆盖（= 逻辑空闲）→ 摘要裁决（ALL_USED→true /
   ALL_FREE→false，免载入）→ MIXED 离线才走慢路径上线读；I/O 失败/换片失败 →
   保守返回 true（占用，泄漏方向，绝不误报 free）。理由：§1.1"语义不变"要求返回
   真值，单选在线读=离线无答案、单选摘要=MIXED 无答案、单选上线读=牺牲零 I/O
   快路径；组合式同时满足语义不变 + §7 摘要免载入收益 + §1.5 不引入双分配。
   测试 T1/T3/T5 分别覆盖三条分支。
3. **`chg_mu` 实现**：`util/lock.h` 只有 try 自旋锁（无睡锁）→ 头文件
   `using bitmap_mutex_t = std::mutex`，注释按移植注记风格写明"ko 移植换内核
   mutex（§12），接口语义不变"（对齐 `rw_try_spinlock_t` 既有风格）。不改 lock.h。
   锁序全局唯一：**chg_mu → slice_lock**（换片/TTL 持 chg_mu 时才拿片锁，反向永不）。

## 施工期裁决（按 §1/§10 红线优先）

- **换片下线置 NONE 再写盘**：§5 流程图允许"快照后仍留旧窗在线"，但那样快路径
  可在写盘期间继续改 buffer → 撕裂写。实现取更严形态：锁内快照+清 dirty+置 NONE
  （buffer 冻结），写失败则恢复旧窗在线+复脏（buffer 仍是真相，状态零损坏）。
  TTL 态 1 写在线窗按 §5 反例持 chg_mu ✓。
- **drain 后按清位数置脏**：`drain_window_locked` 返回实际 1→0 清位数，发布时
  `dirty = cleared>0`；否则"已还账但未落盘"的 free 在 evict 被 dirty==0 跳过 =
  永不写回（§10.6）。
- **alloc 置位同样置脏**（§4 `dirty = ONLINE 且被改`）：alloc/occupy 只置位不置脏
  会让分配在 evict 时丢账 → **双分配方向**（踩 §1.5 红线）。已在窗口迭代器
  成功路径上补 `ctls[c].dirty = 1`，由 T2（evict 写回断言）+ T11（影子对账）双测覆盖。
- **自描述三段校验需要区间参数**：02 §1 两段"尾巴全 1"并段 = block 位图
  `[inode_bm_base, total_blocks)`（全局位号空间），mgr cfg 原本拿不到 → cfg 新增
  `self_check_lo/hi`（默认 0 = 区间空则不查该段）；inode 组 bit0(root) 亮灯恒查；
  `skip_self_check` 供单测合成位图。FULL 忽略（god 侧 `bitmaps_load` 自查）。
  03 §7.4 只写了校验没给字段名，此两字段为最小补充（头文件已注明）。
- **校验区间必须用全局位号**：`wlo/whi` 是片内偏移（02 §3.4 坐标双视角），
  直接拿来和全局区间求交 → 每片 ctl0 都误触 root 灯 + 片1 越界读 buffer。
  **刀④ 测试抓出的真 bug**，已改为 `g_wlo = base_idx + wlo` 换算。
- **record 记账按 §6 原文**：挂 record 时 `free_count += n`（drain 不动计数）；
  在线直清按实际 1→0 计数（同 FULL 口径）。代价：同一区间重复 free 会虚高
  free_count（FULL 不会）→ 正确调用方语义下无差，记为已知差异，不加二次扫描。
- **record 跨连续离线窗合并为单节点**（§4"单次 free 每片至多一节点，长 run 也
  只 1 节点"）；record 绝不覆盖在线窗（在线窗已直改，再记会重复清位）。
- **`alloc_hard` 在 CHUNK 下不跨 chunk**：单片同时只有一个 chunk 上线，跨窗置位
  不可行（§8"每窗单窗 find"的直接推论）；最大硬分配 = 一个窗（order 4 = 524288 位）。
  记为 A/B 记账项（FULL 无此界）。
- **chunk ≤ 最小片字节数**按名义片长 `per_slice_bits/8` 校验（末片可短于 chunk，
  §3 尾块数学已覆盖）。
- **ALL_FREE 免读**（§7 摘要表收益，超出必测项）：目标窗摘要 ALL_FREE 且无
  pending record → `memset(buffer,0)` 代替读盘；T8 用它断言（窗1 全 0 → 免读）。
- **ctor 错误码不细分**：mount I/O 失败与参数错误都回传 `RC_ERR_STATE`
 （ctor 入口默认值，仅成功时置 RC_OK）——FULL 现行为，红线优先不改；测试按
  `!= RC_OK` + 对象惰性断言。
- **TTL 不做 dtor 收尾写回**：§9 未要求；脏 online 窗在析构时丢账 = 泄漏方向
  （§1.5 接受，与 FULL 依赖 god `bitmaps_flush` 同构）。god 侧接线见遗留。

## 刀④ 测试抓出的真 bug（已修，测试为回归锁）

1. **三段校验坐标错**（见上"校验区间必须用全局位号"）——skip=false 装配 100% 拒挂。
2. **occupy 恒 0 认领**：`chunk_soft_occupy` 给 `window_soft_occupy` 的
   `*rest_bits` 入参按"本窗剩余"传了 `gap`，但 FULL 口径入参是"**已满足量**"
   （`real = len − *rest`）→ `real==0` 早退，rest 原样返回。改传 0。
   T11 影子对账里 occupy 成功数 0 → 修复后 521。
3. **alloc 不置脏**（见上裁决）——预审阶段发现，T2/T11 事后锁死。

## 改动文件（全部在写范围内）

| 文件 | 要点 |
|---|---|
| `include/ravenfs/bitmap_slices.hpp` | +224 行零删除：backing 模式/2bit 摘要/`RC_CHUNK_BUSY`/`bitmap_mutex_t`/`bitmap_chunk_io_t`/`chunk_ctl_t`/`rec_node`/`rec_pool_t`/`slice_backing_t` 类型块；片层 `chunk_*` 方法（自管锁声明）；cfg 7 新字段（`backing_mode/chunk_order/chunk_io/chunk_io_ctx/rec_pool_depth/skip_self_check/self_check_lo/hi`）；group_t CHUNK 装配参数；mgr `chunk_ttl_sync()` 公开 + CHUNK 路由私有声明；`ravenfs_bitmap_in_slice_lock()` 审计钩子。 |
| `juvenile_god/bitmap_slices.cpp` | +1046/−2 行。−2 = ctor 裸指针校验拆双模（唯一删行）。6 处就地分叉（ctor 校验、setup_group、teardown、8 个顶层接口各 1 条装配期常量分支）+ 文件尾 CHUNK 节：`slock_*` 深度审计包装、rec_pool（tag\|idx CAS 抗 ABA）、`enable_slice_chunked`、`ctl_window`、摘要重算、在线直清、`drain_window_locked`、`chunk_ensure_online` 换片状态机（快照/下线/evict/读/drain/发布）、`chunk_lock_window` 快慢路径外壳（含 ALL_USED 跳过 + ALL_FREE 免读）、3 个单窗原语、alloc/occupy 窗口迭代器（含绕回）、`chunk_free_interval`（在线直改/挂 record/池空兜底）、`chunk_test_bit`、`chunk_mount_scan`、`chunk_sync[_one]` TTL 三态、`release_backing`、mgr 装配与 6 个路由 + `chunk_ttl_sync`。 |
| `juvenile_god/CMakeLists.txt` | 未动（无新编译单元，全在 bitmap_slices.cpp）。 |
| `tests/test_bitmap_slice_chunk.cpp` | 新建 734 行：mock chunk_io（盘像+计数+§10.3 断言）+ T1..T11 十一组，125 断言。 |
| `tests/CMakeLists.txt` | +10 行：注册 `ravenfs_juv_bitmap_slice_chunk`（CPU_ID_STUB 按既有模式）。 |
| `Docs/runtime_tech/03_progress.md` | 本文件。 |

## 刀④ 覆盖 ↔ 规格条目

| 规格/任务要求 | 用例 |
|---|---|
| 换片快/慢路径 | T2（慢载入 / 快路径 4 连发零 I/O / 换片读+evict 写计数） |
| 离线 free → record → 上线 drain 顺序（§10.4） | T3（挂账零 I/O → 上线后 A 已清 0 → evict 落盘盘像 A=0） |
| 在线 free 直改 | T2（free/test 全程零 I/O、free_total 即时 +1） |
| 池空兜底（§6/§10.9） | T4（pool_depth=1：record 吃满池 → 强制上线 + 读写计数 + TTL 双态收口） |
| 摘要 ALL_USED 跳过与降级（§10.5） | T5（全占用 ENOSPC 零载入 → free 后降级 → 载 1 窗命中 P） |
| hint 绕回（02 §4） | T6（hint 顶到窗界 → 正向摘要跳过 → 绕回命中窗0 且零载入） |
| 尾块（k mod 2^order ≠ 0） | T7（片1 尾窗 32768 bit 恰读 1 页 + 边界账本回初值） |
| occupy 跨 chunk 续圈（§8） | T8（窗0 认 10 + 窗1 续 90，100 位全置，含 ALL_FREE 免读断言） |
| mock 计数验证「I/O 只在 chg_mu 路径」 | 每次回调断言 `ravenfs_bitmap_in_slice_lock()==false`（§10.3 可测化）+ T2 快路径零 I/O 佐证（chg_mu 为库内成员，外部不可见，取可观测代理） |
| TTL 三态（§9） | T9（态1 写脏+干净跳过+态2 回灌读写计数+盘像落账） |
| 装配契约/三段校验（§2/§7.4/裁决点1） | T10（8 小项：FULL/CHUNK raw 双向、缺 io、order 越界、读失败自清理、尾段校验/挖 0 拒挂/skip 放行、root 灯灭拒挂） |
| 端到端不变量 | T11（3000 次随机 alloc/occupy/free/inode 混合 + 池深 8 → TTL 收尾后 **盘像 == 影子**，block+inode 双组 memcmp） |

## 遗留（不在本任务写范围/后续任务）

1. **god 侧 TTL 接线**：`chunk_ttl_sync()` 已是公开接口，需 30s 定时器 + `bitmaps_load`
   改造（§9/§12）——`ravenfs_juvenile.cpp` 属禁改文件，归上帝类集成任务。
2. **§11 A/B 压测接线**（FULL/CHUNK/mmap 三路线对测，§13 刀④ 的另一种读法）——
   归专项压测任务；本次只交付验收测试文件。
3. **`test_interval` 的 CHUNK 窗口化**（§8 列名）未单列：mgr 无区间测试接口，
   单点 `block_test/inode_test` 已由 `chunk_test_bit` 覆盖；日后加区间接口再补。
4. **`linux/` 移植**（§12：chg_mu→mutex、pool→kmem_cache、io→sb_bread、
   TTL/mount 改造）——按 §1/§6 改制顺序，god 测完压测后才做。
5. **重复 free 虚高 free_count**（§6 记账方式的已知差异）——正确调用方语义下无差。
