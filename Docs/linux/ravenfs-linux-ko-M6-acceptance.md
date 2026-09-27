# RavenFS → Linux KO：**M6（xattr / ACL / capability）验收程序**

> 2026-09-27 · RavenFS **测试组（M6）** · 独立于 `juvenile_god`、独立于 ko 实现
> 上位：`ravenfs-linux-ko-requirements-spec.md` §4.6（**R-XA-01..04**）、
> `Docs/disk_layout_v1.2_inode.md` **§5（xattr 块）+ §5.5（audit 规则）**、
> `Docs/linux/ravenfs-linux-ko-schedule-t0-t1.md` §3.3（M6）、
> `ravenfs-linux-test/tmp/M8_rsync_pacman_pilot.md`（探针结论）、
> `ravenfs-linux-ko-M4.5-acceptance.md`（结构范式，本程序沿用）。
> 纪律：靶子与判据**均不引用 ko/god 代码**；host 侧**无 guest 自证**
> （初始镜像结构 + 内容 XXH64 + xattr 块自洽 + bitmap 认领）。

---

## 0. 一句话

**M6 = xattr 四回调 + `security.capability` + POSIX ACL（access + default）**。
验收 = 在 guest 内：① `setfattr`/`getfattr`/`removexattr`/`listxattr` 对 `user.*` 往返；
② `setcap`/`getcap`（`security.capability`）往返；③ `setfacl`/`getfacl`（access + **default 继承**）往返；
④ **`trusted.*` 拒绝**（用户裁决：不做）；⑤ **xattr 值 > 4K 拒 `-E2BIG`**；
⑥ 长值 ~4000B 往返；⑦ `umount`/`remount` 后 xattr/ACL/cap **保留**；
⑧ **`rsync -aHAX` 不再 `rc=23`**（M8 端到端）；⑨ host `audit_v1.2` + `m6_xattr_dump` **xattr 全绿**（卸载态）。

> **M6 范围裁定（2026-09-27，用户）**：`trusted.*` **不做**；xattr 值 **>4K → 拒 `-E2BIG`**；
> 范围 = **必须做全**（xattr 四回调 + `security.capability` + POSIX ACL access+default + `user.*` 含 `user.crtime_usec`）。

> **现状（2026-09-27，M6 动身）**：ko **尚未实现** xattr/ACL/cap → guest 段当前**预期**
> 全 `Operation not supported (95)`、`rsync -aHAX` `rc=23`（同 M8 基线）。**host 自证已 PASS（§5）**。
> 本程序交付 = **靶场 + 判据 + host 自证 + guest 驱动脚本**；guest 实跑待编码组落地 M6 后进行。

---

## 1. 判据（Definition of Done）

| # | 判据 | 依据 | 靶子 / 场景 | 观测 |
|---|---|---|---|---|
| **C1** | **`user.*` xattr 四回调往返** | **R-XA-01** | `setfattr`/`getfattr`/`removexattr`/`listxattr` on `x_no.dat` | 写后可读同值；删后 `ENODATA`/空；`listxattr` 含所设键 |
| **C2** | **`security.capability`（setcap）** | **R-XA-02** | `setcap cap_net_raw+ep` / `getcap` | `getcap` 回 `cap_net_raw=ep`；盘上 `security.capability` 20B |
| **C3** | **POSIX ACL access** | **R-XA-03** | `setfacl -m u:nobody:rwx` / `getfacl` | `getfacl` 含命名条目 + `mask::`；盘上 `system.posix_acl_access` |
| **C4** | **POSIX ACL default（继承）** | R-XA-03；`posix_acl_create` | `setfacl -d -m` on 目录 → 新建子项 | 子项 **access ACL 继承** default（含 mask 自动） |
| **C5** | **`user.crtime_usec` 类私有 xattr** | R-XA-01（M8 `/var/log/journal` 实证） | `setfattr -n user.crtime_usec` | 往返成功（与 C1 同路径，**systemd journal 硬需求**） |
| **C6** | **`trusted.*` 拒绝** | 用户裁决（不做） | `setfattr -n trusted.t` | `errno ∈ {EPERM, EOPNOTSUPP}`（**不得静默成功**） |
| **C7** | **xattr 值 > 4K 拒 `-E2BIG`** | **R-XA-04**（单块 4096B） | 5000B 值 set | `errno == E2BIG`（非 `ERANGE`/静默截断） |
| **C8** | **长值 ~4000B 往返**（单块上限内） | R-XA-04 | `x_long.dat`（4000B）/ `user.long` 3000B | 值逐字节一致；盘上块「条目区末端 ≤ 值区最低」 |
| **C9** | **`umount`/`remount` 后保留** | R-XA-01/03 | P1：写后 `sync`→`umount`→`mount` | 复读 xattr/ACL/cap 一致；`audit` **BAD=0** |
| **C10** | **`rsync -aHAX` `rc=0` 且保真** | M8 端到端；R-XA-01/02/03 | tmpfs fixtures → ravenfs，`-aHAX` | `rc=0`；dest `getfattr`/`getfacl`/`getcap` 与源一致 |
| **C11** | **host 侧自证（无 guest 可跑）** | 本程序纪律 | `mkstates_x` 产物 | `audit_v1.2` + `readback verify` + `m6_xattr_dump verify` **三 PASS**（§5） |

> **门槛**：C1–C11 全过 = M6 **可验收**。任一 errno 不符 / 值不符 / `audit` **BAD>0** / `rsync rc≠0` = **不通过**。
> **豁免（降级项）**：目录 `st_nlink`/`st_size` 非 POSIX 惯例、inline `i_blocks=0`、`rsync` 不带 `-S` 的稀疏膨胀
> —— 见 M8 pilot §3「可降级」，**不属 M6 缺陷**。

---

## 2. 新增 / 改动资产（均 host 侧；**不改** M4.1–M4.5 任何工具）

| 文件 | 作用 | 备注 |
|---|---|---|
| `tools/audit_v1.2/audit_v1.2.cpp` | **追加** xattr 块校验：**终止符存在性**（首个 u32==0）+ **空名规则**（ACL index 2/3 后缀**必须为空**；其余**必须非空**，抄 ext4） | **追加**（原 xattr 校验保留；不改 M4 逻辑） |
| `tools/audit_v1.2/mkstates_x.cpp` | **M6 靶子构造器**（`mkstates_a.cpp` 姊妹件）：造带 xattr 块的镜像并出 `.cexpect`（内容）+ `.xexpect`（xattr） | **新增** |
| `tools/audit_v1.2/m6_xattr_dump.cpp` | **host 侧 xattr 只读回放/校验器**（`verify`/`list`/`dump`；独立解析，自证独立） | **新增** |
| `tools/audit_v1.2/m6_xexpect.h` | xattr 期望清单共享件（producer/consumer 共用；XXH64 指纹） | **新增** |
| `tools/audit_v1.2/Makefile` | **追加** `mkstates_x` / `m6_xattr_dump` 目标（并入 `all`/`clean`；原规则不动） | 追加 |
| `tools/audit_v1.2/README.md` | 记录 xattr 校验规则扩展 + 两个新工具用法 | 追加 |
| `ravenfs-linux-test/tmp/m6-accept.sh` | **guest 侧验收驱动**（P1 往返 / P2 `rsync -aHAX`） | 新增 |
| `Docs/linux/ravenfs-linux-ko-M6-acceptance.md` | 本程序 | 新增 |

构建：`make -C tools/audit_v1.2`（**追加** `mkstates_x` / `m6_xattr_dump`）。

---

## 3. 靶子清单（`m6.img`，host 造；`mkstates_x`）

基准空盘：`truncate -s 64M` + `mkfs.ravenfs -f --inode-count=256`。构造器：`mkstates_x <empty.img> <out.img> [prefix]`。
内容约定：`D` = 填充字节 `m42_fill_byte(name, off)`（与 M4.1–M4.5 同公式）。

| 靶子 | 形态 | 内容 / xattr | 覆盖判据 |
|---|---|---|---|
| `x_no.dat` | REG inline 80 | **无 xattr**（ptr 哨兵对照） | C1（写路径）、C11（无块校验） |
| `x_inline.dat` | REG inline 100 | `user.m6key`=「hello」 | C1、C11 |
| `x_ext.dat` | REG extent 8192 | `user.a` / `user.b`（多短值） | C1、C11 |
| `x_long.dat` | REG inline 16 | `user.big`=**4000B**（单块近满） | C8、C11 |
| `x_cap.dat` | REG inline 32 | `security.capability`（vfs_cap_data 20B，CAP_NET_RAW） | C2、C11 |
| `x_acl.dat` | REG extent 4096 | `system.posix_acl_access`（**5 条**：USER_OBJ/USER(1000)/GROUP_OBJ/MASK/OTHER） | C3、C11 |
| `x_multi.dat` | REG extent 12288 | **多命名空间同块**：`user.m6key`+`user.m6second`+`security.capability`+`system.posix_acl_access`（最小 3 条） | C1/C2/C3、C11 |
| `x_dir/` | DIR（1 子） | `user.small` + `system.posix_acl_access` + **`system.posix_acl_default`** | C4、C11 |
| `x_dir/child.dat` | REG inline 40 | **无 xattr** | C4（继承靶子子项）、C11 |
| `x_link` | LNK → `x_ext.dat` | **无 xattr** | C11（非 REG/DIR 哨兵） |

- **判据**：host `audit_v1.2`（**`xattr_blocks=7`，BAD=0**）+ `readback verify`（8/8）+ `m6_xattr_dump verify`（10/10）全绿。
- `x_dir` 的 `system.posix_acl_default` 即 C4 **default 继承**的盘上真值（guest 侧 `setfacl -d` + 建子项后复验）。

---

## 4. guest 验收步骤（自动化：`tmp/m6-accept.sh`）

**前置**：本地 VM（QEMU）已启，`ssh -p 2222 raven@127.0.0.1` 免密可进（`raven` 有 `NOPASSWD` sudo）；
guest 需 `attr`/`acl`/`libcap`（`getfattr/setfattr/getfacl/setfacl/getcap/setcap`）、`rsync`（P2）。

1. **host 备料**：`make -C tools/audit_v1.2 mkstates_x m6_xattr_dump` → 64M 空盘 + `mkfs` →
   `mkstates_x`（出 `.cexpect`/`.xexpect`）→ host 自证三件套（§5）。
2. **投递**：源码 → `/opt/ravenfs/{linux,include,mkfs}`；靶子/工具 → `/opt/m6/{m6.img,audit_v1.2}`。
3. **guest 编译 ko**：`sudo make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux modules`。

### P1 —— xattr/ACL/cap 往返（`m6.img`，loop-over-file，rw）

4. `insmod ravenfs.ko`；`DEV=$(losetup --show -f -b 4096 m6.img)`；`mount -t ravenfs $DEV /mnt/rfs`。
5. **读路径（C1/C2/C3/C8）**：`getfattr -d -m -` on `x_inline.dat`/`x_ext.dat`/`x_long.dat`；
   `getcap x_cap.dat`；`getfacl x_acl.dat`；`getfacl x_dir`；`getfattr x_no.dat`（应空）。
6. **写路径（C1/C2/C5）**：`setfattr -n user.new`/`user.crtime_usec` → `getfattr` 验；
   `setfacl -m u:nobody:rwx` → `getfacl` 验；`setcap cap_net_raw+ep` → `getcap` 验；
   `setfattr -x` 删 → `getfattr` 空。
7. **拒绝面（C6/C7）**：5000B 值 `setfattr` → **期望 `E2BIG`**；`setfattr -n trusted.t` → **期望拒绝**。
8. **ACL default 继承（C4）**：`setfacl -d -m u:nobody:rwx x_dir` → `touch x_dir/inh_child` →
   `getfacl x_dir/inh_child`（access ACL 应继承 default）。
9. **落盘 + 结构（C9/C11）**：`sync` → `umount` → `audit_v1.2 $DEV`（**BAD=0**）→
   `mount` 复读 xattr/ACL/cap 一致 → `umount`。

### P2 —— `rsync -aHAX` 保真（C10）

10. 金标准 fixtures 造在 **tmpfs**（`/dev/shm`，支持 `user.*`/ACL/cap）：含 `user.rsync`、`user.long`(3000B)、
    `setfacl -m u:nobody:r`、`setcap cap_net_raw+ep`。
11. 空盘 `m6rs.img` + `mkfs` → 挂载 → `rsync -aHAX --delete <src>/ /mnt/rfs/dst/` → **期望 `rc=0`**（M8 基线 = 23）。
12. dest `getfattr`/`getfacl`/`getcap` 与源一致；`umount` → `audit_v1.2`（BAD=0）。

**判定**：P1+P2 全绿 ⇒ M6 PASS；脚本打印 `M6_RESULT: PASS|FAIL` 并以其为退出码。

---

## 5. host 侧自证（**无 guest 也能跑**，一轮即可复现）

```sh
make -C <repo>/tools/audit_v1.2 mkstates_x m6_xattr_dump
cd <work>                                       # 例：.../tmp/m6-work
truncate -s 64M m6_empty.img
<repo>/mkfs/mkfs.ravenfs -f --inode-count=256 m6_empty.img
<repo>/tools/audit_v1.2/mkstates_x m6_empty.img m6.img m6.img
#   → m6.img + .cexpect（内容）+ .xexpect（xattr）
<repo>/tools/audit_v1.2/audit_v1.2 m6.img                    # → BAD=0 / RESULT: PASS（exit 0）
<repo>/tools/audit_v1.2/readback verify m6.img m6.img.cexpect # → 8/8 [OK]（exit 0）
<repo>/tools/audit_v1.2/m6_xattr_dump verify m6.img m6.img.xexpect  # → 10/10 [OK]（exit 0）
```

**实测（2026-09-27，本机 PSLaptop）**：

- `make -C tools/audit_v1.2`（全量重建）→ 含 `mkstates_x` / `m6_xattr_dump`，`-Wall -Wextra`，**无 warning**：
  ```
  g++ -Wall -Wextra -O2 -g -I../../include -o audit_v1.2 audit_v1.2.cpp -lxxhash
  ...
  g++ -Wall -Wextra -O2 -g -I../../include -o mkstates_x mkstates_x.cpp -lxxhash
  g++ -Wall -Wextra -O2 -g -I../../include -o m6_xattr_dump m6_xattr_dump.cpp -lxxhash
  ```
- `mkstates_x`：
  ```
  mkstates_x: cexpect → m6.img.cexpect（8 REG 内容）
  mkstates_x: xexpect → m6.img.xexpect（10 inode）
    xattr_blocks 期望 = 7
    [reg] x_long.dat  inode=20 size=16 xattr=yes (block=8, 1 条: idx1/big=4000B)
    [reg] x_cap.dat   inode=21 size=32 xattr=yes (block=9, 1 条: idx6/capability=20B)
    [reg] x_acl.dat   inode=22 size=4096 xattr=yes (block=11, 1 条: idx2/•=44B)
    [reg] x_multi.dat inode=23 size=12288 xattr=yes (block=17, 4 条: idx1/… idx6/capability idx2/•)
    [dir] x_dir       inode=25 size=1 xattr=yes (block=22, 3 条: idx1/small idx2/• idx3/•)
  ```
- `audit_v1.2 m6.img` → `xattr_blocks=7`、`OK=27 WARN=0 INFO=10 BAD=0`、**`RESULT: PASS`**（exit 0）。
- `readback verify m6.img m6.img.cexpect` → **8/8 `[OK]`**（inline+extent 内容 XXH64 命中，exit 0）。
- `m6_xattr_dump verify m6.img m6.img.xexpect` → **10/10 `[OK]`**（magic/终止符/值指纹 全验，exit 0）：
  ```
  [OK ] x_inline.dat inode=18 xattr_blk=2  1 条
  [OK ] x_long.dat   inode=20 xattr_blk=8  1 条（user.big=4000B）
  [OK ] x_cap.dat    inode=21 xattr_blk=9  1 条（security.capability=20B）
  [OK ] x_multi.dat  inode=23 xattr_blk=17 4 条（多命名空间同块）
  [OK ] x_dir        inode=25 xattr_blk=22 3 条（含 posix_acl_default）
  [OK ] x_no.dat / x_dir/child.dat / x_link → no-xattr（哨兵）
  ```
- **负向自证（新规则确实生效）**：对 `m6.img` 副本注入 3 处损坏，`audit_v1.2` 逐一如实报 BAD：
  | 注入 | 期望 | 实测 |
  |---|---|---|
  | 抹掉 `x_inline.dat` 块终止符（条目填满条目区） | 报「缺少终止符」 | `[BAD] … 缺少终止符`；`RESULT: FAIL`（3 BAD） |
  | `user.*` 条目名长置 0（非 ACL 空名） | 报「空名非法」 | `[BAD] e_name_len==0（仅 ACL 允许空后缀）`（FAIL） |
  | ACL(index=2) 条目名长置 3 | 报「ACL 后缀非空」 | `[BAD] ACL(index=2) e_name_len=3 != 0`（FAIL） |
- **回归**：`mkcontent`（既有最小填充器，含 1 个 xattr 块）在扩展后的 audit 下仍 **PASS**（无回归）。

---

## 6. 未决 / 降级点

1. **⚠ spec 文字需订正（磁盘布局 §5.5）**：`disk_layout_v1.2_inode.md` §5.5 audit 规则写作「`e_name_len>0`」，
   但 **ext4 的 POSIX ACL 后缀为空**（全键即命名空间前缀，`fs/ext4/xattr.c`）。二者**冲突**。
   本程序按 ext4 语义裁定：**ACL(index=2/3) 后缀必须为空；其余必须非空**（audit 已如此实现）。
   **建议 spec §5.5 补此例外**（否则 C3/C4 的 ACL 无法在「全绿」前提下上盘）。→ **待裁决/订正**。
2. **`trusted.*` 不做**（用户裁决）：audit 仍接受 `trusted` index 为**合法**值（仅不产出）；C6 只要求**拒绝**，
   不要求入盘。若将来转向支持，靶场只需加一个 `trusted.*` 条目（index=4）。
3. **>4K 拒绝的 errno 口径**：R-XA-04 只说「放不下 → `-E2BIG`」。C7 硬判 `E2BIG`；若 ko 落为 `ERANGE`
   或静默截断 → **不通过**（须回改 ko 或再裁 spec）。
4. **ACL 条目数上限未压满**：`x_acl.dat` 用 5 条（44B），远未逼近单块 4060B。极端多条 ACL（如 500 条）是否稳定
   塞入 1 块**未覆盖**（M8 §5-3 同问）——建议 M6 落地后另加压满用例（可选）。
5. **`rsync -aHAX` 的稀疏膨胀**：`-a` 不含 `-S` → 空洞填零（ext4 亦然，**非 ravenfs 缺口**）。T3 建议命令用
   `rsync -aHAXS`；**不属 M6 判据**。
6. **guest 段当前不可过**：ko 未落地 xattr → C1–C10 现必 FAIL（`EOPNOTSUPP`/`rc=23`）。**这是预期**，
   非本程序缺陷；host 自证（C11）已实跑 PASS（§5）。`m6-accept.sh` guest 实跑待编码组落地后进行。
7. **未碰 guest / 未改 `linux/` 顶层**（本里程碑测试组约束）：`linux/`、M4/M5 既有工具与 `mkstates*` 既有文件**未动**；
   仅新增/追加 `tools/audit_v1.2/` 下文件。**可 `git add`，未 commit**。

---

## 7. 一句话

**M6 靶场已就绪**：host 侧造出含 `user.*`（含 4000B 长值）/ `security.capability` /
`system.posix_acl_{access,default}` 的 v1.2 镜像，`audit_v1.2`（xattr 块校验已补**终止符存在性**与
**ext4 空名规则**）+ `readback verify` + 新增独立 `m6_xattr_dump verify` **三 PASS**，负向注入如实报 BAD；
guest 判据（C1–C10，含 `rsync -aHAX rc=0`）与驱动脚本就位，**待 ko 落地 M6 即可开跑**。
