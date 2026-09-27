# RavenFS → Linux KO：**T0→T1 阶段排期**（进度无关 · 可长期存活）

> 2026-09-27 · Raven · 从 `ravenfs-linux-ko-handover.md` **抽出的排期层**。
> **为什么单独成文**：handover 是「当前进度」快照 —— 腐败速度极快（改代码即过期）；
> 而 **阶段排期 / 出口判据 / 依赖顺序** 的腐坏速度慢得多 —— 它应当活得比 handover 长。
> 故本文件**只装排期**，不装进度细节；进度真值看 handover，需求真值看 spec。
>
> **设计纪律（防腐）**：正文（§2 起）用**进度无关**口吻写「阶段 = 干什么 + 出口判据 + 依赖 + 需求映射」；
> **进度状态只在 §1 表格的一个 `状态` 列里**，且**只可改这一列** —— 阶段内容本身不得随进度改写。
> 若某阶段内容变了（如路线裁决换框架），那是**契约变更**，须在此处显式记 changelog，不许静默改。

---

## 0. 权威关系（冲突时自上而下服从）

| 文档 | 管什么 | 与本文件关系 |
|---|---|---|
| `ravenfs-linux-ko-requirements-spec.md` | **第一约束需求规格（权威契约）**，R-* 编号 | **需求以它为准**；本文件只引用 R-*，不定义 R-* |
| `ravenfs-linux-ko-acceptance-plan.md` | 验收纲领 + **T0–T4 阶梯定义** + 三层约束 | **阶梯以它为准**；本文件从 T0 讲到 T1 |
| `ravenfs-linux-ko-M4-data-spec.md` | **M4 的第二约束实体化**（走 iomap / 五段管线落点） | **M4 细节以它为准**；本文件只排 M4 的**位置与出口** |
| `ravenfs-linux-ko-handover.md` | **当前进度 + 代码地图 + 契约速查 + 构建流水线** | **进度以它为准**；本文件不复制其进度细节 |
| `ravenfs-vm-env-notes.md` | 测试 VM 环境实录 | 环境以它为准 |
| 本文件 | **T0→T1 的 M 阶段排期**（顺序 / 依赖 / 出口 / 需求映射） | — |

**范围**：**T0（已达成）→ T1（xfstests `-g quick` 缺口表）**，含 T1.5（按缺口表补 M 项至收敛）。
**范围外**：T2 压力、T3 完整安装、T4 启动、T5/T6（另议）—— 只在 §6 留锚点。

---

## 1. 排期总表（**唯一允许随进度变动的表**）

> 图例：✅ 完成 · 🔶 进行中 · ⬜ 未开始。`状态` 列后的 `(日期)` = 状态最后确认日。
> **完成了的阶段不要从表里删** —— 保留行 + ✅，它仍是「后续阶段的依赖前提」。

| 阶段 | 内容（一句话） | 出口判据 | 前置依赖 | 需求映射 | 缺口 | 状态 |
|---|---|---|---|---|---|---|
| **T0** | 静态：mkfs 产物 → 只读挂载 → audit 全绿 | 不变量 §10 + v1.2 §7 零违例 | — | — | — | ✅ (2026-09-26) |
| **M3.5** | 一致性/崩溃窗口（写序 + 非干净卸载后 audit） | flush **之外**崩溃不产生损坏（A/B）；flush 中途 dangling = 设计特性（不修） | M3 | R-SY-04/05/07 | G11 | ✅ (2026-09-27) |
| **M4** | **文件数据**：a_ops + extent 树 + 三态迁移 + truncate/punch/fallocate + **setattr** | `dd/cat/head` 往返一致（跨态 / >4K / 稀疏洞）；`stat.blocks` 吻合；audit 全绿 | M3 | R-FD-01..10、R-MD-01..06、R-SY-04 | G1、G2 | ✅ **M4.1–M4.5 全绿（2026-09-27）** |
| **M5 残** | `link` / `symlink`+`get_link` / `mknod` / `tmpfile` | `ln`/`ln -s`/`readlink`/`mknod` 往返；大目录分裂后 `dents_dump` 仍 DUP=0 MISSING=0；audit `chain_bad=0` | M5 | R-NS-05/06/07/09 | G3、G4、G5、G6 | ✅ **完成（2026-09-27）**：code `5791e9a` + symlink 修复 `f1edbc7` + 孤儿块修复 `1f864bf`；guest 往返 PASS（link / symlink fast+slow / mknod 4 型 / `O_TMPFILE` / 大目录 802·502 DUP=MISSING=0）+ audit BAD=0/orphan=0 |
| **M6** | **xattr / ACL / capability**：4 回调 + `SB_POSIXACL` + 默认 ACL 继承 + `security.capability`；xattr 块 alloc/free | `setfattr`/`setfacl`/`setcap` 往返；**`rsync -aHAX` 保留 ACL+xattr**；audit xattr 校验通过 | M3、M5 | R-XA-01..04、R-NS-12 | G7、G8、G9 | ✅ **完成（2026-09-27）**：guest 实测 PASS（`setfattr`/`setfacl`(access+default+继承+chmod mask)/`setcap` 往返 + `trusted.*` 拒 + >4K 拒 `E2BIG` + remount 保留 + **`rsync -aHAX` rc=0** + audit BAD=0）。**定调（用户）**：`trusted.*` 不做 / xattr>4K→**拒 `-E2BIG`** / 目录 nlink·size **维持非 POSIX** / 范围=必须做全：xattr 四回调 + `security.capability` + POSIX ACL(access+default,`SB_POSIXACL`) + `user.*`(含 `user.crtime_usec`) + **xattr>4K 行为** + 命名空间 `user/trusted/security/system` 裁决 |
| **M7** | **加固**：停机标志 / 周期回写 / 并发面 / `fsync` 全覆盖 / ENOSPC 干净 | `fsstress` 小规模不炸；`drop_caches` + 并发读写下 audit 仍绿；注入不一致后入口返 `-EIO` | M3–G9 | R-LM-09、R-CO-02/04、R-SY-05 | G10 | ⬜ |
| **M8** | **预演最狠测试**：`rsync -aHAX` ext4→ravenfs 克隆 + `pacman -Qkk`（T3 前哨） | 克隆零致命错误 + `pacman -Qkk` 元数据全对 | M4 + M5 + M6 | （T3 前置） | G12 | 🔶 **2026-09-27 pilot PASS**：数据/常规元数据近乎无损（`pacman -Qkk` 与 ext4 逐字一致、audit BAD=0、干净卸载）；**唯一系统性缺口 = xattr/ACL/capability**（`rsync -aHAX` rc=23）→ **M6 必须做全**；全量克隆≈2–4min（T3 建议 `-aHAXS`，源先剔 `/root/*.img`） |
| **T1** | **xfstests `./check -g quick`（generic 组）** | **产出失败清单 = 第一约束缺口表**（区分 fail / skip） | M3–M7 | — | G13 | ⬜ |
| **T1.5** | 按 T1 缺口表逐条补 spec §4 的 **M** 项，重跑至收敛 | 缺口收敛（fail 归零或书面降级） | T1 | — | — | ⬜ |
| *T2* | *范围外留锚点*：`fsx`/`fsstress`/`fs_mark`，每轮 audit | *无静默损坏（audit 恒绿）* | *T1.5* | — | — | *另议* |

### 1.1 关键工具链前置（不属某单一 M，但挡 T1）

| 前置 | 现状 | 影响的阶段 |
|---|---|---|
| **G13：xfstests 工具链进 guest**（guest 无 git/编译链预算） | ⬜ 未解决 | **T1 硬前置**（宿主下载源码 → 投递 → guest 编译；或宿主预编） |
| **G14：fsck** | ⬜ 空实现（`-n` 只查，由 audit 承担） | 不挡 T1；保留 stub，不修 |

---

## 2. 阶段依赖图（顺序纪律的可视化）

```
T0 ✅ ──► M3.5 ✅ ──► [M4 文件数据] ──┐
                              │            ├──► [M8 预演最狠] ──► (T3 前哨)
   M5 ✅ ──► [M5残 link/sym/mknod/tmpfile] ┘        ▲
                              │                     │
                              └──► [M6 xattr/ACL/cap] ──┘
                                        │
   M3..G9 ─────────────────────────────► [M7 加固]
                                        │
   [G13 xfstests 工具链] ────────────────┴──► [T1 xfstests -g quick] ──► [T1.5 补缺口]
                                                                              │
                                                                              └──► (T2 压力，范围外)
```

**顺序约束（硬）**：
1. **T1 的缺口表出来前，不许开 T3**（否则在 Arch 里用「莫名其妙的安装失败」重学 xfstests 早能精确告诉你的东西）。—— 验收纲领 §7 纪律。
2. **M4 先读后写**：M4.1 只读（`read_folio` + `read_iter`/`mmap`）先立住，再进 M4.2 写 —— 见 §3.1。
3. **阶段内禁止越级**：一个 M 未达出口判据，不进下一个 M。
4. **M8 晚于 M4+M5+M6**：它要覆盖 ACL/xattr/硬链接/稀疏/大文件/setuid 全景。

---

## 3. 逐阶段详述（进度无关）

### 3.1 M4 · 文件数据（**当前首发阶段**）

> **M4 细则是独立文档 `ravenfs-linux-ko-M4-data-spec.md`（第二约束实体化）—— 以它为准**。
> ⚠️ **handover §10.3 的 M4 切分已过时**（那份按 buffer_head 路写，含 `write_begin/write_end`）；
> M4-data-spec 已裁决走 **iomap**，`a_ops` 不带 `write_begin/write_end`。**以 M4-data-spec 为准。**

路线定调：必须 mmap-able；必须走 `read_iter`/`write_iter`（**iomap 路线**）；**写时立刻分配（强同步，非 delalloc）**；读用 **iomap 默认**（不自研 64K 单元）；三态迁移 = inline 默认升格 extent（direct 仅出口坍缩收敛态）；**O_DIRECT 后置**（不在 M4）。

建议切分（每步「编译 + guest 实测」可验收才算完成）：

| 子阶段 | 内容 | 出口判据 |
|---|---|---|
| **M4.1 只读** | `a_ops->read_folio/readahead`（**iomap 默认**）+ `read_iter` + `mmap`（`generic_file_mmap_prepare`） | guest `dd/cat/head` 往返一致（三态 / >4K / 稀疏洞）；mmap 读/执行通 —— ✅ **2026-09-27 PASS**（三态/>4K/稀疏洞/2 级树/静态 ELF execve rc=42；audit BAD=0） |
| **M4.2 写数据** | `write_iter` + `iomap_begin(WRITE)` **立刻分配（D-M4-6）** + extent 树插入（insert/叶裂/升根）+ 三态迁移（inline→extent 默认，D-M4-8）+ `i_blocks` | `dd` 写→读往返；`stat.blocks` 吻合；audit 全绿 —— ✅ **2026-09-27 PASS**（7 写场景/IBLK/HOLE/churn/audit BAD=0；已知缺口：mmap 洞写、无合并删除） |
| **M4.3 truncate/punch/fallocate** | 按 `Docs/punch_truncate_pread_spec.md`（形态×操作矩阵、KEEP_SIZE、≤160 强制回迁 inline、出口坍缩） | spec §8 矩阵全过 —— ✅ **2026-09-27 PASS**（punch/truncate/fallocate 逐接口 + 平衡压力 350 extent + audit BAD=0；含 O-14 inline mmap） |
| **M4.4 setattr + 时间戳** | `inode_operations.setattr`（`setattr_prepare` + size→truncate + `setattr_copy`）；时间戳走 generic + `write_inode` 落盘 | `chmod/chown/utimes/truncate` 往返一致 —— ✅ **2026-09-27 PASS**（含 suid/sgid/sticky + remount 保留 + chattr 拒） |
| **M4.5 收尾** | `fsync` 落数据；写序（**经典形态**）；`extent_root_shrink` 旧根延迟 free | `sync` 往返 + 崩溃窗口符合 §8 设计特性 —— ✅ **2026-09-27 PASS**（fsync 定向+`blkdev_issue_flush` 屏障 + `RFS_SHUTDOWN`；CrA/B/C sysrq-b 后 audit BAD=0；深层树 342→337 合并/root_shrink） |

**M4 开工前必须处理的既有缺口**（否则必踩）：见 handover §10.4（`extent_root_shrink` 旧根 free 时序 / 写序不变量 25 / `i_blocks` 口径 / readonly 写拒绝接线）。

**M4 必读文档清单**：spec §4.4/§4.5/§4.8 · `pwrite_pipeline.md`（五段写管线）· `punch_truncate_pread_spec.md` · `disk_layout_v1.1.md` §5+§10（不变量 22–25）· `ravenfs-linux-ko-M4-data-spec.md`（iomap 定调）· god 参考 `juvenile_god/extent_*.cpp`。

### 3.2 M5 残 · 命名空间收尾

- **G3 `link`**：dir `inode_operations.link`（盘上 `link_count++`；dcache 交 VFS）。
- **G4 `symlink` + `get_link`**：短名内联；**T3 前置**（`ld.so`/`/sbin` 依赖 `readlink`）。
- **G5 `mknod`**：4 型（FIFO/CHR/BLK/SOCK）；CHR/BLK 写 `inline_data[0:4]=rdev`。
- **G6 `tmpfile`**：`O_TMPFILE`（journald 用）；**依赖 M4 数据路径**。
- **定位（2026-09-27，用户定）**：**必做、非可选**——`symlink`/`get_link` 是 **T3（启动）硬前置**（`ld.so` 解析全部 `.so`）；`tmpfile` = `journald`；`link`/`mknod` = 安装/运行所需。**不得当可跳项。**

判据：`ln`/`ln -s`/`readlink`/`mknod` 往返；大目录分裂后 `dents_dump` 仍 DUP=0 MISSING=0；audit `chain_bad=0`。

### 3.3 M6 · xattr / ACL / capability

- **G7 xattr 四回调**：`.getxattr/.setxattr/.listxattr/.removexattr` + `xattr_handler`；xattr 块 alloc/free（bitmap 认领）。
- **G8 ACL**：`SB_POSIXACL` + `.get_inode_acl/.set_acl` + `posix_acl_create`（默认 ACL 继承）。
- **G9 capability**：经 xattr 的 `security.capability`。
- **定位（2026-09-27，用户定）**：**测试驱动**——必要性由 **M8 的 `rsync -aHAX` 探针**判定；探针**不丢** ACL/xattr 则可缩水/延后，**丢了**（或 `setcap` 失效）则**必须做全**（`pacman` 的 `security.capability`、`setfacl` 都靠它）。

判据：`setfattr`/`setfacl`/`setcap` 往返；**`rsync -aHAX` 保留 ACL+xattr**；audit xattr 校验通过。

### 3.4 M7 · 加固（「不静默损坏」底线）

- 停机标志：`RFS_SHUTDOWN` 自检失败 → 全局 `-EIO`（xfs force-shutdown 惯例）。
- **周期回写**：ko 现无周期回写线程 → 无人 sync 时 dirty mdata 行累积（实测 9 万目录 ≈ 735MB）。**这是 M7 的头号硬缺口**。
- 并发面系统化（bitmap/alloc、回写/evict/sync vs 前台）。
- `fsync` 全覆盖（数据 + 元数据）。
- ENOSPC 错误路径干净。

判据：`fsstress` 小规模不炸；`drop_caches` + 并发读写下 audit 仍绿；注入不一致后入口返 `-EIO`。

### 3.5 M8 · 预演最狠测试

`rsync -aHAX --delete` ext4→ravenfs 克隆 + `pacman -Qkk`。**T3 前哨**；覆盖 ACL/xattr/硬链接/稀疏/大文件/setuid 全景（同时也是 G8/G9 的端到端验收器）。

### 3.6 T1 / T1.5

- **T1**：guest 内跑 xfstests `./check -g quick`（generic 组）→ **产出失败清单 = 第一约束缺口表**（区分 fail / skip：O_DIRECT/FITRIM/SEEK_HOLE 等降级项会 skip）。**硬前置 G13（xfstests 工具链进 guest）尚未解决**。
- **T1.5**：按缺口表逐条补 spec §4 的 **M** 项，重跑 T1 至收敛。

---

## 4. 缺口 → 阶段 映射（G 编号速查）

> G 编号沿用 handover §6（缺口分析）。**缺口内容本身也几乎不腐坏**，故一并搬来。

| G | 缺口 | 症状 | 阶段 | 需求 |
|---|---|---|---|---|
| G1 | 文件数据读写 | `read/write/mmap` 全 `-EINVAL`；数据 0 落地 | **M4** | R-FD-01..09、R-MD-01、R-SY-04 |
| G2 | `setattr` | 无 `.setattr` → `chmod/chown/truncate/utimes` 不落盘 | **M4.4** | R-MD-02/03/05/06 |
| G3 | 硬链接 `link` | `ln` 失败 | M5 残 | R-NS-05 |
| G4 | `symlink`+`get_link` | `readlink` `-EINVAL`；T3 前置 | M5 残 | R-NS-06 |
| G5 | `mknod` | `mknod` 失败（4 型） | M5 残 | R-NS-07 |
| G6 | `tmpfile` | `O_TMPFILE` 失败（journald 用） | M5 残（依赖 M4） | R-NS-09 |
| G7 | xattr 四回调 | `setfattr/getfattr` 失败 | M6 | R-XA-01..04 |
| G8 | ACL | 无 | M6 | R-NS-12、R-XA-* |
| G9 | capability | 无 | M6 | R-XA-02 |
| G10 | 加固 | 无停机标志 / 无周期回写 / 并发面未系统化 / fsync 不落数据 | M7 | R-LM-09、R-CO-02/04、R-SY-05 |
| G11 | 一致性/崩溃窗口 | **已收口（设计特性，不修）** | M3.5 ✅ | R-SY-04/05/07 |
| G12 | M8 预演最狠测试 | 未做 | M8 | T3 前哨 |
| G13 | xfstests 工具链 | 未解决（guest 无 git/编译链预算） | **T1 硬前置** | T1 |
| G14 | fsck | 空实现（`-n` 由 audit 承担） | 不修 | §4 |

**一句话缺口（决定优先级）**：**文件数据（G1）+ setattr（G2）= 「能读写文件」硬缺口；link/symlink/mknod/tmpfile（G3–G6）= 「Arch 安装/运行」硬缺口；xattr/ACL（G7–G9）= 「pacman/rsync -aHAX 保真」硬缺口；加固（G10）= 「不静默损坏」底线。**

---

## 5. 纪律（照旧，勿忘）

- **三层约束**：① 正确性（满足 Arch 用户态规格）② 框架优先（能用内核成熟框架就用）③ **撞上「非良定义问题」第一时间上报用户**（进度可慢、隐患不留）。
- **小步快跑**：每步「编译 + 实测」可验收才算完成（**只写机构、没接线、跑不了 = 未完成**）；**一步一提交**（commit message 写给接手者）；不憋大招；错了立刻连根铲除；**每步收尾落文档**。
- **成本审计**：大流量/长耗时先量化（`COST-AUDIT-DISCIPLINE.md`）。
- **红线**：宿主 `sudo` 不给；不用录存口令提权（guest 测试 VM 凭证除外，用户已授权）。
- **顺序纪律**：**T1 缺口表出来前不许开 T3**；M4 先读后写；阶段内禁止越级。

---

## 6. 范围外锚点（只留指针，本文不展开）

- **T2 压力**：`fsx`/`fsstress`/`fs_mark`，每轮 audit；判据 = 无静默损坏。
- **T3 完整安装**：ext4 根内 `insmod` → 构造 ravenfs 根 + 装 UEFI bootloader（判据见验收纲领 §2.1）。
- **T4 启动**：ESP → ravenfs root 到 multi-user（判据见 §2.2；T3 做好则水到渠成）。
- **T5/T6**：混沌/崩溃矩阵 · 性能基线 —— **暂缓另议**。
- **单根断奶验证**：见验收纲领 §3.6。

---

## 附录 A · changelog（阶段内容的契约变更记这里，进度状态不算）

| 日期 | 变更 | 依据 |
|---|---|---|
| 2026-09-27 | 本文件从 handover 抽出，覆盖 **T0 → T1** | 用户：handover 腐坏快、排期腐坏慢 |
| 2026-09-28（凌晨） | **🏁 里程碑：M5 起可上实体机灰测** —— M4（文件数据）/ M5（含残 link·symlink·mknod·tmpfile）/ M6（xattr·ACL·capability）**全绿**；**T3 完整安装 §2.1 8/8 PASS**（5.88G 克隆、`pacman -Qkk` 与 ext4 逐字一致、audit BAD=0、loader+initramfs 就绪）；**DKMS 包 `ravenfs-dkms` 起草**（内核更新自动重编 + mkinitcpio 注入 hook）。**T4 就绪未启动** | 用户：里程碑到达 |
| 2026-09-27 | M4 路线 = **iomap**（`a_ops` 无 `write_begin/write_end`）；delalloc；O_DIRECT 后置 | `ravenfs-linux-ko-M4-data-spec.md`（D-M4-1..5）——**覆盖** handover §10.3 旧切分 |
| 2026-09-27（晚） | M4 分配时机改 **立刻分配（强同步，非 delalloc）**；读粒度回退 **iomap 默认**（撤 64K 单元）；三态迁移 = inline 默认升格 extent（direct 仅收敛态） | 用户裁决 O-4/O-5/O-1（M4-data-spec §12） |
| 2026-09-27（晚） | **M4.1 只读 ✅ 实测 PASS**（ko iomap 只读路径；guest 7.2.6；三态/>4K/稀疏洞/2 级树/ELF execve；audit BAD=0） | 提交 `791ad02`（码）+ `97bddbd`（测试资产） |
| 2026-09-27（晚） | **M4.2 写 ✅ 第一版实测 PASS**（立刻分配 + extent 插入树/叶裂/升根 + inline/direct→extent 迁移 + i_blocks + writepages；7 写场景/IBLK/HOLE/churn/audit BAD=0） | 提交 `e61c398`（码）+ `d8f86c1`（测试资产） |
| 2026-09-27（晚） | **M4.3 ✅ 实测 PASS**（O-14 inline 可写 mmap + extent loose/平衡/root_shrink+旧根延迟free + fallocate + truncate/setattr；逐接口 + 350-extent 压力 + audit BAD=0） | 提交 `1735552`（码）+ `440961d`（测试资产） |
| 2026-09-27（晚） | **M4.4 ✅ 实测 PASS**（setattr 全属性 + 补 `file_update_time`/`fileattr EOPNOTSUPP`/EXCHANGE 时间戳；chmod 4755/2755/1777+chown+touch -d+truncate+remount 保留+chattr 拒；audit BAD=0） | 提交 `01a6a1a`（码） |
| 2026-09-27（晚） | **M5残 标「必做（非可选）」**（symlink=T3 前置 / tmpfile=journald / link·mknod=安装）；**M6 标「测试驱动」**（范围由 M8 `rsync -aHAX` 探针决定） | 用户：别把 M5残 当可跳、M6 由测试定 |
| 2026-09-27（晚） | **M4.5 ✅ 实测 PASS**（fsync 定向 + `blkdev_issue_flush` 屏障 + `RFS_SHUTDOWN`；CrA/B/C 崩溃矩阵 + 深层树平衡；audit BAD=0） | 提交 `ddc5f6b`（码）+ `164ba78`（靶场）；裁判：fsync 定向（非卷级），durable-create 归调用方 `fsync(dir)` → **D-M4-15** |
| 2026-09-27（晚） | **M5残 代码 ✅**（link/symlink+get_link/mknod/tmpfile，host make 绿）；**guest 往返待验** | 提交 `5791e9a`（码） |
| 2026-09-27（晚） | **M5残 ✅ 完成**：guest 往返 PASS；途中修 **symlink 两缺陷**（fast usercopy 白名单 / slow i_link+nohighmem）+ **M4 收口孤儿块泄漏**（evict 未接数据回收） | 提交 `f1edbc7`（symlink 修复）+ `1f864bf`（孤儿块修复） |
| 2026-09-27（晚） | **M6 ✅ 完成**：xattr/ACL/capability 全范围（`xattr.c` + SB_POSIXACL + init_acl 继承）；guest PASS（rsync -aHAX rc=0）；途中修 **测试脚本 in-place bug**（`mkstates_x` 仅 in≠out 可用） | 提交 `cc834ea`(靶场)+`254d556`(§5.5)+M6 码 |
| 2026-09-27（晚） | **M8 pilot PASS**：`rsync -aHAX` ext4→ravenfs——内容/权限/属主/ns时间戳/硬链/symlink/设备节点/setuid-sticky/长名/大文件 全保真；**唯一系统性缺口 = xattr/ACL/capability（EOPNOTSUPP，rc=23）→ M6=必须做全**；`pacman -Qkk` 与 ext4 基线一致、audit BAD=0、干净 umount；全量克隆≈2–4min | 用户定 M6 测试驱动 → 探针判「做全」 |
| 2026-09-27（晚） | **M6 定调（用户）**：① `trusted.*` 不做；② xattr 值 >4K → **干净拒 `-E2BIG`**；③ 目录 `nlink/size` **维持非 POSIX 惯例**（不学 ext4）；④ **T3 前必须清 guest `/root` junk**（4.9G 历史 .img） | M6 动身 |
| 2026-09-27（晚） | **M8 观察（记）**：目录 `st_nlink=entries+2` / `st_size=条目数`（非 POSIX 惯例，§10 不变量 18 有意为之）；inline `i_blocks=0`（O-16 邻域）；默认 `-a` 不保稀疏→T3 用 `-S`；金标准 `/root/*.img` 4.9G junk 建议先剔 | M8 pilot |
