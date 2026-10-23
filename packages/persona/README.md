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

```
src/
├── event.ts           PersonaEvent 形态 + threadKey 推导（路由键，不含渠道身份）
├── events.ts          buildEvent / 每源幂等键推导 / renderEventContext（出站纪律注入）
├── dedupe.ts          EventDedupe：有界幂等环（重复事件 = 重复副作用）
├── scheduler.ts       PersonaScheduler：两层队列（会话级串行 + driver 级并发额度）
├── runtime.ts         PersonaRuntime：幂等 → 路由 → 会话解析 → 入队 → 调度 → 交引擎
├── ports.ts           Port 接口（PersonaRunner / SessionResolver / PersonaLookup）
├── source.ts          事件源扩展契约（EventSourceDef / IngressEvent / ctx）
├── discovery.ts       发现：双域扫描 + 目录签名 + 字段校验 + 失败隔离
├── source-manager.ts  EventSourceManager：启停 / 热加载 / emit 桥接 / 诊断
├── sources/file_drop.ts  参考实现（文件投递，也是联调入口）
└── index.ts           对外导出
```

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
cd packages/persona && bun test        # 209 用例
bun x tsc --noEmit                     # 类型检查
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

## 接入服务端（尚未做，属后续阶段）

服务端需实现三个 Port：

```ts
new PersonaRuntime({
  lookup:   /* PersonaLookup：角色与 driver 注册表 + 订阅匹配 */,
  sessions: /* SessionResolver：driver → 常驻会话 */,
  runner:   /* PersonaRunner：engine.run 包装，signal 透传 */,
})
```

**零配置向后兼容**：`lookup.subscribers()` 返回空数组时 `dispatch` 返回 `unrouted`，
调用方走自己原有的路径——未配置任何订阅者时行为与今天逐字节相同。

## 后续阶段（不在本轮范围）

人格定义与严格校验、会话映射与按 kind 分级 GC、审批规则谓词、动作工具族（发送/提交）、
桌面弹窗、霜天框架 4 项增强。

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
