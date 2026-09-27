# RavenFS → Linux KO：**M4.2（写数据）验收程序**

> 2026-09-27 · RavenFS **测试组**（M4.2 写数据）· 独立于 `juvenile_god`、独立于 ko 实现
> 上位：`ravenfs-linux-ko-M4-data-spec.md` §3（三态→iomap type）/§8（一致性·写序）/§11（M4 切分）；
> `ravenfs-linux-ko-M4.1-acceptance.md`（只读验收范式，本程序沿用其结构与投递范式）；
> `ravenfs-linux-ko-handover.md` §10.5（测试流水线）。
> 定调（本里程碑相关裁决）：**D-M4-6**（写时立刻分配，非 delalloc）、**D-M4-8**（inline 默认升格 extent；direct 仅出口坍缩收敛态）、
> **D-M4-10**（短写尾巴保留 + `evict_inode` 清算）、**D-M4-11**（拒 `FALLOC_FL_KEEP_SIZE`）；`i_blocks` 诚实化 = **R-M4-11**。
> 纪律：本程序的**靶子与判据均不引用 ko/god 代码**；host 侧可**无 guest 自证**（初始镜像结构 + 内容往返 + 写后期望双绿）。

---

## 0. 一句话

**M4.2 = 写数据**：`write_iter` + `iomap_begin(WRITE)` **立刻分配** + extent 树移植 + 三态迁移（**inline 默认升格 extent**）+ `i_blocks`。
验收 = 在 guest 内对一组**写就绪靶子**做确定性写（覆盖写 / 洞填 / 跨块增长 / >64K 增长 / inline→extent 迁移），
读回与 host ground-truth 逐字节一致 + `stat.blocks` 吻合诚实 `i_blocks` + `audit_v1.2` 全绿 + 随机写 churn（影子模型）不炸。

---

## 1. 判据（Definition of Done）

| # | 判据 | 依据 | 靶子 / 场景（`m42.img`） | 观测 |
|---|---|---|---|---|
| **C1** | **写→读往返一致**（含空文件/覆盖写/跨块/大文件） | M4 §11；handover §10.3 | `fresh.bin`·`inline100.bin`·`direct5k.bin`·`extent5k.bin`·`big.bin` + `.plan` 的 7 条 `W` | 写后 `cmp` == ground-truth（`wt/`）|
| **C2** | **三态迁移（inline 默认升格 extent，D-M4-8）** | M4 §3/§8 | `fresh.bin`→100B（**保持 inline**）；`inline100.bin`→300B（**inline→extent**）；`inline160.bin`→8192 写（**inline→extent + 造洞**） | 写后 `stat` size 正确 + 读回一致 + audit 认可新态 |
| **C3** | **洞填 / 洞保持零** | M4 §3（写向立刻分配后 `IOMAP_MAPPED`） | `extent_hole.bin` 填 `[1,5)`（洞→数据）；`inline160.bin` 迁移后 `[160,8192)` 应保持 **0** | 填区读回非 0 且一致；原洞/新洞区读回全 **0** |
| **C4** | **`i_blocks` 诚实化（数据块 + extent 节点×2）** | **R-M4-11**；`super.c::ravenfs_inode_blocks4k` | 7 靶子 `.plan` 的 `B` 行 | `stat -c %b`（512B 单位）== 期望值 |
| **C5** | **结构自洽** | `audit_v1.2` 契约（含 extent 树 / 位图认领 / 三态） | 写 + `sync` 后的镜像 | `audit_v1.2` **BAD=0 / exit 0** |
| **C6** | **随机写 churn 不炸** | 写路径鲁棒性 | `rfsfilechurn <mnt>/churn 3 8`（影子模型逐轮回读比对） | churn `PASS` + `sync` 后复检一致 + audit 仍全绿 |
| **C7** | **初始靶子 host 自证** | 无 guest 可跑 | `mkstates_w` 产物 | `audit_v1.2` + `readback verify` 双 `PASS`（exit 0）|

> **门槛**：C1–C7 全过 = M4.2 写数据**可验收**。任一 `cmp` 不等 / `stat.blocks` 不符 / `BAD>0` / churn 不符 = **不通过**。

---

## 2. 新增资产（测试组，均 host 侧构建）

| 文件 | 作用 |
|---|---|
| `tools/audit_v1.2/mkstates_w.cpp` | **M4.2 写靶子构造器**（M4.1 `mkstates.cpp` 的姊妹件）：写副本 → 造 fresh/inline/direct/extent+洞 初始态；产出镜像 **+ 三份清单**（`.expect` 初始 / `.wexpect` 写后 / `.plan` 写操作+i_blocks）。**不改** `mkstates.cpp`。 |
| `tools/audit_v1.2/m42_fill.h` | 确定性填充字节 `m42_fill_byte(name,off)` 的 **C 兼容**实现（与 `m41_expect.h::m41_fill_byte` 同公式；`mkstates_w` 启动交叉断言两版一致）。供 host C++ 与 guest 纯 C 工具共享。 |
| `linux/tools/rfsfilechurn.c` | **guest 侧随机写压测**（类比 `rfsdirchurn.c`）：N 轮随机 `write/pwrite` + **影子模型**（期望字节 + 写过位图）→ 每轮 `pread` 回读逐字节比对；`statfs` 认 ravenfs；`syncfs` 后可跑 audit。host `gcc -static` 可编。 |
| `ravenfs-linux-test/tmp/m42-accept.sh` | **guest 侧验收驱动**（新脚本，复用 `m41-accept.sh` 投递范式）：host 备料 → ssh-pipe 投递 → guest 编译 ko → `insmod` → **rw 挂载** → 写场景 + 读回比对 + i_blocks → churn → audit。（指向 **127.0.0.1:2222**，免密 key，**不硬编码密码**。） |
| `ravenfs-linux-test/tmp/m42_writer.c` | **写场景执行器**（host 静态编译，随脚本投递）：读 `.plan`，按 `W <name> <off> <len>` 对挂载点内文件 `pwrite` 填充字节。 |

构建：`make -C tools/audit_v1.2`（**追加** `mkstates_w` 目标，不替换原规则）与
`gcc -O2 -static -o /dev/null linux/tools/rfsfilechurn.c`（+ `m42_writer.c`）。

---

## 3. 靶子清单（`m42.img`，host 造）

`mkstates_w <empty.img> <out.img> [prefix]`。基准空盘：`truncate -s 64M` + `mkfs.ravenfs -f --inode-count=512`。

### 3.1 初始态（写之前，`.expect` / C7 自证）

| 名字 | 态 | size | 内容 | 逻辑块布局 | 覆盖判据 |
|---|---|---|---|---|---|
| `fresh.bin` | inline | 0 | — | — | C1/C2 |
| `inline100.bin` | inline | 100 | fill | — | C1/C2 |
| `inline160.bin` | inline | 160 | fill（内联上界） | — | C2/C3 |
| `direct5k.bin` | direct | 5000 | **全 0** | `[0,2)` 连续 | C1/C4 |
| `extent5k.bin` | extent | 5000 | **全 0** | `[0,2)` 单条 | C1/C4 |
| `extent_hole.bin` | extent | 32768 | fill + **洞** | `[0,1)+[5,3)`，洞 `[1,5)` | C3 |
| `big.bin` | inline | 0 | — | — | C1（>64K 增长源）|

> 内容生成：`m42_fill_byte(name,off)`（确定性）；洞区期望 **0**。清单落 `<out>.expect`。

### 3.2 写场景（`.plan` 的 `W` 行；写字节 = `m42_fill_byte(name,off)`）

| # | 靶子 | 操作 `W <name> <off> <len>` | 语义 | 写后 size | 写后内容（`.wexpect`）| 判据 |
|---|---|---|---|---|---|---|
| SC1 | `fresh.bin` | `0 100` | 空文件写 100B（**不越 160**）| 100 | `D[0,100)` | C1/C2 |
| SC2 | `inline100.bin` | `0 300` | **inline→extent 迁移**（越 160）| 300 | `D[0,300)` | C1/C2 |
| SC3 | `inline160.bin` | `8192 100` | 迁移 + **造洞** `[160,8192)` + 尾写 | 8292 | `D[0,160) Z[160,8192) D[8192,8292)` | C2/C3 |
| SC4 | `direct5k.bin` | `1024 4096` | direct 覆盖写 + **跨块增长**（仍在 2 块 run 内）| 5120 | `Z[0,1024) D[1024,5120)` | C1/C4 |
| SC5 | `extent5k.bin` | `3072 2048` | extent 覆盖写 + **跨块** | 5120 | `Z[0,3072) D[3072,5120)` | C1/C4 |
| SC6 | `extent_hole.bin` | `4096 16384` | **洞填** `[1,5)` | 32768 | `D[0,32768)` | C3 |
| SC7 | `big.bin` | `0 131072` | **>64K 增长**（128KB）| 131072 | `D[0,131072)` | C1 |

> `D` = 填充字节（`m42_fill_byte`），`Z` = 全 0（洞 / 未写区）。写后内容清单落 `<out>.wexpect`；
> `readback dump <out>.wexpect wt/` 产 ground-truth 供 guest `cmp`。

### 3.3 `i_blocks` 期望（`.plan` 的 `B` 行；512B 单位）

诚实口径（`super.c::ravenfs_inode_blocks4k × (4096/512)`）= `(数据块 + extent 节点×2) × 8`：

| 靶子 | 写后态 | 数据块 | 节点块 | 期望 `st_blocks`（512B）|
|---|---|---|---|---|
| `fresh.bin` | inline（100 ≤160）| 0 | 0 | **0** |
| `inline100.bin` | extent | 1 | 2 | **24** |
| `inline160.bin` | extent（含洞）| 2 | 2 | **32** |
| `direct5k.bin` | direct | 2 | 0 | **16** |
| `extent5k.bin` | extent | 2 | 2 | **32** |
| `extent_hole.bin` | extent（填满）| 8 | 2 | **80** |
| `big.bin` | extent | 32 | 2 | **272** |

> **前提**：extent 目标为「单 leaf 根」（1 节点 = 2 FS_block）。小文件（≤32 个 extent）必落单叶；
> 节点块数若实现不同（多叶/多级）则 C4 会差异 → 属 R-M4-11 的**实测信号**（见 §6）。

---

## 4. guest 验收步骤（自动化：`tmp/m42-accept.sh`）

**前置**：本地 VM（QEMU）已启，`ssh -p 2222 raven@127.0.0.1` 免密可进（`raven` 有 `NOPASSWD` sudo）。

1. **host 备料**：`make -C tools/audit_v1.2` → 造空盘/mkfs → `mkstates_w … `（出 `.expect/.wexpect/.plan`）→
   `readback dump` 产 `gt/`（初始）+ `wt/`（写后）ground-truth；host 自证 `audit_v1.2` + `readback verify`。
2. **投递**（ssh-pipe tar，**排除 `linux/vm`**）：源码 → `/opt/ravenfs/{linux,include,mkfs}`；
   靶子/工具 → `/opt/m42/{m42.img*, gt/, wt/, m42_writer, rfsfilechurn, audit_v1.2, readback}`。
3. **guest 编译 ko**：`sudo make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux modules`。
4. **装载 + 挂载（rw）**：`sudo insmod …/ravenfs.ko`；`DEV=$(sudo losetup --show -f -b 4096 m42.img)`；
   `sudo mount -t ravenfs $DEV /mnt/rfs`（**可写**，对比 M4.1 的 `-o ro`）。
5. **写前读 sanity（C7 延伸）**：对 `.expect` 每文件 `cat | cmp - gt/<f>`（确认只读路径在本镜像上仍绿）。
6. **执行写场景**：`m42_writer /mnt/rfs /opt/m42/m42.img.plan`（按 `.plan` 的 7 条 `W` 写）；`sync`。
7. **写后读回 + i_blocks + 洞断言**（C1/C2/C3/C4）：
   - 每靶子：`cat /mnt/rfs/<f> | cmp - wt/<f>`（写→读往返一致）；
   - `size`：`stat -c %s` == `.wexpect` 的 size；
   - `i_blocks`：`stat -c %b` == `.plan` 的 `B` 期望；
   - 洞断言：`inline160.bin` 的 `[160,8192)` 与迁移后读回全 **0**；`extent_hole.bin` 填区非 0。
8. **随机写 churn（C6）**：`mkdir /mnt/rfs/churn`；`rfsfilechurn /mnt/rfs/churn 3 8`（影子模型逐轮比对）；`sync`。
9. **结构审计（C5）**：`sync` 后 `/opt/m42/audit_v1.2 $DEV` → 要求 `RESULT: PASS`、`BAD=0`、`exit 0`（含 extent 树校验）。
10. **收尾**：`umount /mnt/rfs; losetup -d $DEV; rmmod ravenfs`。

**判定**：步骤 5–9 全绿 ⇒ M4.2 **PASS**；脚本打印 `M42_RESULT: PASS|FAIL` 并以其为退出码。

---

## 5. host 侧自证（**无 guest 也能跑**，一轮即可复现）

```sh
cd <repo>/tools/audit_v1.2 && make
cd <work>                       # 例：/mnt/huge_data/ravenfs-m42-test
truncate -s 64M empty.img
<repo>/mkfs/mkfs.ravenfs -f --inode-count=512 empty.img
./mkstates_w empty.img m42.img            # → m42.img + .expect + .wexpect + .plan
./audit_v1.2 m42.img                      # → BAD=0 / RESULT: PASS（exit 0）
./readback verify m42.img m42.img.expect  # → 7 文件全 OK（exit 0）

# 写后期望自洽（用普通 FS 模拟 pwrite，验证 .plan/.wexpect/m42_writer 三方一致）：
./readback dump m42.img.wexpect wt
cp -r <初始 dump> sim && ./m42_writer sim m42.img.plan
for f in wt/*; do cmp -s "sim/$(basename $f)" "$f" || echo "BAD $f"; done   # 应无输出
```

实测（2026-09-27）：`audit_v1.2` → `OK=15 WARN=0 BAD=0`、`extent_nodes=2`、`RESULT: PASS`；
`readback verify` → 7/7 `[OK]`（inline/direct/extent 三态 + 洞）；host 写模拟（普通 FS）→ 7/7 与 `.wexpect` 逐字节一致。

---

## 6. 未决 / 降级点

1. **`i_blocks` 节点计数的实现形状依赖**：C4 期望假定 extent 目标为**单 leaf 根**（节点 2 块）。若 ko 对同一文件用多叶/多级树（本用例规模 32 extent 不该触发）或采用不同的元数据计入口径，C4 会差异 —— 那是 **R-M4-11 的实测信号**，须按 `super.c::ravenfs_inode_blocks4k` 口径对齐（脚本会打印 expected vs actual）。
2. **direct 增长语义**：SC4 令 `direct5k.bin` 在自身 2 块 run（容量 8192）内长到 5120 → 期望**保持 direct**（不整文件迁 extent）。若实现选择在此处迁移 extent（不变量 24 允许「不可续才迁」，此处可续故不应迁），期望需相应调整。
3. **短写尾巴清算（D-M4-10）未在写入路径直接断言**：本程序只验「常驻文件读回一致 + audit 无 BAD」。`evict_inode` 秋后算账（tail trim）依赖 unlink/umount 触发，属 M4.5 收尾 → 此处不设专项断言（降级为「audit 无孤儿/泄漏」间接覆盖）。
4. **`fallocate` / `truncate` / `punch_hole`** 属 **M4.3**（D-M4-11 的 `KEEP_SIZE` 拒绝面亦然），不在本程序。
5. **O_DIRECT（C）后置**（D-M4-5）：本程序不含直写；`iomap_begin` 的 `IOMAP_WRITE` 分向由 buffered 写覆盖。
6. **churn 规模**：8 文件 × ≤256KB × 3 轮（可用 `ROUNDS/NFILES` 覆盖）。**>64K 单文件**与**跨块**已在 churn + 定向场景覆盖；**多级 extent 树**（>337 条）不在本程序（留待更大压力/后续）。
