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
make                    # 或 g++ -Wall -Wextra -O2 -g -I../../include sbinfo.cpp -o sbinfo
./sbinfo <image-or-device>       # 镜像文件
sudo ./sbinfo /dev/nvme0n1       # 裸盘（读需 root）
```

退出码：`0` 全部检查通过 · `1` 有 BAD · `2` 用法 / IO 错误。

## 检查项

- magic（"RAVN"）。**非 RavenFS 时给指向性诊断**：检出 0x438 ext magic 就直说
  「这盘是 ext4，dumpe2fs 才是正主」（本工具的起因：拿 dumpe2fs 读 RavenFS 盘报找不到超级块）
- version：2(v1.1) / 3(v1.2) 均接受并标注盘面（两版 SB 字节布局全等），仅未知版本判 BAD
- crc32：原始 4096B 拷贝置零 [112,116) 全长重算（与 mkfs / audit 同口径）
- 字段：label / uuid / world_base_lba（换算字节基址并对 mkfs 的 4096 契约校验）/
  total_blocks / inode_array_count / root_inode
- 四时间戳 + 挂载状态判读（§15：umount 0 哨兵 = 非正常关机可检出）
- 派生几何（mkfs `calc_geometry` 逐式一致）+ 设备尺寸交叉验证
- Backup SB 链：逐节点 magic/crc/prev 链一致性、自环/超长护栏、节点数 == backup_count

纪律：只读（无任何写路径）；stdout = 结果，stderr = 致命错误。
