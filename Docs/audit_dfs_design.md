# RavenFS — 可达性审计（`audit_dfs_root`）设计

> 2026-09-30（Raven 拟、Sparrow 口述确认）。**取代** `audit_inode_design.md`（旧
> 「枚举全 inode + 侧账本反推」模型，已退役）。
> 类型面 `include/ravenfs/ravenfs_audit.hpp`；实现 `juvenile_god/audit.cpp`；
> 测试 `tests/test_audit_dfs.cpp`。

---

## 0. 一句话

**从 root 先序 DFS 一路下钻 → 边走边重画两张影子图（块 / inode）→ 收口与真实位图
XOR → 分类「没有被保护 / 疑似泄露」。全部经 `FILE*`（默认 stdout）打印。**

---

## 1. 为什么换模型（旧 ctx 的病根）

旧 `audit_ctx_t` 又大又绕，根因：它用「枚举全部已分配 inode + 侧账本」反推全局
可达性，于是被迫物化 5 个跨 inode 平行数组：

| 旧字段 | 存在理由 | 新模型 |
|---|---|---|
| `claim_bits` | 认领账本 | → 影子图 A（块） |
| `inode_flag` | 每 inode 黄牌 | → 折叠进打印（撤） |
| `visited_nodes` | 树内环/共享 | → 影子图 B 的"已访问"位（撤） |
| `ref_count` | 对 link_count | → 幼年期无硬链接，撤（见 §6） |
| `parent_of`/`dotdot_of` | 对 dir `..` | → DFS 下钻时父天然已知（撤） |
| `heap_occ`/`cur_heaps`/`cur_dir` | 长名归属 | → 当递归参数传（撤） |

**可达性由递归本身携带**——这是整个简化的支点。

---

## 2. 两张影子图（边走边重画）

前提：mmap 后端（`is_device_direct=false`）——据 fd 映射窗口直接访问内存对账；
非 mmap 挂载 → `RC_ERR_STATE`。

```
real_block_bm / real_inode_bm  ← 分配器账本（1 = 已分配；经 block_test/inode_test）
shadow_blk    (1 bit / FS_block) ← 审计重画的"应该被占"
shadow_ino    (1 bit / inode)    ← 审计重画的"应该被占"
```

**置位规则（DFS 中唯一要记的表）：**

| 触达物 | shadow_blk | shadow_ino |
|---|---|---|
| dentry / extent 树节点（各 2 块） | ✅ | |
| direct 数据 run `[base, +ceil(size/4K))` | ✅ | |
| extent 记录 `[block_base, +length)` | ✅ | |
| 名字堆激活区（bitmap 面 + data 面整段） | ✅ | |
| xattr 块 | ✅ | |
| root inode + 每条 dentry 目标 inode | | ✅ |

`shadow_blk` 兼作「已认领」（二次认领检测）；`shadow_ino` 兼作「已访问」（环 /
共享 / 双引用检测，见 `multi_ref_ino`）。

---

## 3. 收口 XOR（核心输出）

设 `A = shadow`、`B = real bitmap`（均 1=在用），对称差：

```
A \ B  → 可达（在用）但分配器标空  = 【没有被保护】 ⚠️ 高危（会被再分配 → 踩踏）
B \ A  → 已分配但不可达            = 【疑似泄露】（空间捞不回：孤儿 inode / 块）
A ∩ B  → 正常在用     ~A ∩ ~B → 真空闲
```

- 块侧只对 `[0, inode_bm_base)` 对账；白名单 = 尾巴三段 `[inode_bm_base, total_blocks)`
  + backup SB 链（mkfs 常亮、无 owner）。
- 块侧按**字节**扫描（`shadow_blk[i] ^ block_bits[i]`）：干净盘零差异 → 极快；
  仅在差异字节处逐位分类。输出合并成**区间**（`[start, +len)`）。
- 老 `dangling_refs` = 「没有被保护的 inode」；老 `orphan_inodes` = 「疑似泄露的
  inode」——两个维度被统一进一次 XOR。

---

## 4. 接口 / 类型

```cpp
/* public（ravenfs_v1.1.hpp） */
result_code_t audit_dfs_root(audit_walk_report_t *rep = nullptr, FILE *out = stdout,
                             bool verbose = false);
/* verbose=false（默认）：只打 发现/HEAP/SUM —— 大盛盘（1.8T 实测 347k inode）
 *   逐 inode WALK 会刷 39MB 日志，故默认关；需 trace 时传 true（CLI `-v`）。 */

/* 类型（ravenfs_audit.hpp）：瘦身 = 两张影子图 + 白名单 + 路径栈 + 计数 */
struct audit_walk_ctx_t {   // 递归携带
    RavenFS_juvenile *fs; FILE *out;
    std::vector<uint8_t> shadow_blk, shadow_ino;   uint64_t blk_bits, ino_bits;
    uint64_t meta_lo, meta_hi;   std::vector<uint64_t> backup_blocks;
    std::vector<uint64_t> path_inum;  std::vector<std::string> path_name;
    int depth, max_depth;   bool verbose;   audit_walk_report_t rep;
};
struct audit_walk_report_t { /* btree/inode/heap/xattr_bad, multi_ref/claim,
    link/dotdot, unprotected_{blk,ino}, leaked_{blk,ino}; total_bad() 求和 */ };
struct audit_heap_region_t { /* 名字堆激活区（长名归属校验；每目录 transient）*/ };
struct audit_walk_tree_t   { /* leaves / leaf_depth / records_total（每树 transient）*/ };
```

打印规格：`[AUDIT][WALK] <path> inum=.. type=.. size=.. lc=..`、
`[AUDIT][BAD*]`、`[AUDIT][MULTI_REF|MULTI_CLAIM]`、`[AUDIT][UNPROT_BLK|LEAK_BLK]`
（区间）、`[AUDIT][UNPROT_INO|LEAK_INO]`、`[AUDIT][SUM]`。

---

## 5. DFS 骨架

```
visit_ino(inum, parent):
  ├─ 深度超限 / inum 越界        → BAD
  ├─ shadow_ino 已置位           → MULTI_REF（环/共享/双引用），return
  ├─ 置 shadow_ino；读 256B inode；print WALK
  ├─ DIR:  lc==size+2 / data_root；名字堆；dentry 树 DFS（认领节点+条目+长名归属）；
  │        先序下钻子目录（父=本目录 inum，校验子目录根 ptrs[1]=='..'==parent）
  ├─ REG/LNK: size 解码 inline/direct/extent（extent 则区段树 DFS）
  ├─ FIFO/CHR/BLK/SOCK: 不变量 26（size==0 ∧ data_root==PTR_INVALID）
  └─ xattr_ptr != 哨兵 → 认领块 + magic/refcount/blocks 校验
```

B+树递归核 `audit_walk_btree` 沿用旧模型的全部结构不变量检查（键序 / 欠载 /
等深 / 环 / hint key==子树 max / 环 / 叶链序 / hash 重算 / 长名归属 / 物理越界），
只是记账口从 `ctx.claim_bits/visited_nodes` 换到 `shadow_blk/shadow_ino`；
节点数据**先拷出**再递归（解耦会话生命周期，跨模式安全）。

---

## 6. 取舍 / 已知边界

1. **无硬链接假设**：1 bit 影子无法区分「合法硬链接」与「双认领」。幼年期无
   `link()` ⇒ 可达的 REG/LNK 期望 `link_count==1`，二次触达即 `multi_ref`。
   将来上硬链接 → 加回 ref 计数（inode_count×4B）或 inode 影子改 2 bit 饱和。
2. **环 vs 菱形**：1 bit 只报「二次触达」，不细分环/共享（幼年期够用）。
3. **owner 取证力**：1 bit 只知「撞了 + 撞在路径哪一段」，不溯源先占者（要溯源
   需 per-block owner 数组，回升内存）。
4. **非 mmap 后端**：直接拒（`RC_ERR_STATE`）——会话指针稳定性依赖 offload 直映。
5. **（已修 2026-09-30）根更迭携 '..'**：v1.2 root=16 下暴露的两处同源缺陷——
   ① `dentry_new_root_inner`（分裂建新根）② `dentry_root_shrink`（收缩提升孩子为根）
   均未携带旧根 `ptrs[1]`（'..'）⇒ 根目录/子目录根更迭后 '..' 丢失或错指自身
   （v1.1 root=0 时 ① 被掩盖；② 在子目录上一直错）。已修：叶分裂会把 root 叶
   `ptrs[1]` 改写为前驱，故 `split_result_t.old_root_ptrs1` 在分裂前捕获；收缩则
   从旧根 `ptrs[1]` 取父，两处均改“继承旧根 '..'”。

---

## 7. 与旧接口的处置

- **重写**：`audit_ctx_t` / `audit_opts_t` / `audit_report_t` / `audit_tree_ctx_t` /
  `audit_begin` / `audit_inode` / `audit_filesystem` / `audit_btree_node` /
  `audit_dir_inode` / `audit_file_inode` 全部退役（模型不兼容，留旧即双模型）。
- **保留旧方法名**：无（唯一入口 `audit_dfs_root`）。
- **消费者迁移**：`tests/audit_check.hpp`（`audit_clean`）、`tools/rfs_compact.cpp`、
  `tests/test_pwrite_big.cpp` 已迁；`tests/test_audit_inode.cpp` 退役 →
  `tests/test_audit_dfs.cpp`。

## 8. 实测（2026-09-30，64MB 镜像）

```
[PASS] 干净盘 total_bad==0
[PASS] 建内容后结构/XOR 全净（btree=0 heap=0 unprot=0 leak=0）
[PASS] 审计条目数 == 成功插入数 (323)
[PASS] dotdot<=1（root-split '..' 已知缺口，审计已捕获：1）
[PASS] 污染（翻 hash）→ btree_bad>=1；还原 → 归零
[PASS] 灭 root 叶块位图 → unprotected_blk>=1（没有被保护）
[PASS] 占空块不引用   → leaked_blk>=1（疑似泄露）
11 passed, 0 failed
```

**真盘（/dev/nvme0n1，1.8T）**：`total_blocks=488378645 inode_count=122094672`，
走 347489 inode / 65172 节点（230 内 + 64942 叶），`bad_total=0`（零 BAD/UNPROT/LEAK）。
日志默认安静（仅 START/HEAP/SUM），`-v` 出逐 inode WALK。
