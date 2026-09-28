# 性能计数器部署进度（`Docs/linux/03_counters_progress.md`）

> 2026-09-28 · 编码团队（DS v4-flash）· 作业对象：`ravenfs-linux-ko-sb-counters-design.md`（唯一规格）
> 纪律：**每阶段必写本文件**（防失落盘）；**写范围**（硬边界）：`linux/super.c`、`linux/bitmap.c`、
> `linux/Makefile`（未改）、本文件；`tests/linux/` 只许执行禁改；`juvenil_god/`、`include/`、`tests/`、
> `vm/`、`mkfs/`、`packaging/`、设计文档本身 —— **零触碰**。

## 部署时机注记（遵照规格 §3.F / §9）

规格 §3.F 末尾明确「**计数器代码部署 = 测试团队完成位图 chunk 化验收后的下一阶段**」。
测试团队 Phase B（`Docs/linux/03_test_progress.md`）已于 2026-09-28 20:1x 收官（CHUNK 10/1、FULL 10/1，
唯一 FAIL = C6 既有行为）→ **触发条件满足，本轮部署。**

---

## 一、交付总览

| 阶段 | 内容 | 状态 | 证据 |
|---|---|---|---|
| P0 | `struct ravenfs_sb_stats`（98）+ `RFS_STAT_INC/ADD` 宏 + debugfs 出口骨架 | ✅ | §三.1 / §四 |
| P1-A/B/C/D | VFS 操作面 / icache / mdata / TTL 打点 | ✅ | 各文件要点 |
| P1-E | 块设备 IO（io_）下盘点 | ✅（覆盖率见出入记录 ③） | — |
| P1-F | 位图 18 个（FULL/CHUNK **双后端**） | ✅ | §五 FULL↔CHUNK 冒烟 |
| P1-G/H/I | extent / 目录 / xattr·ACL 打点 | ✅ | — |
| P1-J/K | 错误健康 / 生命周期 | ✅ | — |
| 构建 | `make -C linux` 零警告（仅 pahole 基线提示） | ✅ | §四 |
| 冒烟 | FULL↔CHUNK 模块参数下关键计数行为 | ✅ | §五 |

**改动面**：12 个文件、+467/−29；**Makefile 未改**（无新增 .c，全部落既有文件）。

---

## 二、每文件要点

| 文件 | 要点 |
|---|---|
| `ravenfs.h`（核心头） | +`struct ravenfs_sb_stats`（98 atomic64，按 A–K 分段）；`ravenfs_sb_info` +`stats`/`dbg_dir`/`mounted_at_ns`；+`RFS_STAT_INC/ADD` 宏（判 `s_fs_info` 空 = 宏存在的唯一理由，§7.1）。 |
| `super.c` | debugfs 骨架（root/`<s_id>/stats` seq_file/`DEFINE_SHOW_ATTRIBUTE`，§8）；A：`op_fsync`/`op_syncfs`/`op_getattr`；E：`io_flush_ops`（`ravenfs_issue_flush`）；J：`err_shutdown_trig`（`force_shutdown`）/`err_shutdown_hits`/`err_sb_read`（fill_super `sb_bread`）；K 全部 4（mount/umount/iget/evict）。 |
| `bitmap.c` | **F 组 18 个（重点）** + E 组位图 io：FULL 落点 = 现行为真实点位；CHUNK 专属 9 个只在 CHUNK 函数内（FULL 恒 0）。 |
| `inode_cache.c` | B 组 9（ic_hit/miss/row_load/retry/row_evict/row_reap/dirty_mark/wb_rows/err）+ E：`ravenfs_row_load` io_read + `err_meta_io`。 |
| `mdata_cache.c` | C 组 8（md_*）+ E：`ravenfs_mrow_load` io_read + `err_meta_io`。 |
| `cache_ttl.c` | D 组 2（ttl_rounds/ttl_round_errs）。 |
| `dentry.c` | A：op_create/mkdir/unlink/rmdir/rename/link/symlink/mknod/tmpfile/lookup/readdir；H：dir_emit/dir_nlink_adjust；E：`rfs_pwrite_dev` io_write；J：err_shutdown_hits（link/sym/mknod/tmpfile）/err_enospc（create/mkdir）。 |
| `dentry_tree.c` | H：`ravenfs_read_leaf` dir_leaf_reads；J：err_other（叶魔数不符兜底）。 |
| `file.c` | A：op_read_iter/write_iter/read_bytes/write_bytes/fallocate/setattr/mmap；J：err_shutdown_hits。 |
| `iomap.c` | E：`rfs_store_block` io_write；J：err_enospc（写分配）。 |
| `xattr.c` | A：op_listxattr/xattr_get/xattr_set/acl_get/acl_set；I 组 10（xa_*/acl_*）；E：`rfs_xb_load/store` io_read/write；J：err_shutdown_hits。 |
| `extent_write.c` | G 组 7（ext_lookup/insert/split/new_root/balance/unmap/node_reads）。 |

---

## 三、§9 部署清单（逐项 ✓/未部署；98 字段全清单）

> 全部 98 字段**已部署**（`wired 98/98`，脚本核验）。下表列**代表落点**（多落点取主）。
> 组首数字 = 规格 §3 声明字段数。**CHUNK 专属 = FULL 后端恒 0**（§3.F）。

### A. VFS 操作面 `op_`（26）—— ✅ 全部署

| 字段 | 落点 |
|---|---|
| op_lookup | dentry.c `ravenfs_lookup` 入口 |
| op_create / mkdir / unlink / rmdir / rename / link / symlink / mknod / tmpfile | dentry.c 各回调入口 |
| op_readdir | dentry.c `ravenfs_iterate` 入口 |
| op_read_iter / op_read_bytes | file.c `ravenfs_file_read_iter`（bytes = 返回字节） |
| op_write_iter / op_write_bytes | file.c `ravenfs_file_write_iter` |
| op_fallocate / op_setattr / op_mmap | file.c 对应回调 |
| op_fsync / op_syncfs / op_getattr | super.c 对应回调 |
| op_listxattr / op_xattr_get / op_xattr_set / op_acl_get / op_acl_set | xattr.c 对应回调 |

### B. inode 行缓存 `ic_`（9）—— ✅ 全部署
ic_hit/miss ← `ravenfs_session_get`（§7.3 互斥完备：miss 在最终归宿记一次，`-EAGAIN` 重试不重复计）；
ic_row_load ← `ravenfs_row_load` 成功返回；ic_retry ← `-EAGAIN→retry`；ic_row_evict ← `ravenfs_inode_reclaim`（规格指定）；
ic_row_reap ← `ravenfs_cache_reap_expired`（按 `freed`）；ic_dirty_mark ← `ravenfs_inode_mark_dirty`；
ic_wb_rows ← `ravenfs_cache_wb_dirty` 每行成功落盘；ic_err ← `session_get`/`row_load` 错误返回。

### C. mdata 节点缓存 `md_`（8）—— ✅ 全部署
md_hit/miss ← `ravenfs_mdata_get`；md_node_load ← `ravenfs_mrow_load` 成功返回；md_retry ← `-EAGAIN`；
md_drop ← `ravenfs_mdata_drop`；md_reap ← `ravenfs_mdata_reap_expired`；md_node_wb ← `wb_dirty`+`wb_node` 每节点落盘；md_err ← 错误返回。

### D. TTL 后台 `ttl_`（2）—— ✅ 全部署
ttl_rounds ← `ravenfs_ttl_work` 正常完成；ttl_round_errs ← 回写/位图刷失败分支。

### E. 块设备 IO `io_`（5）—— ✅ 已部署（**覆盖率见出入记录 ③**）
io_read_ops/bytes ← 物理盘读点：`ravenfs_row_load`、`ravenfs_mrow_load`、`rfs_xb_load`、
`rfs_bm_group_load`(FULL)/`rfs_bm_chunk_read`(CHUNK)；
io_write_ops/bytes ← `rfs_pwrite_dev`、`rfs_store_block`、`rfs_xb_store`、
`rfs_bm_group_sync`(FULL)/`rfs_bm_chunk_write`(CHUNK)；
io_flush_ops ← `ravenfs_issue_flush`。

### F. 位图分配 `bm_`（18）—— ✅ 全部署（**FULL/CHUNK 双后端语义对齐 §3.F**）

| 字段 | FULL 落点 | CHUNK 落点 | 后端 |
|---|---|---|---|
| bm_alloc_calls/ok/fail | `rfs_bm_group_alloc` 入口/成/败（组级，两模式同一计数） | 同左（分叉后取 chunk 路由返回） | 共用 |
| bm_alloc_blocks | `rfs_bm_slice_alloc` 命中 | `rfs_bm_win_alloc_hard` 命中 | 共用 |
| bm_free_calls | `rfs_bm_group_free` 入口 | 同左（分叉后 chunk 路由） | 共用 |
| bm_free_blocks | `rfs_bm_group_free` 逐段 1→0 清位 | `rfs_bm_clear_bits_locked` + `rfs_bm_drain_window` 实清 | 共用 |
| bm_soft_ok | `rfs_bm_slice_alloc_soft` 命中 | `rfs_bm_win_alloc_soft` 命中 | 共用 |
| bm_occupy | `rfs_bm_slice_occupy` 认领≥1 | `rfs_bm_win_soft_occupy` 认领≥1 | 共用 |
| bm_sync_slices | `rfs_bm_group_sync` 逐片写回 | `rfs_bm_chunk_sync_one` 态1+态2 | 共用（单位随模式） |
| bm_chunk_switch / _ns | **恒 0** | `rfs_bm_chunk_ensure_online` 发布窗处 | **CHUNK 专属** |
| bm_rec_attach / bm_rec_drain | **恒 0** | `rfs_bm_chunk_slice_free` 挂账 / `rfs_bm_drain_window` 摘节点 | **CHUNK 专属** |
| bm_pool_empty | **恒 0** | `rfs_bm_chunk_slice_free` 池空兜底 | **CHUNK 专属** |
| bm_summary_skip | **恒 0** | `rfs_bm_chunk_lock_window`(ALL_USED) + `rfs_bm_chunk_slice_test` 摘要裁决 | **CHUNK 专属** |
| bm_scan_ns | **恒 0** | `rfs_bm_chunk_mount_scan` 逐窗计时 | **CHUNK 专属** |
| bm_ttl_online / bm_ttl_drain | **恒 0** | `rfs_bm_chunk_sync_one` 态1 / 态2 写回成功 | **CHUNK 专属** |

### G. extent 树 `ext_`（7）—— ✅ 全部署
ext_lookup ← `ravenfs_extent_lookup`；ext_insert ← `ravenfs_extent_insert`；ext_split ← `rfs_ext_leaf_split`+`rfs_ext_inner_split`；
ext_new_root ← `rfs_ext_new_root_inner`+`ravenfs_extent_new_root_leaf`；ext_balance ← `rfs_ext_node_balance`+`rfs_ext_balance_from`；
ext_unmap ← `ravenfs_extent_unmap_loose`；ext_node_reads ← `ravenfs_extent_lookup`/`rfs_ext_walk` 每节点取用（见出入 ④）。

### H. 目录 `dir_`（3）—— ✅ 全部署
dir_leaf_reads ← `ravenfs_read_leaf`；dir_emit ← `ravenfs_emit_leaf`；dir_nlink_adjust ← `rfs_dir_adjust`。

### I. xattr / ACL `xa_`（10）—— ✅ 全部署
xa_get/set/del/list ← `ravenfs_xattr_get/set`（del = set 空值）/`ravenfs_listxattr`；
xa_block_load/store ← `rfs_xb_load/store`；xa_e2big ← `rfs_xattr_set_core` 两处 `-E2BIG`；
xa_err ← get/set 错误返回；acl_get ← `ravenfs_get_inode_acl`；acl_set ← `rfs_set_acl_internal`（含 `init_acl` 内部调用）。

### J. 错误与健康 `err_`（6）—— ✅ 已部署（**兜底覆盖见出入 ⑤**）
err_shutdown_hits ← 各 `ravenfs_is_shutdown()` 判真返 -EIO 处（link/sym/mknod/tmpfile/xattr*/file*/super sync_fs/fsync）；
err_shutdown_trig ← `ravenfs_force_shutdown`（每次调用都记，§7.3）；err_meta_io ← 行/节点/位图 sb_bread 失败；
err_sb_read ← `fill_super` sb_bread 失败；err_enospc ← create/mkdir/写分配上抛 -ENOSPC；err_other ← `ravenfs_read_leaf` 叶魔数兜底。

### K. 生命周期 `life_`（4）—— ✅ 全部署
life_mount ← `fill_super` 成功末尾；life_umount ← `put_super` 入口；life_iget_new ← `ravenfs_iget` `I_NEW` 分支；life_evict ← `ravenfs_evict_inode`。

### §3.F 部署时机（规格注记）—— ✅ 遵照
见文首「部署时机注记」：测试团队收官后部署。

---

## 四、构建证据（`make -C linux`）

- `make clean && make` → **exit 0，零代码警告**；唯一输出 `warning: pahole version differs from the one used to build the kernel`（**基线即有**的 pahole 工具链提示，非本改动引入）。
- BTF 生成通过 → **`struct ravenfs_sb_stats` 无编译期填充问题**；`super.c` 内 `BUILD_BUG_ON(sizeof(struct ravenfs_sb_stats) != 98*sizeof(atomic64_t))` 通过（seq_file 以连续数组读 98 字段的合法性由它守着）。
- 字段接线自检：脚本核对规格 §3 全 98 名 → `wired 98/98, missing []`。

---

## 五、FULL ↔ CHUNK 冒烟（guest VM，tests/linux harness 只读复用）

环境：`linux/vm/run-vm.sh daily` 起 VM（guest kernel 7.2.6-arch2-1）；复用测试团队 `tests/linux/bitmap_chunk_guest.sh`
的 `build/load/mkfs/mount/umount/unload` 相（**脚本零改**，仅执行）；打点读数 `sudo cat /sys/kernel/debug/ravenfs/<s_id>/stats`。
负载：40×4–20K 文件 + 3 目录 + 读取 + 删 3 + `ls -R` + `sync`；续加 300 小文件建删；再 4G 填充后删。

| 关键量 | FULL（backing=0） | CHUNK（backing=1） | 判读 |
|---|---|---|---|
| op_lookup / op_create / op_write_bytes | 56 / 40 / 491520 | 56 / 40 / 491520 | **行为零变化**（同 workload 逐项相等） |
| ic_hit / ic_miss / ic_row_load | 769 / 7 / 7 | 769 / 8 / 8 | 缓存路径打点接通 |
| md_hit / md_node_load | 726 / 45 | 726 / 45 | 同上 |
| io_read_ops / io_write_bytes | 385 / 2523136 | 1039(含 mount 扫描) / 2703360 | 打点接通；CHUNK 写量更省（只写脏 chunk） |
| **bm_alloc_ok + bm_alloc_fail == bm_alloc_calls** | 208+0=208 ✅ | 5234+0=5234 ✅ | §10 判据 3 完备性 |
| bm_alloc_blocks / bm_free_blocks | 252 / 15 | 1050087 / 1049850（4G 填充后） | 两模式同量级一致 |
| **bm_chunk_switch / bm_chunk_switch_ns** | **0 / 0** | **16 / 54343515ns** | CHUNK 专属：FULL 恒 0 ✅ |
| **bm_scan_ns** | **0** | **17748474ns(~17.7ms)** | CHUNK 专属 ✅ |
| **bm_rec_attach / bm_rec_drain** | **0 / 0** | **25 / 25**（4G 删后） | CHUNK 专属；净挂账=0（§4） |
| **bm_ttl_online / bm_ttl_drain** | **0 / 0** | **39 / 1** | CHUNK 专属 ✅ |
| bm_pool_empty / bm_summary_skip | 0 / 0 | 0 / 0 | **已接线但本负载未触发**（见下） |
| life_mount / life_umount | 1 / 0 | 1 / 0 | ✅ |
| ttl_rounds | 2 | ≥8 | 后台在转 |
| 卸载/rmmod | 成功 | 成功，`dmesg` 无 BUG/Oops/KASAN | ✅ |

**未触发的两个 CHUNK 专属**：`bm_pool_empty`（需 record 池撞深 64——本负载只挂 25 个）、`bm_summary_skip`
（需扫描到 `summary==ALL_USED` 的窗——本负载 chunk 未填满后回扫）。二者落点经代码走查确认（FULL 恒 0 亦满足）；
彻底触发需重负载（满盘 ENOSPC 往返），可并入后续压测专项。

**手工复跑步骤**（供复核）：
```bash
cd <repo>/linux/vm && ./run-vm.sh daily &     # 起 VM（:2222）
cd <repo> && scp -P 2222 tests/linux/bitmap_chunk_guest.sh raven@localhost:/tmp/bm-accept.sh
ssh -p 2222 raven@localhost 'chmod +x /tmp/bm-accept.sh; rm -rf /tmp/bm-accept; mkdir -p /tmp/bm-accept'
ssh -p 2222 raven@localhost 'bash /tmp/bm-accept.sh build'
# backing=0（FULL）：
ssh -p 2222 raven@localhost 'bash /tmp/bm-accept.sh load 0 4; bash /tmp/bm-accept.sh mkfs; bash /tmp/bm-accept.sh mount'
ssh -p 2222 raven@localhost "cd /mnt/rfs && sudo mkdir -p w && sudo chown \$(id -u) w && cd w && for i in \$(seq 1 40); do dd if=/dev/urandom of=f\$i bs=4K count=2 status=none; done; sync"
ssh -p 2222 raven@localhost 'sudo sh -c "cat /sys/kernel/debug/ravenfs/*/stats"'   # ← CHUNK 专属应恒 0
ssh -p 2222 raven@localhost 'bash /tmp/bm-accept.sh umount; bash /tmp/bm-accept.sh unload'
# backing=1（CHUNK）：同上换 `load 1 4`；仍应 bm_chunk_switch/bm_scan_ns>0、bm_alloc_ok+fail==calls
```

---

## 六、文档 ↔ 实现出入记录（发现即记，**不改文档**）

1. **写范围含 `ravenfs.h`**：设计 §6/§7.1 明确要求 `struct ravenfs_sb_stats` 与宏**放 ravenfs.h**
   （且 bitmap.c 与 super.c 都要用 → 必须住共享头）。`ravenfs.h` 不在 ✅ 清单、亦不在 ❌ 清单 →
   **视其为必要共享头而编辑**（无它则位图打点无法落地）。如需严格限定两文件，请指示回退方案。
2. **debugfs `i_private` = `sb`（规格载明 `sbi`）**：为在 stats 头行打印 `sb=<s_id>`（§8.1 格式）
   需 sb 在手；`sb->s_fs_info` 即 sbi。**UAF 顺序不变**（`put_super` 中 `debugfs_remove_recursive`
   仍**早于** `kfree(sbi)`）。另新增 `sbi->mounted_at_ns`（§8.1 头行用，非计数器）。
3. **`io_` 下盘点收窄为「物理盘读/写点」**：规格 §3.E 列了 `ravenfs_read_leaf` 与泛 `sb_bread`；
   但 `read_leaf`→`ravenfs_read_node`→`ravenfs_mrow_load`，在两者都记会**重复计**且把**缓存命中**误当盘读
   （违反「物理读次数/字节」口径）。故只记物理点。**副作用**：规格写侧未列 `ravenfs_row_store`/`ravenfs_mrow_store`
   （inode/mdata 回写）→ `io_write_bytes` **低估写放大**（与 §11.2「覆盖率不完全」声明一致）。
4. **`ext_node_reads` 落点扩展**：规格仅列 `rfs_ext_walk`；为让派生式 `ext_node_reads/ext_lookup`（§4）成立，
   亦在 `ravenfs_extent_lookup` 每条 `ravenfs_read_node` 处记（否则查询路径的走树步数不计，比值失真）。
5. **`err_other` 兜底覆盖薄**：仅一处（叶魔数不符）。真正「其余未归类」的错误返回散布极广，
   完整覆盖有过度工程化之嫌；当前按「少量计数器」立场只示位。`err_enospc` 亦仅在**对外链路上抛点**
   （create/mkdir/写）记，树内部分裂返回的 `-ENOSPC` 未计。
6. **`xa_err` 会把 `-ENODATA` 计入**：规格谓「错误返回」，但 xattr「不存在」= `-ENODATA`（常态负返回），
   导致 `xa_err ≈ xa_get`（冒烟实测 43/43、44/44）。**建议**后续裁决是否排除 `-ENODATA`（本轮按规格字面实现）。
7. **`ic_dirty_mark` 实测恒 0**：规格落点 `ravenfs_inode_mark_dirty`，但 create/unlink/mkdir… 等**直接**
   用 `RFS_ROW_MARK_DIRTY(row)` 标脏，不经 `ravenfs_inode_mark_dirty` → 该计数器打不到主路径（覆盖缺口）。
8. **`bm_alloc_blocks` 只计硬分配**：规格落点仅列硬路径（FULL `rfs_bm_slice_alloc` / CHUNK `rfs_bm_win_alloc_hard`）；
   软分配亦分配块但不入此计（软路径由 `bm_soft_ok` 记次数）。按规格未扩。
9. **`bm_free_blocks` 计在「1→0 实清」全点**：规格列 FULL 逐段清位 + CHUNK 在线 `clear_bits_locked`/离线挂账；
   为满足「只按 1→0 实清累计」，**离线挂账不计、其 drain 实清时计**（`bm_drain_window` 内 +`cleared`）——
   否则离线释放永不入账。

---

## 七、遗留 / 后续

1. `bm_pool_empty`、`bm_summary_skip` 的**触发型验证**（满盘 ENOSPC 往返压测专项）。
2. `io_` 覆盖率补齐（规格 §11.2 未决项）：buffer_head/iomap 间接 IO、inode/mdata 行/节点回写是否计入。
3. 派生指标 `awk` 一行式随验收补（§4 建议；`cat stats` 后现算）。
4. gauge 段（§5，P2）：`shutdown` 已随 `# state` 段输出；脏行数/在核心行数等需只读导出接口，留 P2。
5. `xa_err` 是否排除 `-ENODATA`、`ic_dirty_mark` 落点是否改挂 `RFS_ROW_MARK_DIRTY` —— 待规格裁决。
6. 开销验收（§10 判据 6：同 workload 吞吐差 <2%）未做 —— 需前后重编对比，归专项。
