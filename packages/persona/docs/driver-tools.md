# Driver 管理工具（角色自我组织）

三个工具，让角色**在运行时管理自己的分工**：`driver_list` / `driver_create` / `driver_retire`。

## 为什么需要它

静态配置只能覆盖部署方预见到的分工。真实工作总有新情况：

- 「这次专项排查，我单开一个盯 TSDB 的活儿」
- 「周报那个先停了吧，最近没人看」
- 「支持群问题积压了，我加一个专管它的分身」

如果每次都要人改配置文件 + 重启服务，组织的反应速度就被绑在运维节奏上。把增删分工做成工具，让角色自己判断。

## 三个工具

### `driver_list`（只读，永远可用）

先看见再决定。没有它，模型每次需要时都会新建一个 → 重复分工。

输出标出每条分工的**来源**与**原因**：

```
当前共 2 个工作分身：
- alerts｜盯训练平台告警
    关注：schedule｜静态（部署配置）
- tsdb_watch｜盯 TSDB 慢查询
    关注：message｜动态（自己创建）
    原因：专项排查期间需要持续跟进
```

### `driver_create`

| 参数 | 必填 | 说明 |
|---|---|---|
| `id` | ✅ | `[a-z0-9_]+`，创建后不可改、不可复用 |
| `focus` | ✅ | 这个分身负责什么（一句话，进它的提示词） |
| `subscriptions` | ✅ | 关注的事件类型（见下） |
| `reason` | ✅ | **为什么需要这个分工**——审计靠它 |
| `max_concurrent` | — | 1~16，缺省继承角色额度 |

**创建后立即生效**：下一个符合 `subscriptions` 的事件就投给它。
（实现上靠 `onChange` 回调把变更同步给 `runtime.syncDriver`——注册表变了但调度器不知道，
「创建成功」就是一句谎话。）

### `driver_retire`

撤销自己开的分身。**静态配置里的不能撤**——那是部署方的配置，改配置才是对的做法。

## 订阅写法

| 写法 | 含义 |
|---|---|
| `*` | 关注全部 |
| `message` | 该事件类别 |
| `feishu` | 该渠道 |
| `feishu:message` | 两者都要（且关系，顺序无关） |

**不支持前缀通配**（`feishu*` 无效）：一条写错的通配能让分身收到全部事件（噪音淹没）
或全部收不到（静默失效），两者都难排查。

**不能为空**：不关注任何事件的分身永远不会被唤醒——那样它没有存在意义。

## 装配

```ts
import { DriverRegistry, createDriverListTool, createDriverTools } from "@gebai/persona"

const registry = new DriverRegistry({
  statics: personaConfig.drivers.map((d) => ({ personaId: persona.id, def: d })),
  persistence: driverPersistence,   // 动态 driver 落盘（重启不丢）
})
await registry.init()

const tools = [
  ...createDriverTools({
    registry,
    resolveContext: (ctx) => ({
      // 身份由宿主给定，不由模型声明
      personaId: ctx.personaId,
      sessionId: ctx.sessionId,
    }),
    // 关键：变更后同步运行时，否则新 driver 收不到事件
    onChange: (c) =>
      runtime.syncDriver(c.personaId, c.driverId, c.action === "created" ? "online" : "offline"),
    // requireApproval: true,  // 若希望每次创建都审批
  }),
  createDriverListTool({ registry, resolveContext }),
]

for (const t of tools) toolRegistry.register(t)
```

`registry.init()` 必须在启动时调用一次（加载盘上的动态 driver）。

## 三个刻意的设计取舍

### 1. `reason` 必填

不是形式主义。没有它，一个月后看到一堆 driver 无法判断哪些还有用、
哪些是当时临时开的。这是**唯一**能回答"这条分工是怎么来的"的线索。

### 2. 静态 driver 不能运行时删除

手写配置是**权威**。如果工具能删静态项：
- 模型一次误调用就能干掉部署方精心配置的分工
- 下次重启又复活 → 状态来源变得不可推理

### 3. 创建默认**不**审批，但留痕 + 可回滚

如果每次创建都弹审批，「自我组织」就退化成「每步都要问人」，失去意义。

风险由**可见 + 可回滚**承担，而不是由**事前逐次批准**承担：

- 每条创建写盘（`DriverRegistry` 持久化）
- 进日志（`[persona:drivers] 动态创建 …（订阅 …）：<reason>`）
- `driver_list` 随时可查
- `driver_retire` 一条命令撤掉

若你的场景需要审批，构造时传 `requireApproval: true` 即可。

## 边界：模型能做什么、不能做什么

| 能做 | 不能做 |
|---|---|
| 给自己新建分身 | 给**别的角色**建（身份由宿主给定） |
| 撤销自己建的分身 | 撤销静态配置的分身 |
| 查看自己全部分工 | 查看别的角色的分工 |

身份从 `resolveContext(ctx)` 取，**不接受模型在参数里传 `personaId`**——
那等于让模型自己声明"我代表谁"，一次错误就能越权操作别的角色。

## 失败与降级

| 情况 | 行为 |
|---|---|
| 无宿主上下文 | 拒绝 + 明确说明（不静默当成某个角色） |
| 缺 reason / 空订阅 / id 冲突 | 拒绝 + 指明哪个参数错 |
| 持久化写盘失败 | 内存已生效 + 记日志（下次重启以盘为准） |
| 数量超上限（16 个动态） | 拒绝 + 提示先下线不用的 |
