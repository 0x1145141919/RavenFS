# RavenFS → Linux KO：**交接文档**（新会话从这里读起）

> 2026-09-27 · Raven · 目的：**上下文清洗** —— 新会话只读**本文 + §0 列的权威文档**即可接手，无需翻历史会话。
> **2026-09-27 重写（鸠占鹊巢）**：旧版停留在 M2.5；此后 M3/M3.x/M5 全部落地，旧版与两份移植 notes
> （`m3-port-notes.md` / `m5-dentry-tree-notes.md`）已**作废/移除**——它们的「必需结论」已折进本文
> （§4/§5/§6），「与现状冲突的风险内容」一并铲除。**本文是 ko 侧唯一交接真值。**

---

## 0. 状态一句话 + 先读哪些

**状态**：`ravenfs.ko` 已跑通 **只读路径（M0–M2.5）+ 元数据写基础设施（M3.x）+ 命名空间写
（M5：create/unlink/mkdir/rmdir/rename）**，全部 guest 实测通过；**T0 达成**。
**下一步 = M4 文件数据路径**；距 **T2** 仍缺 `link/symlink/mknod/tmpfile/setattr/xattr/ACL` + 加固（详见 §7）。

**真值源与优先级（冲突时自上而下服从）**：

| 文档 | 角色 | 冲突时 |
|---|---|---|
| `Docs/linux/ravenfs-linux-ko-requirements-spec.md` | **第一约束需求规格（权威契约）**；含 §4.11.1/§4.11.2（缓存/TTL 基调）、§4.11.3（dcache↔ddentry 一致性纪律） | **以它为准** |
| `Docs/linux/ravenfs-linux-ko-acceptance-plan.md` | 验收纲领（三层约束、T0–T4 阶梯、§2.0 进度表） | 服从 spec |
| `Docs/disk_layout_v1.1.md` / `Docs/disk_layout_v1.2_inode.md` | 盘面契约（**v1.2 = 语义重定义版**；§5 xattr 抄 ext4；§7 不变量） | 盘面以 **v1.2** 为准 |
| `Docs/linux/ravenfs-vm-env-notes.md` | 测试 VM 环境实录（认盘 serial、坑） | 环境以它为准 |
| `Docs/KERNEL_DISCIPLINE.md` / `Docs/DISCIPLINE.md` | 项目纪律（AI 协作 / 思维对齐） | — |

代码真值：`include/disk_struct_v1.2.h`（盘面 C/C++ 双兼容头，**唯一结构真值**）、`include/magics.h`（节点/ SB 魔数）、`mkfs/`（产盘）。

**冲突原则（用户定）**：god（`juvenile_god/`，v1.1 盘面）只是**理念/语义参考**；**ko 用 v1.2 盘面 = 代码生殖隔离**。spec 是权威，god 不照搬结构（缓存/IO 已下沉 Linux）。

---

## 1. 代码地图

```
ravenfs/
├── linux/                     ← ko 本体（kbuild 外部模块，C）
│   ├── Makefile               obj-m / ccflags -I$(src)/../include
│   ├── ravenfs.h    (351)     结构+宏+跨文件原型+DEFINE_FREE(session/msession)+几何内联
│   ├── super.c      (722)     fs_type/fs_context(get_tree) + SB 校验 + 几何 + iget/read_inode
│   │                          + getattr/statfs + write_inode + 建/毁三缓存 + mount option(row_size=)
│   ├── dentry.c     (1086)    **读**：lookup(hash 下行)/readdir(叶链 cookie)/名字物化/ d_type
│   │                          **写**：create / unlink / mkdir / rmdir / rename（驱动层：计数、树调用、下游资产）
│   ├── dentry_write.c (1016)  **树机械**：walk / insert(+叶裂/内裂/升根) / delete(+balance/合并/抹匀/根收缩)
│   ├── name_heap.c  (418)     长名(>40B)外置名的 分配/写/释放 + dir_name_heap_release
│   ├── bitmap.c     (520)     **双位图分片分配器**（全量内存；block 组 + inode 组，单 mgr）
│   ├── inode_cache.c(489)     disk_inode_cache_mgr：分片 xarray + 片级睡眠 rwsem + refcount
│   │                          + 行=伙伴页(order) + TTL reap + writeback/flush
│   ├── mdata_cache.c(406)     mdata_piece_cache_mgr：同路线，granule 固定 8K（= B+tree 节点）
│   │                          + ravenfs_read_node / mdata_drop
│   └── cache_ttl.c  ( 98)     自写 TTL 淘汰器（delayed_work，2s，对齐 god）
├── linux/tools/check_super.py         用户态 SB/几何/root inode 快速校验（与 ko 解析同构）
├── linux/tools/rfsdirchurn.c          **名字堆内存压测器**（5 组逐一精准打穿；见 §3.6）
├── tools/audit_v1.2/                  **v1.2 只读契约校验员 + 靶子工具**（host 侧）
│   ├── audit_v1.2.cpp                 递归先序 + 环检测 + 叶链校验 + 名字堆/xattr/rdev/link_count/位图对账
│   ├── mkcontent.cpp / mkbigdir.cpp   内容/大目录靶子（正确 hints + prev/next 叶链）
│   └── dents_dump.cpp                 getdents64 小缓冲续读压测器（COUNT/UNIQUE/DUP/MISSING）
├── mkfs/                              mkfs.ravenfs（v1.2；`RAVENFS_ROOT_INODE=16`；`--inode-ratio=`/`--inode-count=`）
└── include/disk_struct_v1.2.h         盘面真值头
```

> ⚠ `linux/*.o *.ko` 是**宿主构建残留**（vermagic 只配宿主）→ **不可 insmod，必须 guest 内重建**（§2）。
> 临时测试镜像/脚本放工作区（非仓库），勿混进 `Docs/`。

---

## 2. 构建 / 测试流水线（**最关键一节**）

### 2.1 宿主（仅编译，测不了）
```bash
cd /home/PS/PS_git/OS_pj_uefi/ravenfs/linux && make     # 宿主内核 7.2.3-1-vfio
```
- **宿主 sudo 不可用**（用户红线）⇒ 宿主 ko **不 insmod**，只在 guest 跑。

### 2.2 guest（编译 + insmod + 挂载 + 测试）—— 标准动作
- guest 运行内核 = **7.2.6-arch2-1** ⇒ **ko 必须在 guest 内编译**（vermagic 对齐）。
- 免密 SSH（宿主公钥已装 guest `raven`）：
  ```bash
  ssh -p 2222 -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/tmp/kh_vm \
      raven@localhost        # 口令见 linux/vm/creds/（guest 内 raven 有 NOPASSWD sudo）
  ```
- 就地编译（源码在 guest 的 `/rfssrc`（9p 只读）或 `/opt/ravenfs`；**构建要在可写副本**）：
  ```bash
  make -C /usr/lib/modules/$(uname -r)/build M=<可写源码目录>/linux modules
  ```
- 认盘（**一律按 QEMU serial**）：`deadbeef`=ravenfs 盘 / `cafebabe`=ext4 / `beefdead`=esp。
  读 serial：`tr -d '[:space:]' < /sys/block/nvmeXn1/device/serial`（内容**带尾空格**）。
- 挂载模板：
  ```bash
  mount -t ravenfs /dev/nvme1n1 /mnt/rfs          # rw；`stat -f` 的 ID 应为 0x5241564e、Namelen 552
  ```
- 检查：`grep -qw ravenfs /proc/filesystems`；`stat -f /mnt/rfs` 的 `ID:` 与 `dmesg | grep mounted`（`root_ino=16`）。

### 2.3 环境坑（再踩就是浪费 token）
1. **`pacman -Sy` 会把 `linux-headers` 拉到最新 ≠ 运行内核** → 从归档装对齐版（7.2.6）。
2. guest minimal `base` **无 make/gcc** → `pacman -S --needed make gcc binutils`（+ `xxhash`）。
3. **9p/virtfs 大批量读会挂死**（`cp -a`/`tar` 卡 `D` 态；单文件 `cat` 正常）→ 投递用 ssh-pipe tar 或**只取需要的子目录**（**勿**整仓递归，`linux/vm/disks/*.qcow2` 表观 32G 会撑爆 tmpfs）。
4. 宿主/guest 内核不同 ⇒ **永远 guest 内编译再测**。
5. **重载 ko 必须先 `umount`**（模块被 mount 引用时 `rmmod` 失败），再 `rmmod` → `insmod`。
6. 位图是**全量内存态**：`sync`/`syncfs` 才落盘；**异常断电 = 位图丢失且无 fsck 可重建**（见 §4.4）。

### 2.4 工具投递（零 root）
宿主把静态二进制放进 9p 共享目录（guest 挂 `/host`），或在 guest 内编译。例：
```bash
gcc -O2 -static -o rfsdirchurn linux/tools/rfsdirchurn.c   # 宿主
# guest: /host/rfsdirchurn /mnt/rfs/<空目录>
```

---

## 3. 契约速查（当前已实现）

### 3.1 盘面 v1.2
- inode **256B**；第一字 `word1 = type:6 | direct:1 | i_flags:57`；`mode_xattr = mode:12 | xattr_ptr:52`；CHR/BLK 的 rdev 在 `inline_data[0:4]`（小端 u32）。
- **root inode 索引 = 16**（mkfs 硬编码；**不存在保留区**，0..15 与 17.. 均可分配）。
- B+tree 节点 **8K = 2×4K**；节点元数据 `magics[4](32B) | parent_ptr(8B) | ptrs[3](24B)`。
- xattr：**单块（≤1 FS_block）自描述**，条目抄 ext4（`e_value_offs` + 值从块尾向下 + 首个 u32==0 终止）。
- `s_maxbytes = 1<<52`；`s_time_gran=1`；`s_magic = SUPERBLOCK_MAGIC = 0x5241564E ("RAVN"，与 mkfs 同源)`；`blk0_off` 断言 = 4096；`f_namelen = 552`。

### 3.2 两层缓存（`inode_cache.c` / `mdata_cache.c`，**同一技术路线**）
| | disk_inode_cache | mdata_piece_cache |
|---|---|---|
| 记录 | 盘 inode（256B，16/4K） | B+tree 节点（**8K 固定**）= 名字堆 8K 片 |
| 行尺寸 | mount `row_size=`（4K/8K/16K/32K/64K） | 固定 8K（order=1） |
| 键 | `line=(inum>>row_bits)` | `blk>>1` |
| 分片 | `w0=min(7, bits-row_bits)` | `w0=7`（或更小） |
| 存储 | **伙伴系统页块**（`alloc_pages`，直映射 `page_address()`；**非 slab**） | 同（8K → order=1） |
| session | `RAVENFS_SESSION(sb,inum,var)`（`__free`） | `RAVENFS_MSESSION(sb,blk,var)`（`__free`） |

- 行内槽位 = `inum & off_mask`；尾行/尾节点按 array/`total_blocks` **截断读 + 零填充**。
- **锁**：分片 `xarray` + **片级睡眠 `rw_semaphore`** + `refcount`（1=缓存基准）。**M2.4 决议：不做 node_latch**（片级 rwsem + 专属 TTL 已足）。
- **⭐ 锁纪律**：睡眠分配 + 块 I/O **全在分片锁外**（装载 = 锁外分配+读盘 → 临界区双检插入）。防「分配→direct reclaim→重入本缓存取锁」自锁死。
- `ravenfs_mdata_get` **会睡眠**（`GFP_KERNEL` + `down_read` + 可能 I/O）→ **绝不可在 spinlock 内调用**；`ravenfs_block_alloc`（自旋）与 `RAVENFS_MSESSION`（可睡眠）**不可同处一个不可睡眠临界区**（先分配块、再开会话）。

### 3.3 驱逐 / 回写（三条路）
1. **TTL（主）**：`cache_ttl.c`，`ttl=2s`/周期 1s；条件 `ref==1 ∧ !dirty ∧ now-stamp>ttl`；`stamp` 每次 get 刷新。**只清 clean 行**。
2. **压力 shrinker：已移除**（2026-09-26 决议）——冷集淘汰只由 TTL 承担。
3. **拆构/卸载**：`put_super` → **先 `ravenfs_ttl_stop`** → destroy（内部 `flush_all`）。
- **回写权威**：`write_inode = logical inode → mgr 行同步`（**只推 VFS 字段**：uid/gid/mode/nlink/size/时间戳/rdev；**盘专有字段不动**）+ 行标 dirty。**落盘 = `flush_all`**：① `sync_fs` ② umount ③ `fsync` 单行。**TTL 永不写盘**。
- ⚠ **ko 无周期回写线程**（M3 决议）→ 无人 sync 时 **dirty mdata 行持续累积**（实测 91749 目录 ≈ 735MB）→ 属 **M7** 加固范畴。

### 3.4 一致性纪律（spec §4.11.3 摘要，**必须钉死的契约**）
- 三层：① VFS dcache（**投影**，VFS 自管）② 盘上 ddentry 树（`mdata_piece_cache`，**权威内存态**）③ 盘（`flush_all` 落定）。
- **ko 不碰 dcache**：任何经 VFS 的变名，VFS 两头都改（盘上走 ko 回调 + dcache 由 VFS `d_move/d_delete/d_instantiate`）。
- 契约：**单挂载**（同时刻只有一个写入 mount）+ **线下工具只在未挂载镜像上跑** + **回调返回 0 ⇔ 盘上 ddentry 树已改完**（破了 dcache 无 `d_revalidate`、不可自愈）。
- 实现注意：rmdir 判空用**盘上 `size==0`**（**禁用** `simple_empty()`）；rename 未知 flag → `-EINVAL`；跨目录 rename 要自己改盘上 dir 父指针。

### 3.5 位图分片分配器（`bitmap.c`）要点
- 单 mgr 管**两组**：block 组（块位图）+ inode 组（inode 位图），几何共享 slice_count。
- 片下限 **32768 位**（= 1 位图块）；`slice_count` = ≥CPU 数的 2 的幂，受两组位数与「末片非空」约束夹断。
- 路由：alloc 按 **CPU 亲和 try 扫描**（`preferred[cpu]`，失败换片）；free/定点 按 `位号/per` 定位。
- **锁**：`spinlock`；alloc 用 `spin_trylock`（忙则换片），free 自旋强抢；**锁内只做位运算**（零 I/O、零分配）。
- 原语：`find_free_run`（连续 + 对齐）、`next_free_seg`、`soft_occupy`（锚定认领）、`test_interval`、`free_interval`。
- 分配/释放后标脏；**`sync_fs`/`put_super` 全量刷回**（不做 bh 增量）；挂载期逐位自校验「自身描述尾巴」（不满足 → 拒挂）。

### 3.6 名字堆 name_heap（`name_heap.c`）要点
- 尾长 = `name_len − 40`（>40B 才外置）；**10 个控制器** = 3×8B + 2×64B + 2×128B + 2×256B + 1×512B：

| 尾长 tail | 粒度 | units | 槽号 |
|---|---|---|---|
| ≤40 | 8B | `(tail+7)/8`（1..5） | 0–2 |
| 41–64 | 64B | 1 | 3–4 |
| 65–128 | 128B | 1 | 5–6 |
| 129–256 | 256B | 1 | 7–8 |
| 257–512 | 512B | 1 | 9 |

  表：`gran_log2={3,3,3,6,6,7,7,8,8,9}`、`bits_log2={16,17,18,16,17,16,17,16,17,16}`。
- 懒激活 = 位图面 + 数据面各一次连续分配（`2^1` 对齐）；位图 `bit[i]` 管数据字节 `[i×2^gran, …)`。
- 8B 组**逐槽 floor** 打穿台阶 = `13107+26214+52428 = 91749`（**跨槽碎片**，非 `458752/5`）——`rfsdirchurn.c` 即以此为准（§2.4）。
- 全空自动归还：目录变空时 `dir_name_heap_release` 遍历 10 控制器，位图面全 0 → 释放位图面+数据面 + 重置 `NAME_HEAP_INACTIVE`（**重置用 `RFS_NHC_SET`，`memset 0` 是雷**）。

---

## 4. dentry B+树「写入」机械（`dentry_write.c`，原 m5-notes 折入）

### 4.1 常量 / 阈值（**精确**）
| 项 | 值 |
|---|---|
| 阶 `RFS_DENTRY_ORDER` | **126**（叶 `entries[127]` / 内 `hints[127]`；`[126]`=瞬时槽，仅内存**禁落盘**） |
| 分裂触发 | `count == 127`；均分 **左 63 / 右 64**（`low_node_maxhash = victim[62]`） |
| 欠载下限 `DENTRY_MINA` | **63**（非根叶/内；根可 <63，空目录根叶 count=0） |
| 合并条件 | `child + neighbor ≤ 126`；否则抹匀（`left=total/2`） |
| 节点块 | `block_alloc(sb, 2, align_log2=1)`：2 块连续、**8K 对齐** |
| 内路由方向 | dentry 内 = **上界 `≥`（lower_bound）**；extent 是 `>`，**不可互抄** |
| GLB 哨兵 | `0xFFFF`（无下界 → 插入位 `idx=0`；否则 `idx=glb+1`） |

### 4.2 `parent_ptr` 四态 + `ptrs[3]`
- `parent_ptr`：`entry_count:10 | is_inner:1 | is_root:1 | ptr:52`。**根 `ptr` = 自身 inode（`.`）**（叶/内一致）；非根 `ptr` = 父节点块号。
- `ptrs`：非根叶 `[0]=INVALID, [1]=前驱叶, [2]=后继叶`；**根** `[1]=".." inode`。哨兵 = `PTR_INVALID`（52 位形态 `RFS_MASK52`）；**0 是合法块号，禁当「空」用**。
- 内部节点**无最左子树**：`N hints = N children`（上界路由）。

### 4.3 驱动调用链（一眼图）
```
create/mkdir → dir_insert: walk(定位+查重) → ddentry_insert(树机械)
                              ├ bit1: 沿 trace 自底向上刷 hash_max（短路 break）
                              └ bit0: leaf_split → depth==1 ? new_root_inner : apply_up → while(need) inner_split
unlink/rmdir → dir_remove: walk(命中) → ddentry_delete(= 物理删 + 出 flags)
                              ├ bit1: 刷 hash_max
                              └ bit0: node_balance(合并/抹匀) → while(need) 向上 / 根内 count==1 → root_shrink
```

### 4.4 ko 落地实测坑（**必看**，来自 M5 实战）
1. **分裂瞬时 `count==127` 不能被通用 meta 校验拒**：`rfs_meta_ok` 把 `count>126` 当落盘污染返 `-EIO`；但分裂正是要在 127 上工作 → 加 `rfs_meta_ok_x(…, maxc=127)`，仅分裂调用点放行。
2. **释放节点块前必须 `ravenfs_mdata_drop(sb, blk)`**：合并/根收缩把块 `block_free` 后，陈旧脏行会在 `flush_all` 回写到**已被重新分配**的块 → 污染新节点（drop：无人引用→丢弃；被引用→清 dirty 禁回写）。
3. **升根必须显式写 `ptrs[]`**：god `dentry_new_root_inner` 只 memset（`ptrs[]` 全 0），而 0 是合法块号 → ko 必须写 `ptrs[0]=INVALID, ptrs[1]=root_inode(..), ptrs[2]=INVALID`。
4. **叶分裂先读 `victim.ptrs[2]`（旧后继）再改写**，否则「旧后继」= 新叶自己（叶链自指）。
5. **owner 计数在驱动层维护**：ko `ddentry_insert/delete` 只做树机械；`create/unlink(/mkdir/rmdir)` 自己在树调用前后维护目录 `size++/--`、`link_count=size+2`。
6. **52 位截断**：v1.2 宏 `RFS_PTR_ADDR/RFS_PP_PTR` 已自动给 52 位；哨兵判定勿与 64 位 `PTR_INVALID` 直接比。

### 4.5 内核回调约定坑（**linux-7.2.x**）
1. **`.mkdir` 返回 `struct dentry *`**（ext2 约定：成功 `d_instantiate` 后返 NULL；失败 `ERR_PTR`）。`.create/.rmdir/.unlink/.rename` 仍返 `int`。
2. `->rename` **只做盘上工作**：flag 合法性 / 祖先陷阱 / EXCHANGE 目标存在 / 覆盖权限 **VFS 全包**；dcache 的 `d_move/d_exchange` 由 VFS 自做。EXCHANGE = 两名都留，仅交换 `inode_ptr`（目录交换两侧 `..` 互换）。
3. **M4 未接 → 写文件内容 `-EINVAL`**；命名空间测试用 `touch/mv/stat`，勿用 `echo > f`；`mv -T` 才测「覆盖目录」。

---

## 5. 里程碑进度 + 实测证据（截至 2026-09-27）

| 里程碑 | 内容 | 状态 / 证据 |
|---|---|---|
| M0/M1 | 模块注册 + SB 校验 + 几何 + root iget + **只读 mount** + 空目录 readdir/lookup/getattr/statfs | ✅ 实测 |
| M2 | dentry 遍历（lookup hash 下行 / readdir 全量 + 名字堆长名）+ `getattr.blocks` | ✅ `content2` 7 型 |
| M2.1 | readdir cookie → (叶块,槽) + 叶链续读；`i_blocks` 诚实化 | ✅ `dents_dump 256` → DUP=0 MISSING=0 |
| M2.2 | 缓存行尺寸解耦（4K..64K，`row_size=`，尾行截断） | ✅ 5 档 × big5000 全 5001 |
| M2.3 | `mdata_piece_cache_mgr`+session（8K 固定） | ✅ |
| M2.4 | ~~node_latch~~ → **不做**（片级 rwsem + TTL 已足） | ✅ 决议 |
| M2.5/b | 自写 TTL（2s）+ 行缓冲改伙伴页 + 分片锁重入死锁根治 | ✅ buddyinfo 归页 |
| **T0** | mkfs → 只读挂载 → `audit_v1.2` 全绿 | **✅ 2026-09-26** |
| M3.1 | 双位图内存分片分配器 | ✅ |
| M3.2 | `write_inode` 行同步 + `flush_all` 落盘 + `sync_fs`/`fsync` | ✅ |
| M3.3/4/4b | rw 挂载 + create(REG,inline)/unlink + 外置名写侧 | ✅ `c70d6dd`/`ba3cb35` |
| M5a/b | dentry 树写入机构（多级 walk + 叶裂/升根/内裂 + 合并/抹匀/根收缩） | ✅ `716e372`/`678a983` |
| M5 | mkdir/rmdir + `dir_name_heap_release` | ✅ `8d745ae` |
| M5 | rename（replace/NOREPLACE/EXCHANGE + 跨目录目录移动） | ✅ `612626a` |
| M5 | 大魔王压出两真 bug（内裂边界 `pcnt>126`；rmdir 漏释名字堆） | ✅ 修复 `7447eea` |
| M5 | 名字堆 5 组逐一精准打穿压测器（`rfsdirchurn.c`） | ✅ `c50a1e8`（§2.4） |

**M5 现状**：create/unlink/mkdir/rmdir/rename（+EXCHANGE）+ 多级树全套 + 堆回收 **全通**（含 9 万级压测）。
剩 **link / symlink(+get_link) / mknod / tmpfile** → 然后 **M4 文件数据**。

---

## 6. 缺口分析（**距 T2 还缺哪些点**）

> 图例：✅ 已实现 · 🔶 半成（机构在、未接线/未落盘） · ❌ 缺。
> 「落点」= 要改的 VFS 回调 / 模块；「需求」= spec §4 的 R-ID。

| # | 缺口 | 现状 / 症状 | 落点 | 依赖 | 需求 |
|---|---|---|---|---|---|
| **G1** | **文件数据读写（M4）** | `read/write/mmap` **全 `-EINVAL`**；文件数据 0 落地 | `address_space_operations`（`read_folio/readahead/write_begin/write_end/dirty_folio/writepages/invalidate/migrate`）+ `read_iter/write_iter/mmap` + **extent 树对接** + inline↔direct↔extent 三态迁移 + `truncate/punch_hole/fallocate` + `i_blocks` 随写更新 | M3 | R-FD-01..09、R-MD-01、R-SY-04 |
| **G2** | **`setattr`（M4/MD）** | 无 `.setattr` → `chmod/chown/truncate/utimes/`atime 不落盘 | `ravenfs_file_inode_operations.setattr`（现仅 `.getattr`） | M3 | R-MD-02/03/05/06 |
| **G3** | **硬链接 `link`（M5残）** | 无 `.link` → `ln` 失败 | dir `inode_operations.link`（盘上 `link_count++`；dcache 交 VFS） | M5 | R-NS-05 |
| **G4** | **`symlink` + `get_link`（M5残）** | 无 → `readlink` `-EINVAL`；**T3 前置**（`ld.so`/`/sbin`） | `.symlink` + `.get_link`（fast/slow，短名内联） | M5 | R-NS-06 |
| **G5** | **`mknod`（M5残）** | 无 → `mknod` 失败（4 型：FIFO/CHR/BLK/SOCK） | `.mknod`（CHR/BLK 写 `inline_data[0:4]=rdev`） | M5 | R-NS-07 |
| **G6** | **`tmpfile`（M5残）** | 无 `.tmpfile` → `O_TMPFILE` 失败（journald 用） | `.tmpfile` | G1(数据) | R-NS-09 |
| **G7** | **xattr 四回调（M6）** | 无 → `setfattr/getfattr` 失败；xattr 块已设计未落地 | `.getxattr/.setxattr/.listxattr/.removexattr` + `xattr_handler`；xattr 块 alloc/free（bitmap 认领） | M3/M5 | R-XA-01..04 |
| **G8** | **ACL（M6）** | 无 | `SB_POSIXACL` + `.get_inode_acl/.set_acl` + `posix_acl_create`（默认 ACL 继承） | G7 | R-NS-12、R-XA-* |
| **G9** | **capability（M6）** | 无 | 经 G7 的 `security.capability` xattr | G7 | R-XA-02 |
| **G10** | **加固（M7）** | 无停机标志；**无周期回写**（dirty mdata 累积）；并发面（bitmap/alloc、回写/evict/sync vs 前台）未系统化；`fsync` 只落元数据不落数据；ENOSPC 路径未系统验 | `RFS_SHUTDOWN` 自检失败→全局 `-EIO`；周期回写；`fsync` 全覆盖；错误路径 | M3–G9 | R-LM-09、R-CO-02/04、R-SY-05 |
| **G11** | **一致性/崩溃窗口（M3.5）** | 未做非干净卸载 + 写序验证 | 非干净卸载后 `audit_v1.2` 判「不产生损坏」 | M3 | §验收 m3.5 |
| **G12** | **M8 预演最狠测试** | 未做 | `rsync -aHAX` ext4→ravenfs 克隆 + `pacman -Qkk` | G1+G3..G9 | T3 前哨 |
| **G13** | **xfstests 工具链（T1 前置）** | **未解决**（guest 无 git/编译链预算） | 宿主下载源码 → 投递 → guest 编译；或宿主预编 | — | T1 |
| **G14** | **fsck** | **空实现**（`-n` 只查由 audit 承担） | 保留 stub；不修 | — | §4 |

**一句话缺口**：**文件数据（G1）+ setattr（G2）是「能读写文件」的硬缺口；link/symlink/mknod/tmpfile（G3–G6）是「Arch 安装/运行」的硬缺口；xattr/ACL（G7–G9）是「pacman/rsync -aHAX 保真」的硬缺口；加固（G10）是「不静默损坏」的底线。**

---

## 7. 路线图（M4 → T2）

> 纪律：**每阶段必须「可测」才算完成**（audit / guest 挂载实测 / dents_dump / xfstests）；**阶段内禁止越级**（T1 缺口表出来前不碰 T3）。

- **M3.5 一致性/崩溃窗口**（不新增功能，只加证据）：按 spec 不变量验证写序（亮灯→落盘→翻转）；非干净卸载（`echo b > /proc/sysrq-trigger`/kexec）后 audit。判据 = **不产生损坏**（孤儿可被审计标出）。
- **M4 文件数据**（**下一步**，G1/G2）：a_ops + `read_iter/write_iter/mmap` + extent 树 + 三态迁移 + `truncate/punch/fallocate` + `setattr`。判据：`dd/cat/head` 往返一致（跨态、>4K、稀疏洞）；`stat.blocks` 吻合；audit 全绿。
- **M5 残**（G3–G6）：link / symlink+get_link / mknod / tmpfile。判据：`ln/ln -s/readlink/mknod` 往返；大目录分裂后 `dents_dump` 仍 `DUP=0 MISSING=0`；audit `chain_bad=0`。
- **M6 xattr/ACL/capability**（G7–G9）：4 回调 + `SB_POSIXACL` + 默认 ACL 继承 + `security.capability`；xattr 块分配/释放。判据：`setfattr/setfacl/setcap` 往返；**`rsync -aHAX` 保留 ACL+xattr**；audit xattr 校验通过。
- **M7 加固**（G10）：停机标志 / 周期回写 / 并发面 / `fsync` 全覆盖 / ENOSPC 干净。判据：`fsstress` 小规模不炸；`drop_caches` + 并发读写下 audit 仍绿；注入不一致后入口返 `-EIO`。
- **M8 预演最狠测试**（G12）：`rsync -aHAX` 克隆 + `pacman -Qkk`。
- **T1 xfstests `-g quick`**（G13 前置）→ **缺口表**（区分 fail / skip：O_DIRECT/FITRIM/SEEK_HOLE 等降级项会 skip）。
- **T1.5**：按缺口表补 spec §4 的 **M** 项，重跑至收敛。
- **T2 压力**：`fsx` / `fsstress` / `fs_mark`，**每轮跑 audit**。判据：**无静默损坏**（audit 恒绿）。

之后：T3 完整安装 → T4 启动（见验收纲领 §2.1/§2.2）。**T5/T6 暂缓。**

---

## 8. 已知缺口 / 风险 / 坑（速查）

- 文件读/写 → `-EINVAL`（G1）；`readlink` → `-EINVAL`（G4，**T3 前必补**）。
- **无周期回写线程** → 无人 sync 时 dirty mdata 行持续累积，直到 `syncfs`/umount 才吐（M7）。**位图同理：全量内存态，异常断电 = 需重装（无 fsck）**。
- `i_blocks` 口径 = **数据块 + 元数据块**（dentry 节点×2、名字堆 位图面+数据面、xattr 块 1）。
- 行缓冲是**伙伴页**（非 slab）：观测用 dmesg TTL reap + `/proc/buddyinfo`；`/proc/slabinfo` 已无 `ravenfs_inode_row`/`ravenfs_mdata_node`。
- **已根治**：分片锁重入死锁（装载=锁外分配+I/O）。
- `pr_info_ratelimited` 的 TTL 日志会节流，不是漏打。
- **宿主/guest 内核版本不同 ⇒ 永远 guest 内编译再测**；宿主 ko 不 insmod。

---

## 9. 纪律（照旧，勿忘）

- **三层约束**：① 正确性（满足 Arch 用户态规格）② 框架优先（能用内核成熟框架就用）③ **撞上「非良定义问题」第一时间上报用户**（进度可慢、隐患不留）。
- **小步快跑**：每步「编译 + 实测」可验收才算完成；**一步一提交**（commit message 写给接手者）；不憋大招（错误放大前车之鉴）；错了立刻连根铲除；每步收尾落文档（spec / 本文 / 项目纪律），**不留在会话里**。
- **成本审计**：大流量/长耗时先量化（`COST-AUDIT-DISCIPLINE.md`）。
- **红线**：宿主 `sudo` 不给；不用录存口令提权（**guest 测试 VM 凭证除外，用户已授权**）。
- 用户风格：直接、要**思维可见**（设计意图/推理外显）、讨厌废话；**不良定义点主动上报**。

---

> **一句话交接**：读 spec §4.11.1/§4.11.2/§4.11.3 → guest 内编译 ko（§2.2）→ `insmod` + `mount -t ravenfs`（认 serial `deadbeef`）→ 跑 `audit_v1.2` + `rfsdirchurn` 验证 → 然后开 **M4（G1/G2）**。
