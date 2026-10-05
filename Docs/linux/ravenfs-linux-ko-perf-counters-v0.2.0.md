# RavenFS Linux KO · v0.2.0 性能计数器（重设计）

> 2026-10-06 · 随 KO 两跳迁移（v1.2→v1.3→v2.0）重设计。承 `ravenfs-linux-ko-perf-counters-v0.1.2.md`。
> 出口：`/proc/fs/ravenfs/<s_id>/stats`（0444，人人可读，D-11 公开面）+
> debugfs `/sys/kernel/debug/ravenfs/<s_id>/stats`（内部探针）。二者共用 `ravenfs_stats_show`。

## 0. 为什么要重设计

旧 v0.1.2/v0.1.3 计数器绑定「固定 inode 数组 + inode 位图 + 名字堆」的运行时：
- `nh_*`（6，名字堆）—— 名字堆已删，**死组**。
- `ic_*`（9，inode 行缓存）—— 行缓存已删（v2.0 InodeSession 走 mdata 片），多数字段**死**。
- `ext_*`（7）—— extent 机械现服务**三族**树（文件/名字面映射/inode 面映射），单组**串味**。
- 新面（名字数据面、全局 inode 数据面）**无任何计数器**。

## 1. 结构（两段）

| 段 | 位置 | 字段数 | 上报方式 |
|---|---|---|---|
| 主 stats | `struct ravenfs_sb_stats`（`sbi->stats`） | **93** | `RFS_STAT_INC/ADD(sb, field)` 直写 atomic64 |
| al_ 段 | `struct ravenfs_al_stats`（`sbi->al`），下标 = `enum rfs_al_stat` | **47** | 平台中立模块经注入 `struct rfs_stat_hook` 上报；宿主 shim 累加 |

`BUILD_BUG_ON(sizeof == ARRAY_SIZE(names)*8)` + `AL_STAT_COUNT == ARRAY_SIZE(al_names)` 三处自守。
**增删字段须同改：结构 + 名字表（super.c）+ 枚举（al_）。**

## 2. 主 stats 分组（93）

| 组 | 前缀 | 数量 | 变更 |
|---|---|---|---|
| A VFS 操作 | `op_` | 26 | 不变 |
| B inode 会话 | `ic_` | **7** | **重设**（原行缓存 9 → v2.0 InodeSession） |
| C mdata 片缓存 | `md_` | 8 | 不变（现同时服务 B+树节点 + inode 片） |
| D TTL | `ttl_` | 2 | 不变 |
| E 块设备 IO | `io_` | 5 | 不变 |
| G extent | `ext_` | **9** | **+ext_nd_ops/ext_idp_ops**（三族消歧） |
| H 目录 | `dir_` | 3 | 不变 |
| I xattr/ACL | `xa_` | 10 | 不变 |
| J 错误健康 | `err_` | 6 | 不变 |
| K 生命周期 | `life_` | 4 | 不变 |
| L 名字堆 | `nh_` | **0** | **退役** |
| M 名字数据面 | `nd_` | **13** | **新增** |

**B `ic_`（v2.0 InodeSession，经数据面 + mdata）**：
`ic_get`（session_get 成功）· `ic_get_err`（无 sbi/alloc、超容量、kzalloc 失败）·
`ic_locate_err`（inode 面翻译失败）· `ic_dirty`（标脏）· `ic_evict`（inode_reclaim）·
`ic_flush`（inode_flush）· `ic_err`（mdata 等内部错）。

**M `nd_`（v1.3 名字数据面）**：
`nd_resolve` · `nd_map_append`（direct→tree 物化/尾插）· `nd_grow`（增 8K 片）·
`nd_alloc` · `nd_free` · `nd_read` · `nd_write` · `nd_release`（目录销毁全量回收）·
`nd_reclaim`（映射树数据片归还）· `nd_fr_inline`（inline 态 fr 操作）·
`nd_fr_tree`（tree 态 fr 操作）· `nd_fr_split`（fr 叶/内分裂）· `nd_fr_retire`（空叶退役）。

## 3. al_ 段分组（47）

| 段 | 数量 | 变更 |
|---|---|---|
| A 装配/生命周期 | 5 | 不变 |
| B block 组顶层 | 9 | 不变 |
| C inode 组顶层 | 3 | 不变（`al_ino_alloc_ok/fail/free_calls`，路由顶层） |
| D 片内双树 | 8 | 不变 |
| E 融合 | 3 | 不变 |
| F 路由/跨片 | 3 | 不变 |
| G checkpoint/元素池 | 5 | 不变 |
| **H inode 数据面** | **11** | **新增** |

**H `al_idp_*`（v2.0 全局 inode 数据面）**：
`locate_ok` / `locate_fail`（翻译）· `free`（释放，含融合后计数）· `grow` / `grow_pieces`
（增片：次数 / 片数）· `fr_merge`（空闲区间融合）· `tree_dump`（同步 dump 映射）·
`tree_build`（映射树 bulk build）· `snap_write` / `snap_read`（扁 fr 快照写/读）·
`snap_blocks`（快照写块数）。

## 4. 接线

```
内核模块（有 sb）        RFS_STAT_INC/ADD(sb, nd_*)   ─┐
平台中立 fr 模块          rfs_ndfr_ctx.stat (hook)      │
  └ name_data_plane.c 宿主 shim: inc -> RFS_STAT_ADD(   ├─► struct ravenfs_sb_stats (主)
        sb, nd_fr_*, delta)（按 enum rfs_ndfr_stat switch）│
inode 数据面 (plane)      owner_->stat (hook, owner=rfs_allocator) ─► struct ravenfs_al_stats (al_)
  └ allocator_adapter.c   RFS_AL_INC(sb, al_idp_*)     ─┘
extent_write.c            rfs_ext_kind_stat(sb, kind): FILE 只计 ext_*，
                          NAMEDATA/INODE 追加 ext_nd_ops/ext_idp_ops
```
纪律（承 P1/P2）：打点只 `atomic64_add`、**不参与控制流**、零分配零 I/O、可持锁；
未注入 hook = no-op（用户态压测 / fr 单测用 `RFS_STAT_HOOK_NULL`）。

## 5. guest 实测（v2.0，`/proc/fs/ravenfs/<s_id>/stats`）

500 短名 + 500 长名 + 删半后取增量（节选）：
```
ic_get            1 -> 13770      md_hit          0 -> 22656
ext_lookup        0 -> 650        ext_nd_ops      0 -> 654
nd_resolve        0 -> 832        nd_alloc        0 -> 500
nd_write          0 -> 500        nd_read         0 -> 332
nd_grow           0 -> 2          nd_map_append   0 -> 2
nd_fr_inline      0 -> 510        nd_fr_tree      0 -> 159
al_idp_locate_ok  1 -> 13770      al_idp_free     0 -> 416
al_idp_snap_write 0 -> 2          al_idp_snap_blocks 0 -> 4
```
33 个新字段全数出现并随负载跳动；同轮 `audit_v20 bad_total=0`（打点零行为变化）。
（`al_idp_grow`/`ext_idp_ops` 未动：本轮 500 文件未触 inode 面 grow / inode 映射树保持 direct。）
