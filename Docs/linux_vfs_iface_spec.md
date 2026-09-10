# RavenFS → Linux VFS 接口规格（rename / link / symlink / readdir / getattr / setattr / statfs）

> 2026-09-10 调研（Raven）。**地面真值 = 本机内核源码树**
> `/home/PS/PS_git/custom-kernel/linux-vfio/src/linux-7.2.3/`
> （运行中 7.2.3-1-vfio；本文所有签名/常量/注释均逐字摘自该树，非记忆）。
> 权威文字规格：`Documentation/filesystems/vfs.rst`。
> 参考实现：`fs/ext2/`（磁盘型 FS，与 RavenFS 最同构）+ `fs/libfs.c`（极简 helper）。

---

## 0. 一句话总览

RavenFS 的 FS 层算子 → Linux 回调的映射（VFS 层（namei/dcache/icache/锁）由内核提供，不做）：

| 需求 | 回调（结构体.字段） | RavenFS 侧对应实权接口 |
|---|---|---|
| 解析名字→inode | `inode_operations.lookup` | `dir_lookup(inode_idx,name)` |
| 建普通文件 | `.create` / `.mknod` | `dir_insert_dentry(inode_idx,name,REGULAR)` |
| 建目录 | `.mkdir` | `dir_insert_dentry(...,DIR)` |
| 硬链接 | `.link` | **缺**（insert 恒建新 inode） |
| 删文件 | `.unlink` | `dir_rm_dentry(inode_idx,name)` |
| 删目录 | `.rmdir` | 同上（dir 型内含非空校验） |
| 符号链接 | `.symlink` + `.get_link` | **缺**（只有空 SOFT_SYM inode） |
| 改名 | `.rename` | **缺** |
| 读目录 | `file_operations.iterate_shared` | **缺**（readdir 未实装） |
| 取属性 | `.getattr` | **缺**（字段有，无导出） |
| 设属性 | `.setattr` | 部分（truncate 有；chmod/chown/utimes 缺） |
| 空间统计 | `super_operations.statfs` | **缺**（位图在手，无接口） |

---

## 1. 命名空间操作（`struct inode_operations`）

完整结构见 `include/linux/fs.h`。**总纲（vfs.rst 原文）**：

> *all methods are called without any locks being held, unless otherwise noted.*

### 1.1 签名（逐字）

```c
struct dentry *(*lookup) (struct inode *, struct dentry *, unsigned int);
int  (*create) (struct mnt_idmap *, struct inode *, struct dentry *, umode_t, bool);
int  (*link)   (struct dentry *, struct inode *, struct dentry *);
int  (*unlink) (struct inode *, struct dentry *);
int  (*symlink)(struct mnt_idmap *, struct inode *, struct dentry *, const char *);
struct dentry *(*mkdir) (struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
int  (*rmdir)  (struct inode *, struct dentry *);
int  (*mknod)  (struct mnt_idmap *, struct inode *, struct dentry *, umode_t, dev_t);
int  (*rename) (struct mnt_idmap *, struct inode *, struct dentry *,
                struct inode *, struct dentry *, unsigned int);
```

### 1.2 语义 / 义务（vfs.rst 原文要点）

- **lookup**：在父目录里查 dentry 名。命中 → `d_splice_alias(inode, dentry)`（或 `d_add`）；未命中 → NULL inode（negative dentry），返回 NULL。**只有真错误才返 ERR_PTR**，否则 create/mkdir 等会失败。**调用时持有目录 inode semaphore（`i_rwsem`）**。
  - 名字过长：`return ERR_PTR(-ENAMETOOLONG)`。
  - ext2 参考：`ext2_inode_by_name` → `ext2_iget(sb, ino)`（**必须复用 iget 保证 inum→唯一 inode 指针**）→ `d_splice_alias(inode, dentry)`；`iget` 返回 `-ESTALE`（被删 inode）要升级为 `-EIO` 且 `ext2_error`。
- **create**：open(2)/creat(2)。dentry 是 negative。建完 `d_instantiate(dentry, inode)`（新 inode 用 `d_instantiate_new`）。
- **link**：link(2)。`d_instantiate()`。硬链接。
- **unlink / rmdir**：删除。rmdir 要先判空（`-ENOTEMPTY`）。
- **symlink**：symlink(2)。建完 `d_instantiate()`。
- **mkdir**：`d_instantiate_new()`。**目录只能有一个 dentry（VFS 硬规则）**——若可能被另一路径提前实例化（网络 FS / fh 导出），要 `d_drop()` + `d_splice_alias()`。
- **rename**：**不支持的 flags 必须 `return -EINVAL`**。
- **所有 dir 级改操作（create/mkdir/link/unlink/rmdir/symlink/rename）调用时持有父目录 `i_rwsem`（写）**；rename 同时持有两个父目录（`lock_rename()`）。

### 1.3 rename 的 flags（`include/uapi/linux/fs.h`）

```c
#define RENAME_NOREPLACE (1 << 0)  /* 目标存在 → -EEXIST（VFS 已查，本地 FS 等价普通 rename）*/
#define RENAME_EXCHANGE  (1 << 1)  /* 交换源/目标；两者必须存在（VFS 已查）；可异型 */
#define RENAME_WHITEOUT  (1 << 2)  /* 源出白洞（overlay/XFS 用）*/
```
- 现代内核 rename 回调经 `struct renamedata`（`fs/namei.c: vfs_rename()`）：
  ```c
  struct renamedata {
      struct mnt_idmap *mnt_idmap;
      struct dentry *old_parent, *old_dentry, *new_parent, *new_dentry;
      struct delegated_inode *delegated_inode;
      unsigned int flags;
  } __randomize_layout;
  ```
- **rename 是 FS 逻辑**：VFS 只做 syscall→锁→回调。FS 必须自己处理：目标覆盖（unlink 旧目标）、**目录改名的 `..` 重写 + 父目录 nlink 增减**（`old_dir != new_dir && old_is_dir` 时：旧父 `drop_nlink`、新父 `inc_nlink`、改子目录 `..` 指新父）。
- ext2 参考（`fs/ext2/namei.c: ext2_rename`）：只支持 `RENAME_NOREPLACE`，其余 `-EINVAL`；`if (old_is_dir && old_dir != new_dir) ext2_dotdot()` 取目标 `..` 项；`ext2_set_link(...)` 重写；收尾 `mark_inode_dirty(old_inode)`。

### 1.4 最小实现捷径：`fs/libfs.c`

内存型/极简 FS 可直接用：
- `simple_rename`（含 `RENAME_NOREPLACE|RENAME_EXCHANGE` 分支、nlink 维护、`simple_rename_timestamp`）。
- `simple_getattr` / `simple_setattr`（setattr 不含 on-disk size 变更）。

⚠️ RavenFS 是**磁盘型**，rename/getattr/setattr 必须自己写（size 变更要动 extent 树）。

---

## 2. 符号链接（symlink 建 + get_link 解）

### 2.1 建：`inode_operations.symlink`

`int (*symlink)(struct mnt_idmap *, struct inode *dir, struct dentry *, const char *symname);`

两种形态（**ext2 同款判据**：target 长度 vs inode 内联数据区大小）：
- **fast symlink**：target 塞进 inode 内联区（RavenFS 的 `inline_data[160]`）。设 `inode->i_link = 内联地址; memcpy; i_size = len-1;`，`i_op = &fast_symlink_inode_operations`（`.get_link = simple_get_link`）。
- **slow symlink**：`i_op = &symlink_inode_operations`（`.get_link = page_get_link`），`inode_nohighmem(inode)`，`i_mapping->a_ops = ...`，`page_symlink(inode, symname, len)`。走 page cache 读。

> RavenFS 决策点：target ≤160B 走 fast（零数据块）；更长走 slow（占用 extent 树）。`page_symlink` 依赖 `a_ops`/page cache——若坚持私有 buffer cache，需自写 `get_link` 或自管 symlink 页。

### 2.2 解：`inode_operations.get_link`

```c
const char *(*get_link)(struct dentry *, struct inode *, struct delayed_call *);
```
- 返回要跟随的 symlink 体。**可能处于 RCU 模式（dentry 参数为 NULL）**；无法在 RCU 下处理 → `return ERR_PTR(-ECHILD)`。
- 若体存活到 inode 消亡，直接返回即可；否则 `set_delayed_call(done, destructor, arg)` 安排释放。
- **若 target 存 `inode->i_link`，VFS 可跳过回调直接用；但 `->get_link` 仍必须提供。`i_link` 在 iget 后写入需 release barrier，且不得在 RCU grace period 前释放。**
- `->readlink` 一般不用实现（readlink(2) 自动走 get_link）。

---

## 3. readdir（目录迭代）

### 3.1 挂载点：`file_operations.iterate_shared`

```c
int (*iterate_shared) (struct file *, struct dir_context *);
```
- 旧 `.readdir` 已废弃（`.iterate`/`iterate_shared` 单一）。
- **调用上下文**：`fs/readdir.c: iterate_dir()` 会先 `down_read_killable(&inode->i_rwsem)` → 调 `iterate_shared` → `up_read`。即**回调在目录 inode 共享读锁下运行**。`ctx->pos` 由 VFS ↔ `file->f_pos` 同步。

### 3.2 cookie/pos 模型

```c
struct dir_context {
    filldir_t actor;
    loff_t    pos;     /* in/out：FS 负责维护，用户态 telldir/seekdir 的游标 */
    int       count;   /* 缓冲区提示，FS 不得修改 */
    unsigned int dt_flags_mask;
};
typedef bool (*filldir_t)(struct dir_context *, const char *name, int namelen,
                          loff_t pos, u64 ino, unsigned type);
```

- 输出走 `dir_emit(ctx, name, namelen, ino, DT_*)`（= `ctx->actor(...)`）；返回 **bool：false = 缓冲满，停止**。
- `dir_emit_dots(file, ctx)`：**`. ` / `..` 由 helper 合成**（`ctx->pos==0` 发 `.`，`==1` 发 `..`）。**不要落盘 `.`/`..` 条目**——与 RavenFS「readdir 路径合成 . / ..」的设计吻合。
- 类型 d_type 用 `DT_*`（`fs_umode_to_ftype()` 可在有 i_mode 时转换）。
- ext2 用「页号<<PAGE_SHIFT | 页内偏移」当 cookie；RavenFS 没有稳定字节偏移，可用**叶子块号 + 槽内索引**编码 cookie（B+tree hash 序，`ctx->pos` 遍历期间需处理并发修改的 revalidate）。

### 3.3 语义
- 返回 `-ENOTDIR`（非目录）、0（成功读完）。`iterate_dir` 已挡 `IS_DEADDIR` → `-ENOENT`。
- 目录改动的顺序不保证（与 RavenFS「hash 乱序输出」一致，可接受）。

---

## 4. getattr / setattr

### 4.1 getattr

```c
int (*getattr)(struct mnt_idmap *, const struct path *, struct kstat *, u32 request_mask, unsigned int query_flags);
```
**标准写法**（ext2/大多数 FS）：
```c
int xxx_getattr(...) {
    struct inode *inode = d_inode(path->dentry);
    /* 可选：填 stat->attributes / attributes_mask（FS 专属 statx flags） */
    generic_fillattr(idmap, request_mask, inode, stat);
    return 0;                 /* 只读 inode 内存态，不碰盘 */
}
```
`generic_fillattr`（`fs/stat.c`）从 `inode` 填：`dev/ino/mode/nlink/uid/gid/rdev/size/atime/mtime/ctime/blksize/blocks`（含 `STATX_CHANGE_COOKIE`/i_version 分支）。

`struct kstat`（`include/linux/stat.h`）关键字段：`mode, nlink, blksize, ino, dev, rdev, uid, gid, size, atime, mtime, ctime, btime(创建时间), blocks, attributes, attributes_mask, change_cookie, dio_*_align, atomic_write_unit_*`。

> RavenFS 落点：inode 字段 → `inode->i_*` 的填充发生在 `iget`/`read_inode`（你的 `inode_read`）。getattr 本身几乎零逻辑。

### 4.2 setattr

```c
int (*setattr)(struct mnt_idmap *, struct dentry *, struct iattr *);
```
标准骨架（ext2 原文）：
```c
err = setattr_prepare(idmap, dentry, iattr);          /* VFS 做权限/合法性检查 */
if (...) 配额检查...
if (iattr->ia_valid & ATTR_SIZE && iattr->ia_size != inode->i_size)
        err = xxx_setsize(inode, iattr->ia_size);     /* ★ 磁盘型必须自己实现 */
setattr_copy(idmap, inode, iattr);                    /* 把 iattr 拷进 inode 内存态 */
mark_inode_dirty(inode);                              /* ★ 标脏，走 write_inode */
```

`struct iattr`：
```c
struct iattr {
    unsigned int ia_valid;      /* ATTR_* 位图 */
    umode_t ia_mode;
    kuid_t ia_uid;  kgid_t ia_gid;   /* 或 vfsuid_t/vfsgid_t（idmap） */
    loff_t ia_size;
    struct timespec64 ia_atime, ia_mtime, ia_ctime;
    struct file *ia_file;       /* 有 ATTR_FILE 时才是 ftruncate */
};
```

`ATTR_*`（`include/linux/fs.h`）：`MODE/UID/GID/SIZE/ATIME/MTIME/CTIME/ATIME_SET/MTIME_SET/FORCE/CTIME_SET/KILL_SUID/KILL_SGID/FILE/KILL_PRIV/OPEN/TIMES_SET/TOUCH/DELEG`。

- `setattr_prepare` 已做：size 上界、chown/chgrp/chmod 权限、setgid 剥离、时间设置权限、KILL_PRIV。
- **`ATTR_SIZE` = truncate**：VFS 已保证 `i_rwsem` 等；你的 `direct_file_trunacate` 正好接。
- `ATTR_CTIME`/`ATTR_ATIME`/`ATTR_MTIME`：时间戳落盘——RavenFS v1.1 **当前全系不维护时间戳**，这是要补的。
- 也可用 `simple_setattr`（内存型，不含 on-disk size）——RavenFS 不适用。

### 4.3 相关 helper
- `setattr_prepare(idmap, dentry, attr)` / `setattr_copy(idmap, inode, attr)`（`fs/attr.c`）。
- `notify_change(idmap, dentry, attr, delegated_inode)`：VFS 统一入口。
- `inode_set_ctime_current/atime/mtime`、`inode_newsize_ok`、`truncate_setsize`。

---

## 5. statfs（`super_operations.statfs`）

```c
int (*statfs)(struct dentry *dentry, struct kstatfs *buf);
```
ext2 参考填法：
```c
buf->f_type    = EXT2_SUPER_MAGIC;      /* FS 魔数 */
buf->f_bsize   = sb->s_blocksize;       /* 4K */
buf->f_blocks  = 总块数 - overhead;      /* 数据区可用总块 */
buf->f_bfree   = 空闲块数;               /* ★ 位图直接算 */
buf->f_bavail  = f_bfree - 保留块;       /* 常 == f_bfree */
buf->f_files   = inode 总数;
buf->f_ffree   = 空闲 inode;             /* ★ inode 位图直接算 */
buf->f_namelen = 最大文件名长;            /* RAVENFS_MAX_NAME_LEN=552 */
buf->f_fsid    = uuid_to_fsid(sb->uuid);
```
> RavenFS 落点：`f_bfree`/`f_ffree` 从 `block_bits`/`inode_bits` 现算（你的 `bitmap_mgr` 有 `free_count`）。基本是零成本。

---

## 6. 顺带：`super_operations` 里其它必填项

`include/linux/fs/super_types.h`。磁盘型 FS 最少要挂：

| 回调 | 作用 | RavenFS 对应 |
|---|---|---|
| `alloc_inode` / `destroy_inode`(或 `free_inode`) | 从 sb 专属 slab 分配/销毁 `struct inode` | 内存对象池 |
| `write_inode` | 单 inode 写回（wb 调度） | `inode_write` |
| `evict_inode` | inode 淘汰（nlink==0 时回收） | `regular_file_reclaim` / dir 回收 |
| `put_super` | 卸载 | 析构（bitmap flush）→ **多进程下 put_super 与 open 文件的生命周期分离** |
| `sync_fs` | fsync 卷 | 无（当前仅 umount flush）→ 要补 |
| `statfs` | 见 §5 | 缺 |
| `dirty_inode` | 增量标脏（可选） | — |
| `show_options` | /proc/mounts 选项 | 可选 |

---

## 7. 并发/锁契约（移植时最容易踩）

| 回调 | VFS 调用时的锁 |
|---|---|
| `lookup` | 父目录 `i_rwsem`（读） |
| `create/mkdir/link/unlink/rmdir/symlink/mknod` | 父目录 `i_rwsem`（写） |
| `rename` | 两个父目录 `i_rwsem`（写）+ `sb->s_vfs_rename_mutex`（目录级 rename 串行）|
| `iterate_shared` | 目录 `i_rwsem`（读，`down_read_killable`）|
| `getattr/setattr` | 无 VFS 锁（FS 自理）|
| 文件读写 | `i_rwsem` 视 open 模式（读/写锁），另由 `address_space`/`invalidate_lock` 管 |

> **RavenFS 现状**：god 类算子无 inode 级锁。内核里 VFS 会替你串行化**同一目录的目录操作**，但你的 **B+tree 节点写 / buffer cache / 位图必须自证并发安全**（位图分片锁可平移 `spinlock_t`）。

---

## 8. 附：本文引用的源码位置（7.2.3）

```
include/linux/fs.h                       struct inode_operations / file_operations / iattr / ATTR_* / dir_context / filldir_t / dir_emit / dir_emit_dots
include/linux/fs/super_types.h           struct super_operations
include/linux/stat.h                     struct kstat
include/uapi/linux/fs.h                  RENAME_* / O_*
Documentation/filesystems/vfs.rst        ★ 逐字段权威语义（lookup/create/.../rename/get_link/iterate_shared）
fs/namei.c                               vfs_rename / vfs_link / vfs_symlink / vfs_create(RENAME 锁注释)
fs/attr.c                                setattr_prepare / setattr_copy
fs/stat.c                                generic_fillattr
fs/readdir.c                             iterate_dir（i_rwsem 上下文）
fs/libfs.c                               simple_getattr/simple_setattr/simple_rename/simple_statfs
fs/ext2/namei.c                          lookup/create/symlink/link/mkdir/unlink/rmdir/rename 参考
fs/ext2/inode.c                          getattr/setattr 参考
fs/ext2/super.c                          statfs 参考
fs/ext2/symlink.c                        fast/slow symlink inode_operations 参考
fs/ext2/dir.c                            ext2_readdir（cookie 模型）
```
