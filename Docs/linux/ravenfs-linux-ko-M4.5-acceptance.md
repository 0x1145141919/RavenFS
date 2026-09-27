# RavenFS → Linux KO：**M4.5（fsync + 崩溃矩阵 + 深层平衡）验收程序**

> 2026-09-27 · RavenFS **测试组**（M4.5）· 独立于 `juvenile_god`、独立于 ko 实现
> 上位：`ravenfs-linux-ko-M4-data-spec.md` §8（一致性/崩溃/写序）+ §11（M4.5 切分）、
> `ravenfs-linux-ko-requirements-spec.md` §4.8（**R-SY-01..07**）、
> `ravenfs-linux-ko-handover.md` §6.1（M3.5 崩溃窗口实测）+ §10.4（`root_shrink` 延迟 free / 写序 / `i_blocks`）、
> `Docs/punch_truncate_pread_spec.md`（punch/truncate 契约）、`ravenfs-linux-ko-M4.4-acceptance.md`（结构范式，本程序沿用）。
> 纪律：靶子与判据**均不引用 ko/god 代码**；host 侧可**无 guest 自证**（初始镜像结构 + 内容 XXH64 + 三态/深层树合法性）。

---

## 0. 一句话

**M4.5 = fsync 落数据 + 崩溃矩阵 + 深层 extent 树平衡**。验收 = 在 guest 内：
① `fsync`/`fdatasync`/`syncfs(2)` **真持久且错误回报正确**；
② `write→fsync→sysrq-b`（非干净关机）**崩溃 → 重启**后 **同步点之前必落、读回一致、audit 全绿（不产生损坏）**；
③ 未同步改动崩溃后**盘 == 上次落盘点**（数据可丢、结构无损）；
④ **`umount` 干净**（audit 全绿）；
⑤ **3 层区段树**（根内 + 2 内 + 339 叶）上做**大面积 extent 删除**，触发**内层抹匀/合并 + 多层 `root_shrink`**，删后 audit 全绿、内容一致；
⑥ churn（随机写）**每轮 fsync** 逐轮回读一致。

---

## 1. 判据（Definition of Done）

| # | 判据 | 依据 | 靶子 / 场景 | 观测 |
|---|---|---|---|---|
| **C1** | **`fsync`/`fdatasync`/`syncfs(2)` 错误回报** | **R-SY-01/02/05** | `m45_ops probe`（文件/目录/只读 fd/坏 fd/管道） | 见 §1.1 errno 表；`probe` 全 `[OK]`（exit 0） |
| **C2** | **`fsync` 真持久 → 崩溃后同步点前缀必落** | **R-SY-01/04/05**；D-M4-6 写序 | Cr-A：写 [0,8192)→`fsync`→写 [8192,12288)**不同步**→`sysrq-b` | 重启后 `[0,8192)` 逐字节 == `fill`；`check` exit 0 |
| **C3** | **`syncfs(2)` 真持久** | **R-SY-02** | Cr-B：写 [0,16384)→`syncfs(dirfd)`→写 [16384,24576)**不同步**→`sysrq-b` | 重启后 `[0,16384)` == `fill`；`check` exit 0 |
| **C4** | **未同步崩溃不产生损坏**（盘 == 上次落盘点） | **R-SY-07**；handover §6.1 | Cr-C：`mkdir c; touch c/z1..200`（**不 sync**）→`sysrq-b` | 重启后 `audit_v1.2` **BAD=0 / PASS**（数据可丢，结构无损） |
| **C5** | **`umount` 干净**（位图/树全量刷回后 audit 绿） | **R-SY-03/06** | P1 小镜像：probe + mkfill/vfill + churn 后 `umount` | `audit_v1.2` **BAD=0 / PASS**（exit 0） |
| **C6** | **churn + 每轮 `fsync`**：随机写/truncate + 逐轮回读一致 | R-SY-01/04 | `m45_ops churn <dir> 3 8` | 每轮 `fsync` 返回 0；影子比对逐轮一致（exit 0） |
| **C7** | **深层（3 层）区段树**：初始读回一致 + audit 全绿 | disk_layout_v1.1 §10 不变量 31–36 | `s_deep.bin`（339 叶 / 56952 单块 extent / **2 内层**） | `vfill`（zlen=0）一致；`audit` **extent_nodes=342**、BAD=0 |
| **C8** | **大面积 extent 删除**触发内层抹匀/合并 + 多层 `root_shrink` | 不变量 31–36；M4 交接 §10.4 | plan：`punch[0,688128)` → 抹匀；`punch[688128,2064384)` → 合并 + **root_shrink** | 删后 `vfill`（zlen=2064384）一致；`audit` 全绿，**extent_nodes 下降** |
| **C9** | **删除结果 `umount`/`remount` 后落盘一致** | R-SY-01/06 | P2：删后 `umount`→`mount`→`vfill` | remount 后内容一致（exit 0） |
| **C10** | **深层树 audit 中间点**全绿（抹匀阶段结构合法） | 不变量 31–36 | P2：op1 后、op2 后各 `audit` 一次 | 两处 `BAD=0`（`extent_nodes` 按 §3 说明变化） |
| **C11** | **host 侧自证**（无 guest 可跑） | 本程序纪律 | `mkstates_s` 产物 | `audit_v1.2` + `readback verify` + `readback list` 三 `PASS`（§5） |

> **门槛**：C1–C11 全过 = M4.5 **可验收**。任一 errno 不符 / 崩溃后同步点前缀不符 / `audit` **BAD>0** / 深层树删后内容不符 = **不通过**。
> **豁免**：`flush_all` **中途**崩溃的 dangling（= M3.5「设计特性」，见 §6.3）**不计** M4.5 缺陷。

### 1.1 errno 表（C1，`m45_ops probe` 逐项断言）

| 调用 | 期望 | 归属 |
|---|---|---|
| `fsync(fd, O_RDWR)` | `0` | R-SY-01 |
| `fdatasync(fd)` | `0` | R-SY-01 |
| `fsync(dirfd)` | `0` | R-SY-01 |
| `fsync(fd, O_RDONLY)` | `0`（Linux 允许） | R-SY-01 |
| `fsync(-1)` | `-1` / `EBADF` | VFS |
| `fsync(pipe)` | `-1` / `EINVAL` | VFS |
| `syncfs(dirfd)` | `0` | R-SY-02 |
| `syncfs(-1)` | `-1` / `EBADF` | VFS |
| `syncfs(fd, 普通文件)` | `0`（fd 指向本 fs） | R-SY-02 |
| `syncfs(pipe)` | 内核相关（7.2.x 实测 `0`）—— **仅记录，不断言** | VFS |

> **注**：`fsync/syncfs` 的**真持久**判据在 C2/C3/C6（崩溃后读回），errno 表只验**错误回报面**。
> **注**：`fsync` 需落到 **`sync_fs`（卷级）+ 单 inode 落盘**；`fsync` 返回即代表「数据 + 元数据 + 位图」落盘（R-SY-03/05）。
>   M4 前实测已知：`fsync` 只落元数据不落数据（handover §6/G10）——**M4.5 须闭环**（否则 C2 前缀必挂）。

---

## 2. 新增 / 改动资产（均 host 侧构建；**不改** M4.1–M4.4 任何工具）

| 文件 | 作用 | 备注 |
|---|---|---|
| `tools/audit_v1.2/mkstates_s.cpp` | **M4.5 深层树靶子构造器**（`mkstates_t.cpp` 的姊妹件）：造 **3 层区段树**（根内 + 2 内 + 339 叶；叶恰 168 条 = 欠载下限）并出 `.expect`/`.wexpect`/`.plan` | **新增**；不改 M4.1/M4.2/M4.3/M4.4 工具 |
| `tools/audit_v1.2/Makefile` | **追加** `mkstates_s` 目标（并入 `all`/`clean`；原规则不动） | 追加 |
| `ravenfs-linux-test/tmp/m45_ops.c` | **guest 操作器**（host 静态编译）：`probe`（errno 探针）/`mkfill`/`vfill`（fill 公式校验，分块）/`stage`+`check`（崩溃窗口）/`plan`（punch/zero/truncate）/`churn`（**每轮 fsync**） | 新增 |
| `ravenfs-linux-test/tmp/m45-accept.sh` | **guest 侧验收驱动**（复用 `m44-accept.sh` 投递范式）：host 备料 → ssh-pipe 投递 → guest 编 ko → P1（同步面）+ P2（深层平衡）+ **P3（崩溃矩阵，`sysrq-b` + 重启等待）** | 新增（指向 `127.0.0.1:2222`，免密 key，不硬编码密码） |

构建：`make -C tools/audit_v1.2`（**追加** `mkstates_s`）；host 静态编 `tmp/m45_ops.c`（`cc -Wall -Wextra -static`）。

---

## 3. 靶子清单（`deep.img`，host 造；`mkstates_s`）

基准空盘：`truncate -s 256M` + `mkfs.ravenfs -f --inode-count=256`。构造器：`mkstates_s <empty.img> <out.img> [prefix]`。
内容约定：`D` = 填充字节 `m42_fill_byte(name, off)`（与 M4.1–M4.4 同公式）；`Z` = 全 0。

| 靶子 | 形态 | 结构 | 尺寸 | 操作（`.plan`） | 操作后期望 |
|---|---|---|---|---|---|
| `s_deep.bin` | **extent（3 层树）** | 根内 → 2 内（168 / 171 叶）→ **339 叶**；每叶 **168** 条单块 extent（逻辑覆盖连续 `[0,339×168)` 块） | size = **233,275,392 B（222.5 MiB）** | `A PUNCH 0 688128` · `A PUNCH 688128 1376256` | 清零前缀 `[0, 2064384)`；size 不变；`extent_nodes` 342 → （见下） |

- **判据**：初始 `readback verify` / `audit` 全绿；删后 `vfill`（zlen=2064384）一致、`audit` 全绿。
- **`extent_nodes` 期望（信息量，非硬断言）**：初始 = 1 根 + 2 内 + 339 叶 = **342**；
  op1（删叶0 → inner0 = 167 叶 <168 欠载；167+171 = 338 >337 ⇒ **抹匀** 169/169）后 ≈ **341**（338 叶 + 2 内 + 根）；
  op2（再删 2 叶 → 336 叶 ≤337 ⇒ **内层合并** → 根仅 1 子 ⇒ **root_shrink** 深 3→深 2）后 ≈ **337**（336 叶 + 根内）。
  实际平衡路径由 ko 决定，`audit` 只保证**结构合法**（键序 / 子树上界一致 / 无重叠 / 无越界 / 无重入）；`extent_nodes` 仅作形状观察。

> **为何 339 叶**：不变量 31–36 —— 阶 337、非根 count ∈ [168,337]。要 **≥2 内层**须根下 **≥2 内节点** ⇒ **≥ 338 叶**（第 338 叶触发内裂）。叶取**欠载下限 168** 是「不违反下限」的**最小** 2 内层树。
> 构造器 env `M45_E`（每叶条目，默认 168）/ `M45_A`（inner0 叶数，默认 168）/ `M45_B`（inner1 叶数，默认 171）可缩量级——**任意非默认即降级**（见 §6.2）。

---

## 4. guest 验收步骤（自动化：`tmp/m45-accept.sh`）

**前置**：本地 VM（QEMU）已启，`ssh -p 2222 raven@127.0.0.1` 免密可进（`raven` 有 `NOPASSWD` sudo）；
崩溃段需一块 **ravenfs 数据盘**（QEMU serial `deadbeef`）。

1. **host 备料**：`make -C tools/audit_v1.2` → 小空盘（128M）+ 深层空盘（256M）+ `mkfs` → `mkstates_s`（出 `.expect/.wexpect/.plan`）→ host 自证 `audit`×2 + `readback verify`。
2. **投递**（ssh-pipe tar，排除 `linux/vm`）：源码 → `/opt/ravenfs/{linux,include,mkfs}`；靶子/工具 → `/opt/m45/{m45.img, deep.img, deep.img.plan, m45_ops, audit_v1.2}`。
3. **guest 编译 ko**：`sudo make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux modules`。

### P1 —— 同步面（小镜像，loop-over-file）
4. `insmod ravenfs.ko`；`DEV=$(losetup --show -f -b 4096 m45.img)`；`mount -t ravenfs $DEV /mnt/rfs`。
5. **C1**：`m45_ops probe /mnt/rfs/d`（errno 表全 `[OK]`）。
6. **C2**：`m45_ops mkfill` → `vfill`（`fsync` 落数据 + 读回一致）。
7. **C6**：`m45_ops churn /mnt/rfs/d 3 8`（影子随机写 + **每轮 fsync** + 每轮回读）。
8. **C5**：`umount` → `audit_v1.2 $DEV`（**BAD=0**）。

### P2 —— 深层平衡（深层镜像，loop-over-file）
9. `DEV=$(losetup --show -f -b 4096 deep.img)`；`mount -t ravenfs $DEV /mnt/rfs`。
10. **C7**：`m45_ops vfill /mnt/rfs/s_deep.bin s_deep.bin 233275392 0`（初始读回一致）。
11. **C8**：`m45_ops plan /mnt/rfs deep.img.plan`（punch 叶0 → 抹匀；再删 2 叶 → 合并/root_shrink）→ `sync`
    → `m45_ops vfill … 233275392 2064384`（清零前缀一致）。
12. **C10**：op1 后 / op2 后各 `audit`（或 `sync` 后单次 audit；脚本在 P2 末 audit，中间点可在 plan 前后分跑）。
13. **C9**：`umount` → `mount` → `vfill`（落盘一致）；`umount` → `audit`（**BAD=0**，观察 `extent_nodes` 下降）。

### P3 —— 崩溃矩阵（**裸设备** serial `deadbeef`；`echo b > /proc/sysrq-trigger`）
14. **Cr-A（C2 fsync 持久）**：`mkfs` 裸设备 → 挂载 → `m45_ops stage /mnt/rfs crA 8192 4096 fsync`
    （**只 `fsync`，不 `sync` 全盘**）→ `sysrq-b` → **等待重启**（`nvme?n1` 索引会变，**一律按 serial 认盘**）
    → 重新挂载 → `m45_ops check /mnt/rfs crA 8192 4096`（`[0,8192)` 必落）→ `umount` → `audit`（**BAD=0**）。
15. **Cr-B（C3 syncfs 持久）**：同上，`stage … 16384 8192 syncfs`；check 前缀 16384。
16. **Cr-C（C4 未同步不损坏）**：挂载 → `mkdir c; touch c/z1..200`（**不 sync**）→ `sysrq-b` → 重启 → `audit`（**BAD=0**；条目可丢）。
17. **收尾**：`umount /mnt/rfs; losetup -d $DEV; rmmod ravenfs`。

**判定**：步骤 5–16 全绿 ⇒ M4.5 **PASS**；脚本打印 `M45_RESULT: PASS|FAIL` 并以其为退出码。
（崩溃段若 VM 无 `deadbeef` 数据盘 → 需改用 loop-over-file 复挂（**不推荐**：loop 的后备文件页缓存会引入额外不确定性）；脚本默认走裸设备。）

---

## 5. host 侧自证（**无 guest 也能跑**，一轮即可复现）

```sh
make -C <repo>/tools/audit_v1.2                       # 追加 mkstates_s
cd <work>                                             # 例：.../tmp/m45-work
truncate -s 256M deep_empty.img
<repo>/mkfs/mkfs.ravenfs -f --inode-count=256 deep_empty.img
<repo>/tools/audit_v1.2/mkstates_s deep_empty.img deep.img deep.img
#   → deep.img + .expect + .wexpect + .plan
<repo>/tools/audit_v1.2/audit_v1.2 deep.img           # → BAD=0 / RESULT: PASS（exit 0）
<repo>/tools/audit_v1.2/readback verify deep.img deep.img.expect   # → 1/1 [OK]（exit 0）
<repo>/tools/audit_v1.2/readback list deep.img.wexpect             # → 2 段（Z + D）
```

**实测（2026-09-27，本机 PSLaptop）**：

- `make -C tools/audit_v1.2` → 追加编译 `mkstates_s`（`-Wall -Wextra`，**无 warning**）：
  ```
  g++ -Wall -Wextra -O2 -g -I../../include -o mkstates_s mkstates_s.cpp -lxxhash
  make: Nothing to be done for 'all'.
  ```
- `mkstates_s`（默认全量 3 层树）：
  ```
  mkstates_s: E=168 A=168 B=171  => 叶 339, 文件 233275392 B (222.5 MiB)
  mkstates_s: deep inode=17 root_blk=57640 size=233275392
  mkstates_s: nodes = 1 根 + 2 内 + 339 叶 = 342（extent_nodes 期望）
  mkstates_s: ops  punch[0,688128) + punch[688128,2064384) → 清零前缀 2064384 B
    [init] s_deep.bin size=233275392 xxh64=e13456e6034a6f8b
    [post] s_deep.bin size=233275392 xxh64=9c069bafdfe1ef2b
  ```
- `audit_v1.2 deep.img` → `OK=9 WARN=0 INFO=1 BAD=0`、`extent_nodes=342`、`dentry_entries=1`、
  `double_claim=0 leaked_blocks=0 dangling_refs=0 chain_bad=0`、**`RESULT: PASS`**（exit 0）。
- `readback verify deep.img deep.img.expect` → **1/1 `[OK]`**（`state=extent`，xxh64 命中，exit 0）。
- `readback list deep.img.wexpect` → 2 段（`Z [0,2064384)` + `D` 其余），清单自洽。
- `m45_ops.c` 本机 `cc -Wall -Wextra -static` 编译无 warning；在宿主 ext4 上以 `R45_ANYFS=1`
  冒烟：`probe`（除 `syncfs(pipe)` 内核行为外全 `[OK]`）、`mkfill/vfill`、`plan`（punch）、`churn`、
  `stage/check` 均 `[OK]`（操作/回读逻辑正确；ravens 语义待 guest 实跑）。

> **深层性证明**：`extent_nodes=342` = 1 根 + 2 内 + 339 叶；339 > 337（阶）⇒ **单内节点装不下** ⇒ 必有 **2 内层**（深 3）。这是 host 侧可独立判定的「深层」硬证据。
> 独立按节点重数：**`nodes per level = {level0(根): 1, level1(内): 2, level2(叶): 339}`** —— 确为 **3 层 / 2 内层**。

---

## 6. 未决 / 降级点

1. **`extent_nodes` 非硬断言**：删除后的精确平衡路径（抹匀 vs 合并、`root_shrink` 时机）由 ko 实现决定；
   本程序只硬判 **`audit` 全绿 + 内容一致**，`extent_nodes` 作**形状观察**。若 ko 平衡链未做（M4.3 遗留缺口：
   「深层（≥2 层）内平衡 / mid-overflow 叶裂未实测」，M4-data-spec §12 尾注）→ C8 的 audit 仍可能绿但形状不变
   → 记录为**未覆盖**（非 M4.5 判据挂，属 ko 信号）。
2. **量级降级**：默认靶子 = 339 叶 × 168 条 = **222.5 MiB**（≥2 内层的**最小忠实**树）。若投递/guest 磁盘预算不允许，
   用 `M45_E/A/B` 缩小 → **叶低于欠载下限或叶数 <338** ⇒ 只在**形状**上近似（非 ko 稳态），**必须在报告里标注降级**。
   （本机 256 MiB 镜像实测可行：`/home` 余量充足、构造耗时 ~5 s、audit/readback ~2 s。）
3. **`flush_all` 中途崩溃 = 设计特性（不修）**：沿用 M3.5 裁决与 handover §6.1 —— 崩在 `sync`/`syncfs`/`fsync` 的
   `flush_all` **之内**可能产生 dangling（无 journal 边界）。**M4.5 判据只覆盖「同步点之外」的崩溃**（Cr-A/B/C）；
   中途 dangling **不计缺陷**（相关加固 = **G10 / M7**、R-SY-04/05）。本程序**不**做「flush 中途」注入（M3.5 已证）。
4. **`fsync` 的 `i_blocks` / 位图持久**：R-SY-03 要求位图在 `sync_fs`/`fsync` 落盘。若 ko 的 `fsync` 未刷位图，
   崩溃后 audit 会以「已分配未认领/未亮灯」暴露 → 归 C2/C5 的 `audit` 判据。**不另设专项断言**（audit 已覆盖）。
5. **Cr-C 的「盘 == 上次落盘点」为弱判据**：未同步改动全丢是**允许**的（R-SY-07：崩崩目标 = 不损坏，非不丢数据）；
   故 Cr-C 只硬判 `audit` 全绿，不逐字节比「应丢」的期望。
6. **`syncfs(pipe)` 不断言**：内核行为随版本而异（7.2.x 实测返回 `0`）—— 归 VFS，非 ravenfs 属性（§1.1 注）。
7. **崩溃段需裸设备**：P3 走 QEMU serial `deadbeef` 数据盘（`crash{A,B,C}.sh` 同款）；无此盘时脚本不可直接跑
   （loop-over-file 的后备文件页缓存会污染崩溃语义，**不推荐**）。**本程序未改 guest**（编码组在用）。
8. **未跑 guest（本里程碑测试组约束）**：本程序交付 **host 侧资产 + 驱动脚本 + 判据**；`m45-accept.sh` 的 guest 实跑
   待编码组实现 M4.5（fsync 落数据 / `root_shrink` 延迟 free）后进行。**host 自证三件套已实跑 PASS（§5）**。

---

> **一句话记**
>
> M4.5 验收三件套：**同步**（`fsync`/`fdatasync`/`syncfs(2)` 真持久 + errno 正确）、
> **崩溃**（`write→fsync→sysrq-b→重启` 同步点必落、读回一致、`audit` 全绿；未同步崩溃不损坏；`umount` 干净）、
> **深层平衡**（**3 层区段树** 339 叶 × 168 条 → punch 删叶：**抹匀 + 内层合并 + 多层 `root_shrink`**，删后 `audit` 全绿 + 内容一致）
> + churn（每轮 `fsync`）+ host 三件套自证（`audit`/**`extent_nodes=342`** 证深 3 / `readback verify` / `readback list`）。
