# tests/linux/ — Linux 侧位图（CHUNK 后端）验收 harness

> Phase B 交付（2026-09-28 · 测试团队）。规格锚：`Docs/runtime_tech/03_chunk_backed_slices.md`
> §10 十条不变量；被测代码 `linux/bitmap.c`（CHUNK 经模块参数 `bitmap_backing=1` 开启）。
> 与 god 侧三个新测试（`tests/test_bitmap_slice_{differential,churn,invariants}.cpp`）互补：
> god = 算法/对拍层，本 harness = **端到端盘态层**（真 mkfs + 真挂载 + 真落盘 + 重挂载复盘）。

## 文件

| 文件 | 侧 | 说明 |
|---|---|---|
| `bitmap_chunk_guest.sh` | **guest 内执行**（raven，NOPASSWD sudo） | 分相工作段：build / load / mkfs / mount / stress / ttl / umount / audit / verify / unload，亦可 `all` 一键 |
| `run-bitmap-chunk-vm.sh` | **宿主编排** | 起 VM（若未起）→ scp 投递 guest 段 → guest 构建 → 分 pass 驱动 → 日志收拢 |

## 快速跑

```bash
# 默认：CHUNK(backing=1, order=4) + FULL(0) 控制组，各一轮完整 pass
./tests/linux/run-bitmap-chunk-vm.sh

RUN=1 ./tests/linux/run-bitmap-chunk-vm.sh          # 只跑 CHUNK
ORDER=8 RUN=1 ./tests/linux/run-bitmap-chunk-vm.sh  # CHUNK order=8 变体
N_SMALL=2000 FILL_CAP_MB=0 TTL_WAIT=35 ./tests/linux/run-bitmap-chunk-vm.sh   # 调负载参数
```

前置（已具备，见 `Docs/linux/ravenfs-vm-env-notes.md`）：
- `linux/vm/disks/{root-ext4.qcow2,root-ravenfs.qcow2,esp.img}`；guest `raven` 免密 sudo + 宿主公钥；
- 试验盘按 QEMU **serial=deadbeef** 认盘；**mkfs 会抹掉试验盘**（每次 pass fresh mkfs，属预期）；
- 宿主 ssh：`-p 2222 raven@localhost`，known_hosts 落 `/tmp/kh_vm`。

日志与产物：`linux/vm/test-runs/<时间戳>-bitmap-chunk/{build.log, pass1.log(=CHUNK), pass0.log(=FULL), SUMMARY.txt, qemu.log}`；
guest 侧状态目录 `/tmp/bm-accept/`（清单/df/audit/dmesg 切片）。

## guest 段分相（可单独手跑，便于复跑单相）

```bash
bash /tmp/bm-accept.sh build              # /rfssrc → /tmp/rfs-bm-build 可写副本 + make modules
bash /tmp/bm-accept.sh load 1 4           # insmod bitmap_backing=1 bitmap_chunk_order=4
bash /tmp/bm-accept.sh mkfs               # v1.2 默认 mkfs @ serial=deadbeef
bash /tmp/bm-accept.sh mount              # 挂载 + baseline df/f_bfree
bash /tmp/bm-accept.sh stress             # 压测 + 清单 + statfs 快照（见下）
bash /tmp/bm-accept.sh ttl                # sync + 35s（一个 TTL 周期）+ dmesg ttl 切片
bash /tmp/bm-accept.sh umount             # 卸载前快照 + umount
bash /tmp/bm-accept.sh audit              # audit_v1.2 <dev> 离线审计
bash /tmp/bm-accept.sh verify             # 重挂载 → C1–C6 判据 → 收尾 umount
bash /tmp/bm-accept.sh unload             # rmmod
```

## 压测内容（`stress`）

1. **A** 16 × 中型文件（64K~512K，block 分配/释放）；
2. **B** `N_SMALL`（默认 800）小文件建 + 隔一删一（inode 分配/释放往返，记录 `f_iused`）；
3. **C** 追加（`conv=notrunc`）/ `truncate` 增缩（扩缩分配）；
4. **D** **ENOSPC 往返**：`dd` 填满（`FILL_CAP_MB` 上限，默认 33000M ≈ 填穿 32G 盘）→ 记 `f_bfree` 满盘值 → `rm` → 记回到值；
5. **E** 落盘清单：`find + stat(size) + md5sum` 全量文件 + `df` 快照。

## 判据（每条输出 `[BITMAP-ACCEPT] PASS/FAIL`）

| # | 判据 | 验的 §10 条目 |
|---|---|---|
| C0 | ko guest 内编译 / fresh mkfs / 挂载 / TTL 周期 / 卸载 全部成功 | 可用性 |
| C1 | 重挂载后清单（文件数+大小+md5）与卸载前**逐项全等** | §10.2 buffer≡盘态（端到端） |
| **C2** | **statfs 三态全等**：stress 时（内存计数）== 卸载前 == 重挂载后（盘上重算） | **§10.6 计数与驻留无关/保真**（重复 free 虚高、drain 重复记账在此现形） |
| **C3** | **ENOSPC 往返**：填满前 `f_bfree` == 删净后 == 重挂载值 | §10.6 + 分配失败路径无双分配/漏清 |
| C4 | `audit_v1.2 <dev>` 离线审计全绿（位图/extent 树/名字堆无孤儿、无双分配） | §10.7 |
| C5 | dmesg 无 ravenfs 错误（`bm_err≠0`/BUG/Oops/EIO 即 FAIL） | 运行期错误计数 |
| C6 | inode 计数往返：churn 峰值 → 删半 → 重挂载 `f_iused` 一致 | inode 组 §10.6 |

## A/B（FULL 控制组）

`RUN=both`（默认）时先跑 `backing=1`（CHUNK，被测），再跑 `backing=0`（FULL，基线）。
两轮均为 **fresh mkfs 同镜像口径**；FULL 轮应全绿（它就是移植前行为），CHUNK 轮的 FAIL 即
chunk 化回归。两轮 `SUMMARY.txt` 并排即 A/B 结论的一部分（吞吐/RSS 级 §11 指标不在本 harness
范围，属后续 A/B 专项）。

## 已知边界（写在明处）

- `bitmap_backing` 是**全局模块参数**（ko 写范围禁改 super.c，见 `Docs/linux/03_port_progress.md`
  遗留 7）→ 换模式必须 `rmmod/insmod`，脚本已按 pass 处理。
- CHUNK 的 mount 自描述三段校验只在 `bitmap_backing=1` 生效；v1.2 现行 mkfs 产盘可通过
  （移植方 V4 已验；本 harness 每 pass fresh mkfs → 顺带回归该路径）。
- ENOSPC 填满对 32G 试验盘 ≈ 31G 写入（qcow2 走宿主盘，`FILL_CAP_MB=0` 可关：关掉后 C3 退化
  为「前/后仍比对」但不含满盘极值）。
- 本 harness 不做崩溃注入（`sysrq-b`）；§10.7 crash=泄漏方向的验证属 fsck/崩溃专项（遗留）。
