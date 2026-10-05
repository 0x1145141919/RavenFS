# KO v1.3 名字数据面 · guest 验收闸门（中间跳，只测不发）

> 2026-10-06 · 对应 KO 中间态（盘面 v1.3）= god v1.3 的两跳中的**第一跳**。
> 本跳**不发包**（`pkgver` 保持 0.1.3）；仅作 v2.0 之前的运行态验收。
> 实施：worktree 法造 v1.3 审计尺；KO 代码直接在 master 上推进。

## 目的

KO 从盘面 v1.2 追到 v1.3（每目录「名字数据面」取代「10 档名字堆」）后，
用**与 v1.3 匹配的宿主审计尺**（`audit_v1.2` = god `audit_dfs_root` 薄壳，
链接 8043b7d 的 god，即 v1.3 能力）做运行态验收。

## 验证尺 / 造盘器

| 件 | 来源 | 说明 |
|---|---|---|
| `audit_v13` | worktree `ravenfs-v13-wt`（@8043b7d）编 god → `tools/audit_v1.2` | 认 v1.3（`NAMEDATA_SUM` / 映射树 / fr 树） |
| `audit_v20` | master 编 god → `tools/audit_v1.2` | 认 v2.0（留待第二跳） |
| `mkfs.ravenfs` | master `build_v20` | `--v1.1/--v1.2/--v1.3/--v2.0` 四版造盘 |

> 纪律：两把尺同名不同能力，施工时存成 `audit_v13` / `audit_v20`，按盘面版本选尺。

## guest 流程（QEMU daily，ext4 金标准根 + ravenfs 试验盘）

1. 挂 9p（`hostshare` / `rfssrc`）；只拷 `linux/*.c` + `Makefile` + `include/` 到 `~/rfs`（**勿拷整个 linux/**，vm/ 巨物会撑爆 ext4 根）。
2. `make -C /usr/lib/modules/$(uname -r)/build M=~/rfs/linux modules`（guest 内核 7.2.6-arch2-1；宿主 7.2.6-1-vfio，故须 guest 内编）。
3. `insmod ~/rfs/linux/ravenfs.ko`；`mkfs.ravenfs --v1.3 -f /dev/nvme2n1`（serial `deadbeef`）；`mount -t ravenfs`。
4. churn：短名/长名（41..552B）建/删/改名/嵌套目录；`umount` → 重挂载回读。
5. 卸载后 `audit_v13 <dev>`，取 `bad_total`。

## 结果（2026-10-06）

**冒烟（v13-smoke.sh）**：短名/长名读回 MATCH、嵌套目录 OK、删半数再建（fr best-fit 复用）MATCH、rmdir OK、umount OK。
```
[AUDIT][NAMEDATA_SUM] active_dirs=1 | data_blk=2 tree_node_blk=2 | pieces=1
[AUDIT][SUM] inodes=54 ... bad_total=0
```

**压力（v13-stress.sh 3000，tail=360B）**：插入 3000/7s、rename、删 1/3、umount/重挂载 2001 条（spot 回读 MATCH）、二次 churn，映射树升格多叶。
```
[AUDIT][NAMEDATA_SUM] active_dirs=1 | data_blk=242 tree_node_blk=12 | pieces=121
[AUDIT][SUM] inodes=2303 nodes=29 inner=1 leaves=28 recs=2302 ... bad_total=0
     没有被保护: blk=0 ino=0 | 疑似泄露: blk=0 ino=0
```
`dmesg` 扫描无 ravenfs BUG/Oops/panic/WARNING；两次均 `bad_total=0`（无泄漏）。

**结论：第一跳（v1.2→v1.3）运行态 PASS。**

## 本跳提交

`05f0bd2`（H1-B fr 模块）、`c31889f`（H1-C extent kind 参数化）、`50d1613`（H1-D 名字数据面落地 + name_heap 退场）。

## 遗留（第二跳前 / 后处理）

- `linux/tools/rfsdirchurn.c`（名字堆几何）与 `linux/pkg/` 旧快照仍残留 v1.2 引用——非构建路径，待清理。
- `nh_*` 计数器字段已无埋点（保留打印 0）。
- `name_data_rebuild` / `name_data_fr_compact`（离线压实/压缩）未移植；v1.3 未接 CI 工具。
- 未做并发/ENOSPC/mount-umount 风暴与 lockdep/KASAN（stock 内核）——第二跳后并入全量矩阵。
