# DISCIPLINE.md — RavenFS 工程纪律

> 状态：初版（仅 AI 协作纪律，其余领域待补充）

## 项目定位

- **学习性项目**（掌握度高，重点：AI 思维可见性）
- 三端适配：host（用户态）/ sparrowos（内核 kmod）/ linux（内核模块），核心算法在 `core/` 共享
- 配套规划：`Docs/plan.md`、`Docs/raven_toolchain.md`、`Docs/disk_layout_v1.md`

## AI 协作纪律

> 约束对象：AI（Raven / OpenCode 等），非人类开发者。

- **拒绝 token 压缩类 skill**（caveman、ADHD 类降效/压缩等）。
- 理由：省 token 不利于人机思维上下文对齐。维护工作本质是对齐 AI 与人的思维——设计意图、历史决策、踩坑记录；压缩即丢弃对齐材料，重构产物易偏离设计意图且偏差难以察觉。
- 注重内部设计与维护者视角，不是只看结果（许愿式编程）。
- AI 思维可见性：设计文档与代码注释应保留推理过程与决策依据，便于设计方审查。

## 待补充领域

- 构建纪律
- 编码风格
- 文档纪律
- Git 提交纪律
