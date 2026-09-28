# 03 chunk 化懒加载改制 — 施工进度（防失落盘）

> 规格：`Docs/runtime_tech/03_chunk_backed_slices.md`（权威）
> 日期：2026-09-28 ｜ 施工：03 §13 四刀 ｜ 红线：FULL 逐位逐性能不变、盘 I/O 永不进 slice_lock
> 本文件**每完成一刀必写**（不依赖会话记忆）；改一个文件记一条。

## 状态总览

| 刀 | 内容 | 状态 |
|---|---|---|
| ① | backing 开关 + cfg/io 契约 + FULL 分叉 + 换片状态机 | 未开始 |
| ② | record 节点池 + 上线前 drain + 摘要降级 | 未开始 |
| ③ | mount 顺序扫描 + TTL 三态分派 | 未开始 |
| ④ | chunk 专项测试收口 | 未开始 |

## 关键裁决（三个待裁点 + 施工期裁决）

1. **CHUNK 模式 cfg 允许 `*_bits_raw` 为空**：✅ 允许。CHUNK 全部状态走 chunk
   buffer + record，`desc.bitmap_raw` 在 CHUNK 下不参与任何路径（`block_test` 等
   入口先分叉到 CHUNK 实现，不会落进旧的 raw 直读分支）。FULL 模式仍必须非空
   （现行为不变）。
2. **CHUNK 模式 `block_test/inode_test` 语义**：三选一取**组合式兜底**——
   在线读 buffer（零 I/O）→ 离线先查 2bit 摘要短路（ALL_USED→true / ALL_FREE→false，
   免载入）→ MIXED 或摘要不足时**兜底走慢路径上线后读 buffer**；I/O 失败/重试超限
   保守返回 true（占用，泄漏方向，绝不错报 free）。理由：§1.1"语义不变"要求返回
   "当前真值"，单选 (1) 离线无答案、单选 (2) MIXED 无答案、单选 (3) 牺牲在线快路径；
   组合式同时满足语义不变 + 摘要免载入收益。
3. **`chg_mu` 实现**：`util/lock.h` 无睡锁 → 自定 `bitmap_chg_mutex_t`（封装
   `std::mutex`），头注释按移植注记风格写明"内核端换实现，接口语义不变"（对齐
   `rw_try_spinlock_t` 的既有注记风格）。不改 lock.h（写范围外）。

施工期追加裁决（按 §1/§10 红线优先）：

- **换片时先把 `online_chunk` 置 NONE 再写盘**（§5 原文在快照后仍留旧 chunk 在线，
  快路径可在写盘期间继续改 buffer → 撕裂写/丢更新）。置 NONE 后 buffer 冻结 ⇒
  **免快照可裸写**；写失败则恢复旧 chunk + 复脏（不丢数据）。TTL 写在线 chunk
  仍按 §9 快照 + 清脏原子（写期间 fast path 可继续改 → 脏标复位后会被再次置起）。
- **drain 掉的位要置脏**：换片 drain 后 buffer ≠ 盘，发布时 `dirty = (drain 清位数>0)`，
  否则"已还账但未落盘"的 free 永不写回（泄漏）。
- **自描述三段校验需要区间参数**：02 §1 的两段"尾巴全 1"是 FS 块号空间的区间
  （`[inode_bm_base, total_blocks)`），mgr cfg 原本拿不到 → 新增
  `self_check_block_lo/hi` 两个 cfg 字段（默认 0=不校验该段）；root inode 亮灯
  （inode 组 bit0）由 mgr 自己就能查，始终做。FULL 模式忽略这两个字段
  （仍由 god 的 `bitmaps_load` 查）。
- **dtor 写回**：CHUNK 析构先跑一轮 `chunk_ttl_sync()`（写脏 online + 回灌离线
  record），对齐 FULL 模式 dtor 时 god 的 `bitmaps_flush()`；否则上帝类换块图即丢账。
- **record 记账按 §6 原文**：挂 record 时 `free_count += len`（drain 不再动计数）。
  代价：对同一区间重复 free 会虚高 free_count（FULL 的 free_interval 不会）。
  正确调用方语义下无差；记为已知差异，不为它加二次扫描（避免过度工程）。
- **`alloc_hard` 在 CHUNK 下不能跨 chunk**：单片同时只有一个 chunk 在线，跨窗置位
  不可行（§8"每窗单窗 find"已隐含）。最大硬分配 = 一个窗（默认 order 4 = 524288 位）。
- **chunk 约束校验取"名义片长"**（`per_slice_bits/8`，两组都过）：末片可短于
  chunk，尾块数学（§3 tail）已覆盖，安全。

## 改动文件

（每刀更新）
