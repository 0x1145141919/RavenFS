# tools/audit_v1.2 — RavenFS v1.2 递归只读校验员

Host 侧只读审计工具，**基于上帝类**（`RavenFS_juvenile`）：挂载 readonly + mmap 后端后
驱动 `audit_dfs_root`（可达性审计：root 先序 DFS + 两张影子图 + 收口 XOR）。
> **2026-09-30 重写**：由「自带独立解析的 v1.2 审计器（不依赖 juvenile_god）」改为
> **上帝类薄壳**（链接 `libravenfs_juvenile.a`）。旧独立实现见 git 历史。
> 审计模型详见 `Docs/audit_dfs_design.md`。

## 构建

```sh
cd tools/audit_v1.2
make                       # → ./audit_v1.2 ./mkcontent ./mkbigdir ./dents_dump
make mkcontent             # → ./mkcontent（仅供测试的最小内容填充器）
make mkbigdir              # → ./mkbigdir（大目录靶子构造器）
make dents_dump            # → ./dents_dump（getdents64 续读压测/枚举器，纯 libc）
make mkstates_x            # → ./mkstates_x（M6 xattr/ACL/capability 靶子构造器）
make m6_xattr_dump         # → ./m6_xattr_dump（M6 host 侧 xattr 回放/校验器）
```

等价单命令（编译约定）：先构建上帝类静态库，再由 Makefile 链接：

```sh
cmake --build ../../build_fresh --target ravenfs_juvenile    # → libravenfs_juvenile.a
make audit_v1.2
```

(`audit_v1.2` 目标：`g++ ... audit_v1.2.cpp ../../tests/cpu_id_stub.cpp
../../build_fresh/juvenile_god/libravenfs_juvenile.a -lxxhash -lpthread`)

依赖：系统 xxhash（`-lxxhash`，host 已装 0.8.4）。`dents_dump` 无第三方依赖（纯 libc）。

## 运行

```sh
./audit_v1.2 <image-or-device>
```

- 只读：`O_RDONLY` 打开，无任何写路径（不建副文件、不 mmap 写）。
- 日志只走 **stdout**（stderr 仅致命错误）。
- 退出码：`0`=无 BAD；`1`=有 BAD；`2`=用法 / IO 错误。

## 检查范围（v1.2 契约）

- **Superblock**：magic `RAVN` / `version==3` / crc32（清 crc 字段后对 4096B 算，反射 0xEDB88320）。
- **几何**：与 mkfs `calc_geometry` 逐式一致（`FS_block i` 恒在设备字节 `4096*(i+1)`）。
- **递归先序 DFS**（root = `sb.root_inode`=16）：dentry B+tree 枚举 → 子目录递归；已访问 inode 集合 + 目录祖先栈（环检测）。
- **逐 inode**：type 值域 / `mode_xattr` 切片 / xattr 块自校验 / rdev / 无数据类型 / DIR `link_count==size+2` & `size==条目数` / REG·LNK `link_count==引用数` / 数据三态。
- **xattr 块（v1.2，§5.5）**：`xattr_ptr≠哨兵` ⇒ 认领该块 + 校验 `h_magic(RFSX)` / `h_refcount==1` / `h_blocks==1` /
  `h_hash==0`（WARN）/ 条目越界 / `e_name_index` 合法 / 值偏移合法且 4B 对齐 / 值区间互不重叠 /
  条目区末端 ≤ 值区最低地址 / **终止符存在（首个 u32==0）** / 该块 bitmap 亮灯且被**恰一个** inode 认领。
  **空名规则（2026-09-27 追加，抄 ext4）**：ACL（index=2/3）后缀**必须为空**（全键即命名空间前缀）；
  其余命名空间后缀**必须非空**。→ 兼容 `system.posix_acl_access`/`default` 的 ext4 表示。
- **B+tree**：magic、`(hash,name)` 严格递增、`count<=126`、`hints[i].hash_max` 递增且 == 子树 max、extent 树同构检查。
- **名字堆**：10 描述符激活/几何自洽 + 长名（>40B）读 + 堆位图置位校验。
- **名字堆分区级统计 + 空堆泄露判定（2026-10-05 新增）**：
  - 逐堆打 `HEAP`（gran/几何/占用）；分区级汇总打 `HEAP_SUM`（激活堆数 / 占块 /
    占用 bit / 利用率）与按 gran 档分档的 `HEAP_SLOT`（8B / 64B / 128B / …）。
  - **空堆（激活但占用=0）判为泄露** → `heap_bad++`（计入 `bad_total`），并另记
    `zero_used`。用于暴露「长名删后堆未归还」的惰性堆泄漏。
  - 报告字段：`heap_active / heap_active_{bitmap,data}_blk / heap_used_bits /
    heap_total_bits / heap_zero_used / heap_slot_active[10] / heap_slot_data_blk[10] /
    heap_slot_used_bits[10] / heap_slot_total_bits[10]`。
- **叶链校验（v1.2 新增）**：每目录从最左叶沿 `ptrs[2]` 走链，必须恰好覆盖全部叶、顺序 = 中序；
  `prev/next` 对称；首叶前驱 invalid、末叶后继 invalid；非根叶 `ptrs[1]` 指回前驱；
  根叶 `ptrs[1]` = `..` inode。失败计入 SUMMARY 的 `chain_bad`。
- **位图对账**：元数据常亮、可达 inode 位亮、认领块亮灯；分配但不可达 → INFO（不判 BAD）。

## 测试填充器 mkcontent

```sh
./mkcontent <in.img> <out.img> [long]
```

写**副本**（auditor 永不写盘）：root 含 sub/ + inline.txt(+xattr) + link + fifo + sock + chardev + direct.bin + extent.bin；sub 含 nested.txt；`long` 追加一个 >40B 长名条目（激活名字堆）。

## 测试填充器 mkbigdir（大目录）

```sh
# 1G 空盘（inode 数给够）
truncate -s 1G empty.img
./mkfs/mkfs.ravenfs -f --inode-count=$((2*N+256)) empty.img
# 填成大目录（root: N 个 f%05d.dat + sub/；sub: 3 个条目）
./mkbigdir empty.img big.img [N]      # N 默认 5000
./audit_v1.2 big.img
```

必然构造**多叶/多层** dentry 树（按 `(XXH64(seed=0)(name), name)` 排序切叶 ≤126），
叶建**双向链**（非根叶 `ptrs[1]`=前驱 / `ptrs[2]`=后继，首尾 52 位全 1），
多叶时根为**根内部**（`hints[i].hash_max` = 子树最大 hash，严格递增），>126 叶再插中间层。
树根复用块 0（避免 mkfs 根节点变孤儿块）。

## 续读压测器 dents_dump（在 guest 里跑）

```sh
./dents_dump <dir> [bufsize]         # bufsize 默认 4096；给 256 等小值强制多次续读
```

逐条打印 `ino d_type name`（原始 d_type 数值，含 `.`/`..`），末尾：
`COUNT=<n> UNIQUE=<n> DUP=<n> MISSING=<n>`。
`DUP` = 第一遍名字重复数；`MISSING` = 第二遍（1MiB 大缓冲参照）独有而第一遍未见的名字数
（=0 = 小缓冲续读无漏；>0 = 跨缓冲漏读嫌疑）。纯 libc，无第三方依赖。

## M6 靶子构造器 mkstates_x（xattr / ACL / capability）

```sh
truncate -s 64M empty.img
./mkfs/mkfs.ravenfs -f --inode-count=256 empty.img
./mkstates_x empty.img m6.img m6.img       # → m6.img + .cexpect（内容）+ .xexpect（xattr）
./audit_v1.2 m6.img                          # → xattr_blocks=7，BAD=0 / PASS
./readback verify m6.img m6.img.cexpect      # → 内容 8/8 [OK]
./m6_xattr_dump verify m6.img m6.img.xexpect # → xattr 10/10 [OK]
```

在副本里造带 xattr 块的 v1.2 镜像（**不改** M4.x 任何工具）：
`user.*`（含 4000B 长值）/ `security.capability`（vfs_cap_data 20B）/
`system.posix_acl_access`+`default`（ACL blob）/ 无 xattr 对照 / inline+extent 文件与目录。
命名空间 index 抄 ext4；**ACL 后缀为空**（`e_name_len==0`）。

## M6 host 侧 xattr 回放/校验器 m6_xattr_dump

```sh
./m6_xattr_dump list   <image>             # 扫描亮灯 inode 的 xattr 块，逐条打印
./m6_xattr_dump verify <image> <xexpect>  # 独立解析并断言 == m6_xexpect.h 清单
./m6_xattr_dump dump   <image> <out>      # 按 inode 落 xexpect（供与生产者清单比对）
```

独立于 `audit_v1.2.cpp`（自证独立），只读。清单格式见 `m6_xexpect.h`。
`m6_xattr_dump` 与 `audit_v1.2` 互为交叉验证（同一镜像两条独立解析路径）。
