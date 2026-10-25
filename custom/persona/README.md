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

## 这两类东西都放本目录

同一个目录承载两种二开物（**导出名不同**，文件名不冲突即可）：

| 导出 | 是什么 | 文件形态 |
|---|---|---|
| `export const source: EventSourceDef` | 事件源（定时 / webhook / 文件投递 / 桌面…） | 本文下半部分 |
| `export const persona: PersonaDef` | **人格定义**（这是谁、盯什么、有哪些分身） | 见下方「写一个人格」 |

为何不分两个目录：它们都是「拟人模式的二开物」。分开放只会让人记「人格放 A、源放 B」，
而文件名不冲突这件事本来就能靠导出名区分。

## 写一个人格

```ts
// custom/persona/xiaoshuang.ts —— 文件名必须与 id 一致
export const persona: PersonaDef = {
  id: "xiaoshuang",
  name: "小霜",
  role: "训练平台值班工程师",
  identity: "你是小霜……",      // 原样进系统提示词，框架不改写
  drivers: [
    { id: "support", focus: "答支持群", subscriptions: ["message"] },
    { id: "alerts",  focus: "盯告警",   subscriptions: ["webhook", "schedule"] },
  ],
  agents: ["cefu"],            // 可选：会话默认装载的子代理工具包
}
```

### `agents`：给这名角色接上记忆

不写 `agents` 时，人格只靠**会话历史**记事：每轮都从零开始，今天判断过的明天还要重新判断，
用户纠正过的下次还会错。写成 `agents: ["cefu"]` 后两件事同时发生：

| 半边 | 宿主要做什么 | 效果 |
|---|---|---|
| 工具面 | 建会话时写进 `loadedSubAgents` | 它能用 `cefu_search / cefu_get / cefu_put / cefu_review` 等 |
| 提示词 | 人格注记多一段「你的记忆」 | 它**知道何时查、何时写**、写时必给 `title/category/source_ref` |

**两半必须同源**：只装工具不写纪律，模型面对一排工具想不起来用；只写纪律不装工具，
它会去调不存在的工具然后失败。两种错配都不报错，表现都是「配了但没效果」。

默认**不装**：装载一份工具包 = 多一层工具面进上下文（每个工具的名称+描述+schema 都算 token）。
不替已有部署偷偷加开销。

> 写 `agents` 的人应当同时在 `identity` 里写清“哪些知识该从记忆里查、哪些靠你自己判断”——
> 框架负责接通，**分工得部署方定**。`packages/persona/src/personas/xiaoshuang.ts` 是个例子。

`subscriptions` 是**封闭集合**（`*` / `kind` / `channel` / `channel:kind`），不支持正则与通配前缀；
写错会在加载期报错而不是静默失效（一个永不响应的分身最难排查）。

写完不用重启：事件源与人格都按目录签名热加载。改完在「设置 → 拟人」面板里看它是否出现在
「人格与分身」下——出现了但“未通电”，就去看事件源/渠道入站两项。

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

## 排查顺序（配了但没反应时）

「设置 → 拟人」面板的四段就是排查顺序：

1. **出站凭据**不通 → 人格能想、但发不出话（所有计数都正常，最容易误判）
2. **事件源**未启动 → 定时/webhook 类订阅永远没事件（`kind: schedule` 的分身永不醒）
3. **渠道入站**的「无人订阅」持续涨 → 该 `channel` 没被任何人格认领（改订阅，不是渠道坏了）
4. **分身**显示「未通电」→ 还没发生过投递；长期如此就看回第 2、3 项

手动启停单个源（不必重启服务）：

```bash
curl -X POST http://localhost:3000/api/v1/persona/sources/file_drop/start
curl -X POST http://localhost:3000/api/v1/persona/sources/file_drop/stop
```

它是诊断口，**不改变启停名单**（重启后仍按 `GEBAI_EVENT_SOURCES_*` 决定）。

## 完整规范

见 [`packages/persona/docs/event-sources.md`](../../packages/persona/docs/event-sources.md)，
参考实现见 [`packages/persona/src/sources/file_drop.ts`](../../packages/persona/src/sources/file_drop.ts)。

## typecheck

本目录已纳入 `custom/tsconfig.json` 的 include 范围（`bun run typecheck:custom`），
`import type { … } from "@gebai/persona"` 的类型随之可解析。
