# 飞书消息事件源

> **状态：已实现**（`packages/persona/src/sources/feishu.ts`）。启用需在 `GEBAI_EVENT_SOURCES_ENABLE` 里列出 `feishu`。
>
> 与**渠道**（`packages/channels/src/channels/feishu.ts`）的分工：两者不重复，**本源的全部附加价值是
> 「读历史」**——群里有人说「还是不行」时，只有一条正文模型只能猜。
>
> | | 渠道（出站主路径） | 本源（入站增强） |
> |---|---|---|
> | 收消息 | 应用身份长连接 | 应用身份长连接（**复用宿主连接，不自己建**） |
> | 读群历史 | 只能读机器人所在群（且要模型显式 `observe`） | **用户身份回看，自动附在正文尾部** |
> | 出站 | `act(verb:"send"/"reply")` | 不提供 |
> | 配置 | `GEBAI_CHANNELS_FEISHU_*`（独立机器人） | `FEISHU_BOT_*`（回落 `GEBAI_FEISHU_*`） |
>
> 两者**可以同时启用**（一个管发、一个管带着上下文收）。但**同一应用只能有一条长连接**：
> 若拟人源与会话机器人同 AppID，宿主会在启动期拒绝注入并告警（自建 = 互踢 = 两边都偶发收不到消息）。

把飞书消息变成统一 `PersonaEvent`。**收消息用应用身份（协议所限），读上下文与发言优先用用户身份。**

## 身份模型（先读这一节）

飞书两者能力不同，不是设计选择而是协议决定：

| 环节 | 身份 | 说明 |
|---|---|---|
| **接收消息**（长连接 `callback/ws/endpoint`） | **只能应用身份** | 该端点只接受 AppID + AppSecret，无用户 token 入口 |
| **读取消息 / 历史 / 群信息** | **用户身份优先** | 用户能读自己参与的所有群（**含机器人未加入的群**）；应用身份只能读机器人所在群 |
| **规范意义上"发言"** | 用户身份（`requireUser`） | 应用身份发出显示为"机器人"，用户身份显示为你本人 |

**用户身份的价值在「读」这一侧**：机器人不在的群、机器人无权可见的消息，用户身份能读。
这正是"不要直接映射会话"之后仍然需要用户 token 的原因——它是**读世界的权限**，不是路由键。

取令牌顺序：`用户 token（有效→直接用 / 过期→refresh）/ 拿不到 → 应用 token`，
并**如实上报**用了哪种（`fallbackReason` 进日志）。静默降级会让「这个群为什么读不到」永远查不出来。

## 启用

```bash
# 必需（也接受 GEBAI_FEISHU_APP_ID / _APP_SECRET）
FEISHU_BOT_APP_ID=cli_xxx
FEISHU_BOT_APP_SECRET=xxx
# 在事件源名单里启用本源源（不列则仅存在不启动）
GEBAI_EVENT_SOURCES_ENABLE=feishu,file_drop

# 可选
GEBAI_PERSONA_FEISHU_CHATS=oc_a,oc_b # 仅处理这些群（空=全部）
GEBAI_PERSONA_FEISHU_USE_USER=on     # 默认 on（用户身份优先）；off = 一律应用身份
GEBAI_PERSONA_FEISHU_HISTORY=5       # 用户身份回看历史条数（默认 0 = 不回查）
```

**`GEBAI_PERSONA_FEISHU_HISTORY=0`（默认）时本源与渠道的入站能力等价**——那时开它没有意义。
它的价值就在那几个历史条数上。

## 用户 token 从哪来

**复用既有授权链**（与 `feishu_docs` 同一份授权）：会话目录下的 `feishu_user_token.json`。

服务端装配时注入存储即可（`@gebai/agents` 的 `defaultUserTokenStore` 结构上与本包
`UserTokenStore` 兼容，直接传，无需适配器）：

```ts
import { defaultUserTokenStore } from "@gebai/agents"
import { EventSourceManager } from "@gebai/persona"

const manager = new EventSourceManager({
  scanDirs: defaultEventSourceScanDirs({ packageRoot: import.meta.dir }),
  runtime,
  home: config.gebaiHome,
  enable: ["feishu"],
  deps: {
    userTokenStore: defaultUserTokenStore,          // 用户令牌存储
    tokenCtx: { home: config.gebaiHome, user: "admin", sessionId: "persona-runtime" },
  },
})
await manager.start()
```

未注入 `deps` 时：源仍能启动（长连接用应用身份），但**只能读机器人所在群**，
并在启动时明确告警——不静默降级。

### 首次授权怎么做

用户授权是交互式的（浏览器点确认），走既有链路：让 `feishu_docs` 子代理的
`auth_user_authorize` 生成链接 → 用户在浏览器完成授权 → 回调写入
`feishu_user_token.json`。之后本源的 `FeishuAuth` 会自动读取并刷新。

## 事件形态

```ts
{
  kind: "message",
  channel: "feishu",
  source: "飞书群「训练平台支持群」",
  summary: "张三 发来消息",
  body: "帮我看看昨天那个任务为什么失败" + (历史上下文),
  id: "feishu:om_xxx",                    // 幂等键 = 消息 id（重推不重复处理）
  refs: { chatId, messageId, senderId, senderName, extra: { readIdentity } },
}
```

`refs` 三项是出站的关键：`chatId` 决定往哪发，`messageId` 支持引用回复，
`senderId` 用于审批授权校验（"仅任务发起者可批准"）。

## 消息过滤

| 规则 | 原因 |
|---|---|
| `chats` 白名单（配置时生效） | 只处理关心的群，避免全量噪音进人格 |
| `senderType !== "user"` 忽略 | **防自触发循环**：机器人自己发的消息若被当输入，会无限自我对话 |
| 空正文忽略 | 撤回/系统消息不投递 |
| 非文本消息降级为说明 | 图片/文件不丢（给出 `message_id` 供后续取资源），而不是静默消失 |

## 协议要点（为什么这么写）

1. **本源不自己建连**：同一飞书应用只允许一条长连接。本源经 `ctx.deps.feishuLongConn`
   拿宿主已有的连接注册面；自建 = 与应用互踢 = 两边都偶发收不到消息。
   宿主侧的撞车判定见 `packages/server/src/core/session/persona-feishu-deps.ts`。
2. **单条消息处理失败不断流**：投递抛错只记日志，连接继续（否则一条坏消息会让整个源停摆）。
3. **读历史失败不阻断本条投递**：上下文是附加价值，拿不到就该没有，不能因此丢消息。
4. **幂等键 = `feishu:{messageId}`**：飞书会重投同一消息，重复事件 = 重复副作用。

## 诊断

```ts
manager.state()   // stats.unattended 持续涨 = channel 没被任何人格认领
manager.loadErrors()
```

日志里关注四类：
- `回退到应用身份读历史：<原因>` — 用户令牌有问题（未授权/刷新失败）；**会写进正文让模型知道**
- `未注入 deps.feishuLongConn` — 装配漏了，或与会话机器人同应用被拒绝
- `GEBAI_PERSONA_FEISHU_HISTORY=N 但宿主未注入 deps.feishuHistory` — 想要上下文但没给能力
- `未被接管（unattended）` — 人格配置里没有认领 `feishu` 这个 channel

## 已知边界

- **只处理文本（含富文本）消息**：图片/文件/语音降级为可读占位（不做 OCR/附件下载）。
- **不做卡片回调**：人格模式的审批走交互通道（引擎推送），不依赖飞书卡片按钮。
- **不自动回复**：出站一律是模型显式调用动作工具（架构纪律，避免第二条绕过治理的通道）。
- **不自己建连**（见上）：本源只提供「入站增强」，不提供出站。
