# RavenFS → Linux KO：验收测试纲领 + 元总则（独立虚拟机系统版）

> 2026-09-25 · Raven
> **本文件基于「独立虚拟机系统」重写**：自有 QEMU + 自有磁盘，**不依赖 `VMtest.sh` 的任何资源**（只参考其思路：UEFI 固件 / 启动分区与主分区**分盘** / CPU 配置）。
> **2026-09-25 修订**：① **T3 的"安装" = 把 Arch 系统（ravenfs 根）与 UEFI bootloader（`esp.img`）一次都装好** → **T4 自然水到渠成**；② **双根架构**（ext4 根 + ravenfs 根）；③ **T3 通过"在 ext4 根里 `insmod ravenfs.ko`"完成** → **ISO 修补路线作废**；④ T5/T6 暂缓另议；⑤ **元总则升级为「三层约束」**（正确性 / 框架优先 / 用户暴露）——原「两个约束 + 一张判据表」把「框架优先」与「用户暴露」混成一层。
> 配套：**需求规格 `ravenfs-linux-ko-requirements-spec.md`（第一约束实体化，权威真值源；冲突以 spec 为准）**；环境落地 `ravenfs-vm-env-notes.md`。
> ~~缺口与决策 `ravenfs-linux-ko-port-gaps.md`；fsck 设计储备 `ravenfs-fsck-design.md`~~（两文件仓库中不存在，作废——缺口权威表见 spec §4「现状」列 + `disk_layout_v1.2_inode.md` §8.1）。

---

## 0. 元总则（三层约束）

用户口径（原文）：*"能用内核成熟框架就用内核成熟框架，在满足以 archlinux 一大堆用户态程序的规格需求第一约束下的第二约束，目的是尽可能减少 ai 的对于不良定义问题的暴露面。"*

**约束三层，逐层服从上一层**：

```
第一约束（正确性）  ：满足 Arch Linux 用户态程序的规格需求。         ← 目标层，优先
第二约束（框架优先）：能用内核成熟框架/惯例就用，作为默认选项——      ← 手段层
                      哪怕不是最优、哪怕多绕一层。
第三约束（用户暴露）：前两层之下，凡撞上"非良定义问题"，第一时间     ← 纪律层
                      上报用户——那是用户的学习面，也是风险；
                      接受进度慢，不接受隐患。
```

**第二约束的可操作形式（只判"怎么实现"，按顺序过）**：

| 顺位 | 动作 | 判定 |
|---|---|---|
| 1 | **照抄内核成熟框架/惯例** | 默认选项，哪怕不是最优、哪怕要多绕一层 |
| 2 | **降级功能**（`-EOPNOTSUPP`） | 框架不覆盖且非 Arch 必需时 |
| 3 | **拒绝发明** | 禁止自创语义弥补；宁可缺功能 |
| 4 | **必须自创** | 写进契约 + 配检查项 + 写明"为什么框架不行"（对齐 `DISCIPLINE.md`）；**并触发第三约束（上报用户）** |

**第三约束的触发面**：任何"非良定义问题"——规范没说清 / 框架没覆盖 / 行为无从对照——都是风险，**不得自行拍脑袋消化**，一律第一时间上报用户。进度可以慢，隐患不可以留。

**第三约束附加指导：小步快跑（2026-09-27 定）**：

1. **每步必须小且可验收**：一个能独立**编译 + 实测**（guest 挂载 / `audit_v1.2` / 对照）的增量才算"一步"。**只写了机构、没接线、跑不了 = 未完成**。
2. **一步一提交**：commit message 写给**接手者**——本步做了什么 / 定下的契约与决策 / 踩的坑 / 下一步；不靠会话记忆。
3. **不憋大招**：宁可多切几个可测小步，也不要一次性大跳跃。大跳跃 → **错误放大**（前车之鉴：2026-09-26「错误注释幻觉」——一条错误注释被采信后跨文件传播，见 memory/2026-09-26）。
4. **错了立刻回退，连根铲除**：发现前提错误（含文档/代码注释）先停下澄清、全仓铲除再继续；不为已投入的工作辩护。
5. **每步收尾落文档**：规格 / 契约 / 坑写进对应 Docs（spec / handover / 移植 notes），**不留在会话里**。
6. **量力而上报**：一步的规模超出"小"（如 M5 提前做）时，先向用户报清规模与依赖，再动手。

**三条边界（总则管不到，显式豁免）**：
1. **磁盘布局必须自创** → 风险只能靠 **§10 不变量 + audit** 兜。
2. **性能不达标允许退回自创**，但要有退出条件：先框架 → 基准证明不足 → 才自创，且书面论证。
3. **第一约束必须先变成可勾选清单**（见 §1；§1 已冻结为指向 spec 的指针），否则退化成口号。

**代价要写成决定**：会主动放弃**缓存控权**（E1：缓存/IO 全下沉 Linux）、B+tree 某些激进度——"放弃"必须落成决定，不能留"以后再说"。

**552B 名字：保留（2026-09-26 裁决 → 取上限）**：`name_heap`/dentry 外置名路径**原样保留**（v1.2 盘面不动），**`f_namelen=552`，>255 名字为活路径且必须正确**。
（依据：VFS 对单个组件名**没有** `NAME_MAX(255)` 硬拦截，实际上限是 `PATH_MAX` 量级——`fs/namei.c` 走 `EMBEDDED_NAME_MAX`、`fs/readdir.c::verify_dirent_name` 判 `len < PATH_MAX`；故不设 255 收口。）

---

## 1. 第一约束实体化：Arch 用户态规格清单

> **【2026-09-26 · 本节已冻结并作废，改为指向需求规格】**
> 本节内容由 **`ravenfs-linux-ko-requirements-spec.md`（第一约束需求规格书）** 接管。
> **任何冲突以 spec 为准**（spec 是 09-26 决议收敛版，本节多处已被其覆盖/纠正：xattr 槽位、readdir 原子性范围、`s_maxbytes` 已定、`fallocate` 已入需求）。
>
> 原 A–E 分节与 spec 的对应（保留作指针，供旧引用解析）：

| 本节（作废） | 现指向 |
|---|---|
| A. 元数据与权限 | spec §4.5（R-MD-*）、§4.7（R-SP-*） |
| B. 名字空间与文件操作 | spec §4.3（R-NS-*）、§4.4（R-FD-*） |
| C. xattr / ACL | spec §4.6（R-XA-*）；**xattr 槽位 = `permission_bits` 高 52 位、单块指针、≤1 FS_block**（spec R-XA-04 / §4.5.1） |
| D. 容量/规模参数 | spec §4.7（R-SP-*） |
| E. 可降级 | spec §4.10（R-DG-*） |

---

## 2. 测试阶梯（修订：T0–T4 在范围内；T5/T6 暂缓）

| 层 | 内容 | 验收判据 | 状态 |
|---|---|---|---|
| **T0** | 静态：`mkfs.ravenfs` 产物 → 只读挂载 → 契约校验员全绿 | 不变量 §10 + v1.2 §7 零违例 | **✅ 2026-09-26 达成**（校验员=`tools/audit_v1.2`） |
| **T1** | **xfstests `./check -g quick`（generic 组）** | 通过率；失败清单 = 第一约束缺口表 | 在范围 |
| **T2** | 压力：`fsx` / `fsstress` / `fs_mark`；**每轮跑 audit** | 无静默损坏 | 在范围 |
| **T3** | **完整安装**：在 ext4 根里 `insmod` 后构建 Arch 系统（ravenfs 根）+ 装 UEFI bootloader | 见 §2.1 | 在范围 |
| **T4** | **启动**：从 `esp.img` 引导 ravenfs root 到 multi-user | 见 §2.2（**T3 做好则水到渠成**） | 在范围 |
| T5 | 混沌：`pacman -Syu` 循环 + 崩溃矩阵 | — | **暂缓，另议** |
| T6 | 性能基线 vs ext4（`fio`/`bonnie++`/`systemd-analyze`） | 先定可接受倍率 | **暂缓，另议** |

> **「测试矩阵之外」的含义**：**装 ext4 根不算测试步骤**（它是环境准备，用标准工具走标准流程）。测试矩阵从 **"在 ext4 根里 `insmod ravenfs.ko`"** 才开始。

### 2.0 实现进度与工具链（2026-09-26）

> **接手请先读 `ravenfs-linux-ko-handover.md`（交接文档，自包含）。**

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 模块骨架：`fs_type` 注册 + kbuild 外部模块 | ✅ |
| M1 | SB 读+校验(magic/ver/crc32) + 几何 + root iget + 只读 mount + 空目录 readdir/lookup/getattr/statfs | ✅ |
| M2 | dentry B+tree 遍历（lookup hash 下行 / readdir 全量 + 名字堆长名）+ `getattr.blocks` | ✅ |
| M2.1 | readdir cookie → (叶块号,槽) + 叶链续读（大目录 O(log n) 定位 + O(产出)）；`i_blocks` 诚实化 | ✅ |
| M2.2 | 缓存行尺寸解耦（4K/8K/16K/32K/64K，mount `row_size=`，尾行截断） | ✅ |
| M2.3 | `mdata_piece_cache_mgr`+`mdata_session`（8K 固定 granule）；`ravenfs_read_node` 改走 session | ✅ |
| M2.4 | ~~锁层改 node_latch~~ → **不做**（片级 rw_semaphore + 专属 TTL 已足；关闭 spec §4.9 未定项） | **✅ 决议** |
| M2.5 | 自写 TTL 淘汰器（`cache_ttl.c`，2s，对齐 god）——**压力 shrinker 已移除**，TTL 为唯一冷集淘汰 | ✅ |
| M2.5b | 行缓冲改**伙伴系统页**（`order=log2(row/4K)`，直映射访问，淘汰归还）；**分片锁重入死锁根治**（锁外分配+I/O） | ✅ 实测 |
| **T0** | mkfs + 只读挂载 + `audit_v1.2` 全绿 | **✅ 达成** |
| M3.1/M3.2 | 双位图分片分配器 / 写回路径（`write_inode` 行同步 + `flush_all` 落盘 + `sync_fs`/`fsync`） | ✅ |
| M3 | 元数据写路径：取消强制 RO（rw 挂载）+ `create(REG,inline)`/`unlink` + 位图持久化（全量内存分片+全量刷回） | ✅ 实测 |
| M3.5 | 一致性/崩溃窗口验证（写序 + 非干净卸载后 audit） | ⚠️ 已实测：flush 外 PASS；flush 中途 dangling（handover §6.1）→ **待用户裁决** |
| M4 | 文件数据：`a_ops` + extent 树对接 + truncate/punch/fallocate + `i_blocks` 更新 + **`setattr`** | **下一步** |
| M5a/M5b | dentry 树写入机构（多级 walk + 叶裂/升根/内裂 + 合并/抹匀/根收缩） | ✅ `716e372`/`678a983` |
| M5 | mkdir/rmdir + `dir_name_heap_release` + rename(replace/NOREPLACE/EXCHANGE + 跨目录) + 名字堆 5 组精准压测 | ✅ `8d745ae`/`612626a`/`7447eea`/`c50a1e8` |
| M5残 | link / symlink(`get_link`) / mknod / tmpfile | 待做 |
| M6 | xattr/ACL：4 回调 + `SB_POSIXACL` + `posix_acl_create` + `security.capability` | 待 M5残 |
| M7 | 加固：停机标志（`RFS_SHUTDOWN`）/ 周期回写 / 并发（回写·evict·sync vs 前台）/ fsync 全覆盖 / ENOSPC 干净（~~node_latch~~ 已裁不做） | 待 |
| M8 | 预演最狠测试：`rsync -aHAX` 克隆 + `pacman -Qkk`（T3 前哨） | 待 M4+M5+M6 |
| T1 | xfstests `-g quick`（generic） → **缺口表**（前置：xfstests 工具链进 guest 尚未解决） | 待 M3–M7 |
| T1.5 | 按缺口表逐条补 spec §4 的 **M** 项，重跑 T1 至收敛 | 待 T1 |
| T2 | 压力：`fsx`/`fsstress`/`fs_mark`，**每轮 audit** | 待 T1.5 |

> **子阶段的目标/出口判据/依赖/需求映射 + 缺口分析**详见交接文档 `ravenfs-linux-ko-handover.md` **§6 缺口分析 / §7 路线图**。

**工具链（host 侧）**：`mkfs/mkfs.ravenfs`（v1.2，root inode=16）· `tools/audit_v1.2/`：`audit_v1.2`（递归只读校验 + 叶链校验）· `mkcontent` / `mkbigdir`（内容/大目录靶子）· `dents_dump`（getdents64 小缓冲续读压测）· `linux/tools/check_super.py`（SB 快速校验）。

**验证环境**：独立 QEMU VM（`linux/vm/`，ext4 金标准 + ESP）——详见 `ravenfs-vm-env-notes.md` 与交接文档的「构建/测试流水线」。

### 2.1 T3「完整安装」判据
- [ ] `modprobe ravenfs` 后 `/proc/filesystems` 出现 ravenfs
- [ ] `mkfs.ravenfs` 目标盘成功；`mount -t ravenfs` 成功
- [ ] 构建 Arch 根：**零致命错误**（`rsync -aHAX` 克隆 或 `pacstrap`）
- [ ] **ESP 上 kernel + initramfs 就绪且含 `ravenfs.ko`**（⚠️ **必须**：没有引导器能读 ravenfs → 内核与 initramfs **只能在 ESP/FAT 上**）；而 **bootloader 与 ESP 本身在装 ext4 根时已由标准流程装好（矩阵之外）** → **T3 只“加一条 entry”，不重装引导器**
- [ ] `loader/entries/*.conf` 的 `options` 指向 **ravenfs root**（`root=PARTUUID=… rw rootfstype=ravenfs`）；`ravenfs.ko` 已进 initramfs（`MODULES=(ravenfs)`）
- [ ] `arch-chroot` 能进；`/etc/fstab` 正确
- [ ] **`pacman -Qkk` 元数据全对**（Arch 自带的"端到端 fsck"）
- [ ] **卸载干净**：`umount` 不报错（ravenfs `put_super`/`sync_fs` 的验收）

### 2.2 T4「启动」判据（水到渠成）
- [ ] OVMF → ESP 的 loader → kernel → **落 ravenfs root**
- [ ] `systemd` 到 multi-user；**`systemctl --failed` 为空**
- [ ] `findmnt /` 显示 ravenfs；`statfs` 正确
- [ ] SSH（`-p 2222`）可进；能跑用户态程序；`systemd-analyze blame` 有数

---

## 3. 独立虚拟机系统（本文件的基干）

### 3.1 系统组成（2026-09-25 定稿：**不修补 ISO**）

| 件 | 来源 | 说明 |
|---|---|---|
| **ext4 根** | **测试矩阵之外**用**标准工具**装（不改的官方 ISO 走标准流程，或在宿主侧 `pacstrap` 进镜像） | 金标准 / 救援 / 装配车间；**不是测试对象** |
| **`root-ravenfs.qcow2`** | 在 ext4 根里 `insmod ravenfs.ko` 后 `mkfs` + 克隆/安装构建 | 试验组（被测对象） |
| **`esp.img`** | **装 ext4 根时由标准流程建好**（矩阵之外）；GPT+ESP+FAT32 | **一套** kernel+initramfs（含 `ravenfs.ko`）+ **两条 entries**（仅 `root=` 不同） |
| **UEFI 固件** | `/usr/share/edk2/x64/OVMF.4m.fd`（系统自带） | 不用 VMtest 的资源 |
| **工具链投递** | **9p/virtiofs 共享目录**（`-virtfs local,path=…`） | 零 root、零 ISO、零额外磁盘；**双向**（结果可写回宿主） |

> **ISO 修补路线已作废（2026-09-25 晚）**：既然 ext4 根在测试矩阵之外用标准工具装、且它本身就是装配车间，就**不再需要自包含 live 介质**。
> 历史记录：`xorriso -indev/-outdev -boot_image any keep -map` 注入 ISO 树**已实测可行**（1.6GB 产物、引导装备保留）—— **若将来确需自包含 live 介质可复活**；当前不采用。相关临时产物已清理。

### 3.2 工具链投递（取代 ISO 注入）

宿主侧加：
```bash
-virtfs local,path=/home/PS/.openclaw/workspace/ravenfs-linux-test/share,mount_tag=hostshare,security_model=none
```
ext4 根内：
```bash
mkdir -p /mnt/host && mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/host
#  virtiofs 则： mount -t virtiofs hostshare /mnt/host
ls /mnt/host     # mkfs.ravenfs / fsck.ravenfs / install-ravenfs-module.sh / run-t3.sh
```
→ 宿主与 guest **双向**：工具进去，`dmesg` / audit 报告 / 测试结果出来（**不依赖 SSH 也能收敛数据**）。

### 3.3 磁盘布局（**双根架构**）

```
esp.img              512MB–1GB  GPT+ESP+FAT32  ← loader(2 entries) + kernel + initramfs  （两根共享）
root-ext4.qcow2      20–40GB    ext4           ← 控制组 / 救援 / 装配车间（"金标准系统"）
root-ravenfs.qcow2   20–40GB    ravenfs        ← 试验组（被测对象）
```

| 盘 | 角色 | 为什么必须有 |
|---|---|---|
| **ext4 根** | 控制组 + 救援 + 装配车间 | ① **对照**：性能/行为基线——没有它，异常无法归属（ravenfs bug？还是 Arch 本来就这样？）；② **救援**：ravenfs 起不来时用它挂载检查（**fsck 空实现下的唯一救援路径**）；③ 装配：从它克隆/校验 ravenfs 根 |
| **ravenfs 根** | 试验组 | 被测对象 |
| **ESP** | 共享 | 两条 entries：`arch-ext4.conf` / `arch-ravenfs.conf` → **零成本切换** |

**关键纪律**：
1. **金标准 ext4 Arch 必须用官方安装流程装**（不是 pacstrap 进 ravenfs 再克隆回来）→ 这样"差异只可能来自 ravenfs"。
2. **克隆 = 对照的前提**：`rsync -aHAX --delete`（`-H` 硬链接 / `-A` ACL / `-X` xattr）→ **这本身就是最狠的功能测试**（ACL/xattr/硬链接/稀疏/大文件/setuid 全覆盖）；**不支持 xattr 时它会失败 → 正好暴露 spec §4.6 的缺口**。
3. **对照纪律**：跑性能/行为对照时，两侧必须**同一镜像快照起点 + 同一 QEMU 参数**（用 qcow2 快照/commit 在宿主侧统一）。
4. **默认 fallback 走 ext4**（保证永远能起来）。
5. 无 sudo 建 ESP：`mformat`（mtools，出自 `PS_git/OS_pj_uefi/host_tools/mtools/`）写 FAT；分区表 `sgdisk -n 1:2048:+512M -t 1:ef00`（需装 `gptfdisk`，或用 guest 内工具）。

**与 hypervisor 层校验的关系（互补，不替代）**：宿主可直接对 `root-ravenfs.qcow2` 跑离线 `mkfs`/`audit`/`fsck -n`、做快照回滚——这是"上帝视角的静态校验"；但**做不了"同构 Arch 用户态的运行时对照"** → 所以 **ext4 根不可省**。

⚠️ **ESP 上必须放 kernel + initramfs**：因为**没有引导器能读 ravenfs**，`/boot` 放在 ravenfs 里对引导毫无用处。

### 3.4 QEMU 启动（抄 VMtest 思路，资源全换）

```bash
# 阶段①（测试矩阵之外，一次性）装 ext4 根：临时挂**原版** ISO 走标准流程
# 阶段②/③（日常）：三盘常驻 + 工具共享
SHARE=/home/PS/.openclaw/workspace/ravenfs-linux-test/share
qemu-system-x86_64 \
  -machine q35,kernel-irqchip=split -smp 6 -m 8192 \
  -cpu host,+invtsc,tsc-freq=2995200000,+x2apic \    # 有 /dev/kvm；否则 -cpu max,+x2apic
  -bios /usr/share/edk2/x64/OVMF.4m.fd \
  -drive file=root-ext4.qcow2,format=qcow2,if=none,id=ext4  -device nvme,serial=cafebabe,drive=ext4 \
  -drive file=root-ravenfs.qcow2,format=qcow2,if=none,id=rfs -device nvme,serial=deadbeef,drive=rfs \
  -drive file=esp.img,format=raw,if=none,id=esp              -device nvme,serial=beefdead,drive=esp \
  -virtfs local,path=$SHARE,mount_tag=hostshare,security_model=none \
  -device virtio-net-pci,netdev=net0 -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -display none -serial stdio
```

与 VMtest 的**有意差异**：
- **去掉** `intel-iommu` / `vfio-pci`（Linux FS 测试不需要，且拖慢 I/O）
- **去掉** `-s -S`（要能真跑）；**加** `hostfwd=tcp::2222-:22`
- **保留** `-cpu host,+invtsc,tsc-freq=2995200000,+x2apic` —— ⚠️ **不只是 SparrowOS 的时序前提**：ravenfs 的 `mdata_piece` TTL 也用 TSC，Linux 侧同样需要稳定 TSC
- **保留** `q35,kernel-irqchip=split` / `-smp 6` / `-m 8192`
- 固件选项：`-bios OVMF.4m.fd`（合并镜像，**NVRAM 不持久** → 依赖 ESP 的 **fallback 路径 `/EFI/BOOT/BOOTX64.EFI`**）；要持久 NVRAM 条目就改用 `-drive if=pflash,readonly=on,file=OVMF_CODE.4m.fd` + 可写副本 `OVMF_VARS.4m.fd`。
  → **推荐走 fallback 路径**：把 loader 装到 ESP 的 `/EFI/BOOT/BOOTX64.EFI`，OVMF 无需 NVRAM 条目即可引导。

### 3.5 三阶段流程（与 T3/T4 对齐）

```
① 装「金标准」（**测试矩阵之外**，标准工具）：官方流程把 Arch 装进 root-ext4.qcow2——
   **ESP 分区 + UEFI bootloader 同一趟装好** → 系统可启动、工具链持久化
② 造「试验组」（T3，在 ext4 根里）：mount 9p 共享 → install-ravenfs-module.sh（insmod）
   → mkfs.ravenfs root-ravenfs.qcow2
   → rsync -aHAX --delete 从 ext4 根克隆到 ravenfs 根   ← 最狠的功能测试
   → **加一条 loader entry**（arch-ravenfs.conf，root= 指 ravenfs）；
     刷新一次 initramfs 把 ravenfs.ko 塞进去（**两根共用同一套 kernel+initramfs**）
   → **T3 完成判据 = §2.1**
③ 启动（T4）：从 ESP 选 ravenfs entry（或改 loader.conf default）
   → kernel 挂 ravenfs root → systemd → multi-user → ssh 2222
   → **§2.2 判据；T3 做好则水到渠成**
```
> 旁证路线（可选）：从 live 环境直接 `pacstrap` 进 ravenfs（不经 ext4 克隆）——但**失去"金标准对照"与克隆测试**，不作主线。

### 3.6「断奶」收尾验证（单根证伪）

双根长期有"靠 ext4 兜底、掩盖问题"的风险 → 收尾必须做一次**单根实战**：
- ESP **只留** `arch-ravenfs.conf`（删/禁 ext4 条目）+ **拔掉 ext4 盘**；
- 从零启动，T3/T4 判据全绿 → 才算**真验收**。

> **结论：双根为主（对照 + 救援 + 装配），单根作为收尾的"断奶"证伪步骤——两者不矛盾。**

### 3.7 双根切换：bootloader entries + kexec（2026-09-25 定）

**两套机制，定位不同**：

| 机制 | 定位 | 说明 |
|---|---|---|
| **loader entries**（默认路径） | **常规 + 救援** | ESP 上**一套 kernel+initramfs**（含 `ravenfs.ko`）+ **两条 entries 仅差 `root=`**；`loader.conf` 设 `timeout 3` + `default arch-ext4.conf` → **新根起不来时永远能退回 ext4** |
| **kexec** | **对照实验切换器 + 崩溃工具** | 同一 VM 实例内切换，**排除固件/设备重初始化这个变量**（符合 §3.3 对照纪律）；脚本化、秒级 |

**kexec 用法**（UUID 不同不是问题，反而是解——因为两根共用同一套 kernel+initramfs，切换只改 `root=`）：
```bash
kexec -l /boot/vmlinuz-linux --initrd=/boot/initramfs-linux.img \
      --append="root=PARTUUID=$(blkid -s PARTUUID -o value /dev/nvme1n1) rw rootfstype=ravenfs"
kexec -e
```

**三条纪律**：
1. ⚠️ **切换前必须 `sync`（`sync; syncfs /`）** —— ravenfs 位图是**全量内存态**（§4.2），不落盘就丢，而**没有 fsck 可重建** → 直接退化成"必须重装"。**kexec = 一次非干净关机，故它同时是位图持久化的验收器**（这也是它可作崩溃工具的根据：kexec 不跑 `put_super`）。
2. ❌ **kexec 不能当救援**（要求当前内核健康才切得动）→ **默认路径必须仍是 bootloader**（有固件菜单兵底）。kexec 只在"两根都已验证可启动"后用于循环对照。
3. `kexec-tools` 在 **guest 内装**（guest 里是 root，**无需宿主 sudo**）；需内核 `CONFIG_KEXEC`（Arch 官方内核有）；**不得开 secure boot**（否则 `kexec_file_load` 会被拒——我们本来也不用）。

---

## 4. 崩溃/恢复矩阵（**随 T5 暂缓**，但 fsck 决定的连带后果立即生效）

> **决定（2026-09-25）：`fsck` 暂时空实现**（只查不修）。理由（原话）：*"有问题优先考虑我们自己的实现 bug，而不是新开一个不良定义问题大坑。"*

**连带后果（影响 T3/T4 的实现，不是 T5 的事）**：

| # | 后果 | 立即要做的动作 |
|---|---|---|
| 1 | **"位图是派生数据、可重建"的退路失效** | **位图必须持久化正确**：每次 `alloc/free` 标脏 + **`sync_fs` 落盘**。从"可选优化"升级为 **必需** |
| 2 | 孤儿块 / 孤儿 inode **永久泄漏** | 长跑有天花板（空间只减不增）；T5 的 soak 要标注 |
| 3 | **验收目标换向** | 从"崩溃恢复"转为 **"不产生损坏"** → **写序 + flush 从"要定"变成"关键路径"**（D-C1） |
| 4 | **替代 fsck 的内核惯例（框架优先）** | 自检发现不一致 → **内部「停机」标志，所有操作返 `-EIO`**（xfs `force shutdown` 惯例）→ 管理员**彻底 umount + 重新 mount**（可 ro）。**不做在线 remount-ro**（裸置 `SB_RDONLY` 是内核明指的反模式，见 spec 注 R-LM-09）。`report_error` 仅作通知。且**自检失败 = "我们自己 bug"的信号**，与优先级一致 |
| 5 | `fsck -n` 值得保留 | **只查 + 报告**成本极低（`audit_filesystem` 直接搬），且它正是"我们实现 bug"的探测器 → 只 stub 修复半部 |

**崩溃点矩阵**（待 T5 启用；届时每轮跑 **`audit_filesystem`（只读）→ `pacman -Qkk`**，无 fsck 可跑）：
1. 分配后 / inode 提交前（不变量 25 ①→③）= 孤儿块
2. 数据落盘后、节点更新中（8K 节点撕裂）
3. unlink 完成 / `link_count--` 前 = 计数不一致
4. rename 中途（I2 声明不承诺原子）
5. 位图脏行未落盘 → **无工具可修 = 必须重装**（故 #1 的持久化是硬的）
6. 崩溃时仍有 unlinked-open 文件 → 孤儿
> **验收目标（诚实版）**：不是"不丢数据"，而是 **"不产生损坏 / 不放大损害"**。

---

## 5. 性能基线（**暂缓，另议**）

工具 `fio`/`bonnie++`/`dbench`/`iozone`；对照同 VM 同盘位 **ext4**；启动时间 `systemd-analyze blame`。
**前置**：先写死可接受倍率（例：顺序 ≥ ext4 的 70%、随机 4K ≥ 50%），否则"够不够快"永远扯不清。

---

## 6. 现有资产缺口（现场勘察结论）

| 项 | 现状 | 结论 |
|---|---|---|
| `mkfs.ravenfs` | ✅ 已可执行（`mkfs/mkfs.ravenfs`） | 可用 |
| `fsck.ravenfs` | ❌ 全树无 fsck 源码 | **空实现**（保留 `-n` 建议） |
| `audit_filesystem` | ✅ 已实现（只读，41KB 的 `audit.cpp`） | 当检查器 + 搬成 `fsck -n` |
| `ravenfs.ko` | ❌ 未开始（`ravenfs/linux/` 只有一行 TODO 的 Makefile） | **主体工作** |
| ISO 修补链 | ✅ **已作废**（不需要） | `libisoburn` 已装；验证过但当前不采用 |
| OVMF | ✅ 系统自带 | 不用 VMtest |
| xattr / `fallocate` / `link` / `rename` / `readdir` / `getattr` / `setattr` | ❌ 设计缺或未实现 | 第一约束缺口表 → **spec §4** |

---

## 7. 排期（本文件范围内）

```
① 装金标准 ext4 根（测试矩阵之外，标准工具）        ← 环境准备
T0 静态（mkfs + mount RO + audit）                 ← 最快闭环
T1 xfstests -g quick                                ← 缺口表自动生成
  ↑ 此处逐条补 **spec §4** 的 M 项（link/rename/readdir/getattr/setattr/fallocate/xattr）
T2 fsx/fsstress + audit ×N
T3 完整安装（insmod → mkfs → 克隆构建 ravenfs 根 + 装 bootloader）  ← §2.1 判据
T4 从 ESP 启动 ravenfs 系统                          ← 水到渠成，§2.2 判据
—— 以下另议：T5 混沌/崩溃矩阵 · T6 性能基线 · 单根断奶验证 ——
```

**顺序纪律**：**在 T1 的缺口表出来之前，不要开始 T3** —— 否则会在 Arch 里用"莫名其妙的安装失败"重学一遍 xfstests 早就能精确告诉你的东西。
