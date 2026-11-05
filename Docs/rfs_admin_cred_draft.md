# rfs_admin 可选身份层（--uid / --gid）设计草案

> 最终版 — 2026-11-05
> 状态：**draft**（编译 + 冒烟验证通过；未冷却，未升 spec）

---

## 1. 出发点

`rfs_admin` 是离线镜像运维工具，历史人格是“root + 不管权限”——这是刻意的：
运维不该被 DAC 挡住。但验证/复现多用户镜像的权限语义需要一套**真实落盘的
判定**：权限位保存在 inode 里，判定发生在每次访问时，而不是在工具外另建一张表。

设计方结论（原话）：

> rfs_admin 是刻意一个不管 inode 权限的。若是这样的话我看 rfs_admin 里面得加
> 可选参数：uid：gid，不带就是 uid:gid 都是 0，root 用户，带了则是根据 linux
> 的那些 vfs 权限逻辑，表演。

## 2. 契约

1. **默认零变化**：不带 `--uid/--gid` → `uid=gid=0`，不启用任何检查，
   与历史版本行为一致（包括新建 inode 的属主仍是 0:0）。
2. **显式才启用**：出现任一选项即 `enforce=true`；只给 `--uid` 时 `gid` 保持 0。
3. **检查语义对齐 Linux VFS DAC**：
   - 属主匹配 → owner 位；否则属组匹配 → group 位；否则 other 位；
   - 路径分量需要 `x`；列目录需要 `r`；读文件 `r`；写/截断 `w`；
     创建/删除目录项需要父目录 `w+x`；
   - `uid=0` 旁路 rwx（CAP_DAC_OVERRIDE 口径），但普通文件的 `x`
     仍要求任一 `x` 位置位。
4. **新建即铸属主**：`mkdir / put / cpin` 创建的新 inode 写当前身份的
   `uid/gid`；覆盖写不改属主。
5. **新增道具**：`chmod <八进制> <path>`、`chown <uid>:<gid> <path>`。
   enforce 时 `chmod` 需属主或 root，`chown` 仅 root。
6. **不是安全边界**：镜像物理持有者可以直接改盘；本层只解决“在真实盘面上
   演示 VFS 权限语义”。

## 3. 明确不做（本轮）

- sticky bit / ACL / setuid/setgid / capabilities；
- 属组集合（只比较主 gid，不做 supplementary groups）；
- `cpin` 从 host 继承 uid/gid/mode（仍是 god 默认 0644/0755，再 `chmod` 调）；
- 内核模块侧的权限路径改动（本层只在用户态工具里）。

## 4. 落点

- `tools/rfs_admin/rfs_admin.cpp`
  - 全局 `g_cred_{enforce,uid,gid}` + `may()/need_perm()/stamp_owner()`
  - `main()` 选项解析（`--uid N` / `--uid=N` / `-u`，`--gid` 同）
  - `lookup_path / ensure_parent / ls / put / mkdir / rm / cpout / cpin /
    truncate / find` 插检查；新增 `chmod / chown`
- `tools/rfs_admin/README.md` §4.1 记录契约与 §5 多用户示例。

## 5. 待冷却观察

- `ls -l` 目前不显示 uid/gid（保持既有列格式）；多用户场景是否需要在
  enforce 模式下加属主列，等实际使用反馈。
- 目录删除只看父目录 `w+x`，没有 sticky bit；共享目录场景的实际体验待观察。
