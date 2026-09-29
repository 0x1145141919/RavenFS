# RavenFS — Inode 布局 v1.2（增补 `disk_layout_v1.1.md` §3）

> 2026-09-26 · Raven · **状态：草案，待 Sparrow 裁决后随「盘格式 v1.2」冻结**
> **定位**：`disk_layout_v1.1.md` **§3（Inode）的增量替代**。v1.1 其余章节（§4 B+tree / §5 区段树 / §6 dentry / §7 名字堆 / §10 不变量）**继续有效**，本文只改 inode。
> **起因**：`permission_bits` 实测**只写不读**（真·白地：仅 `mkfs/mkfs.cpp:332`、`juvenile_god/dentry_insert_delete.cpp:93/112` 三处赋值，全仓 0 处读取）；`type:2` 撑不起 7 型文件；**xattr 与设备号无落点**。→ 一次打包为 v1.2。
> **关联**：需求规格 `Docs/linux/ravenfs-linux-ko-requirements-spec.md` §4.5.1 / §6-E2；结构真值 `include/disk_struct_v1.1.hpp`（v1.2 落地时升版）。

---

## 0. 结论速览

| 项 | 结论 |
|---|---|
| **字节布局** | **不变**：仍 256B，各字段 offset 与 v1.1 **完全一致** → `static_assert(sizeof==256)` 保持 |
| **本质** | v1.2 = **同一批字节的「语义重定义」**（改的是位域含义与字段解读，不是尺寸/位置） |
| 改动 1 | 第一字：`type:2｜direct:1｜other:61` → `type:6｜direct:1｜i_flags:57` |
| 改动 2 | `permission_bits` → `mode:12｜xattr_ptr:52` |
| 改动 3 | `inline_data` 增第 3 释义：**无数据类型**（chr/blk）存 `rdev` |
| 新增结构 | **xattr 块**（自描述、**单块**、块头带 magic） |
| **破坏性** | **v1.1 盘不兼容**（第一字位域语义变，旧 `other=0` 会被误读为 `type` 高位）→ 需 reformat + `RAVENFS_VERSION` bump（建议 `2 → 3`） |

---

## 1. 动机（问题 → 现状 → 改法）

| # | 问题 | v1.1 现状 | v1.2 改法 |
|---|---|---|---|
| 1 | 权限位无落点且浪费 | `permission_bits:u64` 只写 12 位权限（`0o7777`），**52 位浪费、且无人读** | 重定义 `mode:12｜xattr_ptr:52`（64 位用满） |
| 2 | 文件类型数不够 | `type:2` 仅 REG/DIR/SYM，撑不起 `mknod`（chr/blk/fifo/sock） | `type:6` = `(i_mode & S_IFMT) >> 12`（7 型 + 空位） |
| 3 | **设备号无落点** | 无 `rdev` 字段 | chr/blk 的 `rdev` 存 `inline_data[0:4]`（**ext2 先例：ext2 存 `i_block[0]`**，`fs/ext2/inode.c:1550`） |
| 4 | **xattr / ACL 无落点** | 无 xattr 概念 | inode 存一个 **52 位块指针** → 自描述 **xattr 块**（ext2 `i_file_acl` 先例） |
| 5 | `chattr/lsattr` 无落点 | 无 `i_flags` | 第一字余位放 **`i_flags`**（镜像 Linux `S_*`） |

> **为什么 xattr 用「块指针」而不是复用「大小确定的数据三态」**：xattr 值**大小不固定**，但 ext2/4 的做法是**固定单块 + 块自描述**（条目数/长度写在块头与条目里），**inode 里不需要 size/root/direct 三态字段**。复用三态 = 给 xattr 开第二条数据通道（第二个 size/root/direct_bit）→ 涨 inode 或抢 `padding`，且迁移/物化/fsck 规则整套复制。属「自创语义」（第二约束顺位 4）。详见 §5。

---

## 2. 字段布局（逐位 / 逐字节）

### 2.1 第一字（offset `[0:8)`，u64）—— `type:6 ｜ is_data_direct:1 ｜ i_flags:57`

| 位 | 字段 | 含义 |
|---|---|---|
| `[0:6)` | `type` | `= (i_mode & S_IFMT) >> 12`；值域见 §3 |
| `[6]` | `is_data_direct` | 仅 REG/LNK 有意义（1=直指，0=B+tree）；DIR 与无数据类型**忽略**（`raven_dir_Inode_t` 里此位**恒 0**，两结构第一字位布局保持同构） |
| `[7:64)` | `i_flags` | 镜像 Linux `i_flags`（`S_*`，当前用 `[0:20)`）；其余预留。**存值不移位** |

### 2.2 `permission_bits`（offset `[32:40)`，u64）—— `mode:12 ｜ xattr_ptr:52`

| 位 | 字段 | 含义 |
|---|---|---|
| `[0:12)` | `mode` | `= i_mode & 0o7777`（rwxrwxrwx + suid/sgid/sticky） |
| `[12:64)` | `xattr_ptr` | **52 位块指针**（对齐既有 `ptr_in_idx.ptr:52` 约定）→ 指向 xattr 块（§5）；**无 xattr = 52 位全 1**（`PTR_INVALID` 约定，同 `NAME_HEAP_INACTIVE`）——**不得用 0**（0 是合法块号） |

> **恒等式**：`i_mode = (type << 12) ｜ mode`；`type` 与 `mode` 是 `i_mode` 的**两个不相交切片**，拼回**零冗余**（无「双真相源」）。
> **命名**：`permission_bits` 现已名不副实，**建议改名 `mode_xattr`**（或保留旧名 + 语义注释，二选一）。
> **哨兵与几何（2026-09-30 补）**：`xattr_ptr` 哨兵 = 52 位全 1（`2^52−1`）；有效块号 ∈ `[0, total_blocks)`，而 `total_blocks ≤ 2^52−1`（否则卷 > `2^64` B）→ 哨兵**几何不可达**，无需额外防护。

### 2.3 完整字节表（**offset 与 v1.1 一致，未变**）

`raven_Inode_t`（REG / LNK / CHR / BLK / FIFO / SOCK 共用）：

| offset | size | 字段 | v1.2 说明 |
|---|---|---|---|
| 0 | 8 | `word1` | `type:6｜direct:1｜i_flags:57` |
| 8 | 4 | `uid` | — |
| 12 | 4 | `gid` | — |
| 16 | 4 | `m_ns_stamp` | mtime 纳秒余数 |
| 20 | 4 | `c_ns_stamp` | ctime 纳秒余数 |
| 24 | 4 | `a_ns_stamp` | atime 纳秒余数 |
| 28 | 4 | `link_count` | 语义见 v1.1 §3.3 |
| 32 | 8 | `permission_bits` | **`mode:12｜xattr_ptr:52`** |
| 40 | 8 | `size` | 字节数 / 目录条目数 |
| 48 | 8 | `mtime` | 秒 |
| 56 | 8 | `ctime` | 秒 |
| 64 | 8 | `atime` | 秒 |
| 72 | 8 | `data_root` | `PTR_INVALID`=inline |
| 80 | 16 | `padding[2]` | **保持未来预留**（本方案不必动） |
| 96 | 160 | `inline_data` | 内容 / **无数据类型：`rdev` 在 `[96+0:96+4)`** |
| — | **256** | — | `static_assert` 不变 |

`raven_dir_Inode_t`（DIR）：差异仅两点 —— 第一字为 `type:6｜(bit6=0)｜i_flags:57`（无 `is_data_direct`），`inline_data` 换为 **10 × `name_heap_controller_t`（160B）**（v1.1 §7 不变）。**DIR 不存在 rdev。**

---

## 3. 文件类型（`type` = `S_IFMT >> 12`，镜像）

| `type` | `S_IFMT` | 名字 | 有数据？ | 有 `rdev`？ | `inline_data` 16..0B 含义 |
|---|---|---|---|---|---|
| 1 | `S_IFIFO` `0010000` | FIFO | 否 | 否 | 空闲 |
| 2 | `S_IFCHR` `0020000` | CHR | 否 | **是** | **rdev（4B）** |
| 4 | `S_IFDIR` `0040000` | DIR | dentry 树 | 否 | 堆描述符（另一结构） |
| 6 | `S_IFBLK` `0060000` | BLK | 否 | **是** | **rdev（4B）** |
| 8 | `S_IFREG` `0100000` | REG | 是 | 否 | 内容（`size ≤ 160`） |
| 10 | `S_IFLNK` `0120000` | LNK | target 串 | 否 | target（三态） |
| 12 | `S_IFSOCK` `0140000` | SOCK | 否 | 否 | 空闲 |

- 其余值（含 0）**非法** → audit 硬错。4 位够用，6 位为 headroom。
- **god 常量改名（破坏性）**：`INODE_TYPE_REGULAR 0→8`、`DIR 1→4`、`SOFT_SYM 2→10`；新增 `FIFO=1 / CHR=2 / BLK=6 / SOCK=12`。**需盘点全部 `type` 分派点。**

---

## 4. 无数据类型（CHR / BLK / FIFO / SOCK）

**盘上表示（"只有名字，没有身体"）**：

- `size = 0`、`data_root = PTR_INVALID`（落 **inline 态**）、**无 extent 树、无名字堆**；
- **CHR/BLK**：`rdev` 存 `inline_data[0:4]`（**小端 u32**），编码 = `new_encode_dev()`（`include/linux/kdev_t.h:39`：12 位 major + 20 位 minor 恰打包进 32 位）。**2026-09-26 定：统一小端 u32；上限若需更大 → 升 `inline_data[0:8]` 小端 u64**（同起点）。
- FIFO/SOCK：`inline_data` 无内容（建议置 0）；**不需要 `rdev`**；
- 非设备文件 `rdev` **无定义**（建议置 0），防止 `getattr` 里 `stat->rdev = i_rdev` 带出垃圾。

**创建入口**：VFS 四种类型**都走 `.mknod`**（`mkfifo` → `mknod(S_IFIFO)`；socket `bind()` → `vfs_mknod(S_IFSOCK)`，`net/unix/af_unix.c:1384`）。故 god 侧只需一个"造特殊 inode"入口。

> `rdev` 放 `inline_data` 的依据：**ext2 存 `i_block[0]`**（`fs/ext2/inode.c:1550`）；且 `size==0` ⇒ inline 区对无数据类型**定义性空闲**（与「内联区按 type 分型」一致）。

---

## 5. xattr 块（新增结构）

### 5.1 指针与上限
- inode 侧只有 **一个 52 位块指针**（`permission_bits.xattr_ptr`）；**块自描述** → inode 无需 xattr 的 size/root/direct 字段。
- **单块**（ext2/4 的 `h_blocks` 是死字段：`fs/ext4/xattr.c:201` 硬断言 `!= 1` 即报错，`:2025` 写入恒 1）→ 上限 = 一块；放不下 → `-E2BIG`。
- 块尺寸：**1 个 FS_block（4096B）**——**2026-09-26 定：统一 4K，非 8K piece**。不受 8K 节点对齐约束（8K 是 B+tree 节点形状，xattr 是独立结构；且 R-IO-01b 已弃 8K piece 缓存，b1 buffer_head 天然 4K）。
- **净容量** = `RFS_XATTR_USABLE` = 4096 − 32（块头）− 4（终止零）= **4060B**（条目区 + 值区共存于一块；超出 → `-E2BIG`）。

### 5.2 块头（建议 32B，对齐 ext 形状）
```c
struct rfs_xattr_block_hdr {   // 32B
    uint32_t h_magic;          // 识别魔数 = 0x52465358（"RFSX"）
    uint32_t h_refcount;       // v1.2 恒 1（共享留将来）
    uint32_t h_blocks;         // 恒 1（沿用 ext 死字段约定，防未来）
    uint32_t h_hash;           // 全集 hash（v1.2 可置 0；共享用）
    uint64_t h_reserved[2];    // 保留
};
```

> **为何 xattr magic 只给 u32（2026-09-26 定）**：B+tree 节点用 256-bit 强魔数，是为**容灾重建**——全盘扫描时低误报识别节点类型。而**灾难场景关心的是文件组织 + 目录树**，没人会去重建 xattr → xattr 块**无扫描级强度需求**，一个 u32 token（`"RFSX"`）意思意思即可；且 xattr 只经 inode 指针可达，指针跟随后的轻识别足够。

### 5.3 条目（**抄 ext4**：显式 `e_value_offs` + 值区从块尾向下 + 首个 u32==0 终止）
> **2026-09-26 裁定（用户）**：硬上限仍为 **1 个 FS_block**，但**头部允许加字段**，其余**抄 ext4**。
> 动机：原设计「value 紧随 name 内联 + 隐式推进/终止」对**对齐/终止规则留有歧义**（读/写不同实现会分叉）；ext4 用**显式值偏移**根治。

```c
struct rfs_xattr_entry {           // 8B 定长头；名字紧随其后（4B 对齐）；值在 e_value_offs 处
    uint8_t  e_name_len;           // 名字后缀长（不含命名空间前缀）
    uint8_t  e_name_index;         // 命名空间索引（见下，抄 ext4）
    uint16_t e_value_offs;         // 值在块内偏移（u16；块 ≤4K 够）；e_value_size==0 时忽略
    uint32_t e_value_size;         // 值长（字节）
    // char e_name[e_name_len];    —— 名字后**不内联 value**
};
#define RFS_XATTR_LEN(nl)      (((nl) + 3 + 8) & ~3ULL)     // 条目占位（名字 + 尾对齐），抄 ext4 EXT4_XATTR_LEN
#define RFS_XATTR_IS_LAST(e)   (*(const uint32_t *)(e) == 0u) // 终止：首个 u32 == 0（抄 ext4）
```

**两大区域（同一块内相向生长）**：
- **条目区**：从 `[32)` 起向高地址增长；每条 = `RFS_XATTR_LEN(e_name_len)`；**末条后补 4B 零**作为终止（`RFS_XATTR_IS_LAST`）。
- **值区**：从块尾 `4096` 向低地址堆叠，每个值 **4B 对齐**（`e_value_offs = down_align4(floor − size)`）；值区最低地址须 **≥ 条目区末端**。
- 自洽校验：`e_value_offs ∈ [32, 4096–size]` 且 4B 对齐；**值区间互不重叠**；条目区末端 ≤ 值区最低地址。

**相对 ext4 的删减**：无 `e_value_inum`（我们无「值外置 inode」；单块内联）；无 `e_hash`（v1.2 无共享）。⇒ 定长头 **8B**（vs ext4 16B）。

**命名空间索引**（直接抄 ext4 `fs/ext4/xattr.h:19`，省存前缀）：`user=1`、`posix_acl_access=2`、`posix_acl_default=3`、`trusted=4`、`security=6`、`system=7`。
**完整键** = `<前缀(index)><e_name>`。名长 ≤ `XATTR_NAME_MAX`(255)、值长受单块限制。

### 5.4 共享（v1.2 不做）
`h_refcount`/`h_hash` 仅为**将来**「相同 xattr 集共享一块」预留；v1.2 `refcount=1`、`hash=0`。启动共享需全局 find-or-create + 缓存，v1.2 不做。

### 5.5 audit 规则
`xattr_ptr ≠ 哨兵` ⇒ 认领该块 + 校验：`h_magic` / `h_refcount==1` / `h_blocks==1` / `h_hash==0` / 条目越界 / `e_name_index` 合法 / **`e_name_len` 规则**（`e_name_index ∈ {2,3}`=ACL → 后缀必须为空（`e_name_len==0`，ACL 键即前缀）；其余命名空间 → `e_name_len>0`；**抄 ext4**） / **终止符（首个 u32==0）存在** / **值偏移合法且 4B 对齐、值区间互不重叠、条目区末端 ≤ 值区最低地址** / 总长 ≤ 块；且该块在全局 bitmap 亮灯、被**恰一个** inode 认领。

### 5.6 存储契约完备性（VFS 驱动，待落地到 ko）
> 以下为**盘面外**但必须写进 ko 契约的行为（xattr 面），本节仅登记：
- **限额映射**：`size==0` 查询返回所需长度；`size<需要 → -ERANGE`；值放不进一块 → **`-E2BIG`**；`listxattr` 缓冲不足 → `-ERANGE`（内核 `XATTR_NAME_MAX=255 / XATTR_SIZE_MAX=65536 / XATTR_LIST_MAX=65536`）。
- **旗标**：`XATTR_CREATE`（在存在→`-EEXIST`）/`XATTR_REPLACE`（不存在→`-ENODATA`）⇒ 盘面保证 **同一 `(index,name)` 至多一条**。
- **生命周期**：重复 set → 覆盖（就地换值或重写块）；删最后一条 → **释放块 + 指针回哨兵**；**空 xattr 块禁止留盘**。
- **值语义**：ACL/capability **opaque 存**（FS 不翻译内容）；`system.posix_acl_*` 必须走 ACL 回调（不入通用 setxattr）。
- **不要求条目排序**（ext4 亦不要求）。

---

## 6. `i_flags`（镜像 Linux `S_*`）

- 位表见 `include/linux/fs.h:2098`（`S_SYNC/S_NOATIME/S_APPEND/S_IMMUTABLE/S_DIRSYNC/…/S_ANON_INODE`，当前最高 bit 19）。
- 字段给 57 位，**存值不移位**，余位预留。
- **v1.2 不实现任何 flag**（`chattr/lsattr` 现为可降级项 R-DG-03；**2026-09-26 定：当前版本不做**）；写 0 即可。字段存在的意义 = **将来支持 `chattr` 不再动盘面**。
- **读端（2026-09-30 补）**：inode 载入时**不消费** `i_flags`（忽略，不透传 `inode->i_flags`）——与「v1.2 不实现」一致；将来实现 `chattr` 时再定「透传/过滤」。

---

## 7. 不变量增补（对 v1.1 §10 的追加/修订）

- **26（新）· 无数据类型**：`type ∈ {1,2,6,12}` ⇒ `size==0` ∧ `data_root==PTR_INVALID` ∧ 无树；`rdev` 仅 `type ∈ {2,6}` 有效（**存 `inline_data[0:4]` 小端 u32**，见 §9-5）；`link_count` 服从**非 DIR 通用契约 = 指向本 inode 的 dentry 条目数**（见下条）。
- **27（新）· 类型值域**：`type` 必须 ∈ `{1,2,4,6,8,10,12}`；其余（含 0）= 硬错。
- **28（新）· 权限/指针**：`permission_bits` 低 12 位 = 权限；高 52 位 = 块号或全 1 哨兵（**不得 0**）。
- **29（新）· xattr 块**：见 §5.5。
- **30（修订）· 恒等式**：`i_mode == (type<<12) | mode` 恒成立。
- **link_count（全类型，2026-09-30 补）**：`link_count` 在 `DIR` = `size + 2`；**其余全部类型（REG / LNK / CHR / BLK / FIFO / SOCK）一律 = 指向本 inode 的 dentry 条目总数（含创建者第一个）**。v1.1 §3.3 原文只点名 `REG / SOFT_SYM`（彼时非 DIR 仅此两型），v1.2 显式补全为「**非 DIR 通用**」；`audit_v1.2`（`tools/audit_v1.2/audit_v1.2.cpp:1057` 跳过 DIR，`:1062` 其余比 dentry 引用数）已按此实做。
- v1.1 §10 **其余条目不变**（dentry 树 / 区段树 / 名字堆 / link_count / 文件数据区）。
- **区段树机械参数 31–36**（阶 337 / 分裂左 168 / 欠载 <168 / 合并 ≤337 / 深度 64 / 节点 8K 对齐）见 `disk_layout_v1.1.md` §10。

---

## 8. 迁移影响面（全端）

| 端 | 动作 |
|---|---|
| `include/disk_struct_v1.1.hpp` | 位域/字段改名（`type:2→6`、`other→i_flags`、`permission_bits→mode+xattr`）、新增 `rfs_xattr_*` 结构、常量改名、`RAVENFS_VERSION 2→3`、`static_assert` 保持 |
| `mkfs` | 写 root inode：`type=4`、`mode=0755`、`i_flags=0`、`xattr_ptr=哨兵`；版本号 |
| `juvenile_god` | `INODE_TYPE_*` 常量（0/1/2→8/4/10）+ **全部 type 分派点**；`inode_getattr` 导出 `i_mode/rdev/i_flags`；新增「造特殊 inode」（mknod）+ xattr 四接口 |
| `audit` | §7 之不变量 26–30；xattr 块认领/校验；无类型 inode 的 `inline_data` 第 3 释义；`rdev` 校验 |
| `ravenfs.ko` | `getattr/setattr/mknod` + xattr 四回调；`i_mode/i_rdev/i_flags` 由新字段还原 |
| **兼容性** | **v1.1 盘不兼容**（位域语义变）→ 直接 reformat，不提供迁移工具 |

> **落地顺序（2026-09-26 定）**：**用户态上帝类（`juvenile_god`）刻意暂不迁移 v1.2**（仍用 `disk_struct_v1.1.hpp`）；**`mkfs` 先行适配 v1.2**（✅ 已改：`include/disk_struct_v1.2.h` + root inode v1.2 写法 + `version=3`）。god / audit / tests 的 v1.2 迁移待后续批次（§8.1 的 12 个语义点）。

### 8.1 `type` 常量普查（2026-09-26 实测）

**面**：`INODE_TYPE_` 全仓 **75 处**；其中 **core（god + 头 + mkfs）26 处**，**tests 43 处**（多为 `root_dir_insert_dentry(name, INODE_TYPE_REGULAR)` 纯传参）。
→ **不是 75 个决策，是「2 行定义 + ~12 个语义点 + 43 处机械传参」。**

**A. 常量定义（1 处）**
- `include/disk_struct_v1.1.hpp:66-68`：`REGULAR 0 / DIR 1 / SOFT_SYM 2` → **`REG=8 / DIR=4 / LNK=10`**，新增 `FIFO=1 / CHR=2 / BLK=6 / SOCK=12`。

**B. core 语义点（必须逐个改，不能只改名）**

| # | 位置 | 现状 | v1.2 动作 |
|---|---|---|---|
| 1 | `root_dir.cpp:45-47` | 建条目白名单 = **恰好 3 型** | 放行 7 型 |
| 2 | `dentry_insert_delete.cpp:49-51` | 同上白名单 | 放行 7 型 |
| 3 | `dentry_insert_delete.cpp:61` | `type==DIR` → 建 dentry 树 | 保持 |
| 4 | `dentry_insert_delete.cpp:255/298` | rename 覆盖目标 `tgt_type==DIR` | 保持 |
| 5 | `direct_file_ops.cpp:58/361/708` | 数据操作白名单（REG/SYM） | 保持，但**无数据类型要显式拒** |
| 6 | `direct_file_ops.cpp:872/1185/1387` | `DIR→IS_DIR` / `非REG→STATE` | 增「无数据类型」分支 |
| 7 | `audit.cpp:145-152` | DIR / (REG\|SYM) / **else=bad** | ⚠️ 加「无数据类型」分支，否则 FIFO/CHR/BLK/SOCK 被判 `inode_bad` |
| 8 | `audit.cpp:731` | 非 DIR 分支 | 同上 |
| 9 | `name_heap.cpp:155/243/355` | `type==DIR` 判定 | 保持 |
| 10 | `ravenfs_juvenile.cpp:340` | root `type==DIR` | 保持 |
| 11 | `mkfs/mkfs.cpp:329` | root `type = DIR` | 值改 4 |
| 12 | `include/ravenfs/ravenfs_v1.1.hpp:116` | 注释 | 更新 |

**C. tests（43 处）**：几乎全是构造参数（`INODE_TYPE_REGULAR`）→ **机械改名**；仅 `test_audit_inode.cpp` / `benchmark_radix.cpp` 有真断言（`type==DIR`）。

> ⚠️ **重点不是改名**：现在代码里有多处「**恰好 3 型**」白名单（#1/#2/#5）与「**else = bad**」分派（#7）—— 它们会**直接拒掉/误判 4 个新类型**，是真正的行为改动点。
> **盘上现实**：7 型中 **5 型会上盘**（REG/DIR/LNK/FIFO/SOCK）；**CHR/BLK 基本不落 ravenfs**（`/dev` 是 devtmpfs），但**仍必须支持**（`mknod` 要能建、`getattr` 要能回 `rdev`、镜像解包可能带）——成本仅 `rdev` 4 字节。

---

## 9. 未决 / 待裁

1. ~~**`type` 常量普查**：god 里 `type` 分派点的完整清单（改动前必须先盘）。~~ → ✅ **已完成（2026-09-26）**，见 §8.1。
2. ~~**xattr 块尺寸**：4K（本文推荐，bh 天然粒度）vs 8K piece。~~ → ✅ **已定（2026-09-26）：统一 1 个 FS_block = 4K**。
3. ~~**`h_magic` 取值**（建议与 B+tree 节点 magic 同一套"扫描识别"命名）。~~ → ✅ **已定（2026-09-26）：`0x52465358`（"RFSX"）u32 token**（xattr 无扫描/重建需求，见 §5.2 注）。
4. ~~**字段改名**：`permission_bits → mode_xattr` 是否采纳。~~ → ✅ **已定（2026-09-26）：采纳 `mode_xattr`**（`disk_struct_v1.2.h` 已用此名）。
5. ~~**`rdev` 存法**：`inline_data[0:4]` 小端 —— 是否确认；`inline_data[4:160]` 是否强制置 0。~~ → ✅ **已定（2026-09-26）**：rdev **统一存 `inline_data` 起始 4B = 小端 u32**（`new_encode_dev` 打包）；上限若需更大 → 升 `inline_data[0:8]` 小端 u64（同起点）。**非设备文件 / FIFO / SOCK 置 0**（u32 方案下 `inline_data[4:160]` 置 0）。
6. ~~**`i_flags` 是否现在实现 `chattr`**（本文默认不实现，仅留字段）。~~ → ✅ **已定（2026-09-26）：当前版本不实现 → `-EOPNOTSUPP` 降级**（字段保留，将来实现不动盘面）。
7. ~~**`RAVENFS_VERSION` 新值**（建议 3）。~~ → ✅ **已定（2026-09-26）：3**（`disk_struct_v1.2.h` 已落）。

---

## 10. 附：v1.2 `raven_Inode_t` 参考定义

```c
#define RFS_XATTR_NONE   ((1ULL << 52) - 1)   /* xattr_ptr 全 1 哨兵（同 NAME_HEAP_INACTIVE） */

struct raven_Inode_t{
    uint64_t  type:6;                    // = (i_mode & S_IFMT) >> 12；值域见 §3
    uint64_t  is_data_root_direct_bit:1; // 仅 REG/LNK；DIR 与无数据类型忽略
    uint64_t  i_flags:57;                // 镜像 Linux S_*（存值不移位）
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  m_ns_stamp;
    uint32_t  c_ns_stamp;
    uint32_t  a_ns_stamp;
    uint32_t  link_count;
    uint64_t  mode_xattr;                // [0:12)=mode(0o7777)；[12:64)=xattr_ptr(52)（原名 permission_bits）
    uint64_t  size;
    uint64_t  mtime;
    uint64_t  ctime;
    uint64_t  atime;
    uint64_t  data_root;                 // PTR_INVALID = inline
    uint64_t  padding[2];                // 未来预留（v1.2 未用）
    uint8_t   inline_data[160];          // REG/LNK=内容；无数据类型= rdev @[0:4]（仅 CHR/BLK）
}__attribute__((packed));
static_assert(sizeof(raven_Inode_t)==256,"Inode size must met 256B");
```

> **一句话记**
>
> v1.2 = **字节不变、语义重定义**：第一字 `type:6｜direct:1｜i_flags:57`；`permission_bits → mode:12｜xattr_ptr:52`；
> 无数据类型（chr/blk/fifo/sock）**无数据、rdev 塞 `inline_data`**；xattr = **单块自描述、一个指针、超限 E2BIG**（ext2 模型，不搞三态）。
