# 事件源扩展规范

> 与**子 Agent 同款**的「丢文件即注册」扩展模型。新增一个驱动源（飞书、定时、webhook、
> 邮件、Jira、监控告警…）**不需要改任何核心代码**——放一个文件就生效。

## 一句话

事件源只回答「**发生了什么**」；「这归谁、进哪个会话、什么时候跑、能不能跑」
全部由核心裁决。这条边界让插件永远拿不到绕过治理的能力。

---

## 1. 扩展点：放一个文件

| 域 | 路径 | 说明 |
|---|---|---|
| 内置 | `packages/persona/src/sources/` | 随包分发（`file_drop` 是参考实现） |
| 二开 | `custom/persona/`（仓库内二开域，同名**覆盖**内置）|

支持两种形态（与子 Agent 一致）：

```
sources/
├── my_source.ts          # 单文件形态
└── my_other/             # 目录形态
    └── my_other.ts       # （或 index.ts）
```

**命名约束**：`[a-z0-9_]+`，且 `source.name` 必须与文件名一致（防止改名后配置失配）。
以下不参与注册：`*.test.ts`、`*.d.ts`、`_` 开头（内部辅助）。

## 2. 最小可用源

```ts
// sources/hello.ts
import type { EventSourceContext, EventSourceDef } from "../source"

export const source: EventSourceDef = {
  name: "hello",                       // 必须与文件名一致
  description: "演示：每 30 秒产生一条事件",
  channel: "hello",                    // 路由键，[a-z0-9_]+
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

    // 清理：stop() 无参数，用闭包/栈承接（见 §5 状态隔离）
    cleanups.push(() => clearInterval(timer))
  },

  async stop() {
    cleanups.pop()?.()
  },
}

const cleanups: Array<() => void> = []
```

## 3. 契约

### 3.1 `EventSourceDef`

| 字段 | 必填 | 说明 |
|---|---|---|
| `name` | ✅ | `[a-z0-9_]+`，**须与文件名一致** |
| `description` | ✅ | 非空；面板与诊断可见 |
| `channel` | ✅ | `[a-z0-9_]+`；人格按它做路由匹配，**必须稳定** |
| `envVars` | — | 声明式白名单 `{name, description, secret?}`；未声明的环境变量**读不到** |
| `start(ctx)` | ✅ | 启动订阅；抛错 = 该源不可用（不阻断其它源） |
| `stop()` | — | 清理连接/定时器 |

**未知字段会被拒绝**（启动即报错，不静默忽略）——字段拼错的静默失效是最难排查的一类问题。

### 3.2 `ctx`（插件唯一的出口）

```ts
interface EventSourceContext {
  emit(event: IngressEvent): Promise<EmitResult>  // 投递事件（唯一出口）
  log(level, msg): void                            // 带源名前缀的结构化日志
  home: string                                     // {GEBAI_HOME}
  name: string                                     // 插件名
  env(name): string | undefined                    // 仅 envVars 声明过的变量
}
```

**`ctx` 里没有** engine / store / registry：事件源不需要、也不该有运行任务或读写会话的能力。

### 3.3 `IngressEvent`（插件产出什么）

```ts
interface IngressEvent {
  kind: "schedule" | "message" | "webhook" | "desktop" | "idle"
  channel: string          // 缺省用 def.channel
  source: string           // 人类可读来源，如「飞书群『支持群』」
  summary: string          // 一句话：发生了什么
  body: string             // 主体内容（模型的输入）
  refs?: {                 // 结构化引用（谁说的、回到哪）
    chatId?: string; messageId?: string
    senderId?: string; senderName?: string
    taskId?: string; extra?: Record<string, string>
  }
  id?: string              // 显式幂等键（强烈建议给）
  attachments?: PersonaAttachment[]
  at?: number              // 事件产生时刻
  personaId?: string       // 通常**不该填**：归属是部署配置的事
}
```

### 3.4 `EmitResult`（核心已裁决，插件据此决定回不回话）

| 状态 | 含义 | 插件该做什么 |
|---|---|---|
| `accepted` | 已接管（开跑或排队） | 正常 |
| `unattended` | 无归属人格 | **不要自行处理**，记日志（保证零配置不误吞） |
| `duplicate` | 同 id 已处理（幂等丢弃） | 正常，静默 |
| `overflow` | 队列已满 | 回一句"忙不过来"或记日志 |
| `unavailable` | 运行时未启动/已停止 | 记日志 |

## 4. 幂等键：每个源都必须想清楚

各驱动源都会**重投**（飞书 ACK 前断连会重推、调度器重启会重新触发、webhook 按自身策略重试）。
重复事件 = 重复副作用（重复发消息、重复提交单据），是真实事故形态。

| `kind` | 核心推导规则（缺省） | 建议 |
|---|---|---|
| `message` | 渠道消息 id → `msg:{id}`；缺失时用 `来源+正文哈希` | **用消息 id** |
| `schedule` | `task:{taskId}@{触发时刻}` | 交给核心 |
| `webhook` | 投递方 id → `hook:{id}`；缺失时内容哈希 | **用投递 id** |
| `desktop` / `idle` | 时刻 + 正文哈希 | 显式给 `id` |

插件给了 `id` 就以它为准。**幂等键错了，任何上层机制都救不回来。**

## 5. 状态隔离（最容易踩的坑）

`start` 可能被**同一进程内的多个 manager 实例**调用（多租户、热加载期间新旧并存、测试并行）。
因此：

- ❌ **不要**把运行时状态放模块级变量
- ❌ **不要**挂在共享的 `def` 对象字段上
- ✅ 所有状态放 `start` 的闭包里

`stop()` 没有参数，但它是**同一次 `start` 对应 def 对象上的方法**，用闭包或栈承接即可：

```ts
const cleanups: Array<() => void> = []

async start(ctx) {
  let stopped = false                    // 闭包内，实例私有
  const timer = setInterval(() => { if (!stopped) void poll() }, 1500)
  timer.unref?.()
  cleanups.push(() => { stopped = true; clearInterval(timer) })
},

async stop() {
  cleanups.pop()?.()                     // LIFO：后启动的先停
},
```

> 实测代价：写成模块级变量时，多条用例顺序执行会互相清空 `handled`/`timer`，
> 表现为「事件偶发丢失」，而单实例手动验证完全正常——极难定位。

## 6. 与子 Agent 一致的机制

| 机制 | 行为 |
|---|---|
| **热加载** | 目录签名（`路径:mtime`）变化 → 重扫重启；成本约一次目录遍历 |
| **失败隔离** | 单源加载/启动抛错只记 `loadErrors`，**其它源照常运行** |
| **二开覆盖** | custom 域同名源胜出内置；覆盖版加载失败时**保留旧可用版本** |
| **字段校验** | 未知字段/类型不符/命名非法 → 拒绝并报错（不静默降级） |
| **启停名单** | `enable`（白名单非空仅启用名单内）+ `disable`（先白后黑）|

**失败隔离为什么重要**：若不隔离，"飞书源配错"会连带"定时任务也不跑了"——
表现为「整个拟人模式失效」，而根因只是一个小配置错。

## 7. 程序化注册

不适合写成文件的场景（内置核心源、需要宿主上下文、测试注入）用 `register()`：

```ts
const mgr = new EventSourceManager({ scanDirs, runtime, home })
mgr.register(mySource)   // 语义与扫描一致；同名覆盖；**不受启停名单约束**
await mgr.start()
```

`register()` 同样走字段校验（不能绕过）。显式注册的源在后续重扫中**不会被丢弃**。

## 8. 接入服务端

```ts
const manager = new EventSourceManager({
  scanDirs: defaultEventSourceScanDirs({
    packageRoot: import.meta.dir,      // @gebai/persona 包根（二开域自动推为 <repo>/custom/persona）
  }),
  runtime,                              // PersonaRuntime 实例
  home: config.gebaiHome,               // 仅作 ctx.home（插件自有状态落盘位置）
  enable: splitList(env("GEBAI_EVENT_SOURCES_ENABLE")),
  disable: splitList(env("GEBAI_EVENT_SOURCES_DISABLE")),
  log: { info: log.info, warn: log.warn, error: log.error },
})

await manager.start()                   // 发现 + 启动全部启用源
// 投递前可调（成本约一次目录遍历）：
await manager.refreshIfChanged()
// 关闭：
await manager.stop()
```

## 9. 诊断

```ts
manager.state()        // 每源：started / enabled / channel / stats{emitted,accepted,unattended,duplicate,overflow,unavailable} / error
manager.loadErrors()   // 加载与启动失败清单（{name: reason}）
manager.list()         // 已注册源定义
```

`unattended` 计数持续增长 = 归属配置有问题（该源的 `channel` 没被任何人格认领）。

## 10. 检查清单（写新源时逐条过）

- [ ] `name` 与文件名一致，`channel` 是稳定的 `[a-z0-9_]+`
- [ ] `start` 订阅后**立即 resolve**，长循环在后台
- [ ] 运行时状态全在闭包内（无模块级变量、无 `def` 字段）
- [ ] `stop` 能真正释放定时器/连接
- [ ] 幂等键尽力给显式 `id`（各源重投是常态）
- [ ] 需要的环境变量写进 `envVars`（否则读不到）
- [ ] `emit` 的非 `accepted` 结果有处理（至少记日志）
- [ ] `start` 抛错只用于「配置错/不可恢复」；临时故障自己重试
