# custom/persona/ — 二开拟人事件源（自动扫描域）

本目录是**拟人运行模式的事件源二开域**，与 `custom/agents/`（二开子代理）同级同语义：
**放文件即注册，零改核心代码**；上游更新时整个 `custom/` 拷到新仓库根即完成迁移。

```
custom/
├── agents/          # 二开子代理（install 时自动扫描）
└── persona/         # 二开拟人事件源（本目录，自动扫描）
    ├── .gitkeep
    └── my_source.ts # ← 自建：export const source: EventSourceDef = {...}
```

## 放一个文件就生效

```ts
// custom/persona/hello.ts
import type { EventSourceContext, EventSourceDef } from "@gebai/persona"

export const source: EventSourceDef = {
  name: "hello",          // 须与文件名一致，[a-z0-9_]+
  description: "演示：每 30 秒产生一条事件",
  channel: "hello",       // 路由键：人格按它认领事件
  envVars: [{ name: "HELLO_TARGET", description: "目标标识" }],

  async start(ctx: EventSourceContext) {
    // start 必须尽快返回：订阅后立即 resolve，长驻循环放后台
    const timer = setInterval(() => {
      void ctx.emit({
        kind: "message",
        channel: "hello",
        source: "演示源",
        summary: "定时问候",
        body: "到点该问候了",
        refs: { chatId: "oc_demo" },
      })
    }, 30_000)
    timer.unref?.()
    cleanups.push(() => clearInterval(timer))
  },

  async stop() {
    cleanups.pop()?.()
  },
}

const cleanups: Array<() => void> = []
```

也支持目录形态：`custom/persona/{name}/{name}.ts`（或 `index.ts`）。

## 四条必须知道的约定

1. **`name` 须与文件名一致**、`channel` 是稳定的 `[a-z0-9_]+`。
   `channel` 拼错不会报错，但事件会全部 `unattended`（无人格认领）——表现为「配了但没反应」。
2. **运行时状态一律放 `start` 闭包**，不要用模块级变量，也不要挂 `def` 字段。
   同一进程可能有多个 manager 实例（热加载新旧并存、测试并行、多租户），
   模块级状态会被互相清空，表现为「事件偶发丢失」。
3. **幂等键尽力给显式 `id`**：各源都会重投（飞书 ACK 前断连重推、webhook 重试、
   调度重启重触发）。重复事件 = 重复副作用（重复发消息/重复提单）。
4. **需要的环境变量写进 `envVars`**：未声明的读不到（`ctx.env` 按声明白名单过滤，
   防止「读告警的插件」顺手拿到模型密钥）。

## 自动发现机制（与子代理一致）

| 机制 | 行为 |
|---|---|
| 双域扫描 | 内置 `packages/persona/src/sources/` + 本目录，同名时**本目录胜出** |
| 热加载 | 目录签名（`路径:mtime`）变化即重扫重启，无需重启服务 |
| 失败隔离 | 单源 import/启动抛错只记 `loadErrors`，其它源照常运行 |
| 严格校验 | 未知字段 / 类型不符 / 命名非法 → 拒绝并报错（不静默降级） |

## 完整规范

见 [`packages/persona/docs/event-sources.md`](../../packages/persona/docs/event-sources.md)，
参考实现见 [`packages/persona/src/sources/file_drop.ts`](../../packages/persona/src/sources/file_drop.ts)。

## typecheck

本目录已纳入 `custom/tsconfig.json` 的 include 范围（`bun run typecheck:custom`），
`import type { … } from "@gebai/persona"` 的类型随之可解析。
