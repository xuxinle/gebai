All changes are coherent and verified. Here's the summary:

根因（已用实验/仓库证据坐实）
你朋友那台机器报 [subagents] bundle 注册表缺失：请先运行 bun run scripts/build-subagents.ts、子Agent 一个都没有，根源是：

subagents.bundle.generated.ts 被 .gitignore（构建时生成、不入库）。
SubAgentManager.discover()（subagents.ts:36）在 dist/二进制模式下回退 await import("./subagents.bundle.generated")；若该 exe 是在裸检出未先跑 build-subagents.ts 的旧/不完整工作区构建的，bundle 缺失 → 子Agent 注册表为空 → 你本地「自己用」正常，拷给别人全缺。
连带结果正是你最开始问的「环境变量前端丢失无法配置」：env catalog（getEnvCatalog(allDefs())）只含被打包子Agent 的 envVars，子Agent 全缺 → 目录只剩全局组，前端 filterEnvToCatalog 白名单又过滤掉目录外一切变量 → 即使手动写 localStorage 也会被丢弃；且前端 renderSettingsEnv 在 catalog 请求失败时直接 return，保存按钮与表单都不渲染。
