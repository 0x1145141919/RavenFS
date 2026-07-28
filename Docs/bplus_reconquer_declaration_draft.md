# B+ tree 核心算法重征宣言

> 2026-07-28
> 设计方：PS　　执行方：Raven

---

## 一、逻辑链条

### 背景

- Phase 1 规划是**上帝类**路线——所有算法内聚，面向测试
- `kernel/tests/fs_test_main.cpp` 已在另一个 FS 上实践过上帝类设计
- 人性上抗拒重复劳动（再搓一套上帝类测试框架），期望 AI 代劳核心算法

### 实际演进

1. **B+ tree dentry 没有手搓**，选择先搭建缓存层、VFS 层
2. 缓存层（block cache + bitmap cache + inode cache）已经打磨到位，私有缓存协议经过多轮迭代
3. VFS 上层（路径解析、dentry cache、句柄管理）工作量基本完成
4. 发现 B+ tree separator invariant 损坏 bug，深入追踪后暴露了核心算法的缺陷

### 论断

```
若一开始就征服 B+ tree dentry:
  隐式工作 = 上帝类测试框架 + mock I/O + 边界验证 + ...（工作量巨大）
  
现在重征 B+ tree dentry:
  前置条件 = 缓存层就绪 + VFS 层就绪 + 真实硬件验证环境
  剩余工作 = 纯核心算法修正/重写
  ROI     = 更高（因为有基础设施兜底，不需要白手起家搭验证环境）
```

### 结论

核心算法放在最后征服，反而 ROI 最高——因为前面搭建的每一层（缓存、VFS、测试管线）都是它的验证基石。这不是拖延，是路径依赖下的理性选择。

## 二、当前状态

| 层 | 状态 |
|---|---|
| 磁盘布局 | 定型（`disk_struct.h` 契约） |
| block cache + bitmap cache + inode cache | 已验证 |
| VFS cache（dentry cache + logical inode） | 已验证 |
| 测试管线（mkfs → stress → replay → validate） | 就绪 |
| B+ tree dentry insert/split 核心算法 | **有 bug，需重征** |

## 三、后续方向

- 以 `stress_mkdir_rev + replay_insert --validate` 作为验收标准
- 修复/重写 `dentry_insert` 的 cascade 逻辑，消除 separator invariant 损坏
- 保持现有缓存和 VFS 层不变，只动 `Userspace_God/dentry.cpp`
