# tools/rfs_compact — RavenFS extent 树瘦身 + 单条坍缩 direct 修复器

Host 侧、用户态挂载的**离线盘面修复器**，用于清除 ko `ravenfs-dkms 0.1.1-1` 留下的
元数据/数据失衡创伤（见 `Docs/linux/ravenfs-linux-ko-v0.1.1-defects.md`）：

- **D-6**（inline→extent 不试 direct）：>160B 文件恒背一个 8K 区段树根叶（单 extent 也背）
  → 本工具把「单条覆盖 [0, ceil(size/4K))」的树**坍缩为 direct**，消掉节点税。
- **D-5**（ko 无叶内融合）：相邻 extent 未合并（如 `root-ext4.qcow2` 133k 条 64 块记录）
  → 本工具先 `extent_rebuild_tree`（全量可并对全并）把同段连续记录融合，再坍缩。

## 构建

```sh
cmake --build ../../build_fresh --target ravenfs_juvenile   # 确保 god 静态库在
make                                                        # → ./rfs_compact
```

## 用法

```sh
sudo umount /mnt/huge_data                                  # 内核必须先不可见
sudo ./rfs_compact /dev/nvme0n1                             # DRY-RUN（只读挂载，扫描报告）
sudo ./rfs_compact /dev/nvme0n1 --apply                     # 真正修复（rw 挂载 + 写）
sudo ./rfs_compact /dev/nvme0n1 --apply --audit             # 修复后跑全盘 audit
```

- **默认 dry-run**：`O_RDONLY` + `is_readonly=true` 只读挂载，扫描并报告
  「extent 文件数 / 记录数(前→后) / 可坍缩数」，**不写盘**。
- `--apply`：rw 挂载，对每个 REG extent inode 依次 `extent_rebuild_tree` → `extent_tree_collapse_to_direct`。
- 挂载方式 = **mmap 后端 + inode/mdata_piece 缓存**（offload），非 O_DIRECT。

## 算法

从 `sb.root_inode`（v1.2=16）起 **DFS 全目录树**（`dir_readdir` 未实现 → 走底层
**dentry 树下钻 `hints[0]` + 叶链 `ptrs[2]`** 枚举条目）；对每个 `type==REG` inode：

```
size<=160 (inline) 或已 direct  → 跳过
否则 extent 树形态             → extent_rebuild_tree(root)  → 回写 inode.data_root
                               → extent_tree_collapse_to_direct(inum)  （单条 → direct + free 节点）
```

- 先重建后坍缩（重建负责融合，坍缩负责去节点）。
- 先 inode 提交后 free 节点（崩溃窗口 = 孤儿块，无悬垂）。
- 边界防护：inode 索引 `< inode_count`；目录/REG 各自 visited 去重；深度/叶链上限 guard。
- 单文件失败不中断（计入 `失败=`，跳过继续）。

## 安全

- `--apply` **就地改写盘面**。务必：① `umount` 后再跑（否则双写者）；② 有备份或已知可接受风险；③ 先 `--dry-run` 复核。
- 工具只动**元数据**（区段树节点 + inode 的 `data_root/direct_bit`）与**位图空闲**，**不动文件数据字节**。
- 幂等：修过的盘再跑，`extent 形态=0`。
