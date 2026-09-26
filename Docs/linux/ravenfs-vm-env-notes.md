# RavenFS → Linux KO：虚拟机环境准备实录（ext4 金标准 + ESP）

> 2026-09-26 · Raven · **状态：已完成并冷启验证通过**
> 定位：`Docs/linux/ravenfs-linux-ko-acceptance-plan.md` §3「独立虚拟机系统」的**落地实录**（含实测坑）。
> 操作脚本在 `linux/vm/`（**该目录已 gitignore**：脚本 + 磁盘不入仓）；凭证在 `linux/vm/creds/`（guest-only，亦不入仓）。

---

## 1. 结论

用**官方 live ISO + 标准工具（`pacstrap`/`arch-chroot`/`bootctl`）**，在**零宿主 sudo**、**近乎零下载**的条件下，
把 Arch 装进 `linux/vm/disks/`：

| 件 | 产物 | 状态 |
|---|---|---|
| ext4 金标准根 | `disks/root-ext4.qcow2`（32G，label `ravenext4`） | ✅ 冷启到 multi-user；`ssh -p 2222` 可进 |
| ESP | `disks/esp.img`（512M，FAT32，标签 `RAVENESP`） | ✅ systemd-boot + kernel + initramfs；fallback `EFI/BOOT/BOOTX64.EFI` |
| ravenfs 试验盘 | `disks/root-ravenfs.qcow2`（32G） | ✅ **仍空白**（T3 才 `mkfs.ravenfs`） |
| 基线快照 | `qemu-img snapshot` tag `base-ext4` + `esp.img.bak-<date>` | ✅ |

**成本实测**：复用宿主 `/var/cache/pacman/pkg`（9p 只读注入）→ 需下载 **≈18 MiB**（仅 4 个版本漂移包），
脚本端到端 **≈40 秒**；若不复用缓存则 ≈600 MiB / 8–12 分钟。

**关键机制**：宿主与 guest 同机同内核代 → 宿主包缓存命中 49 包中的 45（含 `linux` 148M、`linux-firmware` 整组），
`CacheDir` 追加一条 9p 路径即零下载。`base` 分组最小集 + `mkinitcpio openssh sudo vim`（按需去 `linux-firmware`）。

---

## 2. 流程（一句话版）

```
造盘(mkdisks.sh) → 直启 ISO 内核(serial 走 pty) → guest 内
  mount 9p(hostshare→/host, hostcache→/hostcache)
  CONFIRM=yes sh /host/install-ext4-unattended.sh
→ 脚本自行 pacstrap + chroot 配置 + bootctl install + 验收 + poweroff
→ 宿主 qemu-img snapshot -c base-ext4 root-ext4.qcow2
```
完整命令级步骤见 `linux/vm/install-ext4-root.md`；调研拆解（路线 A/B 对比、成本）见 `linux/vm/INSTALL_PLAN.md`。

---

## 3. ⚠️ 实测坑（重要，写给后来的 reviewer/实现者）

1. **`/dev/nvmeXn1` 顺序不可靠**：QEMU(11.1.1) 实测出现过「**同一命令行、两次运行把 `serial=cafebabe` 挂到不同文件**」。
   → 首次手装因此把系统装进了 `root-ravenfs.qcow2`（后重建纠正）。**脚本改为按 QEMU serial 认盘**
   （`/sys/block/nvme*n1/device/serial`），并加「三盘不得同指一盘」断言 + 「ravenfs 盘已有文件系统就拒跑」闸门。
   **这是环境级不变量，T3 的 `mkfs.ravenfs` 也必须按 serial 认盘。**
2. **`pacman.conf` 的 `CacheDir` 只能插进 `[options]` 段**：追加到文件末尾会落到 `[repo]` 段被 pacman 忽略 → 缓存失效 → 全量下载。
3. **镜像先钉 `tuna`**：live ISO 默认 mirrorlist 可能选到很慢/超时节点（实测 130 KB/s）。
4. **minimal 安装不带网络管理器**：必须启用 `systemd-networkd`+`systemd-resolved` + DHCP（`/etc/systemd/network/20-wired.network`），
   否则 guest 网卡 DOWN、SSH banner 超时；`sshd` 另需 `UseDNS no`（slirp 下反查 DNS 会卡）。
5. **9p 共享挂 `/host`·`/hostcache`（`/mnt` 之外）**：挂在 `/mnt/host` 会让收尾的 `umount -R /mnt` 把共享一起卸掉。
6. **串口用 `-serial mon:stdio`（pty）**，不要 `-serial unix:...`（实测 headless 收不到 guest 输出）。
7. **`pacman -Qkk` 的 "altered" 多为 backup file 预期变动**（`/etc/{fstab,passwd,group,shadow,shells,subuid,subgid}`，
   genfstab/useradd 所致）→ 验收只把 **missing files** 当真问题。

> 纪律符合性：整个过程**未用宿主 sudo**、**未安装宿主包**、**未改源码/Docs**；ext4 金标准由**官方标准工具**装出（非克隆）。

---

## 6. M1 阶段新增实测坑（2026-09-26，ko 挂载骨架实跑）

> 场景：在 ext4 金标准 guest 内**就地编译并 insmod ravenfs.ko**（宿主 sudo 不可用，全程 guest root）。

1. **ko 必须在 guest 内编译**：guest 内核 `7.2.6-arch2-1` ≠ 宿主 `7.2.3-1-vfio` → 宿主编的 `.ko` 会被 vermagic 拒。
   命令：`make -C /usr/lib/modules/$(uname -r)/build M=/opt/ravenfs/linux modules`。
2. ⚠️ **别用 `pacman -Sy`**：会把 `linux-headers` 拉到最新（实测 7.2.7）≠ 运行内核 7.2.6 → `build/` 目录版本不匹配。
   正解：从归档装对齐版 `pacman -U https://archive.archlinux.org/packages/l/linux-headers/linux-headers-7.2.6.arch2-1-x86_64.pkg.tar.zst`。
3. guest minimal `base` **没有 make/gcc** → 补 `pacman -S --needed make gcc binutils`（不必 `base-devel` 全家桶）。
4. ⚠️ **9p 大批量读会挂死**：`cp -a` / `tar -C /rfssrc ...` 连续读多文件直接卡在 `D` 态（单文件 `cat` 正常）。
   正解：源码投递改 **ssh-pipe tar**：`tar -C <repo> ... -cf - linux include mkfs | ssh guest 'sudo tar -C dest -xf -'`。
   另：**投递时务必排除 `linux/vm`**（qcow2 表观 32G，一次 `tar` 灌了 19G 进 guest）。
5. ⚠️ `/sys/block/*/device/serial` 内容**带尾部空格**：直接 `[ "$(cat ...)" = deadbeef ]` 会失败 → 需 `tr -d '[:space:]'`。
6. 自动化便利：给 guest 的 `raven` 加 `NOPASSWD` sudo + 装宿主公钥（都为免密跑批；仅测试 VM 内）。

**M1 结论**：`insmod` → `/proc/filesystems` 有 ravenfs；`mount -t ravenfs /dev/nvme1n1 /mnt/rfs` 成功（ro）；空根 `ls`/`stat -f`/`stat` 全对；真实 NVMe 512B LBA 路径（`world_base_lba=8`→`blk0_off=4096`）通过。

---

## 4. 验收证据（对齐 §2.1/§2.2 中不依赖 `ravenfs.ko` 的项）

- `test -f /boot/EFI/BOOT/BOOTX64.EFI` ✅（fallback）
- `/boot/vmlinuz-linux` + `/boot/initramfs-linux.img` 在 ESP ✅
- `loader.conf`：`default arch-ext4.conf` + `timeout 3` ✅；`arch-ext4.conf` 的 `root=PARTUUID=` 与实际一致 ✅
- `arch-chroot /mnt true` 可进 ✅；`umount -R /mnt` 无错 ✅
- 冷启：OVMF → ESP fallback → systemd-boot → `Arch Linux (ext4 gold)` → multi-user ✅
- `findmnt /` = ext4；`systemctl --failed` **空**（0 units） ✅；宿主 `ssh -p 2222 raven@localhost` 可进 ✅
- `pacman -Qkk`：无 **missing** 文件 ✅（见坑 7）

---

## 5. 下一步（T3 入口）

- `ravenfs.ko` 仍 **未开工**（`linux/Makefile` 是 TODO）→ 首个可动手项 = **mount 骨架**。
- 就绪后：在 ext4 根里 `mount -t 9p ... hostshare /host` → `/host/install-ravenfs-module.sh`（就地编译 + `insmod`）
  → `/host/run-t3.sh`（`mkfs.ravenfs` → 克隆构建 ravenfs 根 → 加 loader entry）。
- ⚠️ `ravenfs.ko` 必须匹配运行内核；进 initramfs 需 `MODULES=(ravenfs)` + `mkinitcpio -P`。
