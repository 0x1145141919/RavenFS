> ⚠️ **已退役（2026-09-30）**：本文档描述的「枚举全 inode + 侧账本反推」模型已由
> **可达性审计（`audit_dfs_root`）** 取代 —— 见 `Docs/audit_dfs_design.md`。
> 代码（`audit_ctx_t`/`audit_begin`/`audit_inode`/`audit_filesystem` 等）已移除。
> 保留本文仅作历史设计留档。

# RavenFS — `audit_inode` 静态合法性审计 设计基线

> 2026-09-10 定稿（设计方 Raven，据 Sparrow 五轮口径确认）。
> 角色：**内部测试/调试手段**（不对外、不修复、只读静态审阅）。
> 上游契约：`disk_layout_v1.1.md`（§3 三型 inode / §4 B+树节点 / §7 名字堆 / §10 不变量）、
> `block_model.md`（§3 几何 / §6 bitmap 自描述）、`disk_struct_v1.1.hpp`（结构真值）。
> 关联：`runtime_tech/01_mount_mode_contract.md`（readonly mmap 后端 = rfs-audit）。

---

## 0. 定位与形态（锁死的边界）

1. **进上帝类的模块**：审计不是外部类，是 `RavenFS_juvenile` 的一组成员方法
   （代码落 `juvenile_god/audit.cpp`）。直接复用背后的：
   - 几何字段（`total_blocks` / `bitmap_start` / `inode_array_base` / `inode_bm_base` …）
   - 双位图内存态（`block_test` / `inode_test`）
   - 块/节点 I/O 与缓存（`read_buff` / `inode_read` / `mdata_piece_session`）
2. **复用存储设施，不复用导航逻辑**：节点 I/O 走 `mdata_piece_session`（= 复用缓存），
   但 DFS / GLB / 路由校验**自己重新推导**，**绝不调** `dentry_Bptree_walk` /
   `extent_Bptree_walk` / `dir_lookup`。审计器与被审路径共因失效 = 白审
   （对齐 test 侧教训：audit 不信任接口互验）。
3. **`audit_inode` 粒度 = 单 inode**：只查该 inode 的 256B 字段 + 它自己拥有的那棵 B+树。
   - **坚决不下钻子目录 inode**（子目录由驱动枚举各自审）。
   - dir 的"递归"仅指**其 dentry B+tree 的内部 DFS**（树内递归），非跨 inode 下钻。
4. **跨 inode 对账**（引用计数 / 孤儿 / `..`）由驱动枚举 + 收口完成，不在 `audit_inode` 内。
5. **只读不修复**：审计只产标志位 + 计数 + 日志；修复是独立阶段（当前不做）。
6. **重点 = B+树数据结构一致性**；其余维度（字段/堆/对账）为辅助，粒度收敛。

---

## 1. 接口

```cpp
/* ── 单 inode 审计（对外主入口；ctx 累积账本 + 计数 + 标志位） ──
 * 返回 RC_OK 只表示"这一路遍历完成"，不代表无异常；
 * 异常靠 ctx 计数 + inode 标志位 + 日志体现。
 * 遍历性失败（IO 错）返回 RC_ERR_IO / RC_ERR_STATE。 */
result_code_t audit_inode(uint64_t inum, audit_ctx_t &ctx);

/* ── 驱动：全盘审计 ──
 * 1. seed ctx（几何白名单 + backup 块）
 * 2. for inum in [0, inode_count): if inode_test(inum) → audit_inode(inum, ctx)
 * 3. 收口对账（claims vs bitmap / ref vs link / 可达性 / .. ）
 * 4. 汇总日志 */
result_code_t audit_filesystem(const audit_opts_t &opts, audit_report_t &rep);
```

---

## 2. `audit_ctx_t` — 账本 + 计数 + 标志位

设计取舍：**账本用位图/数组，不用 map**——因为对账粒度只到计数，
double-claim 只要一 bit 就能判，无需逐块明细。

```cpp
struct audit_opts_t {
    uint16_t max_tree_depth  = 64;   // 树递归深度上限（belt & suspenders）
    bool     trace_nodes     = false;// true: 每进一个节点发一条 [AUDIT][TRACE]
    bool     check_name_heap = true; // 名字堆描述符 + 占用率
    bool     check_bitmap    = true; // 收口跑 claims↔bitmap 对账
};

struct audit_ctx_t {
    RavenFS_juvenile *fs;

    /* ── 认领账本：与 block bitmap 同尺寸（1 bit/块）──
     * claim 时置位；重入 → double_claim++（红牌）。 */
    std::vector<uint8_t> claim_bits;

    /* ── 引用账本：per-inode（对账用，非"标志位"）── */
    std::vector<uint32_t> ref_count;   // dentry 扫描累加（对 REG/SYM link_count）
    std::vector<uint64_t> parent_of;   // 首次被哪目录引用（对 dir 的 ..）

    /* ── 遍历状态 ── */
    std::unordered_set<uint64_t> visited_nodes;   // 树内环/共享子检测

    /* ── per-inode 标志位：一个 inode 只有一个标志位 ──
     * 1 bit/inode；置 1 = 黄牌（该 inode 有异常，明细走日志）。
     * 红牌类（B+树结构硬错）不单占位，计入全局计数 btree_bad + 日志。 */
    std::vector<uint8_t> inode_flag;   // 位图（1 bit/inode）

    /* ── 全局计数（收口对账项）── */
    uint64_t btree_bad;         // B+树结构不变量违例总数（重点）
    uint64_t inode_bad;         // inode 字段硬矛盾（type 非法 / 解码矛盾）
    uint64_t double_claim;      // 物理块被两 owner 认领
    uint64_t leaked_blocks;     // 认领但 bitmap 暗
    uint64_t orphan_blocks;     // bitmap 亮但无人认领（剔结构白名单）
    uint64_t dangling_refs;     // dentry → 未分配 inode
    uint64_t orphan_inodes;     // 已分配但不可达（非 root）
    uint64_t link_mismatch;     // link_count 对不上
    uint64_t dotdot_mismatch;   // dir 根 .. 对不上 parent_of
    uint64_t heap_bad;          // 名字堆描述符/归属/占用率异常

    /* ── 白名单（structural 常亮块，排除出 orphan_blocks）── */
    uint64_t meta_tail_lo, meta_tail_hi;    // [inode_bm_base, total_blocks)
    std::vector<uint64_t> backup_blocks;    // SB 链表遍历

    /* ── 日志 ── */
    audit_log_t *log;
    const audit_opts_t *opts;

    /* ── 名字堆占用率输出（要报）── */
    struct heap_occ_t { uint64_t dir_inum; uint8_t slot; uint64_t used, total; };
    std::vector<heap_occ_t> heap_occ;
};
```

**内存成本**（可接受）：`claim_bits` ≈ total_blocks/8（512GB 盘 125M 块 → ~15.6MB）；
`ref_count` ≈ inode_count×4；`parent_of` ≈ inode_count×8；`inode_flag` ≈ inode_count/8。

---

## 3. `audit_inode` 单 inode 分派

读 256B 槽（`raven_Inode_t` 载体，dir 时按 `raven_dir_Inode_t` 重解），按 `type:2` 分派：

| type | 审计内容 |
|------|----------|
| **DIR (1)** | 字段一致性（`data_root` 有效 + 8K 对齐；`link_count==size+2`；`size==0⇔lc==2`）<br>名字堆 10 描述符 + 占用率<br>**dentry B+tree DFS**（§4）；每条 entry：`ref_count[child]++`、`inode_test(child)` 查悬挂、长名指针归属校验 |
| **REG (0) / SOFT_SYM (2)** | `size ≤ 160` → **inline**（**只依 size，`direct_bit` 显式忽视**；`data_root` 残留 ≠PTR_INVALID → **黄牌**）<br>`size > 160 ∧ direct_bit` → **direct**：`data_root` 连续区 `[base, base+ceil(size/4096))` 认领 + 落界<br>`size > 160 ∧ !direct_bit` → **extent**：**区段 B+tree DFS**（§4） |
| **type == 3** | `inode_bad++`（非法 type，硬错） |

> `data_root` 残留按黄牌：置该 inode 标志位，不 hard bad（不变量 23 的 fsck 清理另说）。

---

## 4. ★ B+树数据结构一致性检查（重点）

统一 `audit_btree(ctx, root_blk, tree_kind, owner_inum, expect_parent, depth, size_bound)`，
两域（dentry / extent）仅在"键序 + 路由"特化。**每项失败打详细日志 + 计数，
然后只剪该枝返回**（不中断全局）。

### 4.1 节点同一性 / 角色
1. `blk ∈ [0, total_blocks)` 且 `blk % 2 == 0`（8K 节点首块，2 对齐）
2. `visited_nodes.insert(blk)` 重入 = **环 / 共享子节点** → log + 剪枝
3. magic == 对应域签名（`is_dentry_node` / `is_extent_node`）
4. `parent_ptr` 四态：`is_root_node == expect_root`；root → `ptr == owner_inum`；
   非根 → `ptr == expect_parent_blk`
5. `is_inner_node` 与父 hint 期望一致
6. `entry_count`：
   - **恒 ≤ 阶**（126/337）——`[阶]` 是内存瞬时槽，落盘即坏
   - 非根：`≥ mina`（dentry 63 / extent 168）且 `≠ 0`
   - 根 inner：`≥ 2`（`== 1` = 该收缩未收缩的坏态）
   - 根叶：dentry 允许 `0`（空目录）；extent 要求 `≥ 1`（空树应回 inline）

### 4.2 叶子
7. **dentry**：`(hash, name)` **严格递增**；`name_len ≤ 552`；
   `name_len>40 ⇔ full_char_remain_ptr 有效`（≤40 必须 PTR_INVALID）；
   **重算 `XXH64(name) == hash`**（跨挂载稳定，抓落盘 hash 与名字不一致）
8. **extent**：相邻 `fblkbase_i + length_i ≤ fblkbase_{i+1}`（半开，硬红线=重叠）；
   `length ≥ 1`；`block_base` 落界；`fblkbase+length ≤ ceil(size_bound/4096)`（不越 size 块域）

### 4.3 内部
9. `hints[i].hash_max`（dentry）/ `key`（extent）**严格递增**
10. **递归回传的子树 max == 本 hint 的 hash_max/key**（上界路由核心一致性）
11. `sub_ptr` 合法（∈[0,total)、2 对齐、≠PTR_INVALID）

### 4.4 跨节点
12. 叶链：非根叶 `ptrs[1]`=前驱 / `ptrs[2]`=后继；前后互指自洽；
    **链表序 == DFS 叶序**（树级收尾做一次，同 test `audit_chain`）
13. **所有叶子同深度**（B+树平衡；DFS 收一路叶 depth 集合）
14. 目录根节点 `ptrs[1]`（`..`）语义合法性（根=0/自身；非根值域合法，实际父对账留收口）

### 4.5 认领落账
每节点认领 `[blk, blk+1]`；extent 每条记录认领物理 `[block_base, +length)`。

---

## 5. 日志规格（给内部调试，尽可能详细）

结构化、可 grep、带全上下文 + 十六进制：

```
[AUDIT][BAD][BTREE] tree=extent inum=7 root=0x384 blk=0x384 depth=0 role=inner cnt=3
  check=hint_key_mismatch idx=2 hint.key=0x1F40 subtree_max=0x1F3F
[AUDIT][BAD][BTREE] tree=dentry inum=0 root=0x0 blk=0x0A depth=1 role=leaf cnt=64
  check=hash_mismatch idx=13 name_len=3 stored=0xDEAD recalc=0xBEEF
[AUDIT][WARN][INODE] inum=12 size=100 data_root=0x2A (inline 残留，黄牌)
```

- 一条违规 = 一条 `[AUDIT][BAD|WARN]` 行；`trace_nodes` 开启时每进节点发 `[AUDIT][TRACE]`。
- 收尾汇总：`nodes/inner/leaves/records` + 每类计数 + 名字堆占用率表。
- sink = 函数指针（默认 stderr），测试可接文件/缓冲区：

```cpp
struct audit_log_t {
    void (*emit)(void *ud, const char *line);
    void  *ud;               // 默认：fputs → stderr
};
```

---

## 6. 名字堆占用率（要报）

每激活堆一行：`dir_inum`、`slot`、`log2_gran`、`log2_bits`、位图面/数据面块区间、
`used_bits/total_bits`（占用率）；并与该目录长名 entry 数交叉核对。
描述符自洽：未激活 = 双指针 52 位全 1 且 log2==63；激活 = log2 合法
（gran∈{3,6,7,8,9}，bits∈{16,17,18}）+ 位图面/数据面各自连续 + 8K 对齐 + bitmap 亮 + 认领。
长名 entry：`full_char_remain_ptr` 必须落在**本目录**某激活堆数据面内 + 对应 bit 已置。

---

## 7. 收口对账（只到计数）

1. `claim_bits` vs `block_test`：claim 未亮=`leaked_blocks`；亮未 claim（剔
   `[meta_tail]`+`backup_blocks`）=`orphan_blocks`；重入=`double_claim`
2. `ref_count[inum]` vs `link_count`（DIR: `size+2`；REG/SYM: ref）→ `link_mismatch`
3. 可达性：`inode_test` ∧ `ref_count==0` ∧ ≠root → `orphan_inodes`；
   dentry→`!inode_test` → `dangling_refs`
4. `..` 对账：dir 根 `ptrs[1]` vs `parent_of` → `dotdot_mismatch`

---

## 8. 与现有 test 侧 audit helper 的关系

`tests/test_extent_fusion.cpp`（`audit_node`/`audit_chain`/`audit_run`）、
`test_pwrite_big.cpp`、`test_extent_loose_stress.cpp` 里的 audit 是**原型**：
- 从 test 侧 hack（`#define private public` + 直摸 `mdata_piece_session`）→
  类内正式入口（复用位图/缓存）；
- 从"只管 extent 树结构"→ 补三型分派 / 名字堆 / 对账 / 分级日志；
- 结构义务沿用（重叠红线 / 欠载 / 父链 key / 兄弟链序），新增：环/等深/hash 重算/.. 对账。

---

## 9. 集成

- 类型头：`include/ravenfs/ravenfs_audit.hpp`（`audit_opts_t` / `audit_log_t` /
  `audit_report_t` / `audit_ctx_t` / `audit_tree_ctx_t` / `audit_heap_occ_t` /
  `audit_heap_region_t`）。单独成头是为避开 `#define private public` 测试 TU 的
  `<chrono>`/`<sstream>` 可访问性雷（gcc16）；`ravenfs_v1.1.hpp` 只前置声明。
- 实现：`juvenile_god/audit.cpp`，加入 `juvenile_god/CMakeLists.txt` 的 `add_library`。
- 入口（`ravenfs_v1.1.hpp` public）：`audit_begin` / `audit_inode` / `audit_filesystem`。
- 测试：`tests/test_audit_inode.cpp`（干净盘 / 建内容 / 单 inode / 人工污染命中 → 9/0）；
  `tests/audit_check.hpp` 提供 `audit_clean(fs, tag)` 横切封装。
- 已融入的既有测试（末尾追加 `audit_clean` 调用）：insert / delete / extent_walk /
  extent_unmap_loose / extent_ops / extent_fusion / name_heap / pwrite_smoke /
  pread_smoke / punch_truncate_smoke / pwrite_phase1 / pwrite_big /
  extent_loose_stress / name_heap_stress。
- 运行模式：`is_readonly=true` + `is_device_direct=false`（rfs-audit 静态 mmap 只读）
  或普通 mmap 挂载均可；真盘 `/dev/nvme1n1` 已验（1.8T / inode-count 262144）。

### 9.1 实测结论（2026-09-10）

- 结构性维度（btree / dangling / double_claim / link / dotdot / heap）在全部受测场景**零违例**。
- 唯一非零计数 = `orphan_blocks`：出现在**手工造树 / 故意榨干空间 / 占拦路虎块**的测试
  （extent_walk/unmap_loose/ops/fusion/loose_stress/pwrite_big/phase1）——脚手架噪声，非 FS 缺陷。
- 真盘规模：audit 9/0；pwrite_big(2GB/3e4) 15/0（B2 树审计改用 `audit_begin`+`audit_inode`，
  不再自抄 `audit_t/audit_node`；终态 `audit_filesystem` 全净）；name_heap_stress 20/0；
  extent_loose_stress 43/4（S4 “抢块榨干→ENOSPC” 尺度假设，小盘真盘均如此，与审计无关）。

---

## 10. 已决 / 未决

**已决**：进上帝类模块；复用位图+缓存；对账只到计数；B+树错误详细日志；
inline 忽视 `direct_bit`；`data_root` 残留=黄牌；名字堆占用率要报；重点 B+树一致性；
`audit_inode` 不下钻子目录。

**未决**：
1. dentry 非根欠载阈值最终值（暂挂 63，与 `dentry_node_balance` 合并阈值挂钩）；
2. 是否审计 backup SB 链表自身（当前只取其块做白名单）；
3. `size==0` 空目录根叶 `entry_count==0` 的合法性（当前判合法）。

---

> **一句话记**
>
> `audit_inode(inum, ctx)` = 单 inode 静态审计核（dir 只 DFS 自己的 dentry 树，**不下钻**）；
> ctx 是跨 inode 共享的位图账本 + 计数 + 每 inode 一 bit 标志位；
> 重点在 B+树结构一致性（键序/路由/欠载/等深/环/兄弟链），错误详细打日志，只读不修复。
