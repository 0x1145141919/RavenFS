# RavenFS → Linux KO：**M4.4（setattr + 时间戳）验收程序**

> 2026-09-27 · RavenFS **测试组**（M4.4）· 独立于 `juvenile_god`、独立于 ko 实现
> 上位：`ravenfs-linux-ko-M4-data-spec.md` §11（M4.4 切分）+ §3（三态）、
> `ravenfs-linux-ko-requirements-spec.md` §4.5（R-MD-02/03/04/06/07/09）+ 注 R-MD-06/07/09、
> `disk_layout_v1.2_inode.md`（`i_mode = type<<12 | mode`；`mode:12` 含 suid/sgid/sticky）、
> `ravenfs-linux-ko-M4.3-acceptance.md`（结构范式，本程序沿用）。
> 纪律：靶子与判据**均不引用 ko/god 代码**；host 侧可**无 guest 自证**（初始镜像属性 + 内容往返 + 操作后属性/内容期望自洽）。

---

## 0. 一句话

**M4.4 = setattr + 时间戳**。验收 = 在 guest 内对一组**M4.4 就绪靶子**（**普通文件 + 目录 + symlink**）执行确定性操作
（`chmod` 含 **suid/sgid/sticky**、`chown`（跟随）/`lchown`（不跟随）、`touch -d`（m/a 分别或同时）、`truncate`），
读回后 `stat`/`lstat` 与 host ground-truth（`.wexpect`）**逐项一致** + **内容往返不变** + **umount/remount 后属性保留**
（落盘）+ `statx`（`btime` 可降级不炸）+ `chattr` → `-EOPNOTSUPP` + `audit_v1.2` 全绿。

---

## 1. 判据（Definition of Done）

| # | 判据 | 依据 | 靶子 / 场景（`m44.img`） | 观测 |
|---|---|---|---|---|
| **C1** | **chmod 生效**（普通位） | R-MD-05；`setattr_copy` | `a_reg.bin`（0644→0755）、`a_dir`（0755→0700） | `stat %a` == 期望 |
| **C2** | **suid/sgid/sticky 可持久**（含组合） | **R-MD-03**；v1.2 `mode:12` | `a_suid`(4755)·`a_sgid`(2755)·`a_stky`(1755)·`a_7777`(7777)·`a_sgdir`(2755) | `stat %a` 高位保留（`&04777`） |
| **C3** | **chown 生效**（uid/gid） | **R-MD-04** | `a_reg.bin`(1000:1000)·`a_own.bin`(4321:8765)·`a_dir`(1500:1500) | `stat %u %g` == 期望 |
| **C4** | **symlink setattr（不跟随）** | R-MD-02；lchown/lutimes | `a_slink`(→`a_reg.bin`)·`a_slink2`(悬垂) | `lstat` 报链接**自身** uid/gid/时间；悬垂不炸 |
| **C5** | **utimes 生效**（mtime+atime & 分别设） | **R-MD-06**（FS 自建触发表，3 义务中的「FS 自己」项归 M4.5/创建路径） | `a_ts.bin`(m+a=T2+123ms)·`a_ts2.bin`(m=T4, a=T5 分设)·`a_slink`(T3) | `stat %Y`（+ ns）+ `%X` == 期望 |
| **C6** | **truncate（经 setattr ATTR_SIZE）** | R-MD-02；M4.3 `rfs_truncate` | `a_trunc.bin`(16384→5000)·`a_trunc_g.bin`(100→300) | `stat %s` == 期望 + 内容往返一致 |
| **C7** | **内容往返不变**（setattr 不动数据） | 不变量 22/23 | 12 个 REG 靶子 | `cat | cmp` == `wt/`（`cexpect.post` ground-truth） |
| **C8** | **umount/remount 后属性保留**（落盘） | R-MD-06「落盘」；R-SY-01 | 全部 16 靶子 | remount 后 `stat`/`lstat` 再验（mode/uid/gid/mtime/size）绿 |
| **C9** | **`statx` 降级不炸 + `chattr` → `-EOPNOTSUPP`** | **R-MD-08**（btime/i_version 可降级）、**R-MD-09**（chattr 降级） | `a_reg.bin` | `stat`/`statx` 返回 0（btime 可 0/`—`）；`chattr +i` 非 0（EOPNOTSUPP） |
| **C10** | **结构自洽** | `audit_v1.2` 契约 | 初始 + 操作 + remount 后的镜像 | `audit` **BAD=0 / exit 0** |
| **C11** | **初始靶子 host 自证** | 无 guest 可跑 | `mkstates_a` 产物 | `audit_v1.2` + `readback verify` + `m44_attrcmp` 三 `PASS` |

> **门槛**：C1–C11 全过 = M4.4 **可验收**。任一 `stat` 项不符 / `cmp` 不等 / 返回码不符 / **remount 后属性丢失** / `chattr` 意外成功 / `BAD>0` = **不通过**。

---

## 2. 新增 / 改动资产（均 host 侧构建）

| 文件 | 作用 | 备注 |
|---|---|---|
| `tools/audit_v1.2/mkstates_a.cpp` | **M4.4 setattr 靶子构造器**（`mkstates_w/t.cpp` 的姊妹件）：造 **REG + DIR + LNK** 靶子，产出镜像 + `.expect`（初始属性）+ `.wexpect`（操作后属性）+ `.plan`（操作序列）+ `.cexpect`/`.cexpect.post`（初始/操作后内容，M4.1 格式） | **新增**；不改 M4.1/M4.2/M4.3 任何工具 |
| `tools/audit_v1.2/m44_attrcmp.cpp` | **host 侧属性断言小工具**（离线读 inode 字段）：对 `.expect`/`.wexpect` 逐项断言 `kind/size/mode/uid/gid/mtime/atime/ctime`（含 ns） | **新增**；独立于 `audit_v1.2`/`readback` |
| `tools/audit_v1.2/Makefile` | **追加** `mkstates_a` / `m44_attrcmp` 目标（并入 `all`/`clean`；原规则不动） | 追加 |
| `ravenfs-linux-test/tmp/m44_ops.c` | **操作执行 + 属性校验器**（guest 侧，host 静态编译）：`run` 按 `.plan` 执行 `chmod/chown/lchown/utimes/truncate`；`check [--no-atime]` 按 `.wexpect` 逐项 `lstat` 断言 | 新增 |
| `ravenfs-linux-test/tmp/m44-accept.sh` | **guest 侧验收驱动**（复用 `m43-accept.sh` 投递范式）：host 备料 → ssh-pipe 投递 → guest 编 ko → `insmod` → rw 挂载 → 读前 sanity → 执行操作 → sync → 属性/内容校验 → statx/chattr → **umount/remount** 复验 → `audit_v1.2` | 新增（指向 `127.0.0.1:2222`，免密 key，不硬编码密码） |

构建：`make -C tools/audit_v1.2`（**追加** `mkstates_a`/`m44_attrcmp`）；host 静态编 `tmp/m44_ops.c`（`gcc -Wall -Wextra -static`）。

---

## 3. 靶子清单（`m44.img`，host 造）

基准空盘：`truncate -s 64M` + `mkfs.ravenfs -f --inode-count=256`。构造器：`mkstates_a <empty.img> <out.img>`。
内容约定：`D` = 填充字节 `m42_fill_byte(name,off)`（与 M4.1/M4.2/M4.3 同公式）；`Z` = 全 0。
时间基准：`T0=1700000000`（初始）、`T1..T5` 见表；`ctime` 因 setattr 刷新为 `now` → **不断言**（清单记 `-1`）。

| # | 靶子 | 类型 | 初始（size / 形态 / mode / uid:gid / 时间） | 操作（`.plan`） | 操作后期望 |
|---|---|---|---|---|---|
| 1 | `a_reg.bin` | reg | 8192 / extent / 0644 / 0:0 / T0 | `M 0755`·`O 1000:1000`·`T 4096`·`D @T1` | size=4096 / 0755 / 1000:1000 / m=a=T1 / `D[0,4096)` |
| 2 | `a_suid.bin` | reg | 100 / inline / 0644 | `M 4755` | mode=**4755**（suid） |
| 3 | `a_sgid.bin` | reg | 100 / inline / 0644 | `M 2755` | mode=**2755**（sgid） |
| 4 | `a_stky.bin` | reg | 100 / inline / 0644 | `M 1755` | mode=**1755**（sticky） |
| 5 | `a_7777.bin` | reg | 100 / inline / 0644 | `M 7777` | mode=**7777**（suid+sgid+sticky） |
| 6 | `a_ts.bin` | reg | 4096 / extent / 0644 / T0 | `D @T2.123456789` | m=a=T2 + **ns=123456789** |
| 7 | `a_ts2.bin` | reg | 4096 / extent / 0644 / T0 | `DM @T4`·`DA @T5` | m=T4、a=**T5**（分别设） |
| 8 | `a_own.bin` | reg | 0 / inline / 0644 / 0:0 | `O 4321:8765` | uid=4321 / gid=8765（size 0） |
| 9 | `a_trunc.bin` | reg | 16384 / extent / 0644 | `T 5000` | size=5000 / `D[0,5000)` |
| 10 | `a_trunc_g.bin` | reg | 100 / inline / 0644 | `T 300` | size=300 / `D[0,100) Z[100,300)` |
| 11 | `a_dir/c1.bin` | reg | 64 / inline / 0644 | —（子项目只读锚点） | 内容不变 |
| 12 | `a_dir/c2.bin` | reg | 64 / inline / 0644 | —（子项目只读锚点） | 内容不变 |
| 13 | `a_dir` | **dir** | size=2 / 0755 / 0:0 | `M 0700`·`O 1500:1500` | 0700 / 1500:1500 / link_count=4（size+2） |
| 14 | `a_sgdir` | **dir** | size=0 / 0755 | `M 2755` | mode=**2755**（setgid 目录） |
| 15 | `a_slink` | **lnk** | →`a_reg.bin` / 0777 | `H 2000:2000`（lchown）·`DH @T3`（lutimes） | 链接自身 2000:2000 / m=a=T3（**不跟随**） |
| 16 | `a_slink2` | **lnk** | →`no/such/target`（悬垂）/ 0777 | `H 2500:2500` | 链接自身 2500:2500（**悬垂不炸**） |

> `M`=chmod、`O`=chown（跟随）、`H`=lchown（不跟随）、`D/DM/DA/DH`=utimensat（both/mtime-only/atime-only/不跟随）、`T`=truncate。
> **靶面 = 16**（12 REG + 2 DIR + 2 LNK）；覆盖 suid/sgid/sticky、uid/gid、mtime/atime 独立与纳秒、truncate 缩/增、目录与悬垂链接。

### 3.1 属性断言口径（`.wexpect`）

`A <inode> <kind> <size> <mode4oct> <uid> <gid> <m_s> <m_ns> <a_s> <a_ns> <c_s> <c_ns> <path>`，`-1` = **不断言**。规则：

- **ctime**：setattr 必刷 `now` → 一律 `-1`（不断言）。
- **mtime**：Linux `do_truncate` 会刷 `mtime=now`（`ATTR_MTIME|ATTR_CTIME`）→ 纯 truncate 靶子（`a_trunc`/`a_trunc_g`）记 `-1`；
  `a_reg.bin` 把 `T` 排在 `D` **之前**使最终 mtime 定值 T1。
- **atime**：仅**显式** `utimes` 的靶子断言；其余记 `-1`（读内容会扰动 atime，relatime 语义）。
- **size**：DIR 不断言（`stat` 的目录 size 非条目数）。

---

## 4. guest 验收步骤（自动化：`tmp/m44-accept.sh`）

**前置**：本地 VM（QEMU）已启，`ssh -p 2222 raven@127.0.0.1` 免密可进（`raven` 有 `NOPASSWD` sudo）。

1. **host 备料**：`make -C tools/audit_v1.2` → 空盘 + `mkfs` → `mkstates_a …`（出 `.expect/.wexpect/.plan/.cexpect/.cexpect.post`）→
   `readback dump` 产 `gt/`（初始内容）+ `wt/`（操作后内容）；host 自证 `audit_v1.2` + `readback verify` + `m44_attrcmp`。
2. **投递**（ssh-pipe tar，排除 `linux/vm`）：源码 → `/opt/ravenfs/{linux,include,mkfs}`；
   靶子/工具 → `/opt/m44/{m44.img*, gt/, wt/, m44_ops, audit_v1.2}`。
3. **guest 编译 ko**：`sudo make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux modules`。
4. **装载 + 挂载（rw）**：`sudo insmod …/ravenfs.ko`；`DEV=$(sudo losetup --show -f -b 4096 m44.img)`；`sudo mount -t ravenfs $DEV /mnt/rfs`。
5. **读前 sanity（C11/C7）**：对 `gt/` 每文件 `cat | cmp`（只读路径在本镜像仍绿，操作前基线）。
6. **执行操作（C1–C6）**：`m44_ops run /mnt/rfs /opt/m44/m44.img.plan`（`chmod/chown/lchown/utimes/truncate`；**逐条返回码断言** rc=0）；`sync`。
7. **属性校验（C1–C5, C8-part1）**：`m44_ops check /mnt/rfs /opt/m44/m44.img.wexpect`（**在内容读之前**，故 atime 可信；含 ns）。
8. **内容往返（C7）**：对 `wt/` 每文件 `cat | cmp`（`a_reg`/`a_trunc*` 缩后内容 + 其余不变）。
9. **statx / chattr（C9）**：`stat -c '… btime=%W %w' a_reg.bin`（返回 0，`btime` 可 0/`—`）；
   `chattr +i a_reg.bin` → 要求**非 0**（`EOPNOTSUPP`）；`lsattr` 可读。
10. **umount/remount 复验（C8）**：`umount` → `mount` → `m44_ops check --no-atime …`（mode/uid/gid/**mtime**/size 保留）+ `cmp` 内容。
11. **结构审计（C10）**：`umount`；`audit_v1.2 $DEV` → `RESULT: PASS`、`BAD=0`、`exit 0`。
12. **收尾**：`umount /mnt/rfs; losetup -d $DEV; rmmod ravenfs`。

**判定**：步骤 5–11 全绿 ⇒ M4.4 **PASS**；脚本打印 `M44_RESULT: PASS|FAIL` 并以其为退出码。
（remount 复验用 `--no-atime`：relatime 下内容读会扰动 atime，属**已知语义**；mode/uid/gid/**mtime**/size 才是持久化判据。）

---

## 5. host 侧自证（**无 guest 也能跑**，一轮即可复现）

```sh
make -C <repo>/tools/audit_v1.2                       # 追加 mkstates_a / m44_attrcmp
cd <work>                                             # 例：/tmp/m44-final
truncate -s 64M empty.img
<repo>/mkfs/mkfs.ravenfs -f --inode-count=256 empty.img
<repo>/tools/audit_v1.2/mkstates_a empty.img m44.img m44.img
#   → m44.img + .expect + .wexpect + .plan + .cexpect + .cexpect.post
<repo>/tools/audit_v1.2/audit_v1.2 m44.img            # → BAD=0 / RESULT: PASS（exit 0）
<repo>/tools/audit_v1.2/readback verify m44.img m44.img.cexpect   # → 12/12 [OK]（exit 0）
<repo>/tools/audit_v1.2/m44_attrcmp m44.img m44.img.expect        # → 16 靶子全 [OK]（exit 0）
```

**实测（2026-09-27，本机）**：

- `make -C tools/audit_v1.2` → 追加编译 `mkstates_a` / `m44_attrcmp`，**无 warning**（`-Wall -Wextra`）；
- `audit_v1.2 m44.img` → `OK=28 WARN=0 INFO=16 BAD=0`、`dentry_entries=16`、`extent_nodes=4`、`link_mismatch=0`、`chain_bad=0`、`RESULT: PASS`（exit 0）；
- `readback verify m44.img m44.img.cexpect` → **12/12 `[OK]`**（3 extent + 9 inline，含子目录内文件）；
- `m44_attrcmp m44.img m44.img.expect` → **16/16 `[OK]`**（REG×12 + DIR×2 + LNK×2；`mode/uid/gid/mtime/atime/ns` 全中）；
- **plan/wexpect 自洽**（host 普通 FS 模拟）：`m44_ops run` 跑全 20 条操作（`chown` 非 root 的 4 条预期 `EPERM`），
  内容侧 12 文件与 `.cexpect.post`（`readback dump`）**逐字节一致**（truncate 缩/增语义吻合）。

---

## 6. 未决 / 降级点

1. **atime 的 remount 复验降级为 `--no-atime`**：relatime 语义下「内容读」会推进 atime（非 FS 缺陷）；故
   remount 持久化判据 = `mode/uid/gid/**mtime**/size`。in-mount 首轮 `check`（内容读之前）**含 atime**。
2. **ctime 不作断言**：`chmod/chown/truncate` 刷新 ctime=`now`，host 无法预知；仅观测不判。
3. **truncate 与 mtime**：`do_truncate` 传 `ATTR_MTIME|ATTR_CTIME` → truncate **会**刷 mtime；纯 truncate 靶子不判 mtime（§3.1）。
   若某实现**不**刷 mtime（偏离 Linux generic）→ 非本程序误判，属实现信号。
4. **symlink `chmod` 不测（只能测 `lchown`/`lutimes`）**：`chmod(2)` 无 `AT_SYMLINK_NOFOLLOW`，会作用于目标；
   链接自身权限位测试用 `lchown` + `touch -h` 表达（C4）。**悬垂链接**（`a_slink2`）一并覆盖。
5. **`i_version`/change-cookie 未专项断言**：`R-MD-08` 允许降级，`statx` 仅验「可调用不炸」；若需 `STATX_CHANGE_COOKIE` 另立项。
6. **`chattr` 的 errno 口径**：期望 `-EOPNOTSUPP`（`vfs_fileattr_set` 无 `->fileattr_set`）；若 ko 以别的 errno 拒绝
   （如 `ENOTTY`），脚本判 fail —— 届时按 **R-MD-09** 对齐（**应 `EOPNOTSUPP`**）。
7. **新 inode 初始时间 / 目录 mtime（条目增删）= 创建/目录写路径**：属 **R-MD-06 注**中「FS 自己」两处义务，
   由 **M4.5/创建路径**（`create/mkdir/unlink/rename`）覆盖，本程序靶子为**预置**，不测创建期初始时间。
8. **（测试侧观察，供编码组参考，不代表判据）**：`super.c::ravenfs_dir_inode_operations` 当前**未挂 `.setattr`**，
   目录 `chmod/chown` 落 VFS `simple_setattr` 兜底；`ravenfs_read_inode` 对 `INODE_TYPE_SOFT_SYM` 未置 `inode->i_link`。
   M4.4 收口建议：目录 `i_op` 补 `.setattr`；symlink 采 `i_link`（或 `get_link`）。最终以 guest 实跑为准。

---

> **一句话记**
>
> M4.4 验收三件套：**chmod**（普通位 + **suid/sgid/sticky** 含组合，`mode:12` 高位保留）、**chown/utimes**
> （uid/gid + mtime/atime 独立与**纳秒**；symlink 走 `lchown`/`lutimes` **不跟随**）、**truncate**（经 `setattr ATTR_SIZE`，
> 缩/增 + 内容不变）。靶面 = **REG + DIR + LNK（含悬垂）** + `stat`/`lstat` 与 `.wexpect` 逐项一致
> + **umount/remount 后 mode/uid/gid/mtime/size 保留**（落盘）+ `statx` 降级不炸 + `chattr` → `-EOPNOTSUPP`
> + `audit_v1.2` 全绿 + host 三件套自证（`audit`/`readback`/**`m44_attrcmp`**）。
