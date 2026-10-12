# audit_leakscan — `audit/` 的 fork：漏块 8K 单元魔数扫描

> 目的：`audit/` 只把「位图亮但不可达」的块**计数**（且明细只打前 64 条）。
> 本 fork 在收口时**多走一步**：对每个漏块所属的 **8K 单元**读首 32B 魔数，
> 判定它是哪一类节点（还是纯数据），并给出按类型的汇总。用于把「漏了 3609 块」
> 从"数量"推进到"**漏的是哪一层结构**"。

## 与 `audit/` 的差异（只有一处）

改动仅落在 `audit_dfs.cpp` → `audit_session::reconcile_block()`：

- 新增匿名命名空间的 `leak_kind_t` / `leak_kind_str()` / `leak_classify()`；
- 漏块分支里，按 **8K 单元**（`blk & ~1`）去重后扫描该单元首 32B，输出一行 `[TRACE][LEAKSCAN]`；
- 收口结束后恒打印一行 `[AUDIT][SUM][LEAKSCAN]` 类型汇总。

其余文件（引导五门 / 相位 A / 相位 B / 其它收口）与 `audit/` **逐字节同源**，
公共头仍复用 `include/audit/*.hpp`（未改契约）。

## 判据

8K 单元 = 2 连续 FS_block（块/片恒 8K 对齐，见 `Docs/disk_layout_v2.0.md`
§2.4 / §11.2 / §12.2）。单元首 32B 恰好是 `Bptree_meta_t.magics[4]`。类型对表：

| `kind=` | 魔数来源（`include/magics.h`） | 含义 |
|---|---|---|
| `extent_tree_node`  | `BPTREE_MAGIC1..4`               | 文件区段树节点 |
| `dentry_tree_node`  | `DENTRY_MAGIC1..4`               | 目录 dentry 树节点 |
| `namedata_map_node` | `NAMEDATA_EXTENT_MAGIC1..4`      | 名字数据面映射树节点 |
| `namedata_fr_node`  | `NAMEDATA_FREERANGES_MAGIC1..4`  | 名字数据面 fr 树节点 |
| `inode_map_node`    | `INODE_DATA_EXTENT_MAGIC1..4`    | 全局 inode 数据面映射树节点 |
| `inode_fr_snapshot` | `INODE_DATA_FR_SNAPSHOT_MAGIC1..4` | 全局 inode 数据面扁 fr 快照区 |
| `xattr_block`       | `RFS_XATTR_H_MAGIC` = `"RFSX"`   | xattr 块（单块） |
| `data_or_unknown`   | 无 | 文件数据 / 垃圾 / 非节点头 |

> ⚠️ 磁数命中即定型；未命中一律归 `data_or_unknown`，**不等于**"一定是数据"。

## 构建

```sh
cmake --build build_v20 --target modified_audit
# → build_v20/audit_leakscan/modified_audit
```

（根 `CMakeLists.txt` 已 `add_subdirectory(audit_leakscan)`；目标名 `modified_audit`，
静态库目标 `ravenfs_audit_leakscan`，均不与 `audit/` 冲突。）

## 运行

```sh
# 需读块设备权限（root / disk 组）
sudo build_v20/audit_leakscan/modified_audit --trace /dev/nvme0n1 > leakscan_trace.txt 2>&1
```

输出两段：

```
[TRACE][LEAKSCAN] unit=14872692 magic0=0x... magic1=0x... kind=extent_tree_node
...
[TRACE][LEAKSCAN-SUM] units=1804 blocks=3609
[AUDIT][SUM][LEAKSCAN] units=1804 extent_tree_node=... dentry_tree_node=... \
  namedata_map_node=... namedata_fr_node=... inode_map_node=... \
  inode_fr_snapshot=... xattr_block=... data_or_unknown=...
```

- 逐单元 `[TRACE][LEAKSCAN]` 需 `--trace`；
- `[AUDIT][SUM][LEAKSCAN]` 恒打印（无 `--trace` 也有汇总）。

## [临时] 发现一个泄露→修复一个：`--free-leaks`

> ⚠️ 这是**写盘**能力，违反 audit「只读」纪律（`audit/DISCIPLINE.md`）。默认关，仅供本次损坏研究回收空间。

```sh
# 仅在盘【未挂载】时使用；拒绝在 /proc/mounts 里出现的设备
sudo build_v20/audit_leakscan/modified_audit --free-leaks --trace /dev/nvme0n1 > leakscan_repair.txt 2>&1
```

收口（相位 C `reconcile_block`）每判定一块「位图亮但不可达」，就**就地清掉该块在盘上位图的分配位**
（read-modify-write 位图字节，`AUDIT_BLK0 + bitmap_start*4096 + blk/8`）。安全：泄漏块无任何元数据引用，
清位不会踩可达数据，只是把空间还给分配器。

输出新增：
```
[TRACE][LEAKSCAN-REPAIR] blk=<blk> freed=1
[AUDIT][SUM][LEAKREPAIR] freed=<n> failed=<m>
```
注意：本次运行的 `[AUDIT][SUM] leaked=` 仍是「**发现**」数（修复前判定），`exit` 也仍为 1；
修复效果要**再跑一遍**（`leaked=0`）或看 `LEAKREPAIR freed=`。写盘后 `fsync`。

## 提示

- 逐单元明细按**单元**去重（一对 `(偶数, 偶数+1)` 只打一行）；正常 `[AUDIT][E][BLOCK] LEAK`
  仍按**块**逐条、上限 64 条，两者不互相替代。
- 若某个漏块是单元的**第二块**而首块未漏（少见），仍按 `blk & ~1` 的首块头分类。
