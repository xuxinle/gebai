# custom/ — 二次开发子代理与依赖（独立迁移域）

本目录是**歌白二次开发的专属目录**：自研子代理与依赖组件放这里，与上游 `packages/` 完全隔离——
**上游版本更新时，整个 `custom/` 文件夹复制到新版本仓库根即完成迁移**（上游不触碰本目录，无合并冲突面）。

```
custom/
├── agents/          # 二开子代理定义（自动扫描域——同 packages/agents/src/agents/ 布局）
│   └── my_agent/                 # ← 自建：{name}/{name}.ts（export const def: SubAgentDef = {...}）
│       ├── my_agent.ts           #    系统提示词可拆 {name}.md；纯 {name}.md 亦可（零 TS 简化定义）
│       └── my_agent.md
├── core/            # 二开依赖组件（自动可 import——同 packages/agents/src/core/ 布局）
│   └── my_lib/                   # ← 自建：{lib}/index.ts，子代理内相对引用 ../../core/{lib}
│       └── index.ts
└── tsconfig.json    # 已配 paths：@gebai/sdk / @gebai/sdk/node / @gebai/agents 指向上游包
```

（以上 `my_agent/`、`my_lib/` 仅为结构示意——本目录出厂为空骨架，`agents/` 与 `core/` 内的 `.gitkeep`
仅用于占位保留目录，无功能语义，复制迁移时可一并覆盖，不影响任何行为。）

## 自动发现（零注册）

- **dev 运行时**：`custom/agents/` 与内置域**双域扫描自动合并**，新增/修改/删除文件即热加载生效（与内置子代理同机制）——放文件即注册，无需改任何清单或入口
- **构建打包**：`build-subagents.ts` 同样双域扫描，二开子代理自动打进 bundle 注册表
- **同名覆盖**：`custom/agents/{name}` 与内置子代理同名时，**custom 版本胜出**（覆盖内置定义）——可用于改写内置行为而不动上游代码
- **失败隔离**：单个二开子代理 import 抛错只记 loadErrors（模型侧可见根因），不影响内置与其他二开子代理

## 编码约定（与内置子代理一致）

- 子代理名 `[a-z0-9_]+`；入口 `{name}/{name}.ts` 或 `{name}/index.ts`（前者优先）；纯 `{name}/{name}.md` 即零 TS 简化定义
- 契约类型从 `@gebai/sdk` 导入；node 工具值导入走 `@gebai/sdk/node`
- 依赖组件写 `custom/core/{lib}`，子代理内相对引用 `../../core/{lib}`
- typecheck：`bun run typecheck:custom`（根 `bun run typecheck` 已自动包含）

## 迁移

新版本歌白发布后：`cp -r custom/ <新仓库根>/`（目录内文件覆盖同名，`.gitkeep` 无碍），重启即生效。
二开资产与上游升级互不干扰。
