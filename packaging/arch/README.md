# ravenfs-dkms（Arch 打包）

**RavenFS 内核模块** 的 pacman/DKMS 包：内核更新自动重编、initramfs 自动注入。
⚠️ **实验性文件系统**——仅把模块做成「可用」；**勿在真实数据上以 ravenfs 为根使用**，除非 T4/T5 已验证。

## 机制
- **重编**：`dkms.conf` 注册模块源到 `/usr/src/ravenfs-<ver>/`；`dkms` 的 alpm hook 在 `linux`/`linux-vfio` 等内核包安装/升级后自动重编（**要求对应 `*-headers` 已装**）。
- **入 initramfs**：包自带 `/etc/mkinitcpio.conf.d/ravenfs.conf`（`MODULES+=(ravenfs)`，drop-in 优先级高于主配置）→ 每次 `mkinitcpio` 自动打入，无需改主配置。

## 安装（需 sudo 的步骤）
```bash
# 1) 依赖：dkms + 每个已装内核的 headers（本机跑 linux-vfio，另有 linux）
sudo pacman -S --needed dkms linux-vfio-headers linux-headers

# 2) 构建 + 安装（普通用户运行；makepkg 内部会调 sudo pacman）
cd /home/PS/PS_git/OS_pj_uefi/ravenfs/packaging/arch
makepkg -si

# 3) 重新生成全部内核的 initramfs（把 ravenfs 打入）
sudo mkinitcpio -P

# 4) 验证
dkms status                       # 应显示 ravenfs/<ver>, <kernel>: installed
sudo modprobe ravenfs && grep -w ravenfs /proc/filesystems
lsinitcpio /boot/initramfs-linux-vfio.img | grep -m1 ravenfs   # 模块已在 initramfs
```

## 卸载
```bash
sudo pacman -Rns ravenfs-dkms      # 含 drop-in 一并移除
sudo mkinitcpio -P                 # 重生成（移除模块）
```

## 备注
- `makepkg` **必须非 root 运行**。sudo 仅出现在 `pacman`/`mkinitcpio`/`modprobe`。
- 若某内核没装 `*-headers`，DKMS 会跳过它（该内核 initramfs 里也不会有 ravenfs）。
- 源树拷贝用 `RVN_SRC` 可覆盖默认路径。
