# tools/rfs_compact — RavenFS v2.0 离线「extent 文件碎片压缩」运维工具

Host 侧、用户态挂载的**离线盘面修复器**：消化 REG / SOFT_SYM 的 extent 形态文件的
**物理碎片**（逻辑连续、物理断裂的记录串），并把收敛成「单条整域」的稠密文件降级
为 **direct**（顺带消掉 8K 区段树节点税）。契约见 `Docs/disk_layout_v2.0.md §9`。

## 构建

```sh
cmake --build ../../build_v20 --target ravenfs_juvenile   # 确保 god 静态库在
make                                                        # → ./rfs_compact
```

## 用法

```sh
sudo umount /mnt/huge_data                                  # 内核必须先不可见
sudo ./rfs_compact /dev/nvme0n1                             # DRY-RUN（只读挂载，扫描报告）
sudo ./rfs_compact /dev/nvme0n1 --apply                     # 真正修复（rw 挂载 + 写）
sudo ./rfs_compact /dev/nvme0n1 --apply --audit             # 修复后跑全盘 audit
sudo ./rfs_compact /dev/nvme0n1 --max-move-blocks 65536     # 单 run 搬数上限（块；0=不限）
sudo ./rfs_compact /dev/nvme0n1 --apply --no-direct        # 只搬数、不坍缩 direct
```

- **默认 dry-run**：`O_RDONLY` + `is_readonly=true` 只读挂载，扫描并报告
  「extent 文件数 / 记录数(前→后) / runs / 将搬块数 / 将降 direct 数」，**不写盘**。
- `--apply`：rw 挂载，对每个 REG/SOFT_SYM extent inode 调 `regular_file_compact`。
- 挂载方式 = **mmap 后端 + inode/mdata_piece 缓存**（offload），非 O_DIRECT；
  且 `param.slices_mode = DEFRAG`（block 单分片 + `alloc_hard_longest_first`）。

## 算法（`RavenFS_juvenile::regular_file_compact`，实现在 `juvenile_god/file_compact.cpp`）

从 `sb.root_inode`（v2.0 = 0）起 **DFS 全目录树**（dentry 树下钻 `hints[0]` +
叶链 `ptrs[2]` 枚举条目）；对每个 REG/SOFT_SYM inode：

```
size<=160 (inline) 或已 direct  → no-op
否则 extent 树：
  叶链收集记录 → 按逻辑相邻归并成「逻辑 run」
  每个 run：
    k==1                    → 原样保留
    k>1 且 phys 连续        → 零搬数融合成单条
    k>1 且 phys 断裂        → block_alloc_hard(run.len)（DEFRAG=最长空闲段头部）
                              + 逐记录读→写拼到新连续段 → 单条
  洞原样保留（稀疏语义不变；绝不 hard_alloc 整域再打洞）
  结果单条且覆盖 [0, ceil(size/4K)) → 落 direct（不建树）
  否则 extent_build_tree 批量建全新树
  → inode 一次写（唯一提交点）→ free 被搬 run 旧数据 + 释放旧树节点
```

**最后一步（单 run → direct）**：稠密、单条整域的文件落 direct——压缩原语内已内联；
工具出口再做一次**显式收口**（`extent_tree_collapse_to_direct`，幂等），保证
「单 run 文件必为 direct」的不变式（`--no-direct` 可关掉坍缩，只做搬运）。

- **稀疏安全**：只搬真实数据量（Σ 记录长度），不为洞分配。
- **逐 run 尽力**：某 run 分配不到 / 超 `max_move_blocks` → 跳过该 run，其余照提。
- **崩溃安全**：先分配+拷新段 → 建新树（旧树只读）→ inode 提交 → 回收旧资产；
  提交前崩溃 = 旧态完好（新块/新树为孤儿），提交后崩溃 = 新态一致（旧资产孤儿），
  fsck 可收。绝不出现「指向未写数据的新根」。
- 边界防护：inode 索引 `< inode_count`；目录先序 visited 去重；文件 seen 去重；
  深度 / 叶链上限 guard。单文件失败不中断（计入 `失败=`）。

## 安全

- `--apply` **就地改写盘面**。务必：① `umount` 后再跑（ctor 内有
  `device_is_kernel_mounted` 防呆，已挂载 → `RC_ERR_BUSY` 拒挂）；② 有备份或已知
  可接受风险；③ 先 dry-run 复核。
- 工具动**元数据**（区段树节点 + inode 的 `data_root/direct_bit`）与**数据物理位置**
  （被搬 run 的拷贝 + 旧块 free）；**洞不被物化、文件逻辑内容逐字节不变**。
- 幂等：修过的盘再跑，`搬迁=0`（碎裂 run 已收敛为单条）。
