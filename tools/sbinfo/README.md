# sbinfo — RavenFS superblock 只读查看器

`dumpe2fs -h` 之于 ext4，`sbinfo` 之于 RavenFS。用户态工具链（mkfs / audit）里
此前缺的「只看 superblock」轻查询档位，由本工具补上。

## 与 audit 的分工

| | sbinfo | audit_v1.2 |
|:--|:--|:--|
| 范围 | 仅 Primary + Backup Superblock | 全盘递归（bitmap/inode/dentry/extent/xattr） |
| 深度 | 字段直读 + crc + 链 + 几何推导 | 不变量逐一断言 |
| 速度 | 秒开 | 全盘 IO |

盘上完整性问题该找谁：SB 层 → sbinfo；SB 之下 → audit。

## 编译 / 运行

```sh
make                    # 多 TU：sbinfo.cpp + sbinfo_common.cpp + sbinfo_v1.cpp + sbinfo_v20.cpp
./sbinfo <image-or-device>       # 镜像文件
sudo ./sbinfo /dev/nvme0n1       # 裸盘（读需 root）
```

> 多盘面支持（2026-10-05）：v1.x 与 v2.0 的 `RavenSuperblock` 类型重名且布局不同，
> 不能同 TU include → 按 SB magic 分派到 `sbinfo_v1.cpp` / `sbinfo_v20.cpp`，
> 共享设施在 `sbinfo_common.{hpp,cpp}`。

退出码：`0` 全部检查通过 · `1` 有 BAD · `2` 用法 / IO 错误。

## 检查项

- magic 分派：v1.x `"RAVN"`（单字）/ v2.0 `"RAVENFS\0"`（256-bit magic[0] 前缀）。
  **非 RavenFS 时给指向性诊断**：检出 0x438 ext magic 就直说「这盘是 ext4，dumpe2fs
  才是正主」（本工具的起因：拿 dumpe2fs 读 RavenFS 盘报找不到超级块）
- version：v1.x 2/3/4(v1.1/v1.2/v1.3) 与 v2.0 均接受并标注盘面，仅未知版本判 BAD
- crc32：原始 4096B 拷贝置零 crc 字段（走 `offsetof`）全长重算（与 mkfs / audit 同口径）
- **v1.x**：label/uuid/world_base_lba/total_blocks/inode_array_count/root_inode；
  四时间戳（§15：umount 0 哨兵 = 非正常关机可检出）；派生几何（固定 inode 数组 +
  inode 位图）+ 设备尺寸交叉验证。
- **v2.0**：label/uuid/world_base_lba/total_blocks/root idx（契约硬编码 0）；
  256-bit magic[1..3] 每卷随机；latest_mount/syn/umount + abnormal_shutdown_counts；
  派生几何（仅 block bitmap 尾巴）；**inode 数据面**（`inode_data_ctl` direct/tree +
  段/树根、片数）+ 解析 root inode（idx 0）；**扁 fr 快照**（magic/crc/条目列印）。
- Backup SB 链：逐节点 magic/crc/prev 链一致性、自环/超长护栏、节点数 == backup_count
- `--probe`：blkid 风格 `ID_FS_*`（v1.x / v2.0 均可）。

纪律：只读（无任何写路径）；stdout = 结果，stderr = 致命错误。
