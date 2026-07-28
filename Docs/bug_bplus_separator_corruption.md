# Bug: B+tree internal node routing key (separator) corruption

> 排查记录 — 2026-07-28
>
> 设计方：PS　　执行方：Raven

---

## 一、现象

### 1.1 乱序删除 flaky not-found

`stress_delete` 乱序模式（seed=42）下 65536 个目录删除有 5 个 `not found`:

```
RMDIR FAIL [15979/65536]: /stress_test/dir_16909 returned -2
RMDIR FAIL [16640/65536]: /stress_test/dir_16894 returned -2
RMDIR FAIL [17151/65536]: /stress_test/dir_25283 returned -2
RMDIR FAIL [17302/65536]: /stress_test/dir_25292 returned -2
RMDIR FAIL [17579/65536]: /stress_test/dir_16905 returned -2
```

有序删除 (sequential) 全部通过。

### 1.2 确定性复现条件

```
UUID=11111111-2222-3333-4444-555555555555
seed=42
device=/dev/nvme1n1 (PS:PS 权限，无需 sudo)
```

---

## 二、工具链与环境

### 2.1 硬件

| 项目 | 值 |
|---|---|
| 设备 | `/dev/nvme1n1` |
| 类型 | NVMe |
| 容量 | 2TB (2000398934016 bytes) |
| LBA size | 4096 |
| 属主 | `PS:PS` |
| 实测命令 | `ls -la /dev/nvme1n1` 确认 |

### 2.2 测试流程

```
Phase 0: mkfs (固定 UUID)
Phase 1: stress_mkdir_rev (乱序创建 65536 个 dir_00000 … dir_65535, seed=42)
Phase 2: stress_delete (乱序删除, seed=42)
Phase 3: 复盘重建 + replay_delete / replay_insert
```

### 2.3 可执行文件

| 路径 | 用途 |
|---|---|
| `build/mkfs/mkfs.ravenfs` | 格式化 |
| `build/tests/ravenfs_stress_rev` | 乱序创建 (支持 --order-file / --until / --validate) |
| `build/tests/ravenfs_stress_delete` | 乱序删除 (支持 --order-file / --seed / --sequential) |
| `build/tests/ravenfs_replay_delete` | 删除顺序回放 (支持 --probe / --dump-dir) |
| `build/tests/ravenfs_replay_insert` | 插入顺序回放 (支持 --probe / --until / --dump-dir / --validate) |
| `scripts/stress_pipeline.sh` | 全自动管线 |

### 2.4 管线命令

```bash
# 完整一轮
scripts/stress_pipeline.sh /dev/nvme1n1

# 复盘 (重建 + 回放 order file)
scripts/stress_pipeline.sh --replay delete_order.txt /dev/nvme1n1

# 手动分步
build/mkfs/mkfs.ravenfs --uuid=11111111-2222-3333-4444-555555555555 -f /dev/nvme1n1
build/tests/ravenfs_stress_rev /dev/nvme1n1 --order-file insert_order.txt
build/tests/ravenfs_stress_delete /dev/nvme1n1
build/tests/ravenfs_replay_delete /dev/nvme1n1 delete_order.txt --probe 15979 --dump-dir dumps
build/tests/ravenfs_replay_insert /dev/nvme1n1 insert_order.txt --until 2924 --validate
```

---

## 三、Bug 分析

### 3.1 直接原因：routing key invariant violation

B+tree 内部节点的 routing key（separator）违反不变量。

**规则**：对于内部节点 `entries[0].name` 必须是 `ptr1` 子树的最大 key 的**上界**。
即：`ptr1 子树中所有 key < entries[0].name`

**被违反的证据**（validate 输出）：

```
ERROR: block 321400428: routing key entries[0].name="dir_16538" <= ptr1 max="dir_16921"
```

这意味着 `ptr1` 子树中的 key 比 `entries[0].name` 大，查找 `dir_16909` 时遍历走向 `entries[0]` 的叶子（找不到），而不是 `ptr1` 的叶子（真正有数据的地方）。

### 3.2 根因（两个层级）

#### Bug A: split_internal 全排序破坏 ptr1 不变量（已修复）

`split_internal()` 中将 64 个 entry 全量按 `strcmp` 排序，然后平分。但 **`ptr1` 指针不变**。排序后 `entries[0].name` 可能变为一个比原 `entries[0].name` 小得多的值，而 `ptr1` 指向的子树 key 范围没有跟着变——导致 `ptr1.max_key >= entries[0].name`。

**修复**：`split_internal()` 不再全排序，改为在原有序 entries 中插入新 separator 后从中点 (index 32) 分裂。

#### Bug B: cascade 中 path[0].node.entry_count 过时（未修复）

这是目前剩下的问题。`dentry_insert()` 在下溯时把 root 节点读到 `path[0].node`。当 cascade 回到 root 层时，检查 `path[0].node.entry_count` 判断 root 是否已满(>=63)。问题在于：**root 在 cascade 回来的途中可能已被其他 insert 填满**（多线程？不对，这是单线程），但 `path[0].node` 存的是下溯时的 snapshot，entry_count 没跟上实际值。

结果：root 实际 ec=63(满)，`path[0].node` 显示 ec=62(未满) → 试图插入第 64 个 separator → **buffer overflow**（`entries[64]` 越界写）→ 内存/磁盘数据损坏 → `split_internal` 在后续 insert 中拿到 `old_count=64`（本应为 63），无法正确分裂。

**为什么 path[0].node 过时？** 需要进一步排查。可能路径：
- `dentry_insert` 通过 `dinode.data_root` 拿到 root block → 下溯完成 → 在分裂叶子、上溯过程中，root 的磁盘数据和 cache 被前一个 insert 改动 → 当前 insert 的 `path[0].node` 未刷新
- 也可能是 `read_btnode` 与 block cache 的交互导致读到旧版本

### 3.3 衍生 Bug：stale separator 未随 leaf 首项删除更新（已修复）

`dentry_remove()` 在叶子中删除一条记录后，如果删除位置是 pos=0（首项），**parent 中指向该叶子的 separator 未更新**。后续查找 key 在 (旧 separator, 新首项) 范围内时会走到错误叶子。

**修复**：在 `dentry_remove()` 尾部追加逻辑：如果 pos==0 && live>0，更新 parent 中本 leaf 的 separator 为新的 `leaf->entries[0].name`。

### 3.4 定位过程

| 步骤 | 方法 | 结论 |
|---|---|---|
| 1 | stress_delete 跑出 5 个 not-found | 确认乱序删除有 bug |
| 2 | `dump_dentry_tree()` 全量 dump seq-15979 | 磁盘 B+tree 中 `dir_16909` 存在 |
| 3 | 发现 `entries[0].name="dir_16536"` 但 ptr1 叶子有 key `dir_16921` | routing key 违反不变量 |
| 4 | `validate_subtree()` 增加 routing key 检查 | 抓到 2 个损坏的内部节点 |
| 5 | 二分 `replay_insert --until N --validate` | 首次违反在第 **2924** 次插入 (`dir_08828`) |
| 6 | 抓 create_new_root 诊断 | root ec=63 应已减为 32 但未减 |
| 7 | 静态审核 `split_internal` | 发现全排序破坏 ptr1 不变量（已修）|
| 8 | 发现 `path[0].node.entry_count` 过时 | cascade 流程中读到了 stale snapshot（未修复）|

---

## 四、回放记录文件

| 文件 | 内容 | 生成方式 |
|---|---|---|
| `insert_order.txt` | 65536 行，每行一个 `/stress_test/dir_XXXXX` | `stress_mkdir_rev --order-file insert_order.txt` |
| `delete_order.txt` | 65536 行，乱序删除顺序 | `stress_delete` 默认生成 |
| `dentry_dumps/dump_015979.txt` | seq 15979 时的全量 B+tree dump | `replay_delete --probe 15979` |
| `insert_dumps/dump_002923.txt` | 2923 次插入后的全量 dump | `replay_insert --probe 2923` |
| `insert_dumps/dump_002924.txt` | 2924 次插入后的全量 dump（损坏后） | `replay_insert --probe 2924` |

---

## 五、新会话上手步骤

```bash
# 0. 确认环境
ls -la /dev/nvme1n1        # 应为 PS:PS
ls build/tests/ravenfs_*   # 确认可执行文件存在

# 1. 复现删除 bug
scripts/stress_pipeline.sh /dev/nvme1n1

# 2. 验证 routing key 损坏
build/mkfs/mkfs.ravenfs --uuid=11111111-2222-3333-4444-555555555555 -f /dev/nvme1n1
build/tests/ravenfs_stress_rev /dev/nvme1n1 --order-file insert_order.txt
build/tests/ravenfs_replay_insert /dev/nvme1n1 insert_order.txt --until 2924 --validate
# → 应输出: validate FAIL, 含 routing key ERROR

# 3. 确认 2923 次时完好
build/mkfs/mkfs.ravenfs ... -f /dev/nvme1n1
build/tests/ravenfs_stress_rev /dev/nvme1n1 --order-file insert_order.txt
build/tests/ravenfs_replay_insert /dev/nvme1n1 insert_order.txt --until 2923 --validate
# → 应输出: validate PASS (0 errors)

# 4. 查看损坏时的 dump
head -10 insert_dumps/dump_002924.txt   # root 已有新 root block
grep "routing key\|entries\[0\]" insert_dumps/dump_002924.txt
```

---

## 六、关键代码位置

| 文件 | 行 | 内容 |
|---|---|---|
| `Userspace_God/dentry.cpp` | 107-163 | `split_internal` — 已修复（去排序） |
| `Userspace_God/dentry.cpp` | 838-849 | `dentry_remove` — 已修复（首项删除更新 separator） |
| `Userspace_God/dentry.cpp` | 390-464 | `dentry_insert` cascade 循环 — **bug B 所在地** |
| `Userspace_God/dentry.cpp` | 348-388 | `create_new_root` lambda |
| `Userspace_God/validate_dentry.cpp` | 13-100 | `dump_dentry_tree` + `dump_subtree` |
| `Userspace_God/validate_dentry.cpp` | 178-192 | routing key 不变量检查 |
| `Userspace_God/dentry.cpp` | 27-35 | `dentry_node_find` — 线性查找 |
| `Userspace_God/dentry.cpp` | 469-476 | `dentry_find_child_index` |

---

## 七、未解决问题 (Bug B)

**现象**：`create_new_root` 时 `old_root_fblock` 的 entry_count 为 63（应已由 `split_internal` 减为 32）。

**怀疑方向**：`dentry_insert` 下溯时 `path[0].node` 读取了 root 的 snapshot，但 cascade 回到 depth=0 时 `parent->entry_count` 仍为 snapshot 值。若 root 实际已满（ec=63），snapshot 仍显示 ec<N，导致判断为"未满"而插入第 64 个 entry，触发 buffer overflow。

**建议入手点**：
1. 在 cascade 循环中 depth=0 时，比较 `path[0].node.entry_count` 与磁盘实时 `read_btnode` 的值是否一致
2. 检查 `read_btnode` → `read_block` → block cache 是否会返回旧版本数据
3. 检查是否有其他 insert 在 `dentry_insert` 下溯与上溯之间修改了 root

---

## 八、附：现有修改清单

```
M  Userspace_God/CMakeLists.txt          (VFS cache 集成)
M  Userspace_God/bitmap_cache_controller.cpp
M  Userspace_God/dentry.cpp              (split_internal 修复 + stale separator 修复 + 断言)
M  Userspace_God/dir.cpp                 (VFS cache rmdir/mkdirmkdir)
M  Userspace_God/inode_bitmap_controller.cpp
M  Userspace_God/mount.cpp               (VFS cache 初始化)
M  Userspace_God/path.cpp                (PathCursor 使用 dentry_cache_lookup)
M  Userspace_God/validate_dentry.cpp     (dump_dentry_tree + routing key 检查)
M  include/ravenfs/ravenfs.hpp           (新函数声明)
M  include/ravenfsd/bridge_protocol.hpp
M  tests/CMakeLists.txt                  (replay 目标)
M  tests/stress_delete.cpp               (乱序 + 参数化)
M  tests/stress_mkdir_rev.cpp            (--order-file / --until / --validate)
M  tests/test_common.hpp
M  tests/test_main.cpp
A  tests/replay_delete.cpp               (删除回放)
A  tests/replay_insert.cpp               (插入回放)
A  scripts/stress_pipeline.sh            (管线)
?  Docs/bug_bplus_separator_corruption.md (本文档)
```
