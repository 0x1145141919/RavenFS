# RavenFS Linux KO · v0.1.2 选择性改进 / 修复

> 2026-10-01 · Raven
> **对象**：`ravenfs-dkms 0.1.2`（在 v0.1.1 基础上做**选择性**改进与修复）。
> **对应基线**：`Docs/linux/ravenfs-linux-ko-v0.1.1-defects.md`（v0.1.1 缺陷集）。
> **口径**：本文记录 v0.1.1 缺陷集中**被选择处置**的项 + 配套的架构性改动；**未动的缺陷仍以 v0.1.1-defects 为准**（见 §四）。**行为级变更 → 版本升号**。

---

## 版本

- `linux/PKGBUILD`：`pkgver=0.1.2`（打包版本为**单源真值**；`linux/Makefile` 从中提取并 `-DRAVENFS_KO_VERSION` 注入 `version.c` → `modinfo -F version` 自证）。
  - 顺带修正：Makefile 的 pkgver 取值路径早先指向已不存在的 `packaging/arch/PKGBUILD`（版本恒 `dev`）→ 改为 `linux/PKGBUILD`（PKGBUILD 已升格至 `linux/`）。

---

## 一、架构性改动（非逐缺陷）

1. **位图模块重构为独立 C 类 `rfs_bm`**（替换旧单体 `linux/bitmap.c`）：
   - 路由层 `linux/bitmap_core.c`（共享几何 / 亲和扫描 / 跨片·跨窗续圈 / global↔local 换算）；
   - 后端 `linux/bitmap_mem.c`（MEM，god v1 语义）/ `linux/bitmap_chunk.c`（CHUNK 窗口，god v2 语义）；
   - 驱动适配 `linux/bitmap_adapter.c`（**13 个公共 `ravenfs_*` 签名不变**；MEM=装载+快照写回 / CHUNK=`sb_bread` 系 io 回调 + `chunks_asyn`）；
   - 契约头 `include/linux/ravenfs_bitmap_{core,backend,adapter}.h`；平台差异收口 `include/linux/ravenfs_platform.h`（RAII 锁守卫、`RFS_POPCOUNT64`、relaxed 原子）；
   - 旧 `linux/bitmap.c` **已清除**。
   - **两后端语义视角**（关键）：MEM = 位图**内存里即一等事实**；CHUNK = **合成视角**（在线窗 + 挂账归还链 + 盘上块，在合并点 / `chunks_asyn` 后才一致）。
2. **挂载参数**：`bitmap_mode=mem|chunk`（默认 `mem`）；chunk 下 `bbco`/`ibco`（**短名主键**，全名 `block_bitmap_chunk_order`/`inode_bitmap_chunk_order` 为别名；取值 0..6；默认 3/0）；**非法值 loud 拒挂**；保留 `row_size=`。
3. **头文件集中化**：ko 自有头 `linux/*.h → include/linux/*.h`（`Makefile` `ccflags` 同步；`include/` 下只放 `ravenfs*.h`）。

---

## 二、逐缺陷处置

| 缺陷 | 处置 | 证据 |
|---|---|---|
| **D-1** 计数器 / debugfs 出口 | ✅ 接入新 `rfs_bm` 类，**随 MEM/CHUNK 后端模式**呈现；经平台中立**事件出口注入**（类保持 sb-free，用户态零开销） | `8f9d9ed` · `6ceeaff`；VM 双模式计数核对 |
| **D-2** 位图全量常驻（内存浪费） | **部分达成**：CHUNK 后端经 `bitmap_mode=chunk` 落地，运行时常驻内存 ↓；**30s 全量回写未收敛**（`chunks_asyn` 仍整片读+写）——**用户 2026-09-30 接受**（NVMe SLC 写量小） | `72aafd2` 等；VM chunk 模式 |
| **D-3** mount/umount 时间戳不回写 | ✅ 已实现并入仓（`super.c`：`fill_super` 写 mount、`put_super` 写 umount + flush 屏障） | 随 `72aafd2` |
| **D-5** extent 叶内融合缺失 → 碎片 | ✅ **叶内四形态融合**（`none/left/right/both`，只叶内不跨界） + **预判法**（插入前瞻：是否需要新节点，先判定+先备好预算池，再动手） + **D-5c** 数据分配**锚定左邻物理尾**（令融合真触发） | `8f28ad1` · `4cde21c` |
| **D-6** inline→extent 不试 direct | ✅ 迁移**先试 direct**：整域连续分配成功即落 direct（**零 8K 树节点**），否则降级建树 | `ca569b2` |
| **D-9** 挂载参数只进不出（无 show_options） | ✅ `ravenfs_show_options` 落地，**回读集与 `parse_param` 同构**；chunk 时回读**短名** `bbco`/`ibco` | `72aafd2` |

---

## 三、验证（VM 实测）

- **"一箭三雕"**：D-2 两模式（`mem`/`chunk`）挂载可读写；D-1 两模式计数（CHUNK 专属字段在 MEM 恒 0、CHUNK 有数；共同字段守恒；state 段含 `bitmap_mode`）；D-9 参数控制 + `/proc/mounts` 短名回读 + 非法值 loud 拒挂。
- **D-5 收敛**：`cp` 512 MiB 近连续文件 → **2 条 extent 记录**（对比缺陷基线 32.5 GiB→133351 条）。
- **D-6**：`>160B` 小文件无 8K 树节点（实占 12K→8K）。
- **金测试（上实体机前必过）**：读写压测 → **卸载** → `tools/audit_v1.2/audit_v1.2` **卸载校验** → `bad_total=0`、`AUDIT PASS`（两模式均过）。

---

## 四、遗留 / 未动（诚实边界）

- **未动缺陷**：D-4（O_DIRECT 留白）、D-7（分配 hint 不持久化）、D-8（元数据/数据共用 hint → 2 块洞）。
- **D-2 写放大**：按脏写收敛（去掉 30s 全量回写）未做（接受现状）。
- **D-6 边角**：`head`/分段写致 direct→extent **增长**路径仍建 8K 根叶。
- **短写尾 direct 孤儿块**：由 **fsck 兜底**（用户决策）。
- **未覆盖**：O_DIRECT（D-4 相关）、断电/崩溃一致性、并发压测。

---

## 五、关联文档

- `Docs/linux/ravenfs-linux-ko-v0.1.1-defects.md` —— 缺陷基线。
- `Docs/runtime_tech/{02_bitmap_slices.md,03_chunk_backed_slices.md}` —— 位图分片/窗口设计。
- `Docs/linux/ravenfs-vm-env-notes.md` —— 独立测试 VM 环境。
