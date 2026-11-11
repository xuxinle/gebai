# 飞书应用与长连接：一个应用只能干一件事

> 这张表的用途：**在配置飞书之前看它**。拟人模式与飞书渠道的绝大多数「配了但不工作」都源于
> 同一个原因——把同一个飞书应用配给了两个都要建长连接的角色，飞书会把先连的踢掉，
> 表现为「其中一边偶发收不到消息」，日志里只有零星重连，看不出根因。
>
> 本文只讲**约束与对应关系**，不教怎么开人格（那是 `.env.example` 与
> `packages/persona/README.md` 的事）。

## 硬约束（协议决定，非配置项）

**同一个飞书 AppID，只允许一条长连接。** 第二条连上去，第一条被踢。
所以本仓库里凡是要"收消息"的角色，都**必须各用各的应用**。

## 渠道分输入 / 输出两类

渠道实例按**方向**分两组声明，组内再按类型分：

| 变量 | 方向 | 能力 | 建长连接？ |
|---|---|---|---|
| `GEBAI_CHANNEL_IN_<类型>` | 输入（收） | 只有 `startListen` | ✅ 建（每实例一条） |
| `GEBAI_CHANNEL_OUT_<类型>` | 输出（发/读） | 只有 `act` / `observe` | ❌ **不建** |

**为何这样分**：输入和输出是两件独立的事。分开后可以“收 A 群、发 B 群”，
甚至“用甲机器人收、乙机器人发”。而 **OUT 不占长连接**，所以
输出实例可以与输入实例 / 会话机器人**共用同一个飞书应用**（同一应用只能一条长连接，不建连就不冲突）。

实例的能力面按方向**裁剪**（`materializeInstance`）：一个纯输出实例不会挂着
`listens` 声明——否则模型会以为它能收，而它永远收不到（`startListen` 根本不会被调用）。

## 三个角色，各用哪个账号

| 角色 | 读哪个配置 | 建长连接？ | 撞车会怎样 |
|---|---|---|---|
| **会话机器人**（人 ↔ 歌白的对话桥接）<br>`GEBAI_FEISHU_BOT_ENABLED=true` 启用 | `GEBAI_FEISHU_APP_ID` / `_APP_SECRET` | ✅ 建 | 被后来者踢，表现「飞书里回话断断续续」 |
| **飞书渠道·输入** | `GEBAI_CHANNEL_IN_FEISHU[]` 里**每个实例**的 `appId` / `appSecret` | ✅ 每实例建一条 | **启动即失败**（`channels/feishu.ts` 显式抛错），该实例收不到消息 |
| **飞书渠道·输出** | `GEBAI_CHANNEL_OUT_FEISHU[]` 里每个实例的凭据 | ❌ **不建** | 不会撞车（不占连接），可用与输入/机器人相同的应用 |
| **拟人 OAuth 凭据**（以用户本人名义发言） | 同某个渠道实例的 `appId`（回落 `GEBAI_FEISHU_APP_ID`） | ❌ 不建（只用应用凭证换 token） | 无撞车风险，只需应用凭证有效 |

> **已删除：飞书事件源（`feishu` 源）。**
>
> 入站链路就是 **渠道 → 驱动 → 会话**：渠道每个实例自带账号、自带群白名单，
> 还能用 `observe(who:"messages")` 读群历史——它已经是完整的事件来源。
> 旧的事件源是个全局单账号的孪生副本（没有实例、能力更弱），留着只会多一个配置面不一致的坑。
> 剩下的事件源只有渠道替代不了的两个：`file_drop`（联调）与 `schedule`（人格定时）。
>
> 注：本表描述会话机器人的**行为**，不涉及修改它（`packages/server/src/feishu-bot/**` 与
> `GEBAI_FEISHU_BOT_*` 的语义此处不改）。

**两两互检**只剩一处：

| 检查对 | 判定位置 |
|---|---|
| 渠道 ↔ 会话机器人 | `packages/channels/src/channels/feishu.ts`（`start()` 内，同 AppID 且机器人启用 → 抛错） |

> 「会话机器人是否已启用」很关键：机器人**关着**时它根本不建连接，
> 与它同 AppID 并不冲突（残留的 `GEBAI_FEISHU_APP_ID` 不该被当成冲突源误拒）。

### 一句话结论

**要建长连接的角色只有两个**（会话机器人、渠道的**输入**实例），任两个都不能用同一应用。
两个输入实例之间也一样（`feishu_in_a` / `feishu_in_b` 必须不同应用）。
**输出实例不建连接，因此不参与撞车**——它可以与任何一方共用同一应用。

## 应该怎么配

### 形态 A：一个应用（最省事）

一个应用既当会话机器人、又当渠道的输入与输出（输出不建连，所以不冲突）：

```dotenv
GEBAI_FEISHU_APP_ID=cli_xxx
GEBAI_FEISHU_APP_SECRET=xxx
GEBAI_FEISHU_BOT_ENABLED=true

# 渠道：输入与输出用同一个应用（合法）
GEBAI_CHANNEL_IN_FEISHU =[{"name":"feishu_in", "appId":"cli_xxx","appSecret":"xxx","chats":["oc_x"]}]
GEBAI_CHANNEL_OUT_FEISHU=[{"name":"feishu_out","appId":"cli_xxx","appSecret":"xxx","chats":["oc_x"]}]
```

⚠️ 但 `GEBAI_FEISHU_BOT_ENABLED=true` 时，**输入**实例用同一应用会被拒（两条长连接互踢）。
所以本形态要么关机器人（`GEBAI_FEISHU_BOT_ENABLED=false`）、要么让输入实例换个应用。

### 形态 B：两个应用（推荐）

```dotenv
# 应用 1：对话桥接
GEBAI_FEISHU_APP_ID=cli_aaa
GEBAI_FEISHU_APP_SECRET=aaa
GEBAI_FEISHU_BOT_ENABLED=true

# 应用 2：渠道（收 + 发 + 读历史）
GEBAI_CHANNEL_IN_FEISHU =[{"name":"feishu_in", "appId":"cli_bbb","appSecret":"bbb","chats":["oc_x"]}]
GEBAI_CHANNEL_OUT_FEISHU=[{"name":"feishu_out","appId":"cli_bbb","appSecret":"bbb","chats":["oc_x"]}]
```

### 形态 C：输入输出用不同机器人

真正解耦：用 A 机器人收、B 机器人发（B 不建连接，所以 B 甚至可以是会话机器人那个应用）：

```dotenv
GEBAI_CHANNEL_IN_FEISHU =[{"name":"feishu_in", "appId":"cli_aaa","appSecret":"aaa","chats":["oc_ask"]}]
GEBAI_CHANNEL_OUT_FEISHU=[{"name":"feishu_out","appId":"cli_bbb","appSecret":"bbb","chats":["oc_duty"]}]
```

再多一个入站群就再加一个**输入**实例，但每个输入实例必须自己的应用：

```dotenv
GEBAI_CHANNEL_IN_FEISHU=[
  {"name":"feishu_in_main","appId":"cli_aaa","appSecret":"aaa","chats":["oc_ask"]},
  {"name":"feishu_in_ops", "appId":"cli_ccc","appSecret":"ccc","chats":["oc_ops"]}
]
```

人格侧引用实例名：`listen:{"channels":["feishu_in_main"]}`、`send:{"channels":["feishu_out"]}`。

## 当前这台机器的状态（2026-11 实测，已修好）

| 角色 | 应用 | 状态 |
|---|---|---|
| 会话机器人 | `cli_aaf2…` | ✅ 长连接正常 |
| 渠道 `feishu_in`（输入） | `cli_aa7e…` | ✅ 已启动、长连接已建立 |
| 渠道 `feishu_out`（输出） | `cli_aa7e…`（同一应用） | ✅ 可用（不建连接，不占用） |

> 历史：改造前四个变量名指向同一个应用（`cli_aaf2…`），渠道因为“与会话机器人同 AppID”
> 被撞车检查拦下、整体不可用；而当时拟人也是关的，于是“人格不说话”在界面上完全无提示。
> 现在渠道状态可通过 `GET /api/v1/channels/state` 单独查看，不再依赖拟人是否启用。

## 相关文件

| 文件 | 职责 |
|---|---|
| `packages/channels/src/channels/feishu.ts` | 渠道三面（写/读/收，含 `observe messages` 读群历史）+ 与会话机器人的撞车判定 |
| `packages/persona/src/{credentials,oauth,user-token}.ts` | 拟人的用户令牌：授权、刷新、落盘恢复 |
| `packages/server/src/feishu-bot/**` | 会话机器人（本文不改它，只说明它会撞谁） |
