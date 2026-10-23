# @gebai/channels — 对外动作的抽象层

**一句话**：把「发消息 / 提工单 / 写记忆 / 落产物」收敛成同一个动作面，新渠道**放文件即注册**。

## 为什么需要它

按渠道列工具（`send_message` / `create_issue` / `write_memory`…）的分化方式泛化不下去：

- 每加一个渠道就要加工具 → 工具表膨胀 → 每个新渠道都往**全部会话**的系统提示里加一段
- 「发消息」和「提交工单」性质相同（外部副作用、需审批、需留痕、0~N 次）却各造一套
- 新渠道 = 改工具面 = 改提示词 = 影响所有会话

而它们的调用形状几乎一样，只是**目标**与**动词**不同。

## 抽象：三个正交量

```
                     ┌─ 目标 target：`channel:address`（谁）
   一次对外动作 = act ┼─ 动词 verb：渠道声明的封闭集合（做什么）
                     └─ 载荷 args：由动词 schema 约束（什么内容）
```

| 场景 | 调用 |
|---|---|
| 发消息 | `act(to:"feishu:oc_x", verb:"send", args:{text})` |
| 引用回复 | `act(to:"feishu:oc_x", verb:"reply", args:{text, reply_to:"om_1"})` |
| 提交工单 | `act(to:"jira:PROJ", verb:"submit", args:{title})` |
| 改状态 | `act(to:"jira:PROJ", verb:"update", args:{status})` |
| 写记忆 | `act(to:"local:memory", verb:"append", args:{section, text})` |
| 读群历史 | `observe(to:"feishu:oc_x", what:"messages")` |
| 列产物 | `observe(to:"local:outbox", what:"list")` |

**`submit_*` 不需要单独框架** —— 它就是 `verb:"submit"`。任何渠道声明了新动词，
模型立刻能用（这就是「抽象是性质而不是名单」的直接结果）。

## 三层职责

```
渠道插件（可扩展，丢文件即注册）   只回答「怎么把它做掉」/「外面发生了什么」
       │ ChannelDef
       ▼
注册表 + 执行器 + 桥接（本包核心）  裁决「能不能做、要不要问人」；归一幂等与渠道标识
       │ ActOutcome / ObserveOutcome / EmitResult
       ▼
工具层 / 调度内核               act·observe·channels_list；输入事件进 persona
```

## 输入面（渠道 → 调度内核）

输出面是**拉**（模型主动调，有返回值、即调即止）；输入面是**推**（外部世界主动发生、
无同步返回、常驻）。两者方向相反，但**属于同一个渠道**，所以在 `ChannelDef` 上并列声明：

```
verbs?    → act(input, ctx)            写（拉）
observes? → observe(input, ctx)        读（拉）
listens?  → startListen(sink, ctx)     收（推）   ← 本次新增
```

```ts
listens: { messages: { summary: "订阅群消息" } },
defaultKind: "message",
async startListen(sink, ctx) {
  const conn = makeConn({ onEvent: (ev) => {
    void sink.emit({ summary, body, id: `feishu:${msgId}`, refs: { chatId, messageId } })
  }})
  await conn.start()
  cleanups.set(cleanupKey(ctx.home, this), () => conn.stop())
},
async stopListen(ctx) { /* 按同一键取出清理器 */ },
```

**三段职责必须分开**（这是抽象成立的前提）：

| 段 | 归谁 | 混在一起会怎样 |
|---|---|---|
| 「外面发生了什么」 | 渠道（只推 `IngressEvent`） | 每个源重实现幂等与路由，必然分叉 |
| 幂等键 / 渠道标识 / 类别归一 | 桥接（`bridge.ts`，唯一实现） | 同一条消息在不同源里判重规则不同 |
| 路由到哪个人格、进哪个会话 | 调度内核 | 渠道被迫知道人格与会话，插件拿到越权能力 |

桥接层做三件只有一处该做的事（见 `bridge.ts`）：幂等键（显式 `id` 优先 → 内容哈希兜底）、
渠道标识（用注册表给的名字，**渠道伪造无效**）、事件类别（单条 `kind` → 渠道 `defaultKind` → `message`）。

`EmitResult` 五态让渠道看见内核的裁决：`accepted` / `unrouted`（无人订阅，不误吞也不自行处理）/ `duplicate` / `overflow` / `unavailable`。

`PersonaDispatchPort` 刻意**不 import `@gebai/persona`**：channels 是能力层，
不该反向依赖调度内核（依赖方向反了，「内核可独立单测」就不存在）。

## 目录

```
src/
├── channel.ts        ChannelDef 契约（收/发）+ target 解析（`channel:address`）
├── registry.ts       发现：双域扫描 + 目录签名热加载 + 字段校验 + 失败隔离 + 启停名单 + 监听生命周期
├── executor.ts       裁决：渠道/能力/参数/白名单（fail-closed）→ 执行
├── bridge.ts         输入桥接：幂等键/渠道标识/类别归一 → 调度内核
├── channels/local.ts 内置：本机记忆·产物落盘 + 文件投递/收件监听（零凭证，作参照实现）
├── channels/feishu.ts 内置：飞书群收发（**独立机器人**，不复用会话机器人）
└── index.ts          对外导出
```

## 三条硬约束

| # | 约束 | 破坏后果 |
|---|---|---|
| 1 | **声明与实现成对** | 只有一边 = 加载期拒绝；否则表现为「调用永远失败」且错误离配置很远 |
| 2 | **白名单 fail-closed** | 越界**直接拒绝**（不转审批）——发错群是信息泄露，不是「让用户决定」的事 |
| 3 | **`writes` 决定审批** | 声明 `false` 却真的写入 = 绕过审批（实现方责任） |

### 为什么白名单越界不弹审批

审批卡只该出现在「问了有意义」的场合。让用户去点一个「是否允许发给无关群」的按钮，
等于把泄露的决定权交给一次误点。所以：

- **越界 → 执行期直接拒绝**，并且错误信息里列出可用目标（可行动的反馈）
- **越界 → `requiresApproval` 返回 false**（用户不会看到一张注定被拒的卡）

### 为什么未配白名单时 `targets()` 返回 `[]` 而不是 `null`

`null` 的语义是「不做地址级限制」，`[]` 的语义是「什么都不允许」。飞书渠道选后者：
「没配＝不许发」的失败形态是「还没配好」，「没配＝随便发」的失败形态是「把内部信息发错群」。
安全缺省必须选前者。

## 扩展（丢文件即注册）

| 域 | 路径 |
|---|---|
| 内置 | `packages/channels/src/channels/` |
| 二开 | `custom/channels/`（仓库内，同名覆盖内置） |

```ts
export const channel: ChannelDef = {
  name: "my_chan",
  description: "一句话说明",
  envVars: [{ name: "MY_TOKEN", description: "凭据" }],
  verbs: {
    send: { summary: "发文本", writes: true, args: { text: { type: "string", description: "正文", required: true } } },
  },
  observes: { ping: { summary: "探活" } },
  targets: () => ["allowed_addr"],      // 越界直接拒绝
  async act(input, ctx) { return { summary: "已发", ref: "msg_1" } },
  async observe(input, ctx) { return { summary: "可达" } },
}
```

四套机制与子 Agent / 事件源逐条对齐：目录签名热加载 · 单源失败隔离 · 二开覆盖 ·
严格字段校验。完整写法见 [`custom/channels/README.md`](../../custom/channels/README.md)。

## 治理边界

`ChannelContext` 里**没有** engine / store / registry——渠道不需要、也不该有运行任务
或读写会话的能力。裁决全在核心与工具层：

| 裁决项 | 在哪判 |
|---|---|
| 渠道存在 / 已启用 | 执行器 |
| 能力已声明 | 执行器（未声明 → 列出可用能力） |
| 必填参数齐备 | 执行器（缺 → 指出缺哪个） |
| 目标在白名单 | 执行器（越界 → 直接拒绝，fail-closed） |
| 是否需要人批准 | 工具层（按 `writes`） |
| 审批 / 留痕 / 审计 | 引擎 |
| **入站事件归谁** | 桥接（渠道名硬写）+ 调度内核（路由） |

输入面同样不越界：`EventSink` 单独作参数传入（**不是 `ChannelContext` 的字段**），
渠道不能凭空造事件；每个渠道拿到的 sink 已绑定自己的名字；`personaId` 硬写空串
（广播语义），渠道不能指定归属。

## 环境变量

| 变量 | 说明 |
|---|---|
| `GEBAI_CHANNELS_ENABLE` | 启停白名单（非空则仅启用名单内） |
| `GEBAI_CHANNELS_DISABLE` | 启停黑名单（先白后黑） |
| `GEBAI_CHANNELS_FEISHU_APP_ID` / `_APP_SECRET` | 飞书机器人凭据（**本渠道专用，不回落 `GEBAI_FEISHU_*`**） |
| `GEBAI_CHANNELS_FEISHU_CHATS` | 飞书群白名单（逗号分隔；为空则出站不允许任何目标） |
| `GEBAI_CHANNELS_FEISHU_IGNORE_SELF` | 忽略本机器人自己发的消息（默认 on；关掉会自触发循环） |
| `GEBAI_CHANNELS_LOCAL_DROP_DIR` / `_INBOX_DIR` | 本机渠道的投递/收件目录 |

**为什么飞书凭据不回落**：飞书对同一应用只允许一条长连接。会话机器人
（`GEBAI_FEISHU_APP_ID`）负责「人与歌白对话」，本渠道负责「人格对外收发」，
两者应是不同机器人。若误配成同一 AppID，`start()` 直接拒绝启动并说明原因——
否则表现为「其中一边偶发收不到消息」，极难排查。

## 验证

```bash
cd packages/channels && bun test   # 60 用例（3 个文件）
bun x tsc --noEmit
```

用例覆盖的关键行为（都是「改坏了会静默出错」的那类）：

**输出面**

- 白名单越界直接拒绝 + 不弹审批；`targets` 抛错按拒绝处理（fail-safe）
- 声明与实现不成对 → 加载期拒绝
- 失败隔离：单渠道启动抛错不连带其它渠道
- 抽象泛化性：`feishu:send`（外部网络）与 `local:append`（本机文件）走**同一个 act 入口**；
  `verb:submit` / `verb:update` 证明「提交工单」无需新框架
- `local` 渠道并发 append 不丢内容（写串行化）；`get` 拒绝路径穿越

**输入面**

- 端到端：写 JSON 进 drop 目录 → 真 `PersonaRuntime` 的 driver 真跑起来
- 幂等：重投 → `duplicate`（不重复副作用）；无人订阅 → `unrouted`（不误吞也不自行处理）
- 治理：强塞 `channel=feishu` 实际仍为 `local`；`ctx` 只有五字段、无 emit
- 生命周期：`stop()` 后 timer 真停（新文件不再产生事件）；热加载后订阅存活

**飞书独立机器人**

- 只配 `GEBAI_FEISHU_APP_ID` 不配渠道变量 → 渠道不可用（**不复用**会话机器人）
- 渠道 AppID 与会话机器人相同 → 拒绝启动（同应用两条长连接会互踢）
- 未注入 `feishuLongConn` → 如实报错（写/读面仍可用，降级而非全损）
- 自己发的消息忽略（防自触发循环）；非白名单群忽略；非文本消息降级为可读说明
- 缺 `message_id`/`chat_id` → 不投（幂等键与出站目标都靠它）
