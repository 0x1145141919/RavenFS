# RavenFS — 目录项 B+tree（Dentry B+tree）

> 2026-07-19
> 目录项专用 B+tree，与文件区段 B+tree（`Bptree_node_t`）使用不同的 entry 格式。
> 所有 inode 类型为 `RAVEN_DIR` 的数据内容由此树组织。

---

## 1. 节点布局

### 1.1 整体结构

```
Dentry_Bptree_node_t (8K = 8192B)
┌────────────────────────────────────────────┐
│ Dentry_head_t head                   128B  │
├────────────────────────────────────────────┤
│ Dentry_common_t entries[0]           128B  │
│ Dentry_common_t entries[1]           128B  │
│ ...                                        │
│ Dentry_common_t entries[62]          128B  │
├────────────────────────────────────────────┤
│ ← 63 × 128 = 8064B                         │
└────────────────────────────────────────────┘
总尺寸 = 128 + 8064 = 8192B = 8K ✅
```

最大 entry 数：**63**（根节点最少 1 个 entry，非根节点分裂时 ≥ 31）。

---

### 1.2 Dentry_head_t（128B）

```c
struct Dentry_head_t {
    dentry_node_meta_t  head;        // 8B: 节点元数据
    uint64_t            ptr1;        // 8B: 内部→最左子树 | 叶子→前驱
    uint64_t            ptr2;        // 8B: 内部→空闲     | 叶子→后继
    uint64_t            reserved0;   // 8B: 预留
    uint64_t            magic1;      // 8B: 魔数签名
    uint64_t            magic2;      // 8B: 魔数签名
    uint64_t            magic3;      // 8B: 魔数签名
    uint64_t            magic4;      // 8B: 魔数签名
    uint64_t            reserved2[8]; // 64B: 扩展预留
};
```

#### dentry_node_meta_t（8B）

```c
union dentry_node_meta_t {
    struct {
        uint64_t entry_count    : 6;  // 有效 entry 数 - 1（最大 63）
        uint64_t reserved0      : 2;
        uint64_t is_internal_node : 1; // 0=leaf, 1=internal
        uint64_t is_root_node   : 1;  // 1=根节点
        uint64_t reserved       : 2;
        uint64_t parent_ptr     : 52; // 父节点物理块号（0=根或无父）
    } fields;
    uint64_t raw;
};
```

#### 字段语义总表

| 字段 | 叶子节点 | 内部节点 |
|------|---------|---------|
| `head.entry_count` | 有效 entry 数 - 1 | 有效 entry 数 - 1 |
| `head.is_internal_node` | 0 | 1 |
| `head.is_root_node` | 0/1（根叶子=1） | 0/1（根内部=1） |
| `head.parent_ptr` | 非根→父节点块号<br>根→所属 inode 编号 | 非根→父节点块号<br>根→所属 inode 编号 |
| `ptr1` | **前驱 sibling 块号** | **最左子树 child_0 块号** |
| `ptr2` | **后继 sibling 块号** | **空闲** |
| `magic1-4` | 4 × 64-bit = 256-bit 扫描签名 | 同左 |

#### parent_ptr 双语义

`parent_ptr` 的值由 `head.is_root_node` 决定如何解释：

| `is_root_node` | parent_ptr 含义 | 示例 |
|:---:|:---|:---|
| 0 | **父节点物理块号**（向上遍历用） | `parent_ptr = 42` → 父节点在 block 42 |
| 1 | **所属 inode 编号**（恢复定位用） | `parent_ptr = 5` → 这个 B+tree 属于 inode #5 |

根目录（inode 0）的 dentry B+tree 根节点：`parent_ptr = 0`。0 既是「根目录的 inode 编号」，也兼容「无父节点」语义，无冲突。

灾难恢复扫描到任意根节点时，`is_root_node=1` + `parent_ptr=N` → 直接定位到 inode[N]，知道这棵树属于哪个文件/目录。

---

### 1.3 Dentry_common_t（128B）

```c
struct Dentry_common_t {
    uint64_t ptr;        // 8B: 叶子→inode编号 | 内部→child块号
    char     name[120];  // 120B: 文件名（字典序 key）
};
```

文件名字符串长度限制：119 字符 + '\0' 终止符。

---

## 2. 双解码视角

同一个 `Dentry_common_t` 按 `head.is_internal_node` 解码为不同语义。

### 2.1 叶子语义

```
entries[i].ptr   = target inode 编号
entries[i].name  = 文件名（排序 key）

ptr1 → 前驱叶子块号（同级链表左）
ptr2 → 后继叶子块号（同级链表右）
```

叶子节点内容：文件名字典序排列，相邻 entry 满足 `strcmp(name[i], name[i+1]) < 0`。

### 2.2 内部语义

```
                  ┌─── ptr1 ──→ child_0（最左子树）
entry[0].name  =  child_1 子树最小文件名
entry[0].ptr   →  child_1
entry[1].name  =  child_2 子树最小文件名  
entry[1].ptr   →  child_2
...
entry[n].name  =  child_{n+1} 子树最小文件名
entry[n].ptr   →  child_{n+1}
```

共 `entry_count + 1` 个子树（ptr1 算一个，加上每个 entry 的 ptr）。

#### 路由规则

```
target < entry[0].name                  →  ptr1（child_0）
entry[0].name ≤ target < entry[1].name  →  entry[0].ptr（child_1）
entry[1].name ≤ target < entry[2].name  →  entry[1].ptr（child_2）
...
entry[n].name ≤ target                  →  entry[n].ptr（child_{n+1}）
```

伪代码：

```cpp
uint64_t find_child(Dentry_Bptree_node_t *node, const char *target) {
    if (!node->head.is_internal_node)
        return 0; // 不该走到这里

    // 二分查找：第一个 name > target 时回退
    int lo = 0, hi = node->head.get_entry_count() - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(target, node->entries[mid].name);
        if (cmp < 0) {
            // target < entries[mid].name
            // 检查前一个 entry 或 ptr1
            if (mid == 0)
                return node->head.ptr1; // child_0
            hi = mid - 1;
        } else {
            // target ≥ entries[mid].name
            // 继续右半找更后面可能的匹配
            lo = mid + 1;
        }
    }
    // 退出时 lo = 第一个 target < name 的位置
    // 如果 lo > 0，走 entries[lo-1].ptr
    // 如果 lo == 0，走 ptr1
    if (lo == 0)
        return node->head.ptr1;
    return node->entries[lo - 1].ptr;
}
```

简化线性版：

```cpp
uint64_t find_child_linear(Dentry_Bptree_node_t *node, const char *target) {
    int n = node->head.get_entry_count();
    for (int i = 0; i < n; i++) {
        if (strcmp(target, node->entries[i].name) < 0) {
            // target < entries[i].name
            return (i == 0) ? node->head.ptr1 : node->entries[i-1].ptr;
        }
    }
    // target ≥ 所有 entry 的 name
    return node->entries[n-1].ptr;
}
```

---

## 3. 扫描签名

`magic1`–`magic4` 共 256-bit（K=4）固定魔数，用于全盘扫描时低误报识别 B+tree 节点。

假阳性分析详见 `Docs/scan_fp_analysis.md`，核心结论：

| K | 512GB NVMe 盘期望误报 |
|:---:|:---:|
| 4（256-bit） | $5.40 \times 10^{-70}$ |

---

## 4. 同级链表

叶子节点通过 `ptr1` / `ptr2` 连成双向链表：

```
leaf_A.ptr2 = B
leaf_B.ptr1 = A, leaf_B.ptr2 = C
leaf_C.ptr1 = B, leaf_C.ptr2 = 0 (tail)
```

范围遍历（如 `readdir`/`ls`）全部在叶子层沿链表前进，无需回溯父节点。

内部节点理论上也可使用同级链表（分裂时便利），但 v1 不强制。

---

## 5. 基础操作

### 5.1 查找

1. 从 root inode 开始，读 root 节点
2. 如为内部节点：`find_child()` → 读 child 节点 → 递归
3. 到达叶子后二分查找 `entries[]`，`strcmp(target, name) == 0` 命中，返回 `ptr`（inode）

### 5.2 插入（分裂）

插入新 dentry `(filename, inode)`：

1. 沿树下溯到对应叶子
2. 叶子未满（entry_count < 63）：插入保持字典序
3. 叶子已满：**分裂**
   - 右半 32 个 entry 分到新节点
   - 新节点最小 name 提升到父节点作为 routing key
   - 如果父节点也满 → 递归分裂
   - 根节点满 → 分裂创建新根，树增高一层

分裂后更新 sibling 链表。

### 5.3 删除（合并 / 借）

v1 策略：**先简单标记 tombstone，不主动合并。**（跑通为先。）

v2 必要时再实现标准的 B+tree 合并/借逻辑。

---

## 6. 与文件区段 B+tree 的对比

| 维度 | 文件区段 `Bptree_node_t` | Dentry `Dentry_Bptree_node_t` |
|:---|:---|:---|
| Entry 大小 | 32B | 128B |
| 每节点 entry 数 | 256 | 63 |
| Key 类型 | `uint64_t`（fblkbase / max_R） | `char[120]`（字典序） |
| 扫描签名 | 待定（建议 256-bit） | 256-bit（4 × magic） |
| 叶子 ptr | pblkbase | inode 编号 |
| 内部 ptr | sub_blog_num | child 块号 |
| 内部最左子树 | entries[0].words[2]（统一格式） | head.ptr1（独立字段） |
| 同级链表 | entry[1/2].words[3] | head.ptr1 / head.ptr2 |
| 排序 | fblk_cmp（区间序） | strcmp（字典序） |
| 查找 | ≤ 右端点 | ≥ 最小文件名 |

---

## 7. 文件类型与 Dentry 的关系

落盘的三种文件类型共用 Dentry B+tree 作为目录内容：

| 文件类型 | Dentry 的 inode 指向 | 备注 |
|:---|:---|:---|
| `RAVEN_REG` | 普通文件 inode | 文件数据走 `raven_Inode_t.data_root` |
| `RAVEN_DIR` | 子目录 inode | 子目录自己的 Dentry B+tree root |
| `RAVEN_SYMLNK` | 软链接 inode | target 路径在 `inline_data` 或扩展块 |

Dentry 本身不区分类型——类型信息在目标 inode 的 `flags` 中。

---

## 8. 不做什么

| 特性 | 原因 |
|:---|:---|
| Hash 索引 | 字典序直接做 key，无碰撞处理 |
| 变长 dentry | 固定 128B 对齐，性能可预测 |
| 在线压缩 | v1 不做 |
| 目录项缓存 | Phase 1 读盘天然缓存不走额外层 |
