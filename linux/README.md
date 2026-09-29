# RavenFS Linux KO 子项目（`ravenfs.ko`）

> 本目录（`ravenfs/linux/`）= **Linux 端内核模块（`ravenfs.ko`）子项目**的家。源码、构建脚本、打包脚本**同居于此**。
> 纪律：`../Docs/linux/LINUX_KO_DISCIPLINE.md`（三层约束 / 小步快跑 / 上线闸门 / 红线）。缺陷登记：`../Docs/linux/ravenfs-linux-ko-v0.1.1-defects.md`。
> ⚠️ **实验性文件系统**——勿在真实数据上以 ravenfs 为根使用（除非通过 guest 压测 + 实体机灰测）。

---

## 存在形式：pacman 包锚点（2026-09-29 定 · 质变）

**本子项目对外的唯一“存在形式” = 一个 pacman 软件包**（现名 `ravenfs-dkms`）。项目从此**围绕包版本**运转：不在包里的产出，不算“已交付”。

**包的动作被严格限定为「部署源代码 + 构建脚本」**：

- 部署物 = DKMS 源快照：`/usr/src/ravenfs-<ver>/{linux/*.c,*.h, Makefile, include/*.h, dkms.conf}`。
- 由 **DKMS** 在**每个已装内核**（`linux`、`linux-vfio`…）上重编；内核升级由 `dkms` 的 alpm 钩子触发（需对应 `*-headers`）。

**包不部署任何“运行期 / 系统集成件”**——这些是**外部配置**，由各自的自动化钩子自己部署：

| 外部件 | 归属 | 用途 |
|---|---|---|
| mkinitcpio / initramfs hook | 外部配置 | 模块进 initramfs（作根启动用） |
| `modules-load.d`（开机加载） | 外部配置 | 开机即注册 ravenfs |
| udev 规则（如 `by-uuid` 软链） | 外部配置 | UUID 路线 |
| systemd `.mount` 单元 | 外部配置 | 具体挂载点 |
| 用户态工具（mkfs / audit / sbinfo…） | 外部配置 | 独立部署 |

**由该形式直接导出的纪律**：

1. 包**不得依赖**外部件才自洽——外部件缺失时包本身仍完整（装上即有源 + 可编）。
2. 外部件**变更不触发包升版**（是配置，不是包内容）。
3. **版本权威 = 包版本**（`pkgver` 单源）；`dkms.conf:PACKAGE_VERSION` / `modinfo version` / 源目录名跟随。
4. **“对外生效” = 进入包**：mainline 的行为级变更**只有升 `pkgver` 重打包后才算交付**；mainline 有、包里没有 = **未交付**。

> ⚠️ **待裁（与上表的出入）**：现状 `PKGBUILD` 仍会安装 `mkinitcpio-ravenfs-hook` 进 `/usr/lib/initcpio/install/ravenfs`，与“包只部署源 + 构建脚本”有出入。**要么把 hook 移出包（外部部署），要么在存在形式里为它开例外**——待定。

---

## 本目录的一等脚本 / 构件

| 文件 | 角色 |
|---|---|
| **`PKGBUILD`** | **一等打包脚本**（唯一定义包内容与版本）——在 `linux/` 就地 `makepkg` |
| **`dkms.conf`** | DKMS 注册模板（`PACKAGE_VERSION` 由 PKGBUILD 注入） |
| **`Makefile`** | kbuild 外部模块入口（宿主 dev 构建 / DKMS 构建共用） |
| `mkinitcpio-ravenfs-hook` | mkinitcpio install hook 源（是否随包待裁，见上） |
| `*.c` / `*.h` | 模块源码（`super.c`、`bitmap.c`、`dentry*.c`、`iomap.c`… 及 `ravenfs_*.h` 子系统头） |
| `version.c` | 模块版本元数据（`MODULE_VERSION` ← 包版本，`modinfo -F version` 可见） |
| `vm/` | 独立 QEMU 测试环境（ext4 金标准 + ravenfs 试验盘；**非包内容**） |

---

## 机制

- **重编**：DKMS 把源注册到 `/usr/src/ravenfs-<ver>/`；`linux`/`linux-vfio` 等内核包安装或升级时，dkms 的 alpm 钩子自动重编（需对应 `*-headers`）。
- **入 initramfs**：mkinitcpio install hook `ravenfs`（守卫版：该内核无模块则跳过，绝不使 `mkinitcpio` 失败）。→ 一次性把 `ravenfs` 加进 `HOOKS` 后，跨内核更新自动生效。

## 安装（需 sudo 的步骤）

```bash
# 1) 依赖：dkms + 每个已装内核的 headers
sudo pacman -S --needed dkms linux-vfio-headers linux-headers

# 2) 构建 + 安装（普通用户运行；makepkg 内部会调 sudo pacman）
cd /home/PS/PS_git/OS_pj_uefi/ravenfs/linux
makepkg -si

# 3) 一次性把 hook 加入 HOOKS（默认置于 filesystems 附近），再重生成
sudo sed -i 's/^HOOKS=(\(.*\) filesystems \(.*\))$/HOOKS=(\1 ravenfs filesystems \2)/' /etc/mkinitcpio.conf
sudo mkinitcpio -P

# 4) 验证
dkms status                                   # ravenfs/<ver>, <kernel>: installed
sudo modprobe ravenfs && grep -w ravenfs /proc/filesystems
modinfo ravenfs -F version                    # = 包版本
lsinitcpio /boot/initramfs-linux-vfio.img | grep -m1 ravenfs
```

## 卸载

```bash
sudo pacman -Rns ravenfs-dkms
sudo sed -i 's/ ravenfs//' /etc/mkinitcpio.conf
sudo mkinitcpio -P
```

## 备注

- `makepkg` **必须非 root**；sudo 仅用于 `pacman`/`mkinitcpio`/`modprobe`。
- 若某内核没装 `*-headers`，DKMS 跳过它（该内核 initramfs 也不含 ravenfs，hook 安全跳过）。
- 源树路径可用 `RVN_SRC` 覆盖（默认仓库根）。
- 宿主 dev 构建（快速迭代）：`make -C /home/PS/PS_git/OS_pj_uefi/ravenfs/linux`（用运行内核 headers；**宿主只编译，不 insmod、不灰测**）。
