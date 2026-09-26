# RavenFS → Linux KO：第一约束需求规格书（Arch 根文件系统）

> 2026-09-26 · Raven · **v1.0（决议收敛版）**
> **定位**：把验收纲领（`ravenfs-linux-ko-acceptance-plan.md`）**第一约束**「满足 Arch Linux 用户态程序的规格需求」实体化为可签发、可验收、可追溯的正式规格书。**本文件接管纲领 §1**（2026-09-26 已执行：纲领清单改为指向本文的指针；**冲突一律以本文为准**）。
> **本文性质**：**已按 E1–E8 全部决策重推**——不是"列需求 + 待裁"，而是**净需求面**：从三份参考实现（ext2/ext4/btrfs）出发，**减去内核代劳**、加上 **Arch 硬增量**与 **v1.2 盘面增量**。
> **地面真值**：本机内核源码 `/home/PS/PS_git/custom-kernel/linux-vfio/src/linux-7.2.3/`（所有签名/字段/常量逐字摘自该树）。
> **关联**：`disk_layout_v1.2_inode.md`（v1.2 inode 盘面，权威详版）、`concept_iface_design.md`（平台无关接口 I1–I8）、`disk_layout_v1.1.md`（v1.1 盘面不变量）、`audit_inode_design.md`（验收预言机）、`linux_vfs_iface_spec.md`（回调逐字规格）。

---

## 0. 元信息与决议总表

| 项 | 值 |
|---|---|
| 目标 | `ravenfs.ko`（Linux 内核模块，磁盘型块设备文件系统） |
| 运行内核 | `7.2.3-1-vfio`（源码树见上） |
| 需求母体 | 第一约束 = Arch 用户态规格需求 |
| 手段约束 | 第二约束（框架优先）；第三约束（非良定义即上报） |
| 验收预言机 | xfstests `-g quick`(generic) · LTP · `rsync -aHAX` · `pacman -Qkk` · `audit_filesystem` · T3/T4 启动判据 |
| 状态标记 | ✅已定/实测 · ⚠️需注意 · ❌待实现 |

### 0.1 决议总表（E1–E8，全部收敛）

| # | 决议 | 一句话 |
|---|---|---|
| **架构·缓存/IO** | **全部下沉 Linux** | inode 缓存 → **VFS inode**（R-IO-01a）；元数据节点 → **页缓存**（R-IO-01b）；块 I/O → **bio**（R-IO-02）。god 的 `inode_cache_radix` / `mdata_piece_caches` / `cache_mgr_mode` **不进 ko** |
| **架构·机制** | R-IO-01b = **b1 buffer head**（E8） | `sb_getblk`/`__bread_gfp`/`mark_buffer_dirty`；bh 自带 `b_state` 锁 |
| **架构·锁** | **模型 (A)** | B+tree 节点逻辑原子性靠自建 **node latch**（按块号索引的 latch 表）；页锁**逐页、绝不嵌套两页** |
| **E2** | **v1.2 inode 盘面** | 第一字 `type:6｜direct:1｜i_flags:57`；`permission_bits → mode:12｜xattr_ptr:52`；rdev→`inline_data`；xattr=**单块自描述**。**字节布局不变**，但旧盘不兼容 → reformat + `RAVENFS_VERSION 2→3` |
| **E3** | 目录 `nlink` | `i_nlink` **直接报 `size+2`**（`size`=条目数 ⇒ 恒过报；find 只失去优化、不漏） |
| **E4** | readdir cookie | **原子性只到单次 syscall**（VFS `i_rwsem` 读锁，本次调用内目录稳定）；**跨调用不保证原子/一致性**（锁已释放，不阻止并发修改）；并发下只承诺**不崩/不死循环**（允许重/跳）；cookie = 可校验+可重定位 |
| **E5** | 时间戳 | **不自建触发表**（内核已定）→ mtime/ctime 由 `generic_file_write_iter` + **`filemap_page_mkwrite`（mmap）** 免费推；**FS 义务仅 3 处** |
| **E6** | `s_maxbytes` | **`1<<52`（4 PiB，字节）**；≤ `MAX_LFS_FILESIZE` |
| **E7** | xattr | **真需求**；**单块上限**（`h_blocks` 死字段恒 1） |
| 附 | **LBS 排除** | 不用 `FS_LBS`/`s_blocksize=8K`（与 4K `FS_block` 冲突）→ 「8K 节点跨 2 页」既定，由锁模型 (A) 消化 |

---

## 1. 方法：需求从哪来（预言机 + 证据分级）

### 1.1 为什么不能「照着 ext4/btrfs 抄需求」

**抄，只能抄到「手段」，抄不到「需求」。** 需求来自**用户态契约**。参考实现提供两件别的东西：**(a) 能力包线**（哪些回调组合够支撑一个 Arch 根）、**(b) 参考语义 + helper 选型**（第二约束的取材源）。把 (a)/(b) 当需求，会**抄来用不上的重型能力**（journal/CoW/quota）并**漏掉 Arch 特有硬需求**（`security.capability`、`fallocate`）。

### 1.2 四级预言机

| 序 | 预言机 | 回答 | 用法 |
|---|---|---|---|
| **O1** | 用户态真实调用面（systemd/pacman/coreutils/ld.so/tmpfiles/journald） | 谁要、要什么语义 | 需求 + §5 追溯矩阵 |
| **O2** | 内核 VFS 契约（`struct *_operations` + `vfs.rst`） | 接口面与义务 | 「VFS 落点」 |
| **O3** | 参考实现包线（ext2 下限 / ext4·btrfs 上限） | 一组回调怎么组合才够 | §2 + 「框架优先落点」 |
| **O4** | 验证预言机（xfstests/LTP/pacman -Qkk/audit） | 怎么判做到 | 「验收判据」 |

**闭环规则**：O1 提需求 → O2 定接口 → O3 定实现路线 → O4 定判据。任何需求必须能挂到 ≥1 个 O1 使用者；挂不上 = 非需求（§4.10）。

### 1.3 证据分级
**A**=本机源码逐字/实测 · **B**=内核官方文档 · **C**=A/B 组合推断（须实测转 A）。

---

## 2. 需求包线：三份参考 → 内核代劳 → 净需求面

### 2.1 参考实现包线（O3，逐字摘 `linux-7.2.3`）

| 回调域 | ext2（下限，同构） | ext4 | btrfs |
|---|---|---|---|
| `super_operations` | alloc_inode/free_inode, write_inode, evict_inode, put_super, sync_fs, freeze/unfreeze, statfs, show_options | + destroy_inode, dirty_inode, drop_inode, shutdown, quota | drop/evict/put/alloc/destroy/free_inode, statfs, freeze/unfreeze, show_devname/show_stats, nr/free_cached_objects, remove_bdev, shutdown |
| `dir inode_operations` | lookup/create/link/unlink/symlink/mkdir/rmdir/mknod/rename/listxattr/getattr/setattr/get_inode_acl/set_acl/tmpfile/fileattr_* | 同（含 tmpfile/fiemap） | + permission/update_time |
| `file inode_operations` | listxattr/getattr/setattr/get_acl/set_acl/fiemap/fileattr_* | 同 | + permission/update_time |
| `file_operations`（regular） | llseek/read_iter/write_iter/ioctl/mmap/open/release/fsync/get_unmapped_area/splice_*/setlease | + iopoll/mmap_prepare/**fallocate**/fop_flags | + remap_file_range/uring_cmd/**fallocate** |
| `dir file_operations` | open/release/llseek/read(generic_read_dir)/**iterate_shared**/ioctl | + fsync | llseek/read/iterate_shared/open/ioctl/release/fsync |
| `address_space_operations` | read_folio/readahead/write_begin/write_end/dirty_folio/invalidate_folio/bmap/writepages/migrate_folio/is_partially_uptodate/error_remove_folio | journalled/da/dax 变体 | read_folio/writepages/readahead/…/swap |

### 2.2 内核代劳面（E1–E8 决策后新增的关键认识）

**这些我们原本担心、其实内核代劳 → 不是我们的义务：**

| 原担心 | 谁代劳 | 证据 |
|---|---|---|
| inode 内存缓存 | **VFS inode cache**（`iget_locked`/`ilookup`） | ext2 `ext2_inode_info` 模式 |
| 同目录命名空间改操作的串行化 | **`i_rwsem`（父目录写锁）** | `fs/namei.c:2918` |
| readdir 与改操作互斥 | **`i_rwsem` 读/写** | `fs/readdir.c:103` |
| 文件 mtime/ctime（缓冲写） | `file_update_time` | `mm/filemap.c:4462` |
| 文件 mtime/ctime（**mmap 写**） | `filemap_page_mkwrite` / `do_wp_page` | `mm/filemap.c:4003`、`mm/memory.c:3726` |
| 文件 atime | `file_accessed`（read/fault/readdir） | `mm/filemap.c:2987`、`fs/readdir.c:113` |
| 元数据变更 ctime | `notify_change`/`setattr_copy` | `fs/attr.c:301/367` |
| 块缓存（元数据 8K 片） | **bdev page cache + buffer head** | `sb_getblk`/`__bread_gfp` |

### 2.3 净需求面（本文的需求来源）

```
净需求 = ext2 级回调集（下限）
        − 内核代劳（2.2；给我们"零义务"）
        + Arch 硬增量：fallocate / xattr·ACL / O_TMPFILE / report_error / tmpfile
        + v1.2 盘面增量：type:6 / mode+xattr_ptr / rdev / i_flags / xattr 块 / s_maxbytes=1<<52
        − 豁免（§4.10：journal/CoW/swap/fiemap/…）
```

> **判据**：ext2 能当 root 是**下限活证据**；RavenFS 验收口径 =「ext2 能过的 Arch 场景，RavenFS 必须过」+ 上列增量。

---

## 3. 需求总览

| 域 | 编号段 | 条数 | 说明 |
|---|---|---|---|
| **KP** 模块与内核管线 | R-KP-01..05 | 5 | 模块注册、fs_type、initramfs 装载 |
| **LM** 挂载与卷生命周期 | R-LM-01..12 | 12 | mount/umount、inode 生命周期、remount-ro |
| **NS** 命名空间与目录 | R-NS-01..13 | 13 | lookup/…/rename/link/symlink/**mknod**/tmpfile/readdir |
| **FD** 文件数据 | R-FD-01..10 | 10 | 读写/mmap/fsync/splice/**fallocate**/打洞/稀疏 |
| **MD** 元数据与属性 | R-MD-01..09 | 9 | getattr/setattr/权限位/时间戳/nlink/**i_flags** |
| **XA** xattr / ACL | R-XA-01..04 | 4 | xattr 四回调、capability、ACL、xattr 块 |
| **SP** 空间与规模 | R-SP-01..07 | 7 | statfs/容量参数/`s_maxbytes`/ENOSPC |
| **SY** 同步与一致性 | R-SY-01..07 | 7 | fsync/syncfs/写序/flush |
| **CO** 并发与锁 | R-CO-01..04 | 4 | i_rwsem 契约、**节点 latch 表 (A)**、位图锁 |
| **IO** I/O 与缓存后端 | R-IO-01a/01b/02 | 3 | **已定**：inode→VFS、元数据→页缓存、块 I/O→bio |
| **DG** 可降级 / 豁免 | R-DG-01..08 | 8 | 明确 `-EOPNOTSUPP` 面 |

> 共 **~82 条**。

---

## 4. 需求展开

> 每条给「需求 / 依据(级别) / 优先 / 框架优先落点 / 现状」。**优先级**：**M**必须 · **L**很可能 · **C**可降级。

### 4.1 KP — 模块与内核管线

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-KP-01 | 以可加载模块注册文件系统类型：`file_system_type`（`FS_REQUIRES_DEV`），`init_fs_context` + `kill_sb` | A: `include/linux/fs.h`；ext2/4 | **M** | `register_filesystem`；`kill_sb = kill_block_super`（`fs/super.c:1667`） | ❌ |
| R-KP-02 | `fs_flags`：**不设** `FS_USERNS_MOUNT`/`FS_ALLOW_IDMAP`/`FS_LBS` | A: 同上；§0.1 附 | **M** | 照抄 ext2 | ❌ |
| R-KP-03 | 块设备绑定：`sb->s_bdev` → 读 SB 校验（magic/version/CRC），`s_blocksize=4096` | A: 盘面文档 | **M** | `sb_set_blocksize`/`sb_bread` | ❌ |
| R-KP-04 | **initramfs 装载**：`ravenfs.ko` 必须进 initramfs 才能根挂载 | A: 纲领 §3.3 | **M** | `mkinitcpio MODULES=(ravenfs)` | ❌ |
| R-KP-05 | 模块须匹配运行内核（`vermagic`）+ 关 secure boot | A: `vm-env-notes.md` | **M** | 就地 `make -C /lib/modules/$(uname -r)/build` | ❌ |

### 4.2 LM — 挂载与卷生命周期

> `super_operations`（A: `include/linux/fs/super_types.h:83`）与 ext2 `ext2_sops`（`fs/ext2/super.c:361`）为落点参考。

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-LM-01 | `alloc_inode`/`destroy_inode`：分配 **FS 私有结构**（内嵌 `struct inode` + 盘 inode 视图） | A: `fs/ext2/ext2.h`；R-IO-01a | **M** | `alloc_inode_sb`（`fs.h:3037`）+ `RAVENFS_I()` | ❌ |
| R-LM-02 | `write_inode`：内存 inode → 盘（含 `i_{m,c,a}time`+ns） | A: ext2 `ext2_write_inode` | **M** | 映射 god `inode_write` | ❌ |
| R-LM-03 | `evict_inode`：**唯一** inode 回收入口；`nlink==0` 回收数据+inode+位图 | A: 不变量 17/20；`vfs.rst` | **M** | `regular_file_reclaim`/dir 回收 + **自调 `truncate_inode_pages_final()`** | ⚠️ 用户态有，无 VFS 钩 |
| R-LM-04 | `put_super`：卸载析构（位图 flush）；干净卸载（T3 判据） | A: 纲领 §2.1；I8 | **M** | god 析构 + 位图落盘 | ❌ |
| R-LM-05 | `sync_fs`：卷级同步（`sync(1)`/`syncfs(2)`） | A: `super_types.h`；I8 | **M** | `fs_sync(0)`（含位图写回） | ❌ |
| R-LM-06 | `statfs`：填 `struct kstatfs`（§4.7） | A: ext2 `ext2_statfs` | **M** | `fs_statfs` | ❌ |
| R-LM-07 | `dirty_inode`/`drop_inode`：**均不实现**（`drop_inode` 必须 NULL——VFS inode 即缓存） | A: `vfs.rst`；ext2 无此二者 | C | 缺省 | ✅ 缺省即满足 |
| R-LM-08 | `show_options`：`/proc/mounts` 输出挂载选项 | A: ext2 `ext2_show_options` | L | `seq_puts` | ❌ |
| R-LM-09 | `report_error` + 自检失败 `sb->s_flags \|= SB_RDONLY`（remount-ro）：替代 fsck 的「不产生损坏」出口 | A: `super_types.h`（7.2.3，仅 xfs `fs/xfs/xfs_super.c:1349` 实现） | **M** | `report_error` + `sb_rdonly` | ❌ |
| R-LM-10 | sb 关键字段：`s_magic`（**唯一**）、`s_maxbytes`（=§4.7）、`s_time_gran`、`s_op`、`s_flags` | A: `fs.h` | **M** | 分配唯一 magic；`s_maxbytes=RFS_MAX_FILESIZE` | ❌ magic 待定 |
| R-LM-11 | 挂载语义：`ro`/`rw`、`rootflags`；根挂载由内核 `rootfstype=ravenfs` 走 `mount_root` | A: 纲领 §3.5 | **M** | 标准 `fs_context` | ❌ |
| R-LM-12 | `freeze_super`/`thaw_super`：挂起/休眠 | B: `vfs.rst` | C | 可先 `-EOPNOTSUPP` | ❌ |

> **注 R-LM-09**：`report_error`（`struct fserror_event`）是 7.2.3 提供的错误上报钩子——把「自检发现不一致 → 置只读 + 上报」的正规出口；**这是与「崩溃恢复」路线切割后的关键闭环**。

### 4.3 NS — 命名空间与目录操作

> 签名/语义逐字见 `linux_vfs_iface_spec.md`。参考：ext2 `ext2_dir_inode_operations`（`fs/ext2/namei.c:408`）——**下限包线**。

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-NS-01 | `lookup`：名字→inode；命中 `d_splice_alias`，miss 返 NULL；真错误才 `ERR_PTR` | A: `vfs.rst`；ext2 | **M** | `dir_lookup` + `iget_locked` | ❌（god 有 lookup） |
| R-NS-02 | `create`（普通文件）：`d_instantiate_new` | A: ext2 | **M** | `dir_insert_dentry(...,REGULAR)` | ❌ |
| R-NS-03 | `mkdir`/`rmdir`：rmdir 先判空 → `-ENOTEMPTY` | A: ext2 | **M** | `dir_insert_dentry(...,DIR)` / `dir_rm_dentry` | ❌ |
| R-NS-04 | `unlink`：删名字 → `link_count--`；先删 dentry 落盘后计数 | A: 不变量 20 | **M** | `dir_rm_dentry` | ❌ |
| R-NS-05 | `link`（硬链接）：目录禁链 `-EPERM`；已存在 `-EEXIST` | A: 盘面 §3.3；I1 | **M** | `dir_link` | ❌ |
| R-NS-06 | `symlink` + `get_link`：fast（≤160 内联 → `simple_get_link`）/ slow（`page_get_link`）双态 | A: ext2 `symlink.c:24/31`；I5 | **M** | len 自然分派 | ❌ |
| R-NS-07 | `mknod`：建 FIFO/socket/device 节点；**VFS 四型都走此回调**（socket `bind` 亦经 `vfs_mknod`） | A: `net/unix/af_unix.c:1384`；E2 | **L** | `init_special_inode` + v1.2 `type:6`（chr/blk `rdev` 落 `inline_data[0:4]` **小端 u32**） | ❌（v1.2 起支持） |
| R-NS-08 | `rename`：`RENAME_NOREPLACE`+`RENAME_EXCHANGE`；未知 flag → `-EINVAL`；目录改名 `..`+两父 nlink | A: `uapi/linux/fs.h`；I2 | **M** | `dir_rename` | ❌ |
| R-NS-09 | `tmpfile`（`O_TMPFILE`，glibc `tmpfile()`） | A: ext2 `ext2_tmpfile` | **M** | `.tmpfile` | ❌ |
| R-NS-10 | `atomic_open`：可选优化；缺省退化为 lookup→create | A: `vfs.rst` | C | 不做 | ❌ |
| R-NS-11 | `iterate_shared`（readdir）：cookie 续读、`.`/`..` **合成**；**单次 syscall 内原子/稳定**（VFS `i_rwsem` 读锁）；**跨调用不保证原子/一致性**（锁按 syscall 持有→释放，不阻止并发修改）；cookie 失效 → 容错重扫（**不得崩/不得死循环**；允许重/跳） | A: `fs/readdir.c:103`；E4/I4 | **M** | `dir_context.pos` + `dir_emit`；cookie = 叶块号+槽号（**可校验可重定位**） | ❌ |
| R-NS-12 | `permission`：缺省走 `generic_permission`（`fs/namei.c:521`），**不实现** | A: `vfs.rst` | C | 用缺省 | ✅ |
| R-NS-13 | `d_revalidate`：本地磁盘 FS 不需要 | A: 惯例 | C | 不做 | ✅ |

> **注 R-NS-11（readdir 并发模型）**：目录 `i_rwsem` = `lookup`/`readdir` **读**、改操作 **写**（`fs/namei.c:1935/2918`、`fs/readdir.c:103`）→ 读读并发、读写互斥。
> **原子性范围 = 单次 syscall（2026-09-26 裁决）**：锁**按 syscall 持有→释放** → **单次 `getdents` 内目录稳定**；**跨调用之间不保证原子/一致性**（锁已放掉，不阻止并发修改，保留别人原子删除的权利）。⇒ cookie 只需「可校验 + 可重定位」；并发修改下的承诺**只到“不崩/不死循环”**，**允许重/跳**（**不承诺“不丢条”**）。

### 4.4 FD — 文件数据

> 参考：ext2 `ext2_file_operations`（`fs/ext2/file.c:205`）、ext4（`fs/ext4/file.c:974`，含 `fallocate`）、btrfs。
> **架构前提（R-IO-01）**：走 **Linux 页缓存** + **generic 路径**（后者顺带免掉时间戳义务，见 §2.2）。

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-FD-01 | `read_iter`/`write_iter`（缓冲读写；**顺带免费推 mtime/ctime/atime**） | A: ext2；§2.2 | **M** | `generic_file_read_iter`/`generic_file_write_iter`（`mm/filemap.c`） | ⚠️ god 有 pread/pwrite |
| R-FD-02 | `llseek` | A: ext2 | **M** | `generic_file_llseek`（`fs/read_write.c:254`） | ✅ 借用 |
| R-FD-03 | `mmap`（含**可写**+**可执行**）：动态加载器 mmap 全部 `.so`；**mmap 写自动推 mtime** | A: `mm/filemap.c:4003/4022` | **M** | `generic_file_mmap_prepare`（自带 `filemap_page_mkwrite`）+ `filemap_fault` | ❌ |
| R-FD-04 | `fsync`/`fdatasync`：真持久 + 回报错误 | A: ext2 `ext2_fsync` | **M** | `filemap_write_and_wait_range` + 元数据落盘 | ❌ |
| R-FD-05 | `splice_read`/`splice_write` | A: ext2 表 | **M** | `filemap_splice_read` / `iter_file_splice_write` | ⚠️ 借用 |
| R-FD-06 | `copy_file_range`：缺省回退读写循环 | A: `fs/read_write.c` | C | 不实现即回退 | ✅ |
| R-FD-07 | `fallocate`（**普通预分配**，`FALLOC_FL_KEEP_SIZE`）：`journald` 预分配 | A: 纲领 §1-B；ext4/btrfs | **M** | `.fallocate` + god 占位分配 | ❌ **缺口** |
| R-FD-08 | `punch hole`（`FALLOC_FL_PUNCH_HOLE`）/`ZERO_RANGE` | A: 纲领 §1-B | **M** | god `direct_file_punch_hole` | ⚠️ god 有，无 VFS 钩 |
| R-FD-09 | 稀疏：未分配读返 0；打洞要有（`SEEK_DATA/HOLE` 可降级） | A: 不变量 22–24 | **M** | 三态 + 洞 | ✅（god 层） |
| R-FD-10 | `direct_IO`（`O_DIRECT`）：可降级 | A: ext2 无 | C | `-EOPNOTSUPP` | ✅ |

> **注 R-FD-07**：`fallocate` 与「占位分配是 direct 全覆盖表示的物理义务」（不变量 24）**天然契合**——god 已有，缺 VFS 钩 + `KEEP_SIZE` 交互。**优先级 M、成本低。**

### 4.5 MD — 元数据与属性

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-MD-01 | `getattr`：填 `struct kstat`（`mode/nlink/uid/gid/rdev/size/blksize/blocks`+时间） | A: `linux_vfs_iface_spec` §4.1 | **M** | `generic_fillattr`（`fs/stat.c`，含 `stat->rdev = i_rdev`） | ❌ |
| R-MD-02 | `setattr`：`chmod`/`chown`/`utimes`/`truncate` | A: `linux_vfs_iface_spec` §4.2；I6 | **M** | `setattr_prepare`+（size→`direct_file_trunacate`）+`setattr_copy` | ⚠️ 仅 truncate |
| R-MD-03 | **setuid/setgid/sticky 可持久**（`sudo`/`su`/`passwd`） | A: 纲领 §1-A；E2 | **M** | v1.2 `mode:12` → `i_mode` | ✅ v1.2（原字段只写不读，v1.2 启用） |
| R-MD-04 | **uid/gid 持久** | A: 盘面 | **M** | `i_uid/i_gid` | ✅ 盘面具备 |
| R-MD-05 | mode 位完整，含常见 `S_IFMT` 类型（7 型） | A: 纲领 §1-A；E2 | **M** | v1.2 `type:6`（镜像 `S_IFMT>>12`） | ✅ v1.2 |
| R-MD-06 | **时间戳**：采纳内核模型（**不自建触发表**）→ FS 义务仅 3 处 | A: `mm/filemap.c:4003/4462`、`fs/attr.c:301`；E5 | **M** | 走 generic 写/mmap 路径（免费推）+ `write_inode` 落盘 | ❌ v1.1「全系不维护」（v1.2 字段已在） |
| R-MD-07 | 目录 `nlink`：`find` leaf 优化需 `i_nlink ≥ 2+子目录数`；`ls -l` 展示 | A: `man find -noleaf`；E3 | **L** | getattr **直接报 `size+2`**（过报安全、零成本） | ✅ 决议 |
| R-MD-08 | `i_version`/`change_cookie`、`STATX_BTIME`：可降级 | B: `fs/stat.c` | C | 缺省 | ✅ |
| R-MD-09 | `i_flags`（`chattr`/`lsattr` 基础）：v1.2 第一字留 `i_flags:57` | A: 决议 | C | 字段已留，写 0；实现仍降级（R-DG-03） | ✅ v1.2 字段就位 |

> **注 R-MD-06（时间戳义务清单，E5）**
> | 场景 | 谁推 | 义务 |
> |---|---|---|
> | 文件 mtime/ctime（缓冲写） | `__generic_file_write_iter` → `file_update_time`（`mm/filemap.c:4462`） | **零** |
> | 文件 mtime/ctime（**mmap 写**） | `filemap_page_mkwrite`（`mm/filemap.c:4003`）/`do_wp_page` | **零** |
> | 文件 atime | `file_accessed` | **零** |
> | 文件 ctime（chmod/chown/truncate） | `notify_change`/`setattr_copy`（`fs/attr.c:301`） | **零** |
> | **新 inode 初始时间** | **FS 自己**（ext2 在 `ext2_new_inode` 打，`fs/ext2/namei.c:212`） | **要做** |
> | **目录 mtime/ctime（条目增删改）** | **FS 自己**（VFS `namei.c` 不管；ext2 在 `fs/ext2/dir.c:468/555/609`） | **要做** |
> | **落盘** | FS `write_inode` | **要做** |
> ⚠️ 「ctime 交 `alloc_inode`」**精神对、位置错**：初始 ctime 要在**创建路径**打，不能在 `alloc_inode`（后者随 iget 缓存 miss 反复触发）。
>
> **注 R-MD-07（nlink）**：盘面 `size`=dentry 条目数 ⇒ `size+2 ≥ 2+子目录数` ⇒ **恒过报**。find 的 leaf 优化仅在「已 stat 到 `nlink-2` 个子目录」后停 stat 剩余（`man find -noleaf`）→ **过报 = 失去优化（正确）；欠报才会漏**。⇒ 直接报 `size+2`，零成本、不改盘面、不扫树。代价仅 `ls -l` 数字偏大。

#### 4.5.1 v1.2 inode 布局（E2）

> 动机：`permission_bits` 实测**只写不读**（真·白地：仅 `mkfs/mkfs.cpp:332`、`juvenile_god/dentry_insert_delete.cpp:93/112` 三处赋值，**全仓 0 处读取**）；`type:2` 撑不起 7 型；xattr 与设备号无落点。**一次打包为 v1.2。**
> 📄 **权威详版**：`Docs/disk_layout_v1.2_inode.md`（逐字节表 / xattr 块结构 / 不变量增补 / 迁移面 / type 常量普查 §8.1）；本节为摘要。

**第一字（u64）** = `type:6 | is_data_direct:1 | i_flags:57`

| 位 | 字段 | 含义 |
|---|---|---|
| `[0:6)` | `type` | = `(i_mode & S_IFMT) >> 12`；值域 `{FIFO:1, CHR:2, DIR:4, BLK:6, REG:8, LNK:10, SOCK:12}` |
| `[6]` | `is_data_direct` | 仅 REG/SYM；DIR 与无数据类型忽略（dir 里恒 0，两结构同构） |
| `[7:64)` | `i_flags` | 镜像 Linux `i_flags`（`S_*`，当前用 `[0:20)`） |

**`permission_bits`（u64）** = `mode:12 | xattr_ptr:52`

| 位 | 字段 | 含义 |
|---|---|---|
| `[0:12)` | `mode` | `= i_mode & 0o7777`（rwxrwxrwx + suid/sgid/sticky） |
| `[12:64)` | `xattr_ptr` | **52 位块指针**（对齐 `ptr_in_idx.ptr:52`）→ 自描述 xattr 块；**无 xattr = 52 位全 1**（`PTR_INVALID` 约定，**禁用 0**） |

> **恒等式**：`i_mode = (type<<12) | mode`（两切片**不相交**，拼回零冗余）。**建议改名 `mode_xattr`**。
> **rdev（仅 chr/blk）**：存 `inline_data[0:4]` = **小端 u32**（`new_encode_dev`；12 位 major + 20 位 minor；上限若需更大 → `inline_data[0:8]` 小端 u64）——**ext2 先例**（存 `i_block[0]`，`fs/ext2/inode.c:1550`）；`size==0` ⇒ 该区对该类型定义性空闲。
> **xattr = 单块指针（非三态）**：inode 只存一个 52 位指针 → 自描述块；上限 = 一个块（`h_blocks` 死字段，`fs/ext4/xattr.c:201` 硬断言 `==1`）；放不下 → `-E2BIG`。**故 inode 无 xattr 的 size/root/direct 字段，`padding[2]` 不动。**
> **连带**：type 值域变 → god 全部 `INODE_TYPE_*`（0/1/2 → 8/4/10）与分派点；audit 增 xattr 块校验 + 无类型 inode 的 `inline_data` 第 3 释义 + rdev；mkfs 写新字段。**清单见 `disk_layout_v1.2_inode.md` §8.1。**

### 4.6 XA — xattr / ACL

> **E7 已决 = 真需求。** 盘上 = **单块自描述 xattr 块**（ext2 模型）。

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-XA-01 | 通用 xattr：`getxattr/setxattr/listxattr/removexattr`（user/security/system/trusted） | A: ext2/4 `listxattr` | **M** | `.listxattr` + 自写 set/get；v1.2 xattr 块 | ❌ |
| R-XA-02 | **`security.capability`**（file capabilities，`setcap`） | A: 纲领 §1-C | **M** | 同上（首要用例） | ❌ |
| R-XA-03 | `system.posix_acl_access` / `posix_acl_default`（ACL **就是 xattr**） | A: 纲领 §1-C | **M** | `get_inode_acl`/`set_acl` + `posix_acl_*` | ❌ |
| R-XA-04 | **盘面槽位（v1.2 已定）**：xattr 指针落 `permission_bits` 高 **52 位**（**不是 `padding`**）→ 不改 inode 尺寸 | A: `disk_layout_v1.2_inode.md` §2.2；E2 | **M** | 与 `type:6` 打包为一次 v1.2 | ⚠️ 待落地 |

> **注 R-XA-04**：xattr 块 = **1 个 FS_block（4K）**（bh 天然粒度；8K 节点对齐是 B+tree 的，xattr 不受其约束）。块头 `h_magic = 0x52465358`（"RFSX"，**仅识别、非校验和**；u32 足够——xattr 无容灾重建/全盘扫描需求）。命名空间 index 直接抄 ext4（`user=1, acl=2/3, trusted=4, security=6, system=7`）。`h_refcount` 留作将来共享（v1.2 恒 1）。

### 4.7 SP — 空间与规模

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-SP-01 | `statfs`：`f_type`（= `s_magic`，唯一）、`f_bsize`、`f_blocks`、`f_bfree`、`f_bavail`、`f_files`、`f_ffree`、`f_namelen`(552)、`f_fsid` | A: `linux_vfs_iface_spec` §5 | **M** | `fs_statfs`（位图汇总） | ❌ |
| R-SP-02 | inode 数 ≥ **50 万~100 万** | A: 纲领 §1-D | **M** | mkfs 参数 | ⚠️ |
| R-SP-03 | 卷 20–40GB（`root-ravenfs.qcow2`） | A: 纲领 §3.3 | **M** | mkfs | ✅ 盘已备 |
| R-SP-04 | 单文件 ≥ ~1GB（`linux-firmware`） | A: 纲领 §1-D | **M** | extent 树多级 | ⚠️ |
| R-SP-05 | 单目录条目数 ≥ 上千（`/usr/lib`、`/usr/share/man`） | A: 纲领 §1-D | **M** | dentry 树 | ⚠️ |
| R-SP-06 | `s_maxbytes` **= `1ULL<<52`（4 PiB，字节）**；硬约束 ≤ `MAX_LFS_FILESIZE`(2^63-1) | A: `fs.h:1318/1323`；E6 | **M** | `sb->s_maxbytes`；落 `RAVENFS_MAX_FILESIZE` + `static_assert` | ✅ 已定 |
| R-SP-07 | `ENOSPC`：空间耗尽返回干净错误，不损坏 | A: 不变量写序 | **M** | 分配失败上传 | ⚠️ |

> **注 R-SP-06（E6）**：逻辑块号 = 2^40（可由 extent 键 `fblkbase:u64` 表示）；`sector_t`/`kstat.blocks`（512B 单位）= 2^43；全 ≤ `loff_t`。**「52」必须是字节**（52 位块 × 4K = 2^64 > `loff_t`，非法；`fs.h:1318` 明说 FS 必须让 `s_maxbytes ≤ MAX_LFS_FILESIZE`，否则 VM 出乱）。
>
> **注 R-SP-01（名字长度，2026-09-26 裁决 → 取 (a)）**：`name_heap` 的 552B 路径**保留、不丢弃**（v1.2 盘面不动）。**事实校正**：VFS 对单个组件名**没有** `NAME_MAX(255)` 硬拦截——实际上限是 `PATH_MAX` 量级（`fs/namei.c` 走 `EMBEDDED_NAME_MAX`；`fs/readdir.c::verify_dirent_name` 判 `len < PATH_MAX`），故“552 在 Linux 下自动成死代码”**不成立**。**决策（用户 2026-09-26）**：既然 syscall/VFS 层不拦，就**按 (a) 走上限** → **保留 552、`f_namelen=552`、>255 名为活路径且必须正确**；ko 不做 255 收口。⇒ R-SP-01 的 `f_namelen(552)` **定**。

### 4.8 SY — 同步与一致性

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-SY-01 | `fsync`/`fdatasync` 真持久 + 错误回报（卷级见 `sync_fs`） | A: ext2 | **M** | `sync_fs`→`fs_sync` | ❌ |
| R-SY-02 | `syncfs(2)`：遍历卷内 inode 落盘 | A: `vfs.rst` | **M** | `sync_filesystem` | ❌ |
| R-SY-03 | **位图持久化**：每次 `alloc/free` 标脏 + `sync_fs` 落盘（fsck 空实现下的硬约束） | A: 纲领 §4 | **M** | god 位图 flush | ⚠️ 仅 umount flush |
| R-SY-04 | **写序 + flush**（D-C1）：分配类「位图亮灯→数据落盘→inode 翻转」；删除类「先删引用→后计数」 | A: 不变量 20/25 | **M** | 照抄不变量；`blkdev_issue_flush` | ⚠️ |
| R-SY-05 | flush 语义：`blkdev_issue_flush`/FUA，保证「fsync 返回即落盘」 | A: `vfs.rst` | **M** | `blkdev_issue_flush` | ❌ |
| R-SY-06 | `put_super` 干净卸载（T3 判据） | A: 纲领 §2.1 | **M** | R-LM-04 | ❌ |
| R-SY-07 | 崩溃目标 = **「不产生损坏 / 不放大损害」**（非「不丢数据」） | A: 纲领 §4 | **M** | 写序 + remount-ro（R-LM-09） | ❌ |

### 4.9 CO — 并发与锁

| ID | 需求 | 依据 | 优先 | 框架优先落点 | 现状 |
|---|---|---|---|---|---|
| R-CO-01 | 尊重 VFS `i_rwsem` 契约（**不重复加锁**）：同 inode 的目录改操作 / 文件写由 VFS 串行化 | A: `fs/namei.c:1935/2918` | **M** | 不重复加锁 | ✅ |
| R-CO-02 | **节点 latch 模型（A）**：B+tree 节点逻辑原子性由 FS 自建 **node latch**；页锁只挡回收·回写，**逐页 RMW、绝不嵌套两页** | 决议；E1/锁模型 | **M** | **latch 表**（按节点块号索引，独立于 folio 生命周期） | ❌ |
| R-CO-03 | 位图分片锁（`bitmap_slices`）→ 平移 `spinlock_t` | A: `runtime_tech/02_bitmap_slices.md` | **M** | spinlock 平移 | ✅ god 有 |
| R-CO-04 | **跨线程真竞态面**（`i_rwsem` 覆盖不到）：全局 bitmap/allocator、回写·evict·sync 线程 vs 前台 | 推断(C) | **M** | node latch + bitmap 锁 + `invalidate_lock` | ❌ |

> **注 R-CO-02（模型 A）**：锁层次 = `node latch`（外层，管逻辑）→ `folio lock`（内层，管缓存一致性）；**逐页来，绝不同时持两页** → 无 2 页锁序问题。
> **VFS 已免费提供大半**：同 inode 的目录改操作/文件写由 `i_rwsem` 串行化 → 同 inode 的节点并发基本不用管。
> **真正要自建锁的**：① 全局 bitmap/分配器（R-CO-03）；② **回写/evict/sync 线程**（在 syscall 的 `i_rwsem` 之外）→ node latch 必须能被这些路径获取；③ B+tree 分裂/合并多节点（dir/extent 树写入整段在 `i_rwsem` 写锁下 → 顺序问题大半被 VFS 掩盖，主要落在 ②）。
> **latch 存储**：folio 会被回收 → latch **不能存节点内存里** → 用**按块号索引的 latch 表**（`xarray`/哈希）。b1 的 bh 自带 `b_state` 锁可复用，但 8K 跨两个 4K bh 仍需节点级 latch 罩住那一对。

### 4.10 DG — 可降级 / 豁免（明确 `-EOPNOTSUPP`）

| ID | 项 | 归属 |
|---|---|---|
| R-DG-01 | `fiemap` | C |
| R-DG-02 | `SEEK_HOLE`/`SEEK_DATA` | C |
| R-DG-03 | `chattr`/`lsattr`（`fileattr_get/set`；v1.2 字段已留，**当前版本不做** → `-EOPNOTSUPP`） | C |
| R-DG-04 | `FITRIM`（discard） | C |
| R-DG-05 | `i_version`/`btime` | C |
| R-DG-06 | quota | C |
| R-DG-07 | `FICLONE`（`remap_file_range`） | C |
| R-DG-08 | journal / CoW / 在线碎片整理 / 动态 inode 分配 | 非需求（架构边界；原 `plan.md` 已删，git 历史可溯） |

> **注 §4.10（chattr 降级，2026-09-26 定）**：当前版本**不实现** `fileattr_get/set` → `chattr/lsattr` 直接 `-EOPNOTSUPP`；v1.2 `i_flags:57` 字段保留（将来实现**不动盘面**）。
> **纪律（将来做时）**：**做则做真**——`S_IMMUTABLE`/`S_APPEND` 的强制由 **VFS 通用提供**（`fs/namei.c:641/3700`、`fs/file_attr.c:195` 的 `CAP_LINUX_IMMUTABLE`、`fs/inode.c:2276` 的 noatime），FS 只需 `inode_set_flags()` + `fileattr_get/set` 三件；**严禁“只存取不执行”的半开**（`chattr +i` 报成功却仍可写 = 欺骗）。ext 专有 flag（`-j/-e/-d/-s/-t`）永远 `-EOPNOTSUPP`。

### 4.11 IO — I/O 与缓存后端（**已定**）

| ID | 需求 | 依据 | 优先 | 说明 |
|---|---|---|---|---|
| **R-IO-01a** | **inode 缓存 → VFS inode**：不移植 `inode_cache_radix`；`struct inode` 内嵌 FS 私有结构（ext2 `ext2_inode_info` 模式）；`iget_locked/ilookup` 取代缓存查找；三后端（no_cache/self_reliat/offload）消失 | A: `fs/ext2/ext2.h`；E1 | **M** | 连带 R-LM-01/02/03；`.dirty()`→`mark_inode_dirty` |
| **R-IO-01b** | **元数据节点缓存 → Linux 页缓存**：不移植 `mdata_piece_caches`；8K 片走页缓存（4K 页 = 2 页/片）；RMW 采 **(A)**；机制 = **b1 buffer head** | A: E1/E8 | **M** | `sb_getblk`/`__bread_gfp`/`mark_buffer_dirty`；LBS 已排除 |
| R-IO-02 | **块 I/O 后端**：`bio`/`submit_bio`（`sb->s_bdev`）替换用户态 `pread/pwrite` | A: 架构收敛（E1） | **M** | `io_backend.h` kernel 实现 |

> **注（架构收敛）**：god 的 `inode_cache_radix` / `mdata_piece_caches` / `cache_mgr_mode` 三件**不进 ko**；算法层对 inode 的访问改道 **`RAVENFS_I(inode)`** 适配层（`inum`↔`i_ino`）。
> 仍在 god 层保留：`inode_alloc/inode_free`（**盘上槽位**位图分配，非缓存）、B+tree / name_heap / bitmap 算法本体。
> **b1 注**：bh 自带 `b_state` 锁（合模型 A），但 bh=4K → 8K 节点 = 2 bh → **节点级 latch 表仍必需**。⚠️ 遗留实测：`buffer_head.b_size > PAGE_SIZE`（8K bh）是否支持（定 1 bh 还是 2 bh/节点）。

---

## 5. 追溯矩阵（Arch 组件 → syscall → VFS 回调 → 需求）

| Arch 组件 | 关键 syscall | VFS 回调 | 需求 ID |
|---|---|---|---|
| `ld.so` | `openat`/`mmap(PROT_EXEC)`/`read` | `lookup`/`mmap`/`read_iter` | R-NS-01, R-FD-03, R-FD-01 |
| `systemd`(PID1) | `mount`/`statfs`/`readlink`/`statx` | `mount`/`statfs`/`get_link`/`getattr` | R-LM-*, R-SP-01, R-NS-06, R-MD-01 |
| `systemd-journald` | `fallocate`/`fsync`/`O_TMPFILE` | `fallocate`/`fsync`/`tmpfile` | R-FD-07, R-FD-04, R-NS-09 |
| `systemd-tmpfiles` | `mknod`/`setxattr`/`setfacl`/`chmod`/`chown` | `mknod`/xattr/ACL/`setattr` | R-NS-07, R-XA-01/03, R-MD-02 |
| `pacman` | `rename`/`link`/`symlink`/`statfs`/`fsync`/`unlink`/xattr | 同 | R-NS-08/05/06/04, R-SP-01, R-FD-04, R-XA-* |
| `coreutils` `cp`/`mv`/`install` | `copy_file_range`/`renameat2`/`fallocate`/`splice` | `splice`/`rename`/`fallocate` | R-FD-05/06/07, R-NS-08 |
| `sudo`/`su`/`passwd` | `open`+setuid/`chmod` | `getattr`/`setattr`/权限位 | R-MD-03, R-MD-02 |
| `find`/`du` | `readdir`/`getdents`/`stat` | `iterate_shared`/`getattr` | R-NS-11, R-MD-01/07 |
| `tar`/`rsync -aHAX` | `setxattr`/`link`/`mknod`/稀疏 | xattr/ACL/`link`/`mknod` | R-XA-*, R-NS-05/07, R-FD-09 |
| `getcap`/`setcap` | `getxattr`/`setxattr`(`security.capability`) | xattr | R-XA-02 |
| `mount -a`/`fstab` | `mount`/`show_options` | `init_fs_context`/`show_options` | R-LM-11/08 |
| `sync(1)`/`syncfs(2)` | `syncfs` | `sync_fs` | R-LM-05, R-SY-02 |

---

## 6. 决策记录（E1–E8，全收敛）

> 第三约束要求「非良定义即上报」。以下每条均已裁决；**待裁项已清空**。

| # | 议题 | 决议 |
|---|---|---|
| **E1** | 缓存/IO 后端 | **全部下沉 Linux**（E1 → R-IO-01a/01b/02 + 锁模型 (A)）；机制子决策 **E8** |
| **E2** | 盘格式 v1.2 | **已收敛**：见 §4.5.1（`type:6｜direct:1｜i_flags:57`；`mode:12｜xattr_ptr:52`；rdev→`inline_data`；xattr=单块）；**字节 offset 不变、旧盘不兼容** |
| **E3** | 目录 nlink 语义 | **报 `size+2`**（过报安全、零成本；`man find -noleaf`） |
| **E4** | readdir cookie 并发 | **单次 syscall 原子**（读锁）；**跨调用不阻止并发修改**；cookie 容错重扫 |
| **E5** | 时间戳触发表 | **不自建**：采纳内核模型（含 mmap）；FS 义务仅 3 处 |
| **E6** | `s_maxbytes` | **`1<<52`（4 PiB，字节）**；≤ `MAX_LFS_FILESIZE` |
| **E7** | xattr 是否真需求 | **真需求**；单块上限 |
| **E8** | R-IO-01b 机制 | **b1 buffer head**（遗留实测：8K bh 支持） |
| 附 | **LBS 排除** | `sb_set_blocksize(8192)` 合法（需 `FS_LBS`+THP，ext4/xfs 均声明，`block/bdev.c:219-246`），但 `s_blocksize`=分配单元=4K 与 8K 节点语义冲突 → **不适用**。故「8K 跨 2 页」既定，由 (A) 消化 |

---

## 7. 与现有文档的接口

1. **纲领 §1**：本文件接管；纲领清单已改为**指向本文的指针**（2026-09-26 执行完毕）。**冲突一律以本文为准。**
2. **`Docs/plan.md`（超远期路线规划）已删（2026-09-26）**：项目奠基期的分层规划在演进中系统性漂移，**不再作为真值源**；历史在 git 中可回溯。本文 R-IO-02 / R-DG-08 原引用其 Phase/清单，现已本地化，不再外部依赖。
3. **真值源与角色（2026-09-26 定）**：**god（`juvenile_god/`）是参考实现，`ravenfs.ko` 是实际实现，本文是权威规格**。god 提供能力包线 + 参考语义 + helper 选型；**ko 不照搬 god 的代码结构**（缓存/IO 已下沉 Linux，见 R-IO-*）。
4. 纲领引用的 `ravenfs-linux-ko-port-gaps.md` / `ravenfs-fsck-design.md` **仓库中不存在** → 缺口权威表 = 本文 §4「现状」列 + `disk_layout_v1.2_inode.md` §8.1；`fsck -n` 并入 `audit_inode_design.md`。
5. **`disk_layout_v1.2_inode.md`** = v1.2 盘面权威详版（本文 §4.5.1 为摘要）；**v1.2 C/C++ 盘面头 = `include/disk_struct_v1.2.h`**（掩码+宏，无位域）。

---

## 8. 落地顺序（→ 开工）

```
① 盘面：v1.2 C/C++ 盘面头 `include/disk_struct_v1.2.h`（掩码+宏替代位域：type:6/i_flags/mode_xattr/xattr 结构/常量/版本 3）
   └ 按 disk_layout_v1.2_inode.md §8.1 啃 12 个语义点（god/audit/mkfs）
② mount 骨架：fs_type 注册 + sb 读校验 + alloc_inode/evict_inode/write_inode（R-KP/R-LM）
③ 只读映射：T0（mkfs + mount RO + audit_filesystem 全绿）
④ 补齐 §4 的 M 项（link/rename/readdir/getattr/setattr/fallocate/xattr/mknod）
⑤ T1 xfstests -g quick → T2 fsx/fsstress → T3 完整安装 → T4 启动
```

**顺序纪律**：**T1 的缺口表出来前，不要开始 T3**（否则在 Arch 里用「莫名其妙的安装失败」重学 xfstests 早就能精确告诉你的东西）。

---

> **一句话记**
>
> **需求来自用户态（O1），接口来自 VFS 契约（O2），路线来自 ext2→ext4/btrfs 包线（O3），判据来自 xfstests/audit（O4）。**
> **净需求 = ext2 级 − 内核代劳 + Arch 硬增量 + v1.2 盘面**；缓存/IO 全下沉 Linux、锁模型 (A)、时间戳靠内核、xattr 单块、`s_maxbytes=1<<52`。
