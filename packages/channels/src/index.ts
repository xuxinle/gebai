/**
 * `@gebai/channels` — 对外动作的抽象层（动作与渠道解耦）。
 *
 * ## 为什么需要这一层
 *
 * 按渠道列工具（`send_message` / `create_issue` / `write_memory`…）的分化方式泛化不下去：
 * 每加一个渠道就要加工具、改提示词、影响全部会话；而「发消息」「提工单」「写记忆」
 * 在性质上完全同构（外部副作用、需审批、需留痕、0~N 次），只是目标与动词不同。
 *
 * 所以把一次对外动作拆成三个正交量：
 *
 * ```
 *                     ┌─ 目标 target：`channel:address`（谁）
 *   一次对外动作 = act ┼─ 动词 verb：渠道声明的封闭集合（做什么）
 *                     └─ 载荷 args：由动词 schema 约束（什么内容）
 * ```
 *
 * ## 三层职责
 *
 * ```
 * 渠道插件（可扩展，丢文件即注册）   只回答「怎么把它做掉」
 *        │ ChannelDef
 *        ▼
 * 注册表 + 执行器（本包核心）        裁决「能不能做、要不要问人」
 *        │ ActOutcome / ObserveOutcome
 *        ▼
 * 工具层（server/tools/act.ts）      暴露给模型，接审批与卡片
 * ```
 *
 * ## 扩展方式（与子 Agent / 事件源同款）
 *
 * 在 `packages/channels/src/channels/`（内置）或 `custom/channels/`（二开域）放一个
 * `export const channel: ChannelDef = {…}` 的文件即注册。同名时二开胜出。
 */

export type {
  ActInput,
  ActOutcome,
  ArgSpec,
  CapabilitySpec,
  ChannelContext,
  ChannelDef,
  ChannelLoadError,
  EmitResult,
  EventSink,
  IngressEvent,
  IngressKind,
  IngressRefs,
  ObserveInput,
  ObserveOutcome,
  ParsedTarget,
} from "./channel"
export { parseTarget } from "./channel"

export type {
  ChannelDiscoveryResult,
  ChannelRegistryOptions,
  ChannelScanDirs,
  ChannelState,
} from "./registry"
export {
  ChannelRegistry,
  channelDirSignature,
  defaultChannelScanDirs,
  discoverChannels,
  discoverySignature,
  readChannelFile,
  validateChannelDef,
} from "./registry"

export type {
  ActRequest,
  ExecResult,
  ObserveRequest,
} from "./executor"
export { ActionExecutor } from "./executor"

// ── 输入面（渠道 → 调度内核） ──
export type { ChannelInputBridgeOptions, InputCounters, PersonaDispatchPort } from "./bridge"
export { ChannelInputBridge, eventIdFor, hash32 } from "./bridge"
