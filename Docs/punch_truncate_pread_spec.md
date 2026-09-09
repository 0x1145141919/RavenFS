# RavenFS — punch_hole / truncate / pread 规格（Linux VFS 对照定案 · 2026-09-09 起草）

> 状态：**2026-09-09 拍板定案**（P1✓P2✓P3✓P4✓P6✓P9✓P10✓；P5/P7/P8 未拍
> 待定，见 §9）。pwrite 已过测（e97d318），direct_file 系列剩 3 接口。
> 本文定**契约与语义**（Linux VFS 对齐），
> 机械实现依据不重复：punch/truncate 复用 `pwrite_pipeline.md`（五段管线 +
> §10 复用语义），本文补齐 Linux 对照、边界裁决、形态×操作矩阵、失败/崩溃
> 纪律与 pread 完整规格（pwrite_pipeline 不含读路径）。
>
> Linux ground truth 来源：本机权威内核源码 `linux-7.2.3`（fs/open.c /
> fs/read_write.c / mm/filemap.c / mm/truncate.c / fs/ext4/inode.c /
> fs/ext4/extents.c / fs/ext4/ext4.h）+ **本机 ext4 实测**（同一内核，
> /dev/nvme0n1p8，§2.3）。ext4 是 RavenFS 最近参照系（块文件系统 +
> extent 树 + 内联数据），非对齐 punch 行为以实测为准。

## 1. Linux VFS 语义 ground truth

### 1.1 pread（vfs_read + filemap_read）

| # | 语义点 | 出处 | RavenFS 推论 |
|---|--------|------|-------------|
| R1 | count==0 → 返回 0（不发 IO） | filemap.c:2790 `!iov_iter_count` 早退 | len==0 → RC_OK，*out_bytes=0 |
| R2 | pos ≥ i_size → 返回 0（**不是错误**） | filemap.c:2796 循环条件 `pos>=i_size → break`；ext4 实测：pread @20000(len>EOF) → 0 | offset ≥ size → RC_OK + 0 字节 |
| R3 | EOF 截断短读：返回 min(count, size-pos)；短读是**正常结果**不是错误 | filemap.c:2803 `end_offset=min(isize, pos+count)`；实测 pread @EOF-8 len64 → 8 | 必须向调用方回报实际字节数（P1：出参） |
| R4 | 读不越 size：i_size 之后的字节永不拷给用户 | filemap.c:2803 注释（zero-filled 部分不拷） | clamp 到 size；盘上 [size, 块界) 残留不泄漏 |
| R5 | 空洞读 0；无分配、无脏、不扩文件、不碰映射 | 页缓存洞语义 | 洞 → 直接填 0，零副作用 |
| R6 | pos+count 溢出 → EINVAL/EFBIG | read_write.c:465 rw_verify_area | offset+len 溢出 → RC_ERR_STATE（同 pwrite 防御） |
| R7 | 读是"位置无关"（不推进文件偏移，无偏移概念参与） | pread 语义 | 接口本就 (name,offset,len) 无 fd 偏移 |

### 1.2 ftruncate/truncate（do_ftruncate + ext4_setattr + ext4_truncate）

| # | 语义点 | 出处 | RavenFS 推论 |
|---|--------|------|-------------|
| T1 | 受理 = REGULAR only；dir → EISDIR；无写打开 → EINVAL | open.c:169 `!S_ISREG \|\| !FMODE_WRITE → EINVAL` | v1.1 无打开态（§3.1 权限简化）；dir → 拒（P3 错误码） |
| T2 | **缩小**：i_size 先写（提交点）→ 之后才释放块；崩溃窗口 = 孤儿块（journal orphan 恢复），**无悬挂** | ext4_setattr inode.c:6090 区（i_size_write 于 ext4_truncate 前）；ext4_truncate 注释 4602「orphan list...resume truncate」 | 顺序纪律：先 inode 提交（size 收口+形态翻转）→ 后释放物理块/树资产；崩溃 = 孤儿可回收 |
| T3 | 缩小释放域 = [round_up(newsize), ∞)；新 EOF 所在残块**保留**、其 [newsize, 块上界) **清零**（stale 防护） | ext4_truncate inode.c:4631 `ext4_block_zero_eof(i_size, LLONG_MAX)`；ext4_ext_truncate（extents.c）从 round_up(i_size) 起删 | 尾洞整块域释放 + EOF 残块清零到块上界（**不是**只到 newsize） |
| T4 | 文件已 inline：inline 内截断，不转块形态 | ext4_truncate inode.c:4614 `ext4_inline_data_truncate` | inline 态缩小 = 纯收 size（inline_data 尾随槽，无 stale 块问题） |
| T5 | **增大**：纯 i_size 推进，**零分配**（稀疏）；旧 EOF 残块 [oldsize, 块上界) 清零（stale 防泄漏，注释「exposing stale on-disk data」）；mtime/ctime | ext4_setattr inode.c:6071-6079（!shrink 分支 + ext4_block_zero_eof） | 增大无物理需求 → v1.1 决策空间见 P4 |
| T6 | inline 数据增大跨内联容量 → 必须物化（ext4: ext4_convert_inline_data） | ext4_setattr inode.c:6030 | RavenFS 判据同构（size 跨 160 须迁出），见 P4 |
| T7 | 缩到同值：无语义变化（ext4 仍跑 ext4_truncate 清 prealloc；RavenFS 无 prealloc） | ext4_setattr inode.c:6172 | len_new == size → RC_OK no-op |
| T8 | 缩到 0：全释放，文件变空（块形态可保留，ext4 不回迁 inline） | ext4_truncate 全删路径 | **RavenFS 无此自由**：size≤160 判据强制回迁 inline（§5.4，disk_layout §10.24 已定） |

### 1.3 punch hole（vfs_fallocate + ext4_punch_hole + 实测）

| # | 语义点 | 出处 | RavenFS 推论 |
|---|--------|------|-------------|
| P1 | PUNCH_HOLE **必须**配 KEEP_SIZE（size 永不变） | open.c:283-289 缺 KEEP_SIZE → EOPNOTSUPP | size 不变；punch 无 size 提交 |
| P2 | offset<0 / len≤0 → EINVAL；offset+len 溢出 → EFBIG；非 REG/BLK → 拒 | open.c:258/315-320/324-328 | len==0 → RC_OK；溢出 → RC_ERR_STATE；dir → 拒（P3） |
| P3 | offset ≥ i_size → **no-op 成功**（不打洞不报错） | ext4 inode.c:4451 `offset >= i_size → return 0` | 同款：offset ≥ size → RC_OK 零副作用 |
| P4 | 洞越过 EOF：end clamp = round_up(i_size)（省无谓尾块清零；KEEP_SIZE 下洞最远到 EOF 块上界） | ext4 inode.c:4455-4457 | clamp_end = min(off+len, round_up(size)) |
| P5 | **整块域释放**：释放区间 = [round_up(offset), round_down(clamp_end)) 的完整块（`EXT4_B_TO_LBLK = round_up(offset)>>blkbits`，ext4.h:368） | ext4.h:368 + ext4_punch_hole 4507 | 洞的"块级主体"unmap 域：db_start = ceil(off/4096)，db_end = clamp_end>>12 |
| P6 | **边缘残块保留 + 域内就地清零**：非对齐头/尾块**不释放**，洞内部分清零、块外数据保留 | ext4_zero_partial_blocks inode.c:4257 + **实测**：punch [100,5000) 后 [0,100) 完整保留、[100,5000) 全 0、[5000,EOF) 保留 | 首尾残块 = 读-改-写清零（字节粒度），**不入 unmap** |
| P7 | 单块内 punch（范围不跨整块）：全程就地清零，**零释放** | ext4_zero_partial_blocks inode.c:4259（start==end 分支） | 同款：无整块域 → 纯字节清零 |
| P8 | 洞覆盖整块区含 EOF 残块：该块连同 size 内有效尾部一起释放（用户自找，KEEP_SIZE 但内容可全灭） | clamp 后洞域含 [round_down(size), round_up(size)) | 同款：clamp_end=round_up(size) 使 EOF 块可整体入洞 |
| P9 | 后续读洞 → 0；洞语义与"从未写"不可区分 | 实测 | 同款 |
| P10 | 失败原子性：ext4 单事务内原子（journal）；RavenFS 无事务 → 决策见 P5（§7） | — | 逐 loose 原子 + 部分生效可接受？ |

## 2. 本机实测记录（ext4 @ linux-7.2.3，与规格同源）

```
文件 16384B 全 0xAA + 每块首块号标记。
① punch [100,5000)：[0,100) 保留 ✓；[100,5000) 全 0 ✓；[5000,EOF) 保留 ✓（残块保留+清零 证实 P6）
② punch [300,600) 单块内：域内 0，块不释放（P7）
③ punch [9000,20000) 越 EOF：洞收口到 16384（P4）；[9000,12288) 残块清零、
   [12288,16384) 整块释放 → 读 0；size 仍 16384（KEEP_SIZE）
④ pread @EOF-8 len64 → 8（短读，R3）；pread @20000 len64 → 0（越界读，R2）
```

## 3. 通用契约（三接口共用）

- **受理类型（P2✓ 对齐 Linux + P3✓ 定案）**：仅 REGULAR 受理；DIR →
  **RC_ERR_IS_DIR**（P3 新增枚举）；SOFT_SYM → **拒**（RC_ERR_STATE，
  Linux 语义：symlink 本身无 read/write/ftruncate/fallocate，open 默认跟随
  到 target，而 v1.1 无路径跟随解析——按 name 直操 symlink inode ≠ 任何
  Linux 合法操作）。
  差异备注：pwrite 现状仍放行 SOFT_SYM（direct_file_ops.cpp:365 检查
  REG||SOFT_SYM）——既有偏离，将来 readlink/跟随解析落地时统一收紧，
  本批三接口不扩大偏离。
- **权限简化**：Linux 的 FMODE_* / append / immutable / LFS 检查在 v1.1
  root_dir 直操语义下不存在（无 fd 无打开态）——既定简化，规格不补。
  readonly 挂载下的写接口拒绝：**当前 pwrite 也未实现**（God_CTRL_WORD
  READ_ONLY_MASK 未接）→ 记录为统一待办，不在本三接口内单独开洞。
- **字节粒度**：三接口全部字节寻址（非 O_DIRECT 语义；块内部分访问
  内部 read-modify-write / 部分拷贝），与 pwrite 一致。
- **并发**：同 inode 操作互斥由调用方保证（pwrite_pipeline §11.5 同款）；
  本三接口不开锁。
- **参数防御**：offset+len / len_new 溢出 → RC_ERR_STATE（同 pwrite）；
  len==0 / len_new==size / offset≥size 等空操作 → RC_OK。
- **root_dir_* 包装层**（现状：四件套 public 包装**全部未实现**，测试经
  `#define private public` 直调 internal）：包装 = dir_lookup(name) →
  RC_ERR_NOT_FOUND 上抛 → internal 直传。三接口的包装随本批补齐（P6），
  模式与 direct_file_pwrite 的既有 root_dir 层一致。
- **错误码映射**（Linux errno → RC_*）：

| Linux | 场景 | RC |
|-------|------|----|
| (成功) | 空操作 / 越界读 / 越 EOF punch | RC_OK（带 0 字节语义） |
| EINVAL | 参数非法（负、溢出、类型不受理） | RC_ERR_STATE（pwrite 先例） |
| EISDIR | dir 目标 | RC_ERR_IS_DIR（P3✓ 新增枚举） |
| ENOSPC | 树节点/数据块分配失败 | RC_ERR_NO_BLOCK |
| EIO | 盘 IO 失败 | RC_ERR_IO |
| ENOENT | name 无此文件 | RC_ERR_NOT_FOUND（包装层 lookup） |
| - | 内部树结构异常 | RC_ERR_STATE |

## 4. pread 规格

签名（P1✓ 定案：加出参）：

```cpp
result_code_t direct_file_pread(uint64_t inode_idx, uint64_t offset, uint64_t len,
                                void *buf, uint64_t *out_bytes);
result_code_t root_dir_direct_file_pread(const char *name, uint64_t offset,
                                uint64_t len, void *buf, uint64_t *out_bytes);
```

契约：

1. **零副作用**：不分配、不脏 inode、不写盘、不建树。纯读。
   atime：v1.1 时间戳字段存在但全系不维护（pwrite 同款）→ 不更新，一致。
2. 语义序（防御在前）：
   - len==0 → RC_OK，*out_bytes=0（R1）
   - offset+len 溢出 → RC_ERR_STATE（R6）
   - offset ≥ size → RC_OK，*out_bytes=0（R2，非错误）
   - 否则 *out_bytes = min(len, size-offset)（R3，EOF 短读）
3. 按 ftype 形态分流（判据 disk_layout §3.2 / ftype()）：
   - **inline**（size≤160）：源 = inode.inline_data[offset..]，memcpy
     min(len, size-offset)（size 定界，永不越 160 数组）。
   - **direct**：物理公式 base+b、块内偏移 b%4096（§3.2 直指）；
     无块级洞 → 逐块 read_block(整块→临时缓冲→裁字节区间拷 buf)；
     clamp 到 size（R4：尾残块 [size,块界) 内容不拷）。
   - **extent**：d_start=off>>12..d_end=ceil(clamp_end>>12) 逐逻辑块推进：
     extent_Bptree_walk 落点 + extent_iterator 游走；命中记录 → 读 phys
     拷块内区间；洞（prev_gap/next_gap 或树尾）→ buf 区间填 0；
     树尾 / 记录边界外的 [..clamp_end) → 填 0（尾洞语义，R5）。
     迭代器 erro → RC_ERR_STATE（结构异常）；IO 错 → RC_ERR_IO。
4. 实现提示（不构成本文契约）：逐块遍历可接受；命中段连续多块时可
   read_buff 批量优化（非必须，正确性优先）。
5. 测试互验意义：pread 落成后，pwrite 测试的"验证器 = 可信内部原语
   组合、不做接口互验"（test_pwrite_smoke 头注）可**升级为 pread 互验**
   ——两接口各自单测保底后互验是有效差分（P7 测试规划）。

## 5. punch_hole 规格

签名不变：`direct_file_punch_hole(inode_idx, offset, len)`。

### 5.1 域规划（phase1 变体，Linux P3/P4/P5/P6 落地）

- offset ≥ size → RC_OK（P3 no-op）。
- len==0 → RC_OK。溢出 → RC_ERR_STATE。
- clamp_end = min(off+len, round_up(size, 4096))（P4）。洞域 [off, clamp_end)。
- 段化（块粒度，P5/P6）：
  - 头残块：off%4096 != 0 且 off < clamp_end → 该块洞内区间
    [off, min(4096·(off>>12+1), clamp_end)) 就地清零（读-改-写），块保留；
  - 尾残块：clamp_end%4096 != 0 → 区间 [4096·(clamp_end>>12), clamp_end)
    就地清零，块保留；
  - 整块域 [db_start, db_end)：db_start=ceil(off/4096)，db_end=clamp_end>>12；
    db_start < db_end 时才存在 → unmap 域（下）。
  - 单块内 punch（整块域空）→ 纯字节清零结束（P7），形态零变化。

### 5.2 形态分流

| 受理：REGULAR only（§3；SOFT_SYM/DIR 拒）。

| 入口形态 | 整块域空 | 整块域非空 |
|---------|---------|-----------|
| inline（size≤160） | 域 [off,clamp_end)⊆ 单逻辑块域 → 就地 memset inline_data（P7 同构；无块可释） | 不可能（size≤160 ⇒ clamp_end≤160 ⇒ 无整块域） |
| direct | 纯字节清零，**保 direct**（无块级洞产生，P7 直接推论） | **物化上树**（§5.3） |
| extent | 残块清零；无记录触碰（洞区本就是洞）→ 可能零树操作 | 逐记录 loose（§5.4） |

> direct + 整块域非空 → 恒 extent：洞使单 run 失去 [0,ceil(size)) 全覆盖，
> direct 不可表示（disk_layout §3.2 / §10.24「打洞产生块级洞 → extent」）。
> **punch 无 phase4 坍缩出口**（不写 size；KEEP_SIZE；洞不可恢复）——
> 出口形态 = 保留段 ≥1 条的 extent 或空树。与 pwrite（保 direct/坍缩
> 双出口）的根本差异：punch 不改 size，physical 事实不可坍缩。

### 5.3 direct → extent 物化（整块域非空时）

1. 建空根叶（extent_new_root_leaf，已落地）。
2. 保留段（零搬移，记录指向原 phys）：[0, db_start) 与 [db_end, oldceil)
   各一条（oldceil=ceil(size/4096)；段可为空则跳过）→ map_range（空叶纯插）。
   保留段内含头/尾残块 → 其洞内字节区间清零在数据写步做。
3. 空树特例：整块域覆盖全部 [0, oldceil)（洞从 0 对齐打到 round_up(size)）
   → 无保留段 → **空根叶即为终态**（size>160 不可 inline，空 extent 树
   合法：data_root→count=0 根叶，读全 0——Linux 空文件态同构）。
4. 失败面：建树/map 的 alloc（RC_ERR_NO_BLOCK）→ 零改动返回（建树前）；
   建树后 map 失败 → 弃新建树（free_orphan_extent_tree 同 pwrite 机械）。

### 5.4 extent → 逐记录 loose（整块域 [db_start, db_end) 内）

对洞域内每条命中的树记录调 `extent_unmap_range_loose`（pwrite_pipeline §9：
full 整删 / head/tail 缩水 / mid 拆两端三分支 + 预算池 + 欠载平衡链）——
loose 契约要求 ext 被单条记录完全覆盖 → 洞域与记录求交逐条切（全交 →
full；交头/交尾 → 缩水；交中 → mid 拆两端）。残块清零独立于 loose
（loose 只动块级映射；洞内字节区间 ∈ 保留段时由数据写步清零）。

- **失败原子性（P5 拍板）**：loose 单次调用自带预算池，失败 = 该次调用
  零副作用（extent_map_unmap.cpp:466 区）；但 punch 整体 = 逐记录推进，
  中途失败 → **已生效记录不回滚**（洞已打 = 合法形态），返回错误码。
  理由：回滚 = 反向 map 恢复原记录，需树节点预算（map 可能再 NO_BLOCK）
  ——恢复路径非零成本非零失败面，与 pwrite phase2 回滚（loose tail 零分配
  恒成功）本质不同。部分 punch 语义 = 状态恒合法 + 调用方重试可收敛。
  若 P5 拍全原子：需先遍历侦查洞域全部命中记录的 kind + 总预算前置
  （复杂度不值，幼年期拒绝）。**拒绝"已删记录 map 回滚"**。
- root 更迭：loose 内部 balance 链可能 root_shrink（根收缩换根，旧根已
  free）→ data_root 变化 → 需 inode 一次写（data_root=新根，无 size 变更）
  —— 提交纪律见 §7（含已知缺口标注）。

### 5.5 崩溃/提交纪律（KEEP_SIZE ⇒ 无 size 提交）

- 无 inode size 变更：提交点 = 树盘面落盘本身（字节残块清零先于/独立于
  unmap 无顺序要求——洞读 0 由"清零或释放"任一达成）。
- data_root 更迭（root_shrink 场景）：inode.data_root 写 = 唯一提交点，
  必须紧随更迭、在后续任何可失败操作之前完成（§7 缺口警告）。

## 6. truncate 规格

签名不变：`direct_file_trunacate(inode_idx, len_new)`（命名 typo 见 P8）。

### 6.1 语义分流

| len_new vs size | 行为 |
|-----------------|------|
| == size | RC_OK no-op（T7；无 prealloc 需清） |
| > size | 增大：P4✓ 上调大小（§6.5） |
| < size | 缩小（§6.2/6.3/6.4 三出口） |

受理：REGULAR only（§3）。

### 6.2 缩小 — 统一机械（tail-punch 编译，pwrite_pipeline §10）

- 域 = [len_new, size)（无 head 残块；end 恒为旧 size，无需 clamp）。
- 整块域 [newceil, oldceil)：newceil=ceil(len_new/4096)，oldceil=ceil(size/4096)。
- EOF 残块（len_new 非对齐）：块 [len_new>>12] **保留**，区间
  [len_new, 4096·(len_new>>12 +1)) 清零——清零到**块上界**而非 len_new
  （T3/§3 stale 防护：将来 truncate-up/pwrite 增长时残块不泄漏旧数据）。
- 形态出口三选（len_new 定终点形态）：

### 6.3 出口判定

1. **len_new ≤ 160 → 强制回迁 inline**（T8 推论 + disk_layout §10.24）：
   读回 [0, len_new)（源 = 现形态 phys/树）→ 释放全部外置资产（direct
   run / extent 树+数据块，机械同 regular_file_reclaim 但不 free inode 槽）
   → inode 一次写：size=len_new、data_root=PTR_INVALID、direct_bit=0、
   inline_data=[0,len_new) 内容。
   禁止"只打尾洞不搬回"（§10.24：size≤160 却外置映射 = 不一致态，fsck
   以 size 为真值清残留——格式判据决定，无选择）。
2. **160 < len_new（direct 态）→ 直接缩 run**：无树操作——释放
   [base+newceil, base+oldceil) 连续块 → inode 写：size=len_new（base/
   direct_bit 不动）。**direct 是唯一缩小不换形态的通道**。
3. **160 < len_new（extent 态）→ 尾洞 loose + 出口坍缩判定**：
   整块域 [newceil, oldceil) 逐条 loose（尾部记录 full/缩水；欠载平衡链）
   → 提交前读根 O(1) 判定（同 pwrite phase4 坍缩判据，size_ceil=newceil）：
   根叶 && count==1 && 单条 fblkbase==0 && end==newceil → **坍缩 direct**
   （写 size + data_root=base + direct_bit=1，后置 free 旧根，先 inode 后
   free——孤儿语义，pwrite_pipeline §8 同款）；否则留树（写 size +
   data_root=新根(可能更迭) + direct_bit=0）。
   与 disk_layout §10.24「truncate 缩至 >160 态不变…extent→direct 不做」
   冲突 → 以 09-08 定案（pwrite_pipeline §2/§10）为准，§9 P9 同步修订布局文档。

### 6.5 truncate 增大（上调大小，P4✓ 定案 2026-09-09）

语义：文件尾把 size 上调到 len_new。Linux 对照 = 纯 i_size 推进 + 旧 EOF
残块清零 + 零分配（稀疏洞）。RavenFS 形态约束下分派：

| 入口形态 | 行为 | 复用 |
|---|---|---|
| inline，len_new ≤ 160 | memset [size, len_new) 补零 + size 推进（中间无洞概念，inline 内） | pwrite inline→inline 豁免同款 |
| inline，len_new > 160 | **迁出** = 假写 pwrite（写域空）：phase1 dfprealloc_and_plan 整域 [0,ceil(len_new/4096)) hard 候选/降级 → phase1.5 判别 → phase2 物化+map → phase3 零垫底全块 + [0,size) inline 内容拷贝（无 overlay）→ phase4 size=len_new（保 direct/坍缩/留树三出口同 pwrite） | pwrite 五段空写骨架（phase1/1.5 正是用户指的前两 phase；物化与提交仍需 2~4） |
| direct | 跨块（ceil(len_new)>ceil(size)）：增长段 [oldceil,newceil) 尾贴 occupy/物化，同 pwrite direct 增长（phase3 零垫底，无 overlay）；不跨块：纯 size 推进 | pwrite direct 增长 / inode 写 |
| extent | **纯 size 推进零分配**（新区域 = 树尾洞，读 0）——与 Linux 完全同构的真稀疏 | inode 写 |

注：① inline/direct 的物化/占位分配是 RavenFS **direct 全覆盖表示的物理义务**
（Linux 零分配因其无此不变量）；extent 态无此负担。② 增大区清零：
新分配块由 phase3 零垫底；EOF 残块 beyond-size 区恒 0 由两条既有纪律保证
（pwrite 新段零垫底 + §6.2 缩小清块上界）→ 不跨块增大**免清零**（不变量
归纳成立；实现期可选防御性读-改-写，非必需）。③ 提交点 = inode 一次写
（size+形态），先于增长段之外的一切 free（同 §6.4）。

### 6.4 崩溃纪律

- inode 一次写（size+形态+data_root 同写）= 唯一提交点，**先于一切 free**
  （T2）：坍缩的旧树根 free、direct 尾块 free、回迁的全资产 free 全部后置
  → 崩溃窗口 = 孤儿块/节点，fsck 回收，**无悬挂**（pwrite_pipeline §8 纪律
  的缩小镜像——缩小版比 pwrite 更严格：pwrite 的 free 对象是新旧根/账本，
  缩小版含旧数据块，全部后置 free 意味着崩溃 = 最多整文件旧资产成孤儿，
  inode 已指新态，无悬垂）。
- root_shrink 更迭在 loose 内可能提前 free 旧根 → §7 已知缺口，同款警告。

## 7. 既有设计缺口（三接口继承，规格显式声明）

1. **extent_root_shrink free 时序**（extent_balance.cpp:261）——**P10✓ 定案
   （2026-09-09）：调换顺序，旧根延迟 free**。现状：根收缩在 unmap/map
   中途发生，旧根 `block_free(old_root,2)` 先于调用方 inode.data_root
   提交 → 崩溃/失败窗口 = inode 指已 free 旧根 = **悬挂**（非孤儿，最坏
   形态）。定案：更迭时旧根**不 free**，随 root_blk& 一并上抛，由调用方
   在 inode.data_root 提交（phase4）**之后**统一 free → 窗口劣化为**孤儿
   2 块**（inode 仍指旧根 = 树完好，fsck 可回收），悬挂消除。
   影响面：extent_root_shrink / extent_balance_from 签名或上抛通道 +
   全部调用点（map/unmap/pwrite/新三接口）收尾接线——实现轮处理，
   与 pwrite 坍缩“先 inode 后 free 旧根”纪律同构。
2. readonly 挂载（rfs-audit）下写接口拒绝未接线（God_CTRL_WORD
   READ_ONLY_MASK）——全系待办，punch/truncate 是写接口，规格要求挂载
   层拒绝生效后再开放（或本批实现时顺手在写接口入口检查 is_readonly →
   RC_ERR_STATE？——P7 范围外，仅记录）。

## 8. 形态 × 操作矩阵（总表）

| 操作 | inline | direct | extent | size 变化 | 出口形态 |
|------|--------|--------|--------|----------|---------|
| pwrite（已过测） | inline 内写 / 迁出 | 覆盖/尾贴保 direct / 断开物化 | map-only | 可增 | 保 direct / 坍缩 / 留树 |
| pread | inline_data 拷 | 公式读 | walk+洞0 | 无 | 不变 |
| punch_hole | 就地 memset | 整块域空：保 direct；否则物化 | loose | 无（KEEP_SIZE） | inline 不变 / **恒 extent 或空树**（无坍缩出口） |
| truncate < | 收 size | 缩 run（保 direct） | 尾 loose + 坍缩判定 | 减 | inline（≤160 强制回迁）/ direct / extent |
| truncate = | no-op | no-op | no-op | 无 | 不变 |
| truncate > | P4 | P4 | P4 | 增 | P4 |

## 9. 拍板记录（2026-09-09）

- **P1✓ pread 加出参 `uint64_t *out_bytes`**（短读语义必须可表达；§4 签名已定）。
- **P2✓ SOFT_SYM 拒收**（对齐 Linux：symlink 本身无 read/write/ftruncate/fallocate；
  返回 RC_ERR_STATE；§3）。pwrite 现状放行 SOFT_SYM 属既有偏离，记录待统一收紧。
- **P3✓ 新增 `RC_ERR_IS_DIR`**（dir 目标三接口拒用；枚举落 disk_struct_v1.1.hpp）。
- **P4✓ truncate 增大 = 上调大小**（§6.5）：非静默忽略。inline ≤160 补零推进；
  inline >160 迁出复用 pwrite 分配/判别机械（用户拍板方向“像 pwrite 前两个
  phase”）；direct 增长段同 pwrite direct 增长；extent 纯 size 推进零分配（真稀疏）。
- **P5 未拍**：punch 整体原子性待定（草案默认 = 逐记录原子 + 部分生效可接受，
  §5.4 论证；拍板前实现按此草案走）。
- **P6✓ root_dir_* public 包装不补**（VFS 内路径解析未建，测试照旧
  `#define private public` 直调 internal；四件套包装留待 VFS 层）。
- **P7 未拍**：测试形态待定（草案：发烟级三接口单测 + pread↔pwrite 互验升级）。
- **P8 未拍**：`trunacate` typo 保持现名（改名留待后续批处理；实现内注释可标注）。

配套修订（已随拍板执行 / 待执行）：
- **P9✓** disk_layout_v1.1.md §10 不变量 24 已修订：尾部元数据写回提交期判定——
  extent 严格单条且全覆盖 [0,ceil(size)) → 缩水 direct；不做主动/重建式迁移。
- **P10✓** root_shrink 旧根延迟 free 定案（§7.1）：窗口从悬挂劣化为孤儿 2 块；
  实现轮接线（影响 map/unmap/pwrite/新三接口收尾）。

## 10. 测试规划（P7 拍板后细化）

## 10. 测试规划（P7 拍板后细化）

- 发烟级（首批，对齐 test_pwrite_smoke 规模 106 断言级）：
  - pread：三形态 × {整块读/跨块/块内偏移/洞读 0/EOF 短读/越界读 0/零长}；
  - punch：三形态 × {单块内/非对齐跨块/贴头/贴尾/越 EOF/offset≥size/全洞}，
    断言洞读 0 + 洞外数据保留 + size 不变 + 形态判定（direct 保/物化）；
  - truncate：{等值 no-op/缩小不跨 160/direct 缩 run/extent 收敛坍缩/
    强制回迁 inline（含 160 边界）/缩到 0}；
  - 验证器沿用 pwrite 策略（可信内部原语组合），preaad 就位后升级互验。
- 压测级：punch 混沌乱序 + 树形态差分账本（复用 loose_stress 骨架）。

---

> **一句话记**
>
> Linux 对照三接口：pread = 纯读零副作用（EOF 短读/越界 0/洞 0/不越 size
> 拷贝）；punch_hole = KEEP_SIZE 恒不写 size（整块释放 + 边缘残块保留清零、
> 越 EOF 收口到 size 块上界、offset≥size no-op；direct 遇整块洞必物化，
> 出口恒 extent/空树，无坍缩出口）；truncate = 缩小先 inode 后 free（孤儿
> 语义无悬挂），>160 尾 loose + 坍缩判定、≤160 强制回迁 inline、direct 直接
> 缩 run，增大 P4 拍板。统一纪律：inode 一次写 = 提交点先于一切 free；
> loose 单次原子、punch 整体允许部分生效（回滚需 map 预算，不值）。
