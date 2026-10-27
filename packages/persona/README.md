# @gebai/persona — 拟人运行模式

**统一事件驱动内核。** 本包是「拟人是运行模式」这一判断的实现载体：它不是叠在某个渠道上的
模块，而是把**所有驱动源**收敛成同一种事件，再交给引擎按「收集 → 分析 → 输出」跑完一轮。

## 定位与边界

```
   ① 驱动             ② 收集              ③ 分析              ④ 输出
定时 / 消息 /      只读工具            模型推理            动作工具
webhook / 桌面     （无副作用）        （无外部动作）      （0~N 次）
──────────────────────────── 本包负责 ① ────────────────────────────
                              ②③④ 交给引擎
```

**本包只做调度与路由**：决定「这事件归谁、进哪个会话、什么时候跑」。

**刻意不做**（否则会变成第二条能绕过治理的通道）：
- 不组装系统提示词（服务端经 `PersonaIdentity` 注入）
- 不推送任何消息（出站一律是模型在会话里的工具调用）
- 不碰审批、沙箱、工具可见性（一律留在引擎）

## 为什么独立成包

1. **事件驱动是可脱离引擎验证的纯逻辑**——独立包才能用内存 Fake 全量覆盖，不必起服务端。
2. **依赖方向单向**：`server → persona`。若塞进 `server/src/core/`，会不断挑战该目录的分层禁令
   （core 内部不得反向依赖 tools/engine barrel），而这是个横切模块。

零运行时依赖（仅 bun 标准能力），不认识 `AgentEngine` / `SessionStore` / `EventBus`——它们经
`ports.ts` 的接口注入。

## 目录

`src/` 内核（零依赖，不认识引擎）：

```
event.ts           PersonaEvent 形态 + threadKey 推导（路由键，不含渠道身份）
events.ts          buildEvent / 每源幂等键推导 / renderEventContext（出站纪律注入）
dedupe.ts          EventDedupe：有界幂等环（重复事件 = 重复副作用）
scheduler.ts       PersonaScheduler：两层队列（会话级串行 + driver 级并发额度）
runtime.ts         PersonaRuntime：幂等 → 路由 → 会话解析 → 入队 → 调度 → 交引擎
ports.ts           Port 接口（PersonaRunner / SessionResolver / PersonaLookup）
source.ts          事件源扩展契约（EventSourceDef / IngressEvent / ctx）
discovery.ts       发现：双域扫描 + 目录签名 + 字段校验 + 失败隔离
source-manager.ts  EventSourceManager：启停 / 热加载 / emit 桥接 / 诊断
persona.ts         人格定义发现与严格校验
catalog.ts         PersonaCatalog：静态定义 + 动态 driver 合流（含 replace 供热加载）
driver.ts          DriverRegistry：静态 + 模型运行时自建的分身（落盘）
worker.ts          driver/worker 模型的契约（WorkerSpawner / workerReportEvent）
mailbox.ts         插话信箱（运行中到达的事件） 
render.ts          多事件合批渲染
credentials.ts / oauth.ts / user-token.ts   飞书进程级用户凭据
```

扩展域（丢文件即注册）：

```
├─ 内置
│  ├─ packages/persona/src/sources/         # 随包分发（file_drop 参考实现 + feishu）
│  └─ packages/persona/src/personas/        # 内置人格（xiaoshuang 示例）
└─ 二开（recommended）
   └─ custom/persona/                       # 两类定义混放，靠导出名区分
      ├─ my_source.ts   → export const source: EventSourceDef
      └─ my_persona.ts  → export const persona: PersonaDef
```

> 混放是**刻意的**：两者都是「拟人模式的二开物」，分两个目录只会让人记「人格放 A、源放 B」。
> 两套发现逻辑各自只看自己那个导出（`source` / `persona`）：看到对方导出就**安静跳过**，
> 不报「未导出 xxx」。不这样做的话，两边的 `loadErrors` 会互相刷噪声，把真正的错误淹没。

## 三条核心不变量（都有反向验证覆盖）

| # | 不变量 | 破坏后果 |
|---|---|---|
| 1 | **同一会话同时最多一个任务** | 引擎会抛 `已有任务在运行`；消息交错落盘、TaskState 互相覆盖 |
| 2 | **同一 driver 同时运行的会话数 ≤ `maxConcurrent`** | 资源失控（token / API 限流 / CPU） |
| 3 | **FIFO 且不丢**（人的话不能被静默丢弃） | 先到的会话被新会话无限饿死 |

第 3 条的排序键是**单调递增序号**（`QueueItem.seq`），不是时间戳——`Date.now()` 毫秒精度下
同毫秒入队会相等，比较退化为 Map 迭代序，导致「先到的会话跑起来后被追入新事件」时插队。
这个缺陷是反向验证抓出来的（见下）。

## 两层队列：为什么要两层

- **会话级必须串行**（引擎硬约束）。
- **driver 级才有并发**：一个 driver 卡在审批，不该让同角色的其它 driver 停摆。

若只有「每角色一条串行队列」，角色就退化成单线程——这是早期方案犯过的错，靠结构杜绝。

### 额度的真实作用面（容易配错的地方）

调度器是 **per-driver** 的，而一个 driver 只对应**一个常驻会话**（`SessionResolver.resolveDriver`
幂等），因此：

| 配置 | 实际效果 |
|---|---|
| `driver.maxConcurrent > 1` | **不生效**：该 driver 的全部事件都路由到同一会话，会话级串行先把它锁死 |
| `PersonaLookup.maxConcurrent(personaId)` | 只是 driver 未单独配置时的**继承默认值**，**不是**该角色的并发上限 |

真实的并发度 = **同时有事的 driver 个数**（每个各 1 个会话）。要给某个角色做总额限制，
得在宿主侧（或新增跨 driver 闸门）实现，不能靠这两个字段。

## 幂等：每源必有一条

| 来源 | 幂等键 |
|---|---|
| `message` | 渠道消息 id（飞书 `om_xxx`）；缺失时用 `来源+正文哈希` |
| `schedule` | `task:{taskId}@{触发时刻}` |
| `webhook` | 投递方 id；缺失时用内容哈希 |
| `desktop` / `idle` | 调用方显式给 |

各源都会重投（飞书 ACK 前断连会重推、调度器重启会重新触发、webhook 按自身策略重试），
重复事件 = 重复副作用（重复发消息、重复提交单据），是真实事故形态而非理论风险。

## 出站纪律（`renderEventContext`）

取消「任务结束时自动回推」后，最大失败模式是**人格沉默**——它想清楚了、写进了会话记录、
但没调动作工具，用户什么也没收到。所以入站上下文文本明确声明：

> 你的回复不会被自动发送。要让别人看到，必须调用发消息工具显式指定目标；
> 不需要回复时（判断无需处理）直接结束即可，这是正常结果。

## 扩展事件源（丢文件即注册）

与子 Agent 同款：放一个 `export const source: EventSourceDef` 的文件即注册，**不改核心代码**。

| 域 | 路径 |
|---|---|
| 内置 | `packages/persona/src/sources/` |
| 二开 | `custom/persona/`（仓库内，同名覆盖内置） |

**边界**：插件只回答「发生了什么」（`IngressEvent`）；「归谁、进哪个会话、何时跑」由核心裁决。
插件永远拿不到绕过治理的能力。

完整规范见 [`docs/event-sources.md`](./docs/event-sources.md)，参考实现见
[`src/sources/file_drop.ts`](./src/sources/file_drop.ts)。

四套机制与子 Agent 逐条对齐：目录签名热加载 · 单源失败隔离 · 二开覆盖 · 严格字段校验。

## 验证

```bash
cd packages/persona && bun test        # 245 用例
bun x tsc --noEmit                     # 类型检查
```

服务端侧的装配验证（身份边界 / 运行态快照 / 路由契约）：

```bash
cd packages/server && bun test src/core/session/persona-driver-tools.test.ts \
                                 src/core/session/persona-status.test.ts \
                                 src/routes/persona.test.ts
```

**反向验证（改坏实现看测试是否杀掉）** —— 核心层 12 个变异体 / 源扩展层 15 个：

| 变异 | 结果 |
|---|---|
| 会话串行失效 / 并发额度失效 / 在跑去重失效 | ✓ 杀 |
| 位次不计在跑 / FIFO 反序 / FIFO 退化为时间戳 | ✓ 杀 |
| FIFO 排序键改为事件时间（迟到重推的旧消息插队） | ✓ 杀 |
| FIFO 排序键改为入队毫秒（系统时钟回拨后插队） | ✓ 杀 |
| 取消不清队列 / stop 不中止在跑 / 队列上限失效 | ✓ 杀 |
| 淘汰函数空操作 / launch 不递归 pump | ✓ 杀 |
| 淘汰时不删空会话槽 | 等价存活（`pickNext` 会清理，行为不变） |

其中 **FIFO 退化为时间戳** 与 **淘汰只标记不清理** 两个变异最初存活，分别暴露了真实缺陷
（跨会话插队）与死代码（不可达分支），已修。这类「测试绿但没守住」的情形是反向验证的唯一
判据来源。

### 取消接口的入参链（改这里时注意）

`PersonaScheduler` 的表按 **sessionId** 建（`Map<sessionId, AbortController>`），而对外接口按
`(personaId, driverId)` 说话。`PersonaRuntime` 内部维护 `driverKey → sessionId` 映射把两者
接起来——少了它，`cancelRunning` / `cancelWaiting` 永远查不到东西（形同死代码，
表现为「人格卡在长任务上无法取消」）。两个接口都有专项用例看 `signal` 是否真的 abort。

### 幂等记账的时机（改这里时注意）

投递链路是「先 `dedupe.admit` 记账，后入队」，入队可能失败（存储瞬时不可达 / 队列已满）。
**失败必须 `revoke` 撤回**：否则该事件 id 被永久占位，源按规范重投会被判 `duplicate`
静默丢弃——违反「FIFO 且不丢」。

### 两条路径都有界（插话信箱与待跑队列）

插话信箱默认容量 = `queueLimit`（64）。**满了之后新插话降级到队列**（而不是丢弃）：
插话是尽力而为的高优先路径，可靠投递仍由队列保证。`maxRounds` 只封顶轮次数，
不封顶每轮吸收多少条——所以信箱本身必须有界，否则事件洪峰会一次性拼成超大输入。

## 接入服务端

服务端实现三个 Port（`packages/server/src/boot/compose.ts`）：

```ts
new PersonaRuntime({
  lookup:   PersonaCatalog      // 静态定义 + DriverRegistry（模型自建分身）
  sessions: PersonaSessionResolver  // (人格, 分身) → 常驻会话
  runner:   EnginePersonaRunner // 事件 → prompt → engine.run（engine 零改动：身份走 channelNote）
})
```

**零配置向后兼容**：`lookup.subscribers()` 返回空数组时 `dispatch` 返回 `unrouted`，
调用方走自己原有的路径——未配置任何订阅者时行为与今天逐字节相同。

### 启用后同时装配的五面

只接渠道是不够的——「人格只对聊天有反应，定时/webhook 永远不触发」就是把驱动面漏了：

| 面 | 组件 | 服务端位置 |
|---|---|---|
| 入站（消息） | `ChannelInputBridge` → 渠道长连接 | `boot/compose.ts`（`channels.attachDispatcher`） |
| 入站（驱动） | `EventSourceManager` | `boot/compose.ts`（`channels.init()` 之后启动，失败各自隔离） |
| 自我组织 | `driver_list/create/retire` | `core/session/persona-driver-tools.ts`（身份从 `SessionData.persona` 读，不按模型自报） |
| **派活** | `worker_dispatch/list/cancel` | `core/session/worker-{spawner,tools}.ts`（独立会话，跑完即删，结论走事件） |
| 运行态 | `collectPersonaState` | `core/session/persona-status.ts` → `GET /api/v1/persona/state` → 设置→拟人面板 |

### 派活（driver + worker）：一个人格同时办多件事

```
事件 ──▶ driver 会话（串行，可被新事件插入）
              │  worker_dispatch（异步，立刻返回 runId）
              ├──▶ worker A（独立会话，隔离上下文）──┐
              └──▶ worker B ───────────────────────┤
                                                    ▼
                      workerReportEvent（定向回到派它的那个 driver）
```

**为何 worker 不是 `subsession_run`**（两者都能隔离上下文，差别在「谁拥有它」）：

| | subsession | worker |
|---|---|---|
| 归属 | 挂在某个父任务的 `ToolContext` 里，随 run 生命周期 | 独立会话，不依附于 driver 这一轮 |
| 父轮结束 | 连带取消 | **继续干完**（它本该如此） |
| 退出后 | 存档留在父会话 | 会话删除（结论已进 driver 事件） |

四条实作约定（都是真实事故形态，见 `worker.test.ts`）：

1. **结论必带 `verify`**：没有验收标准的活回来是「我看了一下，大概是……」——工具直接拒绝。
2. **跑完无文本结论 → `failed`**，不能当 `done`：否则 driver 会以为「查完了，没发现问题」而漏报。
3. **异步派活的 runId 由工具预分配**：否则结论回来前 `worker_list`/`worker_cancel` 找不到它。
4. **叫停后不改回 done**：模型已经决定不要这个结果了，反过来标成功就是谎报。

### 热加载：人格定义与事件源都改完即生效

两者**各自节流 5s**（目录不同，合并节流会让其中一个等另一个），挂在 runner 每轮之前——
不放定时器：空闲进程不该白跑目录遍历。

人格定义热加载必须同时做三件（缺一件就是「改了好像没生效」）：

1. `catalog.replace()` 整体替换——下一次读身份/订阅就是新的
2. **新增 driver → `syncDriver(online)`**，删掉的 → `offline`（不调则新分身从不响应）
3. 订阅/并发改过的 → `syncConcurrency`（调度器要知道新额度）

**重扫结果为空时不清空现有的人格**（目录暂不可读 ≠ 人格被删）：把全部人格下线比“配置暂不可读”严重得多。

### 两个宿主侧的坑（本包已按此实现，改装配时别拆）

1. **`onChange` 必须调 `runtime.syncDriver`**：不调则新建的分身只在盘上/注册表里，
   调度器没条目——工具回「已创建」而那个分身**从不响应**（现象极像订阅配错）。
2. **身份不能由模型传**：`driver_*` 工具的 `personaId` 由宿主从**会话归属**算出后注入
   （`personaContextResolver`），模型传什么都被忽略；写成 `resolveContext: () => null`
   会让所有人格会话里这三个工具都恒返回「当前通道不支持」。

## 后续阶段（不在本轮范围）

会话按 kind 分级 GC（worker 会话已跑完即删，无需 GC）、审批规则谓词、桌面弹窗源。

## 源扩展层的反向验证（15 变异体 / 14 杀）

| 变异 | 结果 |
|---|---|
| 加载期失败不隔离 · 启动期失败不隔离 | 杀 |
| custom 不覆盖内置 · discover 覆盖显式注册 | 杀 |
| channel / name 校验失效 · 未导出 source 静默跳过 | 杀 |
| 签名不含 mtime（热加载永不触发）· refresh 不比对签名 | 杀 |
| env 白名单失效 · 启停名单失效 · register 不校验 | 杀 |
| unattended 误映射为 accepted · 已处理文件不删除 | 杀 |
| 先处理后登记（并发重复） | 等价存活（当前 poll 串行） |

其中 **discover 覆盖显式注册** 与 **签名不含 mtime** 两个变异最初存活，暴露了真实缺陷
（`register()` 的源在首次 `start()` 时被扫描结果整体替换掉；改文件内容不触发热加载），已修。

## 踩过的坑（写新源时务必看）

1. **状态不能放模块级**：多实例（热加载新旧并存、测试并行、多租户）会互相清空
   `handled`/`timer`，表现为「事件偶发丢失」，而单实例手动验证完全正常。全部放 `start` 闭包。
2. **变量遮蔽**：`const st = await stat(full)` 遮蔽了实例状态 `st`，导致 `st.handled`
   报 undefined。字符串替换式改写特别容易引入这类错误——改完必须跑类型检查。
3. **测试别依赖真实时序**：文件轮询（1.5s）只用于端到端那一条；桥接语义用注入 fake 源
   精确断言，否则失败时分不清「逻辑错」还是「没等到」。

## 已定稿：投递目录双消费——默认目录分离（2026-10-27 定，方案 A）

**背景**（2026-10-27 实测）：`file_drop` 事件源与内置 `local` 渠道的 `drop` 监听**原默认共用**
（`{GEBAI_HOME}/persona-drop/`），两者都用 1.5s 轮询、都「处理完即删文件」。同一批文件被谁抢到是竞态：

| 被谁抢到 | 后果 |
|---|---|
| `file_drop` 源 | 尊重事件里的 `kind`/`channel` → 按订阅正确路由 |
| `local` 渠道 drop 监听 | 归类为渠道消息（`kind` 取渠道 `defaultKind`）→ **路由到别的 driver** |

实测（8 条 `kind:webhook` 同批投递）：`alerts`（webhook 订阅）只收到 1 条，另 7 条被 `local` 渠道抢走，
按 message 类落到 `support`。**这不会报错、计数也全绿**，只表现为「订阅好像没按配置生效」。

**第二次实测（2026-10-27，动态分身）**：投一条 `kind:idle, channel:probetick` 给动态分身 `probe_echo`（订阅 `probetick`），
结果文件同样被 `local` 渠道抢走 → 按渠道 message 归类 → 无人订阅 `probetick:message` → `unrouted`，
目标 driver **永远收不到**（`probe_echo.accepted=0`，会话都未创建）。这比「路由错位」更隐蔽：
**订阅正确的 driver 直接饿死，且不报错**。

**定稿决定（用户拍板：不用兼容，最优化改）**：

- **默认目录分离**（方案 A）：`local` 渠道的 drop 监听改为 `{GEBAI_HOME}/channel-drop/`，
  `file_drop` 事件源保持 `{GEBAI_HOME}/persona-drop/`。两个入口各自独立，channel/kind 路由可靠。
- 改动：`packages/channels/src/channels/local.ts`（envVars 描述/`startListen`/注释）+
  `packages/channels/src/input.test.ts`（测试目录）+
  `packages/persona/src/sources/file_drop.ts`（注释补目录归属说明）。
- **不做兼容迁移**：既有部署若在老的 `persona-drop/` 里放文件给渠道用，需改用 `channel-drop/`。
- 已有 `GEBAI_CHANNELS_LOCAL_DROP_DIR` / `GEBAI_PERSONA_FILEDROP_DIR` 显式配置不受影响（优先于默认值）。
