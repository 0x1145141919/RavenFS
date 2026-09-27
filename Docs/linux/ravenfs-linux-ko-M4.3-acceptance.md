# RavenFS → Linux KO：**M4.3（truncate / punch_hole / fallocate）验收程序**

> 2026-09-27 · RavenFS **测试组**（M4.3）· 独立于 `juvenile_god`、独立于 ko 实现
> 上位：`ravenfs-linux-ko-M4-data-spec.md` §11（M4.3 切分）+ §8（一致性/提交纪律）、**D-M4-8**（三态迁移基调）、
> **D-M4-11**（拒 `FALLOC_FL_KEEP_SIZE` 纯预留）、**D-M4-14**（inline 可写 mmap 回灌 `inline_data`）；
> `punch_truncate_pread_spec.md`（punch/truncate 契约 + §8 形态×操作矩阵）；`disk_layout_v1.1.md` §5.3/§10（不变量 22–25、31–36）；
> `ravenfs-linux-ko-M4.2-acceptance.md`（结构范式，本程序沿用）。
> 纪律：靶子与判据**均不引用 ko/god 代码**；host 侧可**无 guest 自证**（初始镜像结构 + 内容往返 + 操作后期望双绿）。

---

## 0. 一句话

**M4.3 = truncate / punch_hole / fallocate**。验收 = 在 guest 内对一组**M4.3 就绪靶子**执行确定性操作
（punch 对齐/非对齐/跨 EOF/单块/整块域；truncate 回迁/缩 run/坍缩/增大；fallocate 四模式），
读回与 host ground-truth 逐字节一致 + **形态×操作矩阵**正确（size / 终态形态 / i_blocks）+ **三态迁移**
+ `audit_v1.2` 全绿 + 随机 churn（含 truncate/punch）不炸 + **inline 可写 mmap 落盘**（D-M4-14）。

---

## 1. 判据（Definition of Done）

| # | 判据 | 依据 | 靶子 / 场景（`m43.img`） | 观测 |
|---|---|---|---|---|
| **C1** | **punch_hole 往返一致**（对齐 / 非对齐 / 跨 EOF / 单块内 / 整块域） | spec §5 + P3–P9 | `p_al`·`p_ua`·`p_eof`·`p_sb`·`p_whole` + `f_punch` | punch 后 `cmp` == `wt/`（洞读 0、洞外保留、`size` 不变）|
| **C2** | **truncate 往返一致**（缩 ≤160 回迁 / 缩 direct run / 缩 extent 坍缩 / 增大跨 160） | spec §6 + T3–T8 | `t_inl`·`t_dir`·`t_ext`·`t_grow` | truncate 后 `cmp` == `wt/` + `size` 正确 |
| **C3** | **fallocate 四模式**（`flags==0` 抬 size / `KEEP_SIZE` → `-EOPNOTSUPP` / `PUNCH_HOLE\|KEEP_SIZE` / `ZERO_RANGE`） | **D-M4-11** + R-FD-07 | `f_plain`·`f_keep`·`f_punch`·`f_zero` | 各操作返回码符合期望（`m43_ops` 断 errno）+ 读回一致 |
| **C4** | **形态×操作矩阵**（size + 终态形态 + 内容三方吻合） | spec §8 | 全部 15 靶子 | `stat %s` == `S`；`stat %b` == `B`；`audit` 三态行命中 `X` |
| **C5** | **三态迁移**（inline↔direct↔extent；回迁 / 升格 / 坍缩） | **D-M4-8**；不变量 22–25 | `t_inl`(→inline)·`t_grow`(→extent)·`t_ext`(→direct 坍缩)·`t_dir`(保 direct) | `audit_v1.2` 逐 inode 三态行反查 == `X` |
| **C6** | **`i_blocks` 诚实化**（`stat.blocks` 吻合） | **R-M4-11**；`super.c::ravenfs_inode_blocks4k` | 可定靶子的 `B` 行（12/15） | `stat -c %b`（512B 单位）== 期望 |
| **C7** | **多级 extent 树**（>337 条强制多叶；>168 触发欠载/平衡） | 不变量 31–36；M4.2 遗留的压力缺口 | `deep`（505 条→3 叶，只读锚点）·`deep_p`（punch 叶 0 → 欠载合并） | `audit` 认可多叶树；`deep` B == 4104；`deep_p` 读回一致 |
| **C8** | **inline 可写 mmap（D-M4-14）** | **D-M4-14**；O-14=(a) | `m43_inline_mmap`（`im_inline.bin`，size=100） | mmap 写 [0,100) → `msync` 无 EIO → 逐出 + **remount** 后读回全 `0x42`；**size 不变**；`st_blocks==0`（**未升格**）；越 size 写静默丢弃 |
| **C9** | **结构自洽 + 鲁棒性** | `audit_v1.2` 契约 + churn | 操作 + churn 后的镜像 | `audit` **BAD=0 / exit 0**；`rfsfilechurn`（写+truncate+punch）`PASS` |
| **C10** | **初始靶子 host 自证** | 无 guest 可跑 | `mkstates_t` 产物 | `audit_v1.2` + `readback verify` 双 `PASS`（exit 0） |

> **门槛**：C1–C10 全过 = M4.3 **可验收**。任一 `cmp` 不等 / 返回码不符 / `X` 不符 / `BAD>0` / churn 不符 / inline mmap 静默丢 = **不通过**。

---

## 2. 新增 / 改动资产（均 host 侧构建）

| 文件 | 作用 | 备注 |
|---|---|---|
| `tools/audit_v1.2/mkstates_t.cpp` | **M4.3 靶子构造器**（`mkstates_w.cpp` 的姊妹件）：造 deep/punch/truncate/fallocate 初始态，产出镜像 + `.expect`（初始）+ `.wexpect`（操作后）+ `.plan`（操作序列 + `B/S/X` 期望） | **新增**；不改 M4.1/M4.2 任何工具 |
| `tools/audit_v1.2/Makefile` | **追加** `mkstates_t` 目标（并入 `all` / `clean`；原规则不动） | 追加 |
| `linux/tools/rfsfilechurn.c` | **追加**随机 `ftruncate` / `fallocate(PUNCH_HOLE\|KEEP_SIZE)` 操作 + 影子模型同步（缩/增 = 洞、punch = 洞）；**能力探测**：目标 ko 未实现则自动关闭该类操作（保 M4.2 兼容） | 追加，向后兼容 |
| `ravenfs-linux-test/tmp/m43_ops.c` | **操作执行器**（guest 侧，host 静态编译）：读 `.plan`，按 `T`/`A` 执行 `ftruncate`/`fallocate` 并**断言返回码** | 新增 |
| `ravenfs-linux-test/tmp/m43_inline_mmap.c` | **inline 可写 mmap 自证**（D-M4-14）：`write`/`verify` 两模式 | 新增 |
| `ravenfs-linux-test/tmp/m43-accept.sh` | **guest 侧验收驱动**（复用 `m42-accept.sh` 投递范式）：host 备料 → ssh-pipe 投递 → guest 编 ko → `insmod` → rw 挂载 → 读前 sanity → 操作 → 读回/size/i_blocks/终态 → inline-mmap + remount → churn → audit | 新增（指向 `127.0.0.1:2222`，免密 key，不硬编码密码）|

构建：`make -C tools/audit_v1.2`（**追加** `mkstates_t`）；host 静态编 `linux/tools/rfsfilechurn.c`、
`tmp/m43_ops.c`、`tmp/m43_inline_mmap.c`（`gcc -Wall -Wextra -static`）。

---

## 3. 靶子清单（`m43.img`，host 造）

基准空盘：`truncate -s 64M` + `mkfs.ravenfs -f --inode-count=512`。构造器：`mkstates_t <empty.img> <out.img>`。

### 3.1 初始态 + 操作 + 操作后期望

内容约定：`D` = 填充字节 `m42_fill_byte(name,off)`（与 M4.1/M4.2 同公式）；`Z` = 全 0（洞 / 未写 / 被 punch / 截断区）。

| # | 靶子 | 初始态（size / 形态 / 布局） | 操作（`.plan`） | 操作后 size | 操作后内容 | 操作后形态 | `B`（512B）|
|---|---|---|---|---|---|---|---|
| 1 | `deep.bin` | 2068480 / extent / 505 条单块记录 → **根内+3 叶（168/168/169）** | —（只读锚点） | 2068480 | `D[0,S)` | extent | 4104 |
| 2 | `deep_p.bin` | 同 `deep` | `A PUNCH 0 4096`（打叶 0 首块 → 叶 0 = 167 <**168** 欠载） | 2068480 | `Z[0,4096) D[4096,S)` | extent | — |
| 3 | `p_al.bin` | 16384 / extent / 单条 `[0,4)` | `A PUNCH 4096 4096`（对齐中段） | 16384 | `D[0,4096) Z[4096,8192) D[8192,S)` | extent | 40 |
| 4 | `p_ua.bin` | 16384 / extent / 单条 `[0,4)` | `A PUNCH 100 4900`（非对齐） | 16384 | `D[0,100) Z[100,5000) D[5000,S)` | extent | 48 |
| 5 | `p_eof.bin` | 16384 / extent / 单条 `[0,4)` | `A PUNCH 9000 11000`（跨 EOF） | 16384 | `D[0,9000) Z[9000,S)` | extent | 40 |
| 6 | `p_sb.bin` | 4096 / extent / 单条 `[0,1)` | `A PUNCH 300 300`（单块内） | 4096 | `D[0,300) Z[300,600) D[600,S)` | extent | 24 |
| 7 | `p_whole.bin` | 8192 / extent / 单条 `[0,2)` | `A PUNCH 0 8192`（整块域全释放） | 8192 | `Z[0,S)` | extent（**空根叶**）| 16 |
| 8 | `t_inl.bin` | 8192 / extent / 单条 `[0,2)` | `T 100`（≤160） | 100 | `D[0,100)` | **inline**（强制回迁）| 0 |
| 9 | `t_dir.bin` | 16384 / **direct** / 连续 4 块 | `T 8192` | 8192 | `D[0,8192)` | **direct**（缩 run）| 16 |
| 10 | `t_ext.bin` | 16384 / extent / 单条 `[0,4)` | `T 8192`（单条覆盖 `[0,2)`）| 8192 | `D[0,8192)` | **direct**（坍缩）| 16 |
| 11 | `t_grow.bin` | 100 / **inline** | `T 300`（跨 160 增大） | 300 | `D[0,100) Z[100,300)` | **extent**（升格）| 24 |
| 12 | `f_plain.bin` | 0 / inline | `A 0 0 8192`（`flags==0`） | 8192 | `Z[0,8192)` | extent/direct（视实装）| — |
| 13 | `f_keep.bin` | 100 / inline | `A KEEP 0 4096`（**期望 `-EOPNOTSUPP`=95**） | 100 | `D[0,100)`（不变）| inline | 0 |
| 14 | `f_punch.bin` | 8192 / extent / 单条 `[0,2)` | `A PUNCH 0 4096` | 8192 | `Z[0,4096) D[4096,S)` | extent | 24 |
| 15 | `f_zero.bin` | 8192 / extent / 单条 `[0,2)` | `A ZERO 4096 4096` | 8192 | `D[0,4096) Z[4096,S)` | extent | — |

> `B` 缺省 = **不断言**（`f_plain`/`f_zero`/`deep_p`：终态节点/分配形状取决于 ko 实装，见 §6）。
> `X`（终态形态）由脚本以 `audit_v1.2` 的三态行（`inline` / `direct base=` / `extent_root=`）**反查 inode** 断言。

### 3.2 形态 × 操作矩阵（总表，映射 `punch_truncate_pread_spec.md` §8）

| 操作 | inline | direct | extent | size 变化 | 出口形态 |
|---|---|---|---|---|---|
| punch（`PUNCH_HOLE\|KEEP_SIZE`） | 就地 memset（`f`无）| 整块域空 → 保 direct；否则物化 extent | 逐记录 loose | 无 | inline 不变 / **恒 extent 或空树**（`p_whole` 空根叶）|
| truncate < | 收 size | 缩 run（保 direct，`t_dir`）| 尾 loose + 坍缩判定（`t_ext` → direct）| 减 | inline（`t_inl`，≤160 强制回迁）/ direct / extent |
| truncate = | no-op | no-op | no-op | 无 | 不变 |
| truncate > | ≤160 补零；跨 160 升格（`t_grow` → extent）| 跨块增长 | 纯 size 推进（真稀疏）| 增 | 升格 / 保 direct / 保 extent |
| fallocate `flags==0` | 抬 size（`f_plain`）| 抬 size | 抬 size | 增 | 视实装 |
| fallocate `KEEP_SIZE` | **`-EOPNOTSUPP`**（`f_keep`，**D-M4-11**）| 同 | 同 | 无 | 不变 |
| fallocate `ZERO_RANGE` | 就地清零 | 同 | 就地清零（`f_zero`）| 不变（区间内）| 不变 |

### 3.3 `i_blocks` 期望（`.plan` 的 `B` 行；512B 单位）

诚实口径（`super.c::ravenfs_inode_blocks4k × (4096/512)`）= `(数据块 + extent 节点×2) × 8`：

| 靶子 | 终态 | 数据块 | 节点块 | 期望 |
|---|---|---|---|---|
| `deep.bin` | extent 3 叶 | 505 | 4×2 | **4104** |
| `p_al.bin` | extent（1 叶 2 记录）| 3 | 2 | **40** |
| `p_ua.bin` | extent（1 叶 1 记录）| 4 | 2 | **48** |
| `p_eof.bin` | extent（1 叶 1 记录）| 3 | 2 | **40** |
| `p_sb.bin` | extent（1 叶 1 记录）| 1 | 2 | **24** |
| `p_whole.bin` | extent（空根叶）| 0 | 2 | **16** |
| `t_inl.bin` | inline | 0 | 0 | **0** |
| `t_dir.bin` | direct | 2 | 0 | **16** |
| `t_ext.bin` | direct（坍缩）| 2 | 0 | **16** |
| `t_grow.bin` | extent | 1 | 2 | **24** |
| `f_keep.bin` | inline | 0 | 0 | **0** |
| `f_punch.bin` | extent（1 叶 1 记录）| 1 | 2 | **24** |

> 前提：小 extent 目标为「单 leaf 根」（1 节点 = 2 FS_block）；`deep` 为「根内 + 3 叶」（4 节点 = 8 块）。
> 若 ko 的节点计数/形状不同，`B` 会差异 —— 那是 **R-M4-11 的实测信号**（脚本打印 expected vs actual）。

---

## 4. guest 验收步骤（自动化：`tmp/m43-accept.sh`）

**前置**：本地 VM（QEMU）已启，`ssh -p 2222 raven@127.0.0.1` 免密可进（`raven` 有 `NOPASSWD` sudo）。

1. **host 备料**：`make -C tools/audit_v1.2` → 造空盘/mkfs → `mkstates_t …`（出 `.expect/.wexpect/.plan`）→
   `readback dump` 产 `gt/`（初始）+ `wt/`（操作后）ground-truth；host 自证 `audit_v1.2` + `readback verify`。
2. **投递**（ssh-pipe tar，排除 `linux/vm`）：源码 → `/opt/ravenfs/{linux,include,mkfs}`；
   靶子/工具 → `/opt/m43/{m43.img*, gt/, wt/, m43_ops, m43_inline_mmap, rfsfilechurn, audit_v1.2, readback}`。
3. **guest 编译 ko**：`sudo make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux modules`。
4. **装载 + 挂载（rw）**：`sudo insmod …/ravenfs.ko`；`DEV=$(sudo losetup --show -f -b 4096 m43.img)`；`sudo mount -t ravenfs $DEV /mnt/rfs`。
5. **读前 sanity**：对 `.expect` 每文件 `cat | cmp - gt/<f>`（确认只读路径在本镜像上仍绿）。
6. **执行操作（C1/C2/C3）**：`m43_ops /mnt/rfs /opt/m43/m43.img.plan`（`T`/`A` + **返回码断言**；`f_keep` 须返 `EOPNOTSUPP`）；`sync`。
7. **读回 + 三方吻合（C1/C2/C4/C6）**：
   - 每靶子：`cat /mnt/rfs/<f> | cmp - wt/<f>`（往返一致）；
   - `size`：`stat -c %s` == `.plan` 的 `S`；
   - `i_blocks`：`stat -c %b` == `.plan` 的 `B`（12 靶子）。
8. **inline 可写 mmap（C8，D-M4-14）**：`m43_inline_mmap /mnt/rfs write`（写 [0,100) → `msync` 无 EIO → 逐出后读回全 `0x42`、size=100、`st_blocks==0`）；
   `sync` → **umount/remount** → `m43_inline_mmap /mnt/rfs verify`（磁盘真值）。
9. **随机 churn（C9）**：`mkdir /mnt/rfs/churn`；`rfsfilechurn /mnt/rfs/churn 3 8`（写 + truncate + punch，影子模型逐轮比对）；`sync`。
10. **结构审计 + 三态反查（C5/C7/C9）**：`audit_v1.2 $DEV` → 要求 `RESULT: PASS`、`BAD=0`、`exit 0`；
    再对 `.plan` 的 `X` 行按 inode 反查 audit 三态行（`inline` / `direct base=` / `extent_root=`）。
11. **收尾**：`umount /mnt/rfs; losetup -d $DEV; rmmod ravenfs`。

**判定**：步骤 5–10 全绿 ⇒ M4.3 **PASS**；脚本打印 `M43_RESULT: PASS|FAIL` 并以其为退出码。

---

## 5. host 侧自证（**无 guest 也能跑**，一轮即可复现）

```sh
cd <repo>/tools/audit_v1.2 && make
cd <work>                       # 例：/mnt/huge_data/ravenfs-m43-test
truncate -s 64M empty.img
<repo>/mkfs/mkfs.ravenfs -f --inode-count=512 empty.img
./mkstates_t empty.img m43.img            # → m43.img + .expect + .wexpect + .plan
./audit_v1.2 m43.img                      # → BAD=0 / RESULT: PASS（exit 0）
./readback verify m43.img m43.img.expect  # → 15 文件全 OK（exit 0）

# 操作后期望自洽（用普通 FS 模拟 truncate/punch/fallocate，验证 .plan/.wexpect/m43_ops 三方一致）：
./readback dump m43.img.wexpect wt
./readback dump m43.img.expect  sim && grep -v '^A f_keep.bin' m43.img.plan > sim/plan.nokeep
gcc -O2 -o m43_ops /path/to/m43_ops.c && ./m43_ops sim sim/plan.nokeep
for f in wt/*; do cmp -s "sim/$(basename $f)" "$f" || echo "BAD $f"; done   # 应无输出
```

**实测（2026-09-27）**：

- `audit_v1.2 m43.img` → `OK=23 WARN=0 BAD=0`、`extent_nodes=17`、`dentry_entries=15`、`RESULT: PASS`（exit 0）；
- `readback verify m43.img m43.img.expect` → **15/15 `[OK]`**（inline/direct/extent 三态 + 多叶树 + 空根叶）；
- host 操作模拟（ext4/btrfs 普通 FS，13 条操作 `T`/`A` 全符合期望）→ **15/15 与 `.wexpect` 逐字节一致**
  （含 `flags==0` 预分配读 0、`ZERO_RANGE` 就地清零、punch 非对齐头/尾残块、跨 EOF 收口、truncate 缩/增）。

---

## 6. 未决 / 降级点

1. **`i_blocks` 节点计数的形状依赖（未断言 3 靶子）**：`f_plain`（`flags==0` 终态 extent/direct 未知）、`f_zero`
   （全零块是否降为洞依实装）、`deep_p`（punch 触发欠载/合并后节点数不定）→ 不作 `B` 硬断言（脚本打印实际值）。
   其余 12 靶子的 `B` 假定「单叶根 / deep 3 叶」；实现形状不同 = **R-M4-11 实测信号**。
2. **`fallocate flags==0` 的内容语义（推断）**：本程序期望「新分配区读 **0**」（+抬 `size`）。若 ko 把纯预分配区
   做成**未写已分配**（读回盘上残留）→ 内容断言差异；那是**数据泄漏信号**（须零垫底或标洞），非本程序误判。
3. **`fallocate ZERO_RANGE` / `PUNCH_HOLE` 的块级释放粒度**：`f_zero` 终态若把「全零块」降为洞则 `B` 变 24
   （故未断言）；内容不受影响。
4. **多级树只到 2 层（根内 + 叶）**：3 层需 >337 叶（>~56k 记录、单文件 >200MB），本轮 **降级不构造**；
   2 层已覆盖「>337 强制叶裂 + >168 欠载/平衡」这一 M4.2 遗留压力点。
5. **短写尾巴清算（D-M4-10）不在本程序直接断言**：只验「读回一致 + audit 无孤儿/泄漏」；`evict_inode` 秋后算账
   归 M4.5 收尾。
6. **`m43_ops` 的 `EOPNOTSUPP` 期望仅 `f_keep`**：ko 若以别的 errno（如 `EINVAL`）拒绝 `KEEP_SIZE`，
   `f_keep` 会 fail —— 届时按 **D-M4-11** 口径对齐 errno（**应 `EOPNOTSUPP`**）。
7. **inline mmap 磁盘真值依赖 remount**：脚本已含 `umount/mount` 复验；若 ko 的 `msync` 仅刷页缓存未落 inode，
   remount 读回会暴露（正是 D-M4-14 要堵的点）。
8. **O_DIRECT（C）后置**（D-M4-5）：本程序不含直写。

---

> **一句话记**
>
> M4.3 验收三件套：**punch**（KEEP_SIZE 恒不写 size；整块释放 + 边缘残块保留清零；越 EOF 收口；direct 遇整块洞必物化；
> 出口恒 extent/空树）、**truncate**（缩小先 inode 后 free 无悬挂；≤160 强制回迁 inline；>160 尾 loose + 坍缩判定；
> direct 直接缩 run；增大 = 上调 size，跨 160 升格）、**fallocate**（`flags==0` 抬 size；`KEEP_SIZE` 拒 `-EOPNOTSUPP`；
> `PUNCH_HOLE\|KEEP_SIZE` 与 `ZERO_RANGE` 照常）。靶面 = **多级 extent 树 >337** + 形态×操作矩阵 + 三态迁移 +
> `stat.blocks` 吻合 + audit 全绿 + churn 不炸 + **inline 可写 mmap 落盘**。
