# tools/audit_v1.2 — RavenFS v1.2 递归只读校验员

Host 侧独立执行的只读审计工具（不依赖 juvenile_god / 内核模块）。

## 构建

```sh
cd tools/audit_v1.2
make                       # → ./audit_v1.2 ./mkcontent ./mkbigdir ./dents_dump
make mkcontent             # → ./mkcontent（仅供测试的最小内容填充器）
make mkbigdir              # → ./mkbigdir（大目录靶子构造器）
make dents_dump            # → ./dents_dump（getdents64 续读压测/枚举器，纯 libc）
```

等价单命令（编译约定）：`g++ -Wall -Wextra -O2 -g -I../../include audit_v1.2.cpp -o audit_v1.2 -lxxhash`

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
- **B+tree**：magic、`(hash,name)` 严格递增、`count<=126`、`hints[i].hash_max` 递增且 == 子树 max、extent 树同构检查。
- **名字堆**：10 描述符激活/几何自洽 + 长名（>40B）读 + 堆位图置位校验。
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
