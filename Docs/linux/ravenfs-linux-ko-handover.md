# RavenFS → Linux KO：**交接文档**（新会话从这里读起）

> 2026-09-26 深夜 · Raven · 目的：**上下文清洗** —— 新会话只读本文 + 下面列的三份权威文档即可接手，无需翻历史会话。
> 状态一句话：**`ravenfs.ko` 只读路径已跑通（M0–M2.5 全部实测通过），T0 达成；下一步 M3（写路径）。**

---

## 0. 先读哪些（真值源与优先级）

| 文档 | 角色 | 冲突时 |
|---|---|---|
| `Docs/linux/ravenfs-linux-ko-requirements-spec.md` | **第一约束需求规格（权威契约）**，含 §4.11.1/§4.11.2 缓存与 TTL 的基调修订 | **以它为准** |
| `Docs/linux/ravenfs-linux-ko-acceptance-plan.md` | 验收纲领（T0–T4 阶梯、环境、§2.0 进度表） | 服从 spec |
| `Docs/disk_layout_v1.1.md` / `Docs/disk_layout_v1.2_inode.md` | 盘面契约（v1.2 = 语义重定义版；§5 xattr 抄 ext4；§7 不变量） | 盘面以 v1.2 为准 |
| `Docs/linux/ravenfs-vm-env-notes.md` | 测试 VM 环境实录（含 §6 血泪坑） | 环境以它为准 |
| `Docs/KERNEL_DISCIPLINE.md` / `Docs/DISCIPLINE.md` | 项目纪律（AI 协作/思维对齐） | — |

代码真值：`include/disk_struct_v1.2.h`（盘面 C/C++ 双兼容头，**唯一结构真值**）、`include/magics.h`（节点魔数）、`mkfs/`（产盘）。

**冲突原则（用户定）**：god（`juvenile_god/`，v1.1）只是**理念参考**；**ko 用 v1.2 盘面 = 代码生殖隔离**。spec 是权威。

---

## 1. 代码地图

```
ravenfs/
├── linux/                     ← ko 本体（kbuild 外部模块，C）
│   ├── Makefile               obj-m / ccflags -I$(src)/../include / KDIR 包装
│   ├── ravenfs.h    (269 行)  结构 + 宏 + 跨文件原型 + DEFINE_FREE(session/msession)
│   ├── super.c      (708 行)  fs_type/fs_context(get_tree_bdev) + SB 校验 + 几何 +
│   │                          iget/read_inode + getattr/statfs +
│   │                          建/毁 三个缓存 + mount option(row_size=) + i_blocks
│   ├── dentry.c     (387 行)  lookup(hash 下行) / readdir(叶链 cookie) / 名字物化(名字堆) /
│   │                          d_type 探测；**节点读已改走 mdata session**
│   ├── inode_cache.c(394 行)  disk_inode_cache_mgr：分片 xarray + 片级睡眠 rwsem +
│   │                          refcount + 行=伙伴系统页(order) + TTL reap/释放
│   ├── mdata_cache.c(362 行)  mdata_piece_cache_mgr：**同一技术路线，granule 固定 8K**；
│   │                          `ravenfs_read_node()` 即「mdata session 取→拷贝→put」
│   └── cache_ttl.c  ( 98 行)  **自写 TTL 淘汰器**（delayed_work；参数对齐 god：2s）
├── linux/tools/check_super.py         用户态 SB/几何/root inode 快速校验（与 ko 解析同构）
├── tools/audit_v1.2/                  **v1.2 只读契约校验员 + 靶子工具**（host 侧）
│   ├── audit_v1.2.cpp                 递归先序 + 环检测 + 叶链校验 + 名字堆/xattr/rdev/
│   │                                  link_count/位图对账；**只读、日志只走 stdout**；退出码 0/1/2
│   ├── mkcontent.cpp                  小内容靶子（含 xattr 块 / 长名 / 各类型）
│   ├── mkbigdir.cpp                   多叶大目录靶子（正确 hints + prev/next 叶链）
│   └── dents_dump.cpp                 getdents64 **小缓冲续读**压测器（COUNT/UNIQUE/DUP/MISSING）
├── mkfs/                              mkfs.ravenfs（v1.2；`RAVENFS_ROOT_INODE=16`；`--inode-count=`）
└── include/disk_struct_v1.2.h         盘面真值头
```

**临时区（工作区，非仓库）**：`~/.openclaw/workspace/ravenfs-linux-test/tmp/`
= 测试镜像（`empty/test/content2/big5000/t2(尾部行)/bad_*`）+ 测试脚本 `m1c3.sh`/`m22test.sh`/`m23test.sh`/`m25test.sh` + audit 日志 `audit_v12_*.log`。

---

## 2. 构建 / 测试流水线（**最关键一节**）

### 2.1 宿主（仅编译，测不了）
```bash
cd /home/PS/PS_git/OS_pj_uefi/ravenfs/linux && make          # 用 /lib/modules/7.2.3-1-vfio/build
```
- 宿主内核 = **7.2.3-1-vfio**；产物 vermagic 只配宿主。
- **宿主 sudo 不可用**（用户红线）⇒ 宿主编的 ko **不 insmod**，只在 guest 里跑。

### 2.2 guest（编译 + insmod + 挂载 + 测试）—— 标准动作
```bash
# ① 投递源码 + guest 内就地编译（tar over ssh，避开 9p 大批量读挂死的坑）
bash ~/.openclaw/workspace/ravenfs-linux-test/tmp/m1c3.sh
#    内部：ssh-pipe tar linux/ include/ mkfs/ → /opt/ravenfs → make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux
# ② 跑回归
ssh -i ~/.ssh/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/tmp/kh_vm -o BatchMode=yes -p 2222 raven@localhost 'bash -s' < <脚本>
```
- guest 免密：宿主公钥已装 `raven`；guest 内 `raven` 有 **NOPASSWD sudo**（自动化用，仅测试 VM）。
- guest 运行内核 = **7.2.6-arch2-1** ⇒ **ko 必须在 guest 内编译**（vermagic 对齐）。
- 手动挂载模板：
```bash
sudo losetup --show -f -b 4096 /tmp/xxx.img        # 必须 -b 4096（页 4K）
sudo mount -t ravenfs [-o row_size=8k] /dev/loopN /mnt/rfs
sudo ls -laU /mnt/rfs; sudo stat -f /mnt/rfs; sudo dmesg | tail
```
- **现状：只读挂载**（`ravenfs_get_tree` 里强制 `SB_RDONLY`；写路径 M3 才做）。

### 2.3 环境坑（再踩就是浪费 token）
1. **`pacman -Sy` 会把 `linux-headers` 拉到最新（7.2.7）≠ 运行内核** → 从归档装对齐版：
   `sudo pacman -U https://archive.archlinux.org/packages/l/linux-headers/linux-headers-7.2.6.arch2-1-x86_64.pkg.tar.zst`
2. guest minimal `base` **无 make/gcc** → `sudo pacman -S --needed make gcc binutils`（+ `xxhash` 供 audit/mkcontent）。
3. **9p 大批量读会挂死**（`cp -a`/`tar` 卡 `D` 态；单文件 `cat` 正常）→ 投递一律 **ssh-pipe tar**，且**排除 `linux/vm`**（qcow2 表观 32G）。
4. `/sys/block/*/device/serial` 内容**带尾部空格** → `tr -d '[:space:]'` 再比较；**认盘一律按 QEMU serial**（`deadbeef`=ravenfs 盘 / `cafebabe`=ext4 / `beefdead`=esp）。
5. 检查 `stat -f` 的 `ID:` 与 `dmesg` 的 `root_ino=16` 判断挂载对象对不对。

---

## 3. 契约速查（当前已实现）

### 3.1 盘面（v1.2）
- inode **256B**；第一字 `type:6|is_data_direct:1|i_flags:57`；`mode:12|xattr_ptr:52`；rdev 在 `inline_data[0:4]`（小端 u32）。
- **root inode 索引 = 16**（mkfs 硬编码；不再是 0）。
- B+tree 节点 **8K = 2×4K**；节点元数据 `magics[4](32B)|parent_ptr(8B)|ptrs[3](24B)`；`ptrs` 四态（非根叶：前驱/后继；根节点：`..` inode）。
- xattr：**单块（4K）自描述**，条目**抄 ext4**（显式 `e_value_offs` + 值从块尾向下 + 首个 u32==0 终止）；净可用 4060B。
- `s_maxbytes = 1<<52`；`s_time_gran=1`；`s_magic = SUPERBLOCK_MAGIC = 0x5241564E ("RAVN"，**与 mkfs 同源**)`；`blk0_off` 断言 = 4096。

### 3.2 两层缓存（`linux/inode_cache.c` / `mdata_cache.c`）
| | disk_inode_cache | mdata_piece_cache |
|---|---|---|
| 记录 | 盘 inode（256B，16/4K） | B+tree 节点（**8K 固定**） |
| 行尺寸 | **mount `row_size=` 4K/8K/16K/32K/64K** | 固定 8K（2×4K） |
| 键 | `line=(inum>>row_bits)`，片内掩码 | `blk>>1`，片内掩码 |
| 分片 | `w0=min(7, bits-row_bits)`，`n_slices=2^w0` | `w0=7`（或更小），同构 |
| 存储 | 专用 `kmem_cache`（`SLAB_RECLAIM_ACCOUNT|SLAB_ACCOUNT`） | 同 |
| session | `RAVENFS_SESSION(sb, inum, var)`（`__free`） | `RAVENFS_MSESSION(sb, blk, var)` |
- 行内槽位 = `inum & (inodes_per_row-1)`（session 里带 `off_mask`）。
- **尾行/尾节点**：按 array/`total_blocks` **截断读 + 零填充**。
- **内存**：行缓冲 = **伙伴系统页块**（`alloc_pages`，`order=log2(row_size/4K)`；8K 节点 order=1），直映射 `page_address()` 访问；淘汰/卸载 `__free_pages` 归还（2026-09-26 深夜改；不再经 slab）。
- **锁**：分片 `xarray` + **片级睡眠 `rw_semaphore`** + `refcount`（1=缓存基准）。读锁 ⊃ 查表+inc；写锁 ⊃ 摘表+释放。**M2.4 决议：不做 node_latch**（片级 rw_semaphore + 专属 TTL 已足；关闭 spec §4.9 未定项）。
- **⭐ 锁纪律（2026-09-26 深夜）**：睡眠分配 + 块 I/O **全在分片锁外**（装载=锁外分配+读盘 → 临界区双检插入）。防「分配→direct reclaim→重入本缓存取锁」类自锁死（压力 shrinker 已于同轮移除；纪律保留作护栏）。

### 3.3 驱逐/回写（三条路）
1. **TTL（主）**：`cache_ttl.c`，`ttl=2s`/周期 1s；条件 `ref==1 ∧ !dirty ∧ now-stamp>ttl`；`stamp` 每次 get 刷新。**只清 clean 行**。
2. ~~压力兜底~~ → **压力 shrinker 已移除（2026-09-26 深夜）**：冷集淘汰只由 TTL 承担。
3. **折构/卸载**：`put_super` → **先 `ravenfs_ttl_stop`** → `mdata/icache destroy`（内部 `flush_all`）。
- **回写权威（M3）**：`write_inode` = **logical inode → mgr 行同步**（只推 VFS 字段：uid/gid/mode/nlink/size/时间戳/rdev；**盘专有字段不动**）+ 行标 dirty —— **不落盘**（logical inode 与盘 inode 解耦，写不出盘记录）。落盘 = `flush_all`：① `sync_fs` ② umount ③ `fsync` 单行。**TTL 永不写盘（只摘 clean 行）。**
- 源码事实（7.2.3）：内核 **TTL 只在回写层**（`dirty_expire_centisecs`=30s / `dirty_writeback_centisecs`=5s / `dirtytime_expire_seconds`=12h）；**回收无 TTL，只有压力**（`shrink_slab` 仅 vmscan 触发）。

---

## 4. 里程碑与证据（复现要点）

| 里程碑 | 证据（怎么复现） |
|---|---|
| M0/M1 | guest `insmod` → `/proc/filesystems` 有 ravenfs；空盘 mount + `ls -laU` 只有 `.`/`..`；`stat -f`（`Type 0x5241564e`, `Namelen 552`） |
| M2 | `content2.img`（`mkcontent … long`）：7 型齐全 + 41B 长名 lookup/readdir + blocks(inline 0/8、direct 16、extent 40、chr rdev 1,3) + 未命中 → ENOENT |
| M2.1 | `big5000.img`（`mkbigdir`）：`dents_dump /mnt/rfs 256` → `COUNT=5003 UNIQUE=5003 DUP=0 MISSING=0`；`ls -U`=5001 |
| M2.2 | 5 档 × big5000 全 entries=5001；`inode_count=272`+`N=240` 尾行专项 4k/64k 均 OK；`-o row_size=5k` 被拒（dmesg `invalid row_size=`） |
| M2.3 | dmesg 两行 `inode cache:` / `mdata cache:`（含 order）；缓存占用经 dmesg TTL reap + `/proc/buddyinfo` |
| M2.5 | 无压力静置 5s → dmesg `ttl reap: freed=354 total=354 (ttl=2s round=2)`；重读仍 5001 |
| M2.5b | 行缓冲改**伙伴系统页**（`order=log2(row/4K)`，直映射）+ **锁重入死锁根治**：`m25b.sh` → M1/M2 全绿、`ttl reap freed=354`、重读 5001、buddyinfo 页归还可见 |
| T0 | host `tools/audit_v1.2/audit_v1.2 <img>` → `RESULT: PASS`（空盘与 big5000 均 PASS） |

**脚本**：`m1c3.sh`（投递+编译）、`m22test.sh`（5 档+尾行）、`m23test.sh`（缓存+slab+shrinker，**旧 slab 版证据，已被 m25b 取代**）、`m25b.sh`（**当前权威**：buddy 页缓冲 + M1/M2 + TTL 归页）、`m25test.sh`（TTL 旧版）——都在 `~/.openclaw/workspace/ravenfs-linux-test/tmp/`。

---

## 5. 未决 / 待裁（新会话需向用户确认）

1. ~~**M2.4 node_latch**~~ → **已裁：不做**（片级 `rw_semaphore` + 专属 TTL 已足；spec §4.9 未定项关闭）。
2. ~~位图回写策略~~ → **已裁**：位图 = **全量内存分片**（学 god `bitmap_slices`），`sync_fs`/`put_super` **全量刷回**（不做 bh 增量）。
3. ~~压力 shrinker~~ → **已裁：移除**（冷集淘汰只由 TTL 承担）。
4. ~~`s_magic`~~ → **已裁**：`= SUPERBLOCK_MAGIC = 0x5241564E`（**与 mkfs 同源**）。

---

## 6. 里程碑路线图：**M2.5 → T1 → T2**（每阶段含出口判据 / 依赖 / 需求映射）

> 当前在 **M2.5 完成**（只读路径 + 缓存层 + TTL）。以下是到 T2 的完整子阶段划分。
> 纪律：**每阶段都必须"可测"才算完成**（host `audit_v1.2` / guest 挂载实测 / dents_dump / xfstests），且**阶段内禁止越级**（T1 缺口表出来前不碰 T3）。

### M3 元数据写路径（写基础设施 + 最小写操作）
- **内容**
  1. `write_inode(row)` = 落盘该行（4K/8K）+ 清 `dirty`；**与 `mark_inode_dirty(inode)` 同置同清**（§3.3）。
  2. `sync_fs` → `flush_all`；文件 `fsync`/`fdatasync` 语义接通（含其 inode 记录）。
  3. `dirty` 回写：`flush_all` 内 dirty 先回写再摘（现仅 `WARN_ON_ONCE`）。
  4. **位图持久化**：全量内存分片（学 god）；`sync_fs`/`put_super` 全量刷回（依 §5.2 决议）。
  5. **取消强制只读**：允许 rw 挂载（`ravenfs_get_tree` 里的 `SB_RDONLY` 强置去掉），但**只对已实现的写路径开放**。
  6. **最小写操作集**（否则 write_inode 无从验证）：`create`(REG) / `unlink` / dir 的 `size`·`link_count` 维护（含父目录）。
- **出口判据**：空盘 rw 挂载 → `touch` + 写 `inline` 数据 + `unlink`；**audit 全绿**；**umount→remount 后内容仍在**（持久性）；`sync` 前后 dmesg 无 WARN。
- **依赖**：无（当前只读路径已就绪）。
- **需求映射**：R-LM-02/03/04/05、R-SY-01/02/03/04/05/06/07、R-CO-01/03、R-NS-02/04(部分)。

### M3.5 一致性/崩溃窗口验证（不新增功能，只加证据）
- **内容**：按 spec 不变量 20/25 验证写序（"位图亮灯→数据落盘→inode 翻转"；"先删引用→后计数"）；做**非干净卸载**（kexec / `echo b > /proc/sysrq-trigger` 或直接断电模拟）后跑 audit。
- **出口判据**：崩溃后 audit 判据 = **"不产生损坏 / 不放大损害"**（非"不丢数据"）；孤儿块/孤儿 inode 允许存在且可被 audit 标出。
- **依赖**：M3。

### M4 文件数据路径（a_ops + extent 树）
- **内容**：`address_space_operations`（`read_folio`/`readahead`/`write_begin`/`write_end`/`dirty_folio`/`writepages`/`invalidate_folio`/`migrate_folio`）+ `generic_file_read_iter`/`write_iter`/`mmap`；**extent 树对接**（god `extent_*` 算法移植）；inline↔direct↔extent 三态迁移；`truncate`/`punch_hole`/`fallocate`(`KEEP_SIZE`)/`ZERO_RANGE`；`i_blocks` 随写更新。
- **出口判据**：`dd`/`cat`/`head` 读写往返一致（含 >160B 跨态、>4K、稀疏洞）；`stat.blocks` 与实际分配吻合；audit 全绿。
- **依赖**：M3。
- **需求映射**：R-FD-01/01/02/03/04/05/07/08/09、R-MD-01(blocks)、R-SY-04。

### M5 命名空间写（全量目录操作）
- **内容**：`mkdir`/`rmdir`/`link`/`symlink`+**`get_link`(fast/slow)** ⚠️T3 前置/`rename`(NOREPLACE+EXCHANGE)/`mknod`(4 型)/`tmpfile`；**dentry B+tree 的插入/分裂**（含**叶链 `ptrs[1]/[2]` 维护**、根叶→根内部的转换、`hints` 上界维护）；目录 `size/link_count` 同步；`-ENOTEMPTY`/`-EEXIST`/`-EPERM`(目录硬链) 等错误语义。
- **出口判据**：`mkdir -p`/`mv`/`ln`/`ln -s`/`readlink`/`mknod` 往返；大目录**分裂后** `dents_dump 256` 仍 `DUP=0 MISSING=0`；audit 叶链校验 **`chain_bad=0`**。
- **依赖**：M3（写路径）；**这是 T1 的主要缺口来源**。
- **需求映射**：R-NS-02..09、R-MD-02/07、R-IO-01b(节点脏页)。

### M6 xattr / ACL / capability
- **内容**：4 回调（get/set/list/remove）+ `s_xattr` handlers；**`SB_POSIXACL`** + `get_inode_acl`/`set_acl` + **默认 ACL 继承（`posix_acl_create`）**；`security.capability`；xattr 块分配/释放（bitmap 认领）；`listxattr` 尺寸语义/`-ERANGE`/`-E2BIG`。
- **出口判据**：`setfattr`/`getfattr`/`setfacl`/`getfacl`/`setcap`/`getcap` 往返；**`cp -a`/`rsync -aHAX` 保留 ACL+xattr**；audit xattr 块校验通过。
- **依赖**：M3、M5（e_name_index 与命名空间路由）。
- **需求映射**：R-XA-01..04、R-NS-12(缺省 permission + SB_POSIXACL)。

### M7 加固（并发/停机/全覆盖同步）
- **内容**：**停机标志（`RFS_SHUTDOWN`，自检失败 → 所有入口 `-EIO`，拒绝在线 remount）**；~~node_latch~~（**已裁不做**）；跨线程真竞态面（bitmap/分配器、回写/evict/sync vs 前台）；`fsync` 全覆盖（数据+元数据）；错误路径（ENOSPC 干净返回）。
- **出口判据**：`fsstress` 小规模不炸；`echo 3 > drop_caches` + 并发读写下 audit 仍绿；人工注入不一致后入口返回 `-EIO`。
- **依赖**：M3–M6。
- **需求映射**：R-LM-09、R-CO-02/04、R-SP-07、R-SY-05。

### M8 预演"最狠的功能测试"（T3 前哨，可选但强烈建议）
- **内容**：`rsync -aHAX` 从 ext4 金标准**克隆到 ravenfs 挂载点**（覆盖 ACL/xattr/硬链接/稀疏/大文件/setuid），再 `pacman -Qkk` 比对元数据。
- **出口判据**：克隆零致命错误；`pacman -Qkk` 只报**预期 backup file**；audit 全绿。
- **依赖**：M4+M5+M6（缺一不可）。
- **为什么值得**：这就是 T3 的核心内容，提前在**开发机上**跑，避免到 T3 才发现缺口。

### T1 `xfstests -g quick`（generic 组）→ **缺口表**
- **前置（尚未解决，需先办）**：**xfstests 工具链如何进 guest**（需 git clone/编译；guest 网络可用但"近零下载"原则要权衡）——可选：宿主下载源码 → tar 投递 → guest 内编译；或宿主预编 → 投递。
- **出口判据**：跑出通过率 + **失败清单 = 第一约束缺口表**（区分 fail vs skip：O_DIRECT/FITRIM/SEEK_HOLE 等降级项会 skip）。
- **依赖**：M3–M7（写路径齐全）；**M8 强烈建议先做**。

### T1.5 按缺口表补课
- 逐条补 spec §4 里标 **M** 的项（link/rename/readdir/getattr/setattr/fallocate/xattr…），每条补完重跑 T1 相应 case。
- **出口判据**：缺口表收敛（剩余项皆是 spec 里明确的降级/豁免项）。

### T2 压力（每轮跑 audit）
- **内容**：`fsx`（随机读写/truncate/mmap）、`fsstress`、`fs_mark`；**每轮结束跑 `audit_v1.2`**。
- **前置**：同上（工具链投递）。
- **出口判据**：**无静默损坏**（audit 恒绿），失败可归因。
- **依赖**：T1.5 收敛。

### 之后（本交接不展开）
T3 完整安装（`insmod` → `mkfs` → 克隆构建 ravenfs 根 + loader entry）→ T4 从 ESP 启动到 multi-user（见验收纲领 §2.1/§2.2）。**T5/T6 暂缓。**

## 7. 已知缺口 / 风险 / 坑（截至交接）

- 文件数据读 → `EINVAL`（a_ops/`read_iter` 未接，M3/M4）。
- 符号链接 `readlink` → `EINVAL`（缺 `.get_link`；**T3 前必补**）。
- `i_blocks` 口径已定：**数据块 + 元数据块**（dentry/extent 节点×2、name_heap 位图+数据面、xattr 块 1）。
- 行缓冲已改**伙伴系统页**（非 slab）：观测 = dmesg TTL reap + `/proc/buddyinfo`；`/proc/slabinfo` 不再有 `ravenfs_inode_row`/`ravenfs_mdata_node`（`ravenfs_inode_cache` 仍在 = VFS inode slab）。
- **已根治：分片锁重入死锁**（装载=锁外分配+I/O）——见 §3.2 锁纪律。
- `pr_info_ratelimited` 的 TTL 日志会节流，不是漏打。
- 宿主/guest 内核版本不同 ⇒ **永远 guest 内编译再测**。

---

## 8. 纪律（照旧，勿忘）

- **三约束**：① 满足 Arch 用户态规格 ② 能用内核成熟框架就用 ③ **撞上"非良定义问题"第一时间上报用户**（进度可慢、隐患不留）。
- **成本审计**：大流量/长耗时先量化（`COST-AUDIT-DISCIPLINE.md`）。
- **红线**：宿主 `sudo` 不给；不用录存口令提权（**guest 测试 VM 凭证除外，用户已授权**）。
- 用户风格：直接、要**思维可见**（设计意图/推理外显）、讨厌废话；**有不良定义点主动上报**。

---

> **一句话交接**：读 spec §4.11.1/§4.11.2 → 跑 `m1c3.sh` 编译进 guest → `m25test.sh` 看 M1/M2/TTL 全绿 → 再动 M3。
