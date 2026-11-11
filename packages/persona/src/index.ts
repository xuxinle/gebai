/**
 * 拟人运行模式（`@gebai/persona`）：统一事件驱动内核 + 通用事件源扩展规范。
 *
 * 定位：**运行模式**的实现载体，不是「引擎的一个分支」。本包零依赖（仅 bun 标准能力），
 * 不认识 `AgentEngine` / `SessionStore` / `EventBus`——它们经 `ports.ts` 的接口注入。
 *
 * 三层结构：
 * ```
 * 事件源插件（可扩展，丢文件即注册）
 *      │ IngressEvent（只回答「发生了什么」）
 *      ▼
 * 核心：幂等 → 路由 → 会话 → 两层队列 → 调引擎
 *      │ PersonaEvent（已裁决归属与线程）
 *      ▼
 * 引擎（收集 → 分析 → 输出，出站靠显式工具调用）
 * ```
 *
 * 扩展方式（与子 Agent 同款）：在 `packages/persona/src/sources/`（内置）或
 * `custom/persona/`（二开域）放一个 `export const source: EventSourceDef` 的文件即注册。
 * 详见 `docs/event-sources.md` 与 `src/sources/schedule.ts` 参考实现。
 */

// ── 事件模型 ──
export type {
  PersonaAttachment,
  PersonaDispatchResult,
  PersonaDispatchStatus,
  PersonaEvent,
  PersonaEventContext,
  PersonaEventKind,
  PersonaEventRefs,
} from "./event"
export { deriveThreadKey } from "./event"

export type { BuildEventInput } from "./events"
export { buildEvent, eventIdFor, hash32, renderEventContext } from "./events"

// ── 定时表达式与人格定时工作面 ──
export type { TaskSchedule } from "./cron"
export { isOneShotSchedule, parseSchedule } from "./cron"
export type { DueEvent, DueResult, ScheduleDeclaration, ScheduleOwnerView } from "./schedules"
export {
  DEFAULT_CATCHUP_MS,
  MAX_SLOTS_PER_TICK,
  collectDue,
  collectScheduleDeclarations,
  nextSlot,
  scheduleEventId,
  scheduleKey,
  validateDriverSchedule,
} from "./schedules"

// ── 定时任务启停控制面（界面开关 + 手动运行）──
export type {
  ScheduleAction,
  ScheduleControlEntry,
  ScheduleControlPersistence,
  ScheduleControlResult,
  ScheduleControlState,
} from "./schedule-control"
export {
  MemoryScheduleControlPersistence,
  ScheduleControl,
  manualRunEventId,
  parseScheduleAction,
} from "./schedule-control"

// ── 幂等与调度 ──
export { EventDedupe } from "./dedupe"
export type { PersonaSchedulerOptions, PersonaSchedulerState, RoundInput, SessionQueueState, InsertStrategy } from "./scheduler"
export { PersonaScheduler } from "./scheduler"

// ── 订阅广播（事件层核心语义） ──
export type { EventCoords, Subscription } from "./subscription"
export { coordsOf, matchesAny, matchesSubscription, validateSubscription } from "./subscription"

// ── 整编与插入（多事件成会话） ──
export type { PersonaMailbox, MailboxWriter } from "./mailbox"
export { InterjectionMailbox } from "./mailbox"
export type { RenderOptions } from "./render"
export { renderBatch, replyHint, summarizeBatch } from "./render"

// ── 大脑/手：driver 注册表、管理工具与 worker ──
export type {
  CreateDriverInput,
  DriverOrigin,
  DriverPersistence,
  DriverRecord,
  DriverRegistryOptions,
} from "./driver"
export { DriverRegistry, validateCreate } from "./driver"
export type { CreateDriverToolsOptions, DriverToolContext, PersonaTool } from "./driver-tools"
export { createDriverListTool, createDriverTools } from "./driver-tools"
export type { WorkerOutcome, WorkerSpawnInput, WorkerSpawner } from "./worker"
export { workerReportEvent } from "./worker"

// ── 运行时与端口 ──
export type {
  DriverDef,
  DriverSchedule,
  OutputTarget,
  PersonaActor,
  PersonaDelivery,
  PersonaDeliveryStatus,
  PersonaIdentity,
  PersonaLogger,
  PersonaLookup,
  PersonaRunInput,
  PersonaRunner,
  ResolvedSession,
  SessionResolver,
} from "./ports"
export type { DriverRuntimeState, PersonaRuntimeOptions } from "./runtime"
export { driverKey, PersonaRuntime } from "./runtime"

// ── 事件源扩展规范 ──
export type {
  EmitResult,
  EventSourceContext,
  EventSourceDef,
  EventSourceLoadError,
  IngressEvent,
} from "./source"
export type { DiscoveryResult, EventSourceScanDirs } from "./discovery"
export {
  defaultEventSourceScanDirs,
  discoverEventSources,
  discoverySignature,
  readSourceFile,
  sourceDirSignature,
  validateSourceDef,
} from "./discovery"
export type { EventSourceManagerOptions, EventSourceState } from "./source-manager"
export { EventSourceManager } from "./source-manager"

// ── 会话映射（可配置规则表：按话题/群/发言者/工单切上下文）──
export type { MissingPolicy, SessionMapContext, SessionMapRule } from "./session-map"
export {
  KNOWN_PLACEHOLDERS,
  SESSION_MAP_PRESETS,
  isKnownPlaceholder,
  matchSessionMap,
  parseSessionMap,
  placeholdersOf,
  renderSessionKey,
  validateSessionMap,
} from "./session-map"

// ── 身份模板（角色身份由部署环境的白名单变量提供）──
export { ACTOR_FIELDS_JSON_KEY, actorFieldsFromEnv, renderTemplate, resolveActorFields, templateVars, validateTemplate } from "./identity"

// ── 发送者判定（防回灌 + 是否回复用户本人；两条入站路径共用同一口径）──
export type { SpeakerInput, SpeakerKind, SpeakerVerdict } from "./speaker-filter"
export { classifySpeaker, replyToOwnerFromEnv, speakerNote } from "./speaker-filter"

// ── 自动学习（从人话里识别值得进知识库的东西；纯函数，可穷举单测）──
export type { LearnedCandidate } from "./memory-learn"
export { detectConsensus, detectCorrection, detectProcedure, shouldLearn } from "./memory-learn"

// ── 部署配置面（启用名单/入站收窄/输出目标/审批工具名单/会话路由）──
export type { EnvView, PersonaEnvReport } from "./env-config"
export {
  applyPersonaEnv,
  chatSessionKeyOf,
  narrowSubscriptions,
  parseOutputTargets,
  sessionRoutingMode,
} from "./env-config"

// ── 行为参数（逐分身：replyToOwner / yieldSeconds / humanInterrupt / sessionRouting）──
export type { BehaviorParams } from "./behavior-params"
export { BEHAVIOR_DEFAULTS, behaviorParamsFor } from "./behavior-params"

// ── 工具审批（默认全部不审批，只有名单里的工具弹窗）──
export type { AskToolPattern } from "./approval-tools"
export {
  askTools,
  askToolsFor,
  matchesAskTool,
  needsApproval,
  parseAskToolList,
  readAskToolsFromPersonaJson,
  unknownAskTools,
} from "./approval-tools"

// ── 声明式部署配置（`GEBAI_PERSONA_<ID>` JSON 数组，每项一个分身）──
export type { DriverEnvItem, ParsedPersonaEnv } from "./persona-config"
export { mergeDriverEnv, outputsFromChannels, parsePersonaEnv } from "./persona-config"

// ── 人格定义扩展规范（与事件源、子 Agent、渠道同款「丢文件即注册」）──
export type { PersonaCatalogOptions, PersonaCatalogDiff } from "./catalog"
export { PersonaCatalog } from "./catalog"
export type { PersonaDef, PersonaDiscoveryResult, PersonaLoadError, PersonaScanDirs } from "./persona"
export { defaultPersonaScanDirs, discoverPersonas, readPersonaFile, validatePersonaDef } from "./persona"

// ── 飞书凭据（进程级用户令牌 + 授权流程）──
export type { PersonaTokenStoreOptions, PersonaUserToken, TokenState } from "./user-token"
export { PersonaTokenStore } from "./user-token"
export type { AuthorizeOptions, AuthorizeTicket, ExchangeOptions, PendingAuthorize } from "./oauth"
export { extractCode, extractState, mergeScopes, ERR_OFFLINE_ACCESS_NOT_ENABLED, OFFLINE_ACCESS_SCOPE, PERSONA_SCOPES, PersonaOAuth } from "./oauth"
export type { PersonaCredentialStatus, PersonaCredentialsOptions } from "./credentials"
export { PersonaCredentials, isOfflineAccessRejection } from "./credentials"
