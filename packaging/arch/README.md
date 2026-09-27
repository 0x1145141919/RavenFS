# ravenfs-dkms（Arch 打包）

**RavenFS 内核模块**（实验性）：DKMS 自动重编 + mkinitcpio hook 自动入 initramfs。
⚠️ **实验性文件系统**——本包只把模块做成「可用」；**勿在真实数据上以 ravenfs 为根使用**，除非 T4/T5 已验证。

## 机制
- **重编**：`dkms.conf` 把源注册到 `/usr/src/ravenfs-<ver>/`；`dkms` 的 alpm hook 在 `linux`/`linux-vfio` 等内核包安装/升级后自动重编（**需对应 `*-headers`**）。
- **入 initramfs**：包提供 mkinitcpio install hook `ravenfs`（`/usr/lib/initcpio/install/ravenfs`，**守卫版**：该内核没有 ravenfs 模块就跳过，绝不使 `mkinitcpio` 失败）。→ 一次性把 `ravenfs` 加入 `HOOKS` 后，**跨内核更新自动生效**。

## 安装（需 sudo 的步骤）
```bash
# 1) 依赖：dkms + 每个已装内核的 headers（本机跑 linux-vfio，另有 linux）
sudo pacman -S --needed dkms linux-vfio-headers linux-headers

# 2) 构建 + 安装（普通用户运行；makepkg 内部会调 sudo pacman）
cd /home/PS/PS_git/OS_pj_uefi/ravenfs/packaging/arch
makepkg -si

# 3) 一次性把 hook 加入 HOOKS（默认即可置于 filesystems 附近），再重生成
sudo sed -i 's/^HOOKS=(\(.*\) filesystems \(.*\))$/HOOKS=(\1 ravenfs filesystems \2)/' /etc/mkinitcpio.conf   # 或手动编辑
sudo mkinitcpio -P

# 4) 验证
dkms status                                   # ravenfs/<ver>, <kernel>: installed
sudo modprobe ravenfs && grep -w ravenfs /proc/filesystems
lsinitcpio /boot/initramfs-linux-vfio.img | grep -m1 ravenfs
```

## 卸载
```bash
sudo pacman -Rns ravenfs-dkms
sudo sed -i 's/ ravenfs//' /etc/mkinitcpio.conf   # 移除 hook 名；或手动编辑
sudo mkinitcpio -P
```

## 备注
- `makepkg` **必须非 root**；sudo 仅用于 `pacman`/`mkinitcpio`/`modprobe`。
- 若某内核没装 `*-headers`，DKMS 跳过它（该内核 initramfs 也不含 ravenfs，且 hook 会安全跳过）。
- 源树路径可用 `RVN_SRC` 覆盖。
