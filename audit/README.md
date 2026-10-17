# audit/ — RavenFS 顶层独立只读校验器

与 `juvenile_god/`、`linux/` 平级的**独立项目**。参数 = 路径；自己 `O_RDONLY` 打开 +
整盘 mmap；唯一地址语言 = `base + blk×4096`（坐标偏移量）；自己解 SB / 几何 / 位图；
**不链 `libravenfs_juvenile.a`**。

> 为什么要独立：审计器与被审路径共因失效 = 白审。绑定上帝类就意味着共享
> `fs_allocator`（区间重建 / 位图解码）与两个 cache（`InodeSession` /
> `mdata_piece_cache`）——那正是最不该被共享的存取层。详见 `DISCIPLINE.md`。

## 当前进度

| 步骤 | 状态 |
|---|---|
| ① 立项 + 坐标层 `audit_image` + **引导期五道 fail-fast 硬门 F0–F4** | ✅ |
| ② DFS 迁移（接坐标层）：A1 翻译层 / A2 分配账本 / B 双遍历 DFS / C 三空间收口 | ✅ |
| ③ 独立位图解码 + 几何外部锚（设备尺寸 / world_base）交叉校验 | ✅ |
| ④ 测试迁移（flush + 重开，活内存审计退役） | ⬜ |
| ⑤ 摘除 god 成员 `audit_dfs_root` / `audit_fsck_root` | ⬜ |
| ⑥ 已挂载防呆 + `--force` | ⬜ |
| ⑦ 文档重写 | ⬜ |

### 实现文件

```
audit/audit_types.cpp        枚举字符串
audit/audit_report.cpp       reporter（stdout 单流 + [AUDIT][SUM] 汇总）
audit/audit_image.cpp        坐标层（引导五门 + mmap + 盘面位图解码）
audit/audit_inode_plane.cpp  A1 inode_resolver / A2 inode_alloc_ledger / inode_plane_auditor
audit/audit_dfs.cpp          相位 B（inode_audit + dfs_descend + enumerate_dir_children
                             + walk_btree/leaf_chain + 名字面 3-way）+ 相位 C（XOR）
audit/audit_entry.cpp        run_audit（路径 → 引导 → A/B/C → verdict）
audit/audit_cli.cpp          CLI
```

### 已验（2026-10-06）

- 干净盘（mkfs --v2.0）：`PASS` / exit 0。
- 内容盘（god 造：sub/ + file + symlink + 200B 长名）：独立 audit 与 god 版
  `audit_v1.2` 报告**逐项一致**（inodes=5 nodes=2 leaves=2 recs=4，全 0 BAD）。
- 变异：块位图翻位（unprotected/leaked）、dentry magic 损坏（btree_bad）、
  位图自述位（bitmap_bad）、五道 fail-fast 门 → 全部命中。

## 构建

```sh
cmake --build ../../build_v20 --target audit      # → audit/audit（在 build 树里）
# 或
cmake --build ../../build_fresh --target audit
```

## 运行

```sh
./audit <image-or-device>                     # 默认：仅结论/汇总（无 trace）
./audit --trace <image-or-device> > out.log   # 附带逐 inode/逐节点原始 trace（建议重定向）
./audit -v <image-or-device>                  # reporter 逐 inode WALK 摘要行
./audit -vv <image-or-device>                 # reporter RAW 详细度
```

逐 inode / 逐节点的**原始数据面 trace 由 `--trace`（简写 `-t`）开关控制，默认关**，
避免每次运行都付出巨量输出/格式化的代价。打开后直接 `printf` 到 stdout，不经过
reporter、也不受 `-v`/`-vv` 影响，外部自行重定向即可。关时连实参（`path_str` /
`sanitize` 等）都不求值（`AUDIT_TRACE` 宏内联 `if`）。

### trace 日志 schema（事后挖掘）

每行形如 `[TRACE][<tag>] key=value ...`；数值为十进制整数，磁盘指针用 `%#llx`，
`path` 为真实路径、非可打印字节转义为 `\xHH`（保持一行一条）。

| tag | 粒度 | 关键字段 |
|---|---|---|
| `IDP` | inode 面整体 | `mode=direct|tree ctl ptr pieces target_blk inode_count` |
| `IDPDIRECT` | inode 面 direct 基址 | `base data_blk` |
| `IDPTREE` | inode 映射树节点 | `blk owner depth role inner cnt` |
| `IDPIDX` | inode 映射树内节点 hint | `blk depth idx key child` |
| `IDPEXT` | inode 映射树叶 extent | `blk idx fblk pblk len end` |
| `IDPFR` | inode 面空区快照 | 汇总 `ctl ptr count limit`；逐条 `idx base len end` |
| `INODE` | 单 inode | `inum parent depth type form size lc mode xattr_ptr uid gid mtime ctime atime word1 data_root iblk path` |
| `NDCTL` | 目录名字面控制头 | `inum ddroot nd_ctl nd_direct nd_ptr pieces fr_ctl fr_inline fr_ptr fr_count path` |
| `ND` | 名字映射面控制 | `inum direct ptr pieces target_blk fr_count` |
| `NDMAP` | 名字映射 | direct：`inum mode=direct base blk_len`；tree 叶 extent：`inum blk idx beg pblk len end` |
| `NDTREE` | 名字映射树节点 | `inum depth blk role inner cnt` |
| `NDIDX` | 名字映射树内节点 hint | `inum blk depth idx key child` |
| `NDFR` | 名字面空区 | inline 汇总 `inum mode=inline count limit`；tree 汇总 `inum mode=tree root count limit`；逐条 `inum idx base len end` |
| `FRTREE` | fr 树节点 | `inum depth blk role inner cnt` |
| `FRIDX` | fr 树内节点 hint | `inum blk depth idx max_len max_base child` |
| `NDSUM` | 目录名字面收口 | `inum active direct used_blk map refs free` |
| `TREE` | dentry/extent 树节点 | `kind inum depth blk role inner cnt order` |
| `TREEIDX` | B+ 树内节点 hint | `kind inum blk depth idx key child` |
| `EXTENT` | extent 树叶记录 | `inum blk idx fblk pblk len end`（`fblk/len`=文件逻辑区间，`pblk`=物理块） |
| `DENT` | dentry 叶记录 | `inum blk idx hash name_len target rem_off name` |
| `EXT` | 文件数据形态 | inline：`form=inline inum size`；direct：`form=direct inum base blocks size`；extent：`form=extent inum root blocks size` |

`role=root|inner|leaf`；`pblk`/`base`/`root`/`child` 均为 FS 逻辑块号；`iblk` 为承载该
inode 的物理 FS 块（inode 面布局挖掘用）。2 TB 盘全量 trace ≈ 1.8 M 行 / ~250 MB。

## 退出码

| 码 | 含义 |
|---|---|
| `0` | 引导通过，且非门观察（crc / 位图自述）全净 |
| `1` | 引导通过，但存在 BAD（crc 坏 / 位图自述坏） |
| `2` | 用法 / IO 错误 |
| `3` | **fail-fast 硬门命中（F0–F4）**：坐标空间无指称，拒绝审计 |

## 五道 fail-fast 硬门

F0 身份（magic/version）· F1 原点（`world_base_lba×lba==4096`）· F2 退化/溢出 ·
F3 落底（字节源覆盖声称块世界）· F4 inode 面根可解。判据详见 `DISCIPLINE.md §3`。
