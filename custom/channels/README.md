# custom/channels/ — 二开渠道（自动扫描域）

本目录是**对外动作渠道的二开域**，与 `custom/agents/`（二开子代理）、`custom/persona/`（二开事件源）
同级同语义：**放文件即注册，零改核心代码**；上游更新时整个 `custom/` 拷到新仓库根即完成迁移。

```
custom/
├── agents/          # 二开子代理
├── persona/         # 二开事件源
└── channels/        # 二开渠道（本目录）
    └── my_chan.ts   # ← 自建：export const channel: ChannelDef = {...}
```

## 这个抽象解决什么

按渠道列工具（`send_message` / `create_issue` / `write_memory`…）泛化不下去：每加一个渠道
就要加工具、改提示词、影响**全部会话**。而「发消息」「提工单」「写记忆」在性质上完全同构
（有外部副作用、需审批、需留痕、0~N 次），只是**目标**与**动词**不同。

所以一次对外动作被拆成三个正交量：

```
                     ┌─ 目标 target：`channel:address`（谁）
   一次对外动作 = act ┼─ 动词 verb：渠道声明的封闭集合（做什么）
                     └─ 载荷 args：由动词 schema 约束（什么内容）
```

模型只需要两个工具：`act`（做）与 `observe`（查），外加 `channels_list`（先看见再决定）。

## 放一个文件就生效

```ts
// custom/channels/webhook.ts
import type { ChannelContext, ChannelDef } from "@gebai/channels"

export const channel: ChannelDef = {
  name: "webhook",           // 须与文件名一致，[a-z0-9_]+
  description: "往运维验收地址推事件",
  envVars: [{ name: "WEBHOOK_URL", description: "目标地址" }],

  // 写面声明（动词 → 能力）
  verbs: {
    post: {
      summary: "POST 一段 JSON 到验收地址",
      writes: true,                    // true = 需审批
      args: {
        event: { type: "string", description: "事件名", required: true },
        detail: { type: "string", description: "详情" },
      },
    },
  },

  // 读面声明
  observes: {
    ping: { summary: "探活" },
  },

  // 目标白名单：越界直接拒绝（不转审批）
  targets() {
    return ["acceptance"]
  },

  async act(input, ctx) {
    if (input.verb === "post") {
      const url = ctx.env("WEBHOOK_URL")
      if (!url) throw new Error("缺少 WEBHOOK_URL")
      const res = await fetch(url, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ event: input.args.event, detail: input.args.detail }),
      })
      if (!res.ok) throw new Error(`推送失败 HTTP ${res.status}`)
      return { summary: `已推 ${String(input.args.event)}`, ref: String(res.status) }
    }
    throw new Error(`不支持 verb ${input.verb}`)
  },

  async observe(input, ctx) {
    if (input.what === "ping") {
      const res = await fetch(ctx.env("WEBHOOK_URL")!, { method: "HEAD" })
      return { summary: res.ok ? "可达" : `不可达 HTTP ${res.status}` }
    }
    throw new Error(`不支持 observe ${input.what}`)
  },
}
```

也支持目录形态：`custom/channels/{name}/{name}.ts`（或 `index.ts`）。

## 六条必须知道的约定

1. **`name` 须与文件名一致**、`verb`/`what` 是稳定的 `[a-z0-9_]+`。
2. **声明与实现必须成对**：`verbs` 有声明就必须有 `act()` 实现，反之亦然。
   只有一边 = 加载期直接拒绝（否则表现为「调用永远失败」而错误离配置很远）。
3. **`writes` 决定审批**：`true` 走审批，缺省/`false` 免审。声明 `false` 却真的写入
   等于绕过审批——这是实现方责任，不是框架能兜住的。
4. **`targets()` 是安全项**：返回数组 = 只允许清单内；返回 `null` = 不做地址级限制
   （仅当地址本身即权限边界时用）。**越界会被直接拒绝，不进审批**——发错群是信息泄露，
   不是「让用户决定」的事。返回 `[]` = 什么都不允许（未配置时的安全缺省）。
5. **运行时状态放本次调用的闭包**，不要用模块级单变量（同进程可能有多个注册表实例：
   热加载新旧并存、测试并行、多租户）。清理器按 `(ctx.home, this)` 登记，`stopListen(ctx)`
   按同一键取出。
6. **需要的环境变量写进 `envVars`**：未声明的读不到（`ctx.env` 按声明白名单过滤，
   防止「读告警的渠道」顺手拿到模型密钥）。

## 输入面：渠道也能收事件

渠道可以是纯输出（只发）、纯输入（只收），或两者兼有。收事件在 `ChannelDef` 上并列声明：

```ts
export const channel: ChannelDef = {
  name: "webhook_in",
  description: "接收运维告警推送",
  listens: { alerts: { summary: "订阅告警" } },
  defaultKind: "webhook",

  async startListen(sink, ctx) {
    // 三条约束：
    //  ① 尽快返回（长驻循环/建连放后台，否则启动阻塞）
    //  ② 状态放本次调用的闭包（模块级会被并行实例互相清空）
    //  ③ 不自己建第二条连接（宿主已有同一系统的连接时经 ctx.deps 取）
    const timer = setInterval(async () => {
      const alerts = await pull(ctx.env("ALERT_FEED_URL")!)
      for (const a of alerts) {
        await sink.emit({
          summary: a.title,
          body: a.detail,
          id: `alert:${a.uuid}`,       // ← 幂等键：各源都会重投，务必给
          refs: { extra: { host: a.host } },
        })
      }
    }, 5000)
    cleanups.set(cleanupKey(ctx.home, this), () => clearInterval(timer))
  },

  async stopListen(ctx) {
    const k = cleanupKey(ctx.home, this)
    const fn = cleanups.get(k)
    if (fn) {
      cleanups.delete(k)
      fn()
    }
  },
}
```

渠道只需回答「发生了什么」（`summary`/`body`/`refs`/`id`）。**不要试图**在事件里指定
渠道名或归属人格——那是核心职责，`IngressEvent` 根本没有这些字段（强塞也无效）。

`sink.emit()` 返回五态，渠道据此决定记不记日志：

| 返回值 | 含义 | 渠道该做什么 |
|---|---|---|
| `accepted` | 至少一个订阅者已接管 | 什么也不做 |
| `unrouted` | **无人关注** | 记日志，**不要**自行处理（否则零配置时会误吞） |
| `duplicate` | 同 id 已被处理 | 正常，什么也不做 |
| `overflow` | 订阅者队列已满 | 记日志（可考虑重投） |
| `unavailable` | 调度内核未启动 | 记日志（不要崩掉轮询循环） |

## 治理边界（这条不能破）

渠道只回答「**怎么把这件事做掉**」/「**外面发生了什么**」；「**能不能做**」「**归谁**」由核心裁决：

| 裁决项 | 在哪判 |
|---|---|
| 渠道是否存在/已启用 | 执行器 |
| 能力是否已声明 | 执行器（未声明 → 列出可用能力） |
| 必填参数是否齐备 | 执行器（缺 → 指出缺哪个） |
| 目标是否在白名单 | 执行器（越界 → 直接拒绝，fail-closed） |
| 是否需要人批准 | 工具层（按 `writes` 判定） |
| 入站事件归谁 | 桥接（渠道名硬写）+ 调度内核（路由） |
| 审批、留痕、审计 | 引擎 |

`ChannelContext` 里**没有** engine / store / registry——渠道不需要、也不该有
运行任务或读写会话的能力。它只有两个出口：`act`/`observe` 的返回值，
以及 `startListen` 传入的 `sink`（已绑定本渠道名字，无法伪造）。

## 自动发现机制（与子代理/事件源一致）

| 机制 | 行为 |
|---|---|
| 双域扫描 | 内置 `packages/channels/src/channels/` + 本目录，同名时**本目录胜出** |
| 热加载 | 目录签名（`路径:mtime`）变化即重扫重启，无需重启服务 |
| 失败隔离 | 单渠道 import/启动抛错只记 `loadErrors`，其它渠道照常运行 |
| 严格校验 | 未知字段 / 类型不符 / 命名非法 / 声明无实现 → 拒绝并报错（不静默降级） |
| 启停名单 | `GEBAI_CHANNELS_ENABLE`（白名单非空仅启用名单内）+ `GEBAI_CHANNELS_DISABLE`（先白后黑）|

## 内置渠道

| 渠道 | 写（act） | 读（observe） | 收（listens） |
|---|---|---|---|
| `local` | `append`（记忆分段追加）/ `replace` / `put`（产物落盘） | `all` / `sections` / `section` / `list` / `get` | `drop`（JSON 投递）/ `inbox`（文本投喂） |
| `feishu` | `send` / `reply`（引用回复） | `messages`（群历史）/ `members` | `messages`（群消息） |

飞书渠道需要 `GEBAI_CHANNELS_FEISHU_APP_ID` / `_APP_SECRET`（**不回落** `GEBAI_FEISHU_*`——
会话机器人与对外人格是两个不同应用），以及**必须配置** `GEBAI_CHANNELS_FEISHU_CHATS`
（群白名单，逗号分隔）——未配置时出站不允许任何目标（`targets()` 返回 `[]`），
这是刻意的 fail-closed。

## 完整规范

见 [`packages/channels/README.md`](../../packages/channels/README.md)。

## 验证

```bash
cd packages/channels && bun test    # 60 用例
bun x tsc --noEmit
```
