# RavenFS → Linux KO：**M4.1（只读数据）验收程序**

> 2026-09-27 · RavenFS **测试组**（M4.1 只读）· 独立于 `juvenile_god`、独立于 ko 实现
> 上位：`ravenfs-linux-ko-M4-data-spec.md` §3（三态→iomap type）/§5（mmap）/§6（读粒度）；
> `ravenfs-linux-ko-handover.md` §10.5（测试流水线）；`ravenfs-linux-ko-acceptance-plan.md` §2.0。
> 定调：iomap 路线（**D-M4-2**）；读粒度 = **iomap 默认**（**D-M4-7**，不做自研 64K 单元）；**mmap 是硬约束**（**D-M4-1**，`read_folio` 为其硬前置）。
> 纪律：本程序的**靶子与判据均不引用 ko/god 代码**；host 侧可**无 guest 自证**（镜像结构 + 内容往返双绿）。

---

## 0. 一句话

**M4.1 = 只读数据**：`a_ops.read_folio/readahead`（iomap）+ `read_iter` + `mmap_prepare` 三件。
验收 = 在 guest 内对四类靶子（**三态 / >4K / 稀疏洞 / mmap 读·执行**）做 `cat/dd/head` 往返一致 + `cmp`，并跑 `audit_v1.2` 全绿（exit 0）。

---

## 1. 判据（Definition of Done）

| # | 判据 | 依据 | 靶子（`m41.img`） | 观测 |
|---|---|---|---|---|
| **C1** | **三态齐读**（size 优先判据） | M4 §3；v1.1 不变量 22 | `inline.bin`(100) / `direct5k.bin`(5000,direct) / `extent1.bin`(5000,extent) | `cat`==gt |
| **C2** | **>4K 跨页/跨块** | M4 §6 | `direct5k.bin`(5000) / `direct13k.bin`(12345) / `extent_2lvl.bin`(160K) | `dd`==gt |
| **C3** | **稀疏洞读 0** | M4 §3（树未命中→`IOMAP_HOLE`，`folio_zero_range`） | `extent_hole.bin`（extents `[0,1)+[5,8)`，洞 `[1,5)`） | 洞区全 0 |
| **C4** | **≥1 棵 2 级 extent 树**（内节点路由 `key=子树 max 开边界`） | v1.1 §5.2 / §10-9 | `extent_2lvl.bin`（1 内节点 + 2 子叶） | 内容正确（须走内节点） |
| **C5** | **mmap 读** | **D-M4-1** | 任一文件（含 `extent_hole.bin`） | `m41_mmap <f> <gt> [read]`==OK |
| **C6** | **mmap 执行** | **D-M4-1**（动态加载器 `mmap(PROT_EXEC)` 全走 `read_folio`） | `elfexec.bin`（**真实静态 ELF** @extent 态） | ① `m41_mmap … exec`==OK；② 直接 `execve` 返回约定码 |
| **C7** | **结构自洽** | `audit_v1.2` 契约（含 extent 树校验 `extent_node/size_bound/重入`） | `m41.img` | `audit_v1.2` **BAD=0 / exit 0** |

> **门槛**：C1–C7 全过 = M4.1 只读**可验收**。任一 `BAD` / `cmp` 不等 / 洞非 0 = **不通过**。

---

## 2. 新增资产（测试组，均在 host 侧构建）

| 文件 | 作用 |
|---|---|
| `tools/audit_v1.2/mkstates.cpp` | **M4.1 靶子构造器**：写副本 → 造三态/>4K/洞/2 级树（+可选真实 ELF）；产出镜像 **+ `.expect` 期望清单**。与 `mkcontent.cpp` 同构、**不改它**。 |
| `tools/audit_v1.2/readback.cpp` | **回放/校验器**（host，独立于 ko）：`dump <expect> <outdir>` 产 ground-truth 文件；`verify <image> <expect>` 独立解析镜像逐字节断言；`list <expect>` 打印清单。 |
| `tools/audit_v1.2/m41_expect.h` | 清单格式（`F`/`S`/`RAW` 行）+ 确定性内容生成 `m41_fill_byte`（生产/消费**唯一实现**，两侧字节一致）。 |
| `ravenfs-linux-test/tmp/m41-accept.sh` | **guest 侧验收驱动**：host 备料 → ssh-pipe 投递 → guest 编译 ko → `insmod` → 挂载 → `dd/cat/head`+`cmp` → `audit_v1.2`。（指向**本地 127.0.0.1:2222**，免密 key，**不硬编码密码**。） |
| `ravenfs-linux-test/tmp/m41_mmap.c` | mmap 读/执行**代理**（host 编译静态，随脚本投递）：全页触碰 + `memcmp` 参考文件。 |

构建：`make -C tools/audit_v1.2`（**已追加** `mkstates`/`readback` 目标，不替换原规则）。

---

## 3. 靶子清单（`m41.img`，host 造）

`mkstates <empty.img> <out.img> [expect] [elfpath]`。基准空盘：`truncate -s 64M` + `mkfs.ravenfs -f --inode-count=512`。

| 名字 | 态 | size | 逻辑块布局 | 覆盖判据 |
|---|---|---|---|---|
| `inline.bin` | inline | 100 | （`inline_data`） | C1 |
| `inline160.bin` | inline | 160 | （内联上界） | C1 边界 |
| `direct5k.bin` | direct | 5000 | `[0,2)` 连续 | C1/C2 |
| `direct13k.bin` | direct | 12345 | `[0,4)` 连续 | C2 |
| `extent1.bin` | extent | 5000 | `[0,2)` 单条 | C1 |
| `extent_hole.bin` | extent | 32768 | `[0,1)+[5,8)`，**洞 `[1,5)`** | C3 |
| `extent_2lvl.bin` | extent | 163840 | 叶A`[0,2)+[4,2)`；叶B`[10,3)+[20,5)`；**内节点 key=6,25** | C2/C4 |
| `sub/nested.bin` | inline | 7 | 子目录（递归/叶链路径） | 回归 |
| `elfexec.bin` | extent | （ELF 原样） | 单条全文件 | C6 |

> 期望内容：`m41_fill_byte(name, off)`（确定性）；洞区期望 **0**；`elfexec.bin` 期望 = 真实 ELF 字节（清单 `RAW` 行）。清单落盘为 `<out.img>.expect`。

---

## 4. guest 验收步骤（自动化：`tmp/m41-accept.sh`）

**前置**：本地 VM（QEMU）已启，`ssh -p 2222 raven@127.0.0.1` 免密可进（`raven` 有 `NOPASSWD` sudo）。

1. **host 备料**：`make -C tools/audit_v1.2` → 造空盘/mkfs → `mkstates … [elf]` → `readback dump`（ground-truth）。
2. **投递**（ssh-pipe tar，**排除 `linux/vm`**；镜像/工具走同一管道）：
   - 源码 → `/opt/ravenfs/{linux,include,mkfs}`
   - 靶子+工具 → `/opt/m41/{m41.img,m41.img.expect,gt/,m41_mmap,audit_v1.2,readback}`
3. **guest 编译 ko**（**宿主不 insmod**）：
   `sudo make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux modules`
4. **装载 + 挂载**：
   `sudo insmod /opt/ravenfs/linux/ravenfs.ko`
   `DEV=$(sudo losetup --show -f -b 4096 /opt/m41/m41.img)`（mkfs 于普通文件 → LBA 4096，`world_base_lba=1`）
   `sudo mount -t ravenfs -o ro $DEV /mnt/rfs`
   > 持久试验盘（serial `deadbeef`）路径亦可：`for d in /sys/block/nvme*n1; do [ "$(tr -d '[:space:]' <$d/device/serial)" = deadbeef ] && DEV=/dev/$(basename $d); done`。
5. **读往返 + 比对**（对清单每文件）：
   - `cat /mnt/rfs/<f> | cmp - /opt/m41/gt/<f>`（**cat 路**）
   - `dd if=/mnt/rfs/<f> bs=4096 status=none | cmp - /opt/m41/gt/<f>`（**dd 路**，跨页）
   - `head -c <size> /mnt/rfs/<f> | cmp - /opt/m41/gt/<f>`（**head 路**，离线收尾）
   - 洞文件额外断言：洞区（off 4096..20480）字节全 0。
6. **mmap 读/执行**：
   - `m41_mmap /mnt/rfs/<f> /opt/m41/gt/<f>`（`PROT_READ`，全页触碰 + memcmp）
   - `m41_mmap /mnt/rfs/elfexec.bin /opt/m41/gt/elfexec.bin exec`（`PROT_READ|PROT_EXEC`）
   - **真执行**：`/mnt/rfs/elfexec.bin; echo $?` → 约定码（ELF 内 `main` 返回）
7. **结构审计**：`/opt/m41/audit_v1.2 $DEV`（**含 extent 树校验**）→ 要求 `RESULT: PASS`、`BAD=0`、`exit 0`。
8. **收尾**：`umount /mnt/rfs; losetup -d $DEV; rmmod ravenfs`。

**判定**：步骤 5–7 全绿 ⇒ M4.1 **PASS**；脚本打印 `M41_RESULT: PASS|FAIL` 并以其为退出码。

---

## 5. host 侧自证（**无 guest 也能跑**，一轮即可复现）

```sh
cd <repo>/tools/audit_v1.2 && make
cd <work>                       # 例：/mnt/huge_data/ravenfs-m41-test
truncate -s 64M empty.img
<repo>/mkfs/mkfs.ravenfs -f --inode-count=512 empty.img
./mkstates empty.img m41.img m41.img.expect /path/to/static_elf
./audit_v1.2 m41.img            # → BAD=0 / RESULT: PASS（exit 0）
./readback verify m41.img m41.img.expect   # → 9 文件全 OK（exit 0）
./readback dump  m41.img.expect gt         # → ground-truth 文件（供 guest cmp）
```

实测（2026-09-27）：`audit_v1.2` → `OK=20 WARN=0 BAD=0`、`extent_nodes=6`、`RESULT: PASS`；
`readback verify` → 9/9 `[OK]`（inline/direct/extent 三态 + 洞 + 2 级树 + ELF）、`RESULT: PASS`。

---

## 6. 未决 / 降级点

1. **mmap「执行」的忠实度**：`elfexec.bin` 用**真实静态 ELF** 覆盖 `execve`（无需解释器/`ld.so`）。若靶子只能造**动态** ELF，则退化为「`m41_mmap … exec`（`PROT_EXEC` 映射 + 全页 fault）」= 只验 `filemap_fault` 的 exec-mapping 路径，**不作 `execve` 断言**（本程序默认走静态 ELF，已满足）。
2. **2 级树规模**：仅到「1 内节点 + 2 子叶」（最小可辨内节点），**未造 ≥3 层 / 分裂 / 欠载平衡**——那些属 M4.2（写路径），M4.1 只读不需要。
3. **洞的形式**：只测「**块级洞**」（整块未映射）。**页内部分洞 / 残块清零**属写路径（M4.2），不在此。
4. **`iomap` 默认读粒度**：按 **D-M4-7** 不做 64K 单元；本程序不设单元上限断言（与定调一致）。
5. **挂载只读**：本轮 `mount -o ro`（M4.1 只读语义）；写路径往返留待 M4.2。
