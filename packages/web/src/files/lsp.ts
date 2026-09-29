/**
 * 文件工作台 · 语言服务器（LSP）客户端：文档同步 + Monaco provider 注册 + 诊断落地。
 *
 * 与内置语言服务的分工（见 `symbols.ts` 的说明）：TypeScript / JavaScript / JSON / CSS / HTML 由 Monaco
 * 自带 worker 提供，仓库**不**给它们起外部服务器；其余语言若本机 PATH 上探到服务器（gopls /
 * rust-analyzer / clangd / pyright-langserver …），本模块接管补全、悬停、跳转、引用、重命名、格式化与诊断。
 *
 * 四条不变量：
 * 1. **没有 LSP 一切照旧**：清单为空即不注册任何 provider、不建连接，工作台行为与从前完全一致；
 * 2. **文档同步以服务端为准**：前端只认 `docId`（它不知道绝对路径），变更一律发全文（对 LSP 的
 *    Full/Incremental 两种模式都合法），debounce 收敛高频键入；
 * 3. **诊断是推送**：服务器随时可能推 `publishDiagnostics`，按 `docId` 找到 model 后整体替换 markers；
 * 4. **符号与跳转以服务端为最强来源**：有服务器时文件符号（大纲/面板）也取它的
 *    `textDocument/documentSymbol`（语义级，比 tree-sitter / 词法都准），详见 `lspDocumentSymbols()`。
 *
 * 跳转到其它文件：服务器返回的是 `file://` uri，`registerEditorOpener` 把它折算回「本工作台的根 +
 * 相对路径」再交给工作台打开标签（未打开的文件也能跳，而不是 Monaco 默认的静默失败）。
 * 折算必须跨平台正确：POSIX 的 `file:///a/b` 是**绝对路径**、Windows 的 `file:///C:/a/b` 要去掉盘符前的
 * 斜杠——早期实现一律 `replace(/^\//, "")`，在 Linux/macOS 上会把根斜杠吃掉、整条跳转链断掉。
 */

import { wbUrl } from "./url-base"
import { WorkbenchSocket, readAuthToken } from "./ws-client"
import { UnsupportedMethods } from "./lsp-capabilities"
import type { FlatSym } from "./symbols-core"
import {
  toCompletionItem,
  toDocumentSymbols,
  toLocations,
  toLspSymTree,
  toMarkers,
  toMarkdown,
  toPanelSymbols,
  toRange,
  toTextEdits,
  type LspSym,
} from "./lsp-convert"

type Monaco = typeof import("monaco-editor")
type Model = import("monaco-editor").editor.ITextModel
type Position = import("monaco-editor").Position

/** 服务器清单响应（`GET /api/v1/lsp/servers`）。 */
interface ServerListResp {
  enabled: boolean
  reason?: string
  servers?: Array<{ language: string; id: string; command: string; args: string[] }>
  missing?: Array<{ language: string; id: string; command: string }>
  errors?: string[]
}

/** 打开的 LSP 文档（model ↔ docId ↔ 服务器）。 */
interface AttachedDoc {
  docId: string
  model: Model
  serverId: string
  /** 服务器登记这份文档时用的 `file://` uri——判定「这条结果是不是本文」用它（见 `toLocations`）。 */
  uri: string
  /** 服务器报告的同步模式：0=不推变更 / 1=全量 / 2=增量（前端一律发全文）。 */
  sync: number
  /** 服务器能力（原样留一份：决定哪些请求值得发出去，见 `serverSupports`）。 */
  capabilities: Record<string, unknown>
  /** 会话根（root id 与其绝对路径：把响应里的 file:// uri 折算回工作台路径用）。 */
  rootId: string
  rootAbs: string
  path: string
  language: string
  markerOwner: string
  /** 变更上报的 debounce 定时器。 */
  timer: ReturnType<typeof setTimeout> | null
  disposed: boolean
}

interface LspState {
  enabled: boolean
  reason: string
  /** 有可用服务器的语言（Monaco 语言 id）。 */
  languages: Set<string>
  /** 语言 → 服务器标识（状态栏显示用）。 */
  byLanguage: Map<string, string>
  /** 启动了服务器进程的文档数（状态/诊断用）。 */
  attached: number
  /** 服务器标识 → 它实际拿到的**工程根**（服务端按工程标记向上探测的结果；状态栏用）。 */
  projectRoots: Map<string, string>
}

const state: LspState = {
  enabled: false,
  reason: "",
  languages: new Set(),
  byLanguage: new Map(),
  attached: 0,
  projectRoots: new Map(),
}

let initPromise: Promise<void> | null = null
/** 清单是否已**成功**（含服务端明确回 `enabled:false`）——失败（网络/超时）时保持 false 以便重试。 */
let settled = false
let socket: WorkbenchSocket | null = null
let monacoRef: Monaco | null = null
let installed = false
let sessionProvider: () => string | undefined = () => undefined
let opener: ((target: LspJumpTarget) => void) | null = null
let notifier: ((msg: string, kind?: "info" | "warn" | "error") => void) | null = null

const docs = new Map<string, AttachedDoc>()
const byModel = new Map<Model, AttachedDoc>()
const logTail: string[] = []

/** 变更上报节流（毫秒）：低于此间隔的连续键入合并为一次 didChange。 */
const CHANGE_DEBOUNCE_MS = 220

/** 语言服务器是否可用（清单非空且服务端开启）。 */
export function lspEnabled(): boolean {
  return state.enabled && state.languages.size > 0
}

/** 有可用服务器的语言集合（`symbols.ts` 据此让位，避免同一跳转出现两份候选）。 */
export function lspLanguages(): Set<string> {
  return state.languages
}

/** 某语言的服务器标识（状态栏显示；无则空串）。 */
export function lspServerOf(language: string): string {
  return state.byLanguage.get(String(language ?? "").toLowerCase()) ?? ""
}

/** 未探测到服务器时的原因（设置面板/诊断用）。 */
export function lspReason(): string {
  return state.reason
}

/** 最近服务器日志（stderr / window/logMessage；诊断用，最多 50 条）。 */
export function lspLogTail(): string[] {
  return [...logTail]
}

/** 会话 id 提供者（`sess:` 根解析与审计用）。 */
export function setLspSessionProvider(fn: () => string | undefined): void {
  sessionProvider = fn
}

/** 跨文件跳转的落地回调（由工作台注册：按根内/根外两态分别打开并定位）。 */
export function setLspOpener(fn: (target: LspJumpTarget) => void): void {
  opener = fn
}

/**
 * 提示回调（由工作台注册为 toast）：用于「这个定义打不开」这类**必须让用户看到**的情况。
 * 与 `log()` 分开：日志是给排障看的技术尾巴，这里是给人看的一句话。
 */
export function setLspNotifier(fn: (msg: string, kind?: "info" | "warn" | "error") => void): void {
  notifier = fn
}

function notify(msg: string, kind: "info" | "warn" | "error" = "info"): void {
  notifier?.(msg, kind)
}

/**
 * 拉取本机可用服务器清单（一次；**网络类失败允许下次重试**）。
 * 工作台在预热 Monaco 之前调用它：符号 provider 的分工（有 LSP 的语言让位）依赖这份清单。
 * 返回的 Promise 不抛错（失败视为无 LSP，绝不阻塞打开文件）。
 */
export function initLsp(): Promise<void> {
  if (initPromise) return initPromise
  initPromise = (async () => {
    try {
      const token = readAuthToken()
      const ctl = new AbortController()
      const timer = setTimeout(() => ctl.abort(), 5000)
      const res = await fetch(wbUrl("/api/v1/lsp/servers"), { headers: token ? { Authorization: `Bearer ${token}` } : {}, signal: ctl.signal })
      clearTimeout(timer)
      if (!res.ok) {
        // 确定性拒绝（404 页面关闭 / 403 沙箱）：本页不再重试
        state.reason = res.status === 403 ? "沙箱模式下不开放语言服务器" : `清单请求失败（HTTP ${res.status}）`
        return
      }
      const body = (await res.json()) as ServerListResp
      state.enabled = body.enabled !== false
      state.reason = body.reason ?? ""
      for (const s of body.servers ?? []) {
        const language = String(s.language ?? "").toLowerCase()
        if (!language) continue
        state.languages.add(language)
        state.byLanguage.set(language, s.id)
      }
      if (!state.enabled) state.reason = body.reason ?? "语言服务器未启用"
      settled = true
    } catch {
      /* 网络波动 / 超时：本次当无 LSP，但**不固化**——下次打开文件会重新拉一次 */
      state.reason = "无法获取语言服务器清单"
    } finally {
      // Monaco 可能先就绪（用户在清单到达前就打开了文件）：补装一次 provider
      if (monacoRef && state.languages.size) installLspProviders(monacoRef)
      if (!settled) initPromise = null
    }
  })()
  return initPromise
}

/** 该语言是否有可用服务器。 */
export function hasLsp(language: string): boolean {
  return lspEnabled() && state.languages.has(String(language ?? "").toLowerCase())
}

function sock(): WorkbenchSocket {
  if (!socket) {
    socket = new WorkbenchSocket({ label: "语言服务器", context: () => sessionProvider(), timeoutMs: 20000 })
    socket.on("lsp.notify", onServerNotify)
    socket.on("lsp.exit", onServerExit)
    socket.on("lsp.log", onServerLog)
  }
  return socket
}

/**
 * 打开文档并建立同步（打开文件成功后调用；失败/无服务器时静默返回 null）。
 * 返回服务器标识（供状态栏显示），无 LSP 时返回 null。
 *
 * `reuseFrom`：把这个文档挂到**另一份文档的服务器会话**上（用于跨到工作区外的库文件——应由原来那个
 * 服务器回答语义问题：它已经把这个头文件纳入索引、手里有编译参数；另起一份既没这两样，还会白吃一个
 * 并发槽位。服务端仍会校验「同一个用户 + 同一个服务器二进制」，不满足就自己建会话）。
 */
export async function attachDocument(input: { model: Model; rootId: string; path: string; language: string; reuseFrom?: Model | null }): Promise<string | null> {
  // 清单可能尚未到达（deep link / 记忆恢复会在启动早期就打开文件）：这里等一次，
  // 否则首次打开的文件永远挂不上（清单是一次性拉取，不会再触发挂载）
  await initLsp()
  if (!hasLsp(input.language)) return null
  const model = input.model
  const previous = byModel.get(model)
  if (previous && !previous.disposed) detachDocument(model)
  const reuse = input.reuseFrom ? byModel.get(input.reuseFrom) : undefined
  const res = await sock().request("lsp.open", {
    root: input.rootId,
    path: input.path,
    language: input.language,
    text: model.getValue(),
    version: model.getVersionId(),
    attachTo: reuse && !reuse.disposed ? reuse.docId : undefined,
  })
  if (!res.ok) return null
  const payload = res.payload ?? {}
  if (payload.available === false) return null
  const docId = String(payload.docId ?? "")
  if (!docId) return null
  const serverObj = (payload.server ?? {}) as { id?: string; command?: string }
  const rootObj = (payload.root ?? {}) as { id?: string; abs?: string }
  const serverId = String(serverObj.id ?? state.byLanguage.get(input.language) ?? "lsp")
  const projectRoot = String(payload.projectRoot ?? "")
  if (projectRoot) state.projectRoots.set(serverId, projectRoot)
  const doc: AttachedDoc = {
    docId,
    model,
    serverId,
    uri: String(payload.uri ?? ""),
    sync: Number(payload.sync ?? 1),
    capabilities: (payload.capabilities ?? {}) as Record<string, unknown>,
    rootId: String(rootObj.id ?? input.rootId),
    rootAbs: String(rootObj.abs ?? ""),
    path: input.path,
    language: input.language,
    markerOwner: `lsp:${serverId}`,
    timer: null,
    disposed: false,
  }
  docs.set(docId, doc)
  byModel.set(model, doc)
  state.attached = docs.size
  // 键入 → 节流上报全文（服务端按同步模式决定是否下发 didChange）
  const changeSub = model.onDidChangeContent(() => {
    if (doc.sync === 0 || doc.disposed) return
    if (doc.timer) clearTimeout(doc.timer)
    doc.timer = setTimeout(() => {
      doc.timer = null
      if (doc.disposed) return
      sock().send("lsp.change", { docId: doc.docId, version: doc.model.getVersionId(), text: doc.model.getValue() })
    }, CHANGE_DEBOUNCE_MS)
  })
  model.onWillDispose(() => {
    changeSub.dispose()
    if (!doc.disposed) detachDocument(model)
  })
  return serverId
}

/** 关闭文档（标签关闭 / 切换语言服务）：通知服务器并清掉该文件的诊断标记。 */
export function detachDocument(model: Model): void {
  const doc = byModel.get(model)
  if (!doc) return
  doc.disposed = true
  if (doc.timer) clearTimeout(doc.timer)
  byModel.delete(model)
  docs.delete(doc.docId)
  state.attached = docs.size
  try {
    monacoRef?.editor.setModelMarkers(model, doc.markerOwner, [])
  } catch {
    /* model 已销毁 */
  }
  sock().send("lsp.close", { docId: doc.docId })
}

/** 保存后通知服务器（触发重新诊断）；文件内容已由工作台写入磁盘。 */
export function notifySaved(model: Model): void {
  const doc = byModel.get(model)
  if (!doc || doc.disposed) return
  sock().send("lsp.save", { docId: doc.docId, text: model.getValue() })
}

/** 当前文档挂到了哪个服务器（状态栏用；未挂返回空串）。 */
export function attachedServerOf(model: Model | null | undefined): string {
  if (!model) return ""
  return byModel.get(model)?.serverId ?? ""
}

/**
 * 当前文档的服务器状态说明（状态栏 title 用）：服务器名 + 它实际拿到的工程根/工作目录。
 *
 * 工程根是服务端按工程标记（go.mod / Cargo.toml / compile_commands.json …）向上探测的结果——
 * 打开子目录里的文件时它与工作台根不同，「补全为何有效/为何没反应」往往就靠这一行判断。
 */
export function lspServerDetailOf(model: Model | null | undefined): string {
  if (!model) return ""
  const doc = byModel.get(model)
  if (!doc) return ""
  const project = state.projectRoots.get(doc.serverId) ?? doc.rootAbs
  return project ? `${doc.serverId}（工程根 ${project}）` : doc.serverId
}

/* ------------------------------ Monaco provider 注册 ------------------------------ */

/**
 * 注册 LSP provider（幂等）：只在**存在可用服务器**时注册，且按语言 selector 限定——
 * 没有服务器（清单为空、探测失败、`GEBAI_LSP=false`）时本函数直接返回，工作台一切照旧。
 *
 * 清单是异步拉取的，两条路径都会收敛到这里：Monaco 就绪时若清单已到就装；清单后到则由
 * `initLsp` 在末尾补装（`monacoRef` 已记下内核引用）。
 */
export function installLspProviders(m: Monaco): void {
  monacoRef = m
  if (installed) return
  const langs = [...state.languages]
  if (!langs.length) return
  installed = true
  m.languages.registerCompletionItemProvider(langs, { provideCompletionItems, resolveCompletionItem })
  m.languages.registerHoverProvider(langs, { provideHover })
  m.languages.registerDefinitionProvider(langs, { provideDefinition })
  m.languages.registerReferenceProvider(langs, { provideReferences })
  m.languages.registerRenameProvider(langs, { provideRenameEdits })
  m.languages.registerDocumentFormattingEditProvider(langs, { provideDocumentFormattingEdits })
  // 区域格式化：能力声明里早就写了 `rangeFormatting`，provider 先前缺位（右键“格式化选区”没反应）
  m.languages.registerDocumentRangeFormattingEditProvider(langs, { provideDocumentRangeFormattingEdits: provideRangeFormattingEdits })
  m.languages.registerSignatureHelpProvider(langs, { provideSignatureHelp })
  // 文档符号（大纲）：本模块不直接注册 provider——符号来源要跟 tree-sitter / 词法**统一仲裁**，
  // 因此改由 `symbols.ts` 的 DocumentSymbolProvider 单点处理（它按 model 问 `lspDocumentSymbols`）。
  // 跨文件跳转：把服务器给的 file:// uri 折算回「工作台的根 + 相对路径」（或工作区外的绝对路径）再交给工作台
  m.editor.registerEditorOpener({
    openCodeEditor: (source, resource, selectionOrPosition) => openResource(source, resource, selectionOrPosition),
  })
}

/** Monaco 位置（1 基）→ LSP 位置（0 基）。 */
function lspPos(p: Position): { line: number; character: number } {
  return { line: Math.max(0, p.lineNumber - 1), character: Math.max(0, p.column - 1) }
}

/** 发一条 LSP 请求（文档未挂载 / 服务器未运行时返回 null，调用方静默降级）。 */
async function lspRequest<T>(model: Model, method: string, params: Record<string, unknown>): Promise<T | null> {
  const doc = byModel.get(model)
  if (!doc || doc.disposed) return null
  if (!unsupported.allows(doc.serverId, doc.capabilities, method)) return null
  const res = await sock().request("lsp.request", {
    docId: doc.docId,
    method,
    params: { textDocument: { uri: doc.docId }, ...params },
  })
  if (!res.ok) {
    if (unsupported.note(doc.serverId, method, res.error)) log(`[${doc.serverId}] 不支持 ${method}（本页不再请求）`)
    return null
  }
  return ((res.payload?.result ?? null) as T | null)
}

/* ------------------------------ 服务器能力协商 ------------------------------ */

/**
 * 服务器明确回过「method not found」（JSON-RPC -32601）的方法登记表。
 *
 * 实测 gopls 0.21 不实现 `textDocument/rangeFormatting` 与 `completionItem/resolve`：这类请求发出去只会
 * 换回一个错误，而 Monaco 那侧看到的是「空编辑 / 空文档」。记住一次就不再问（同一次页面会话内；
 * 判定与登记表在 `lsp-capabilities.ts`，纯函数 + 单测）。
 */
const unsupported = new UnsupportedMethods()

/**
 * 补全项的原始 LSP 对象 + 它所属的 model（延迟解析用）。
 *
 * Monaco 在选中候选时会回调 `resolveCompletionItem`，它把**同一个对象**传回来——但它只认识自己的
 * `CompletionItem` 形状，`completionItem/resolve` 需要发回**原始 LSP 项**（data 字段、完整 label）与
 * 该文档的 `docId`（而回调参数里没有 model）。因此在这里挂一份弱引用映射；取不到就跳过解析
 * （结果仍可用，只是文档/附加编辑得等直接给出的那份）。
 */
const rawCompletion = new WeakMap<object, { raw: Record<string, unknown>; model: Model }>()

/** LSP `additionalTextEdits`（如自动导入的 import 行）→ Monaco 的附加编辑。 */
function additionalEdits(model: Model, raw: unknown): import("monaco-editor").languages.CompletionItem["additionalTextEdits"] {
  const edits = toTextEdits(raw)
  if (!edits.length) return undefined
  return edits.map((e) => ({ range: toRange(model, e.range, { expandEmpty: false }), text: e.text }))
}

async function provideCompletionItems(model: Model, position: Position): Promise<import("monaco-editor").languages.CompletionList> {
  const m = monacoRef
  if (!m) return { suggestions: [] }
  const result = await lspRequest<unknown>(model, "textDocument/completion", { position: lspPos(position) })
  const items = Array.isArray(result) ? result : ((result as { items?: unknown[] } | null)?.items ?? [])
  const word = model.getWordUntilPosition(position)
  const fallbackRange: import("monaco-editor").IRange = {
    startLineNumber: position.lineNumber,
    startColumn: word.startColumn,
    endLineNumber: position.lineNumber,
    endColumn: word.endColumn,
  }
  const suggestions = (items as Record<string, unknown>[]).map((item) => {
    const out = toCompletionItem(m, model, item, fallbackRange)
    // 已弃用标记：LSP CompletionItemTag.Deprecated(1) 与 Monaco 同值，Monaco 会画删除线
    if (Array.isArray(item.tags)) {
      const tags = (item.tags as unknown[]).filter((t) => t === 1)
      if (tags.length) out.tags = tags as import("monaco-editor").languages.CompletionItemTag[]
    }
    // 附加编辑（自动导入最常用）：Monaco 的 CompletionItem 有这一字段，选中即一并应用
    const extra = additionalEdits(model, item.additionalTextEdits)
    if (extra) out.additionalTextEdits = extra
    rawCompletion.set(out as unknown as object, { raw: item, model })
    return out
  })
  return { suggestions }
}

/**
 * 选中候选时向服务器补全缺失信息（`completionItem/resolve`）。
 *
 * 许多服务器（gopls / clangd）把类型详情与文档放在这一步返回，或只在这一步才给出自动导入所需的
 * `additionalTextEdits`——不实现 resolve 就只能看到光秃秃的名字。
 */
async function resolveCompletionItem(
  item: import("monaco-editor").languages.CompletionItem,
): Promise<import("monaco-editor").languages.CompletionItem> {
  const entry = rawCompletion.get(item as unknown as object)
  if (!entry || !monacoRef) return item
  const resolved = await lspRequest<Record<string, unknown>>(entry.model, "completionItem/resolve", entry.raw)
  if (!resolved) return item
  if (typeof resolved.detail === "string" && !item.detail) item.detail = resolved.detail
  if (resolved.documentation !== undefined && !item.documentation) item.documentation = toMarkdown(resolved.documentation)
  const extra = additionalEdits(entry.model, resolved.additionalTextEdits)
  if (extra) item.additionalTextEdits = extra
  return item
}

async function provideHover(model: Model, position: Position): Promise<import("monaco-editor").languages.Hover | null> {
  const result = await lspRequest<{ contents?: unknown; range?: { start: { line: number; character: number }; end: { line: number; character: number } } }>(
    model,
    "textDocument/hover",
    { position: lspPos(position) },
  )
  if (!result) return null
  const md = toMarkdown(result.contents)
  if (!md.value) return null
  return result.range ? { contents: [md], range: toRange(model, result.range) } : { contents: [md] }
}

async function provideDefinition(model: Model, position: Position): Promise<import("monaco-editor").languages.Location[] | null> {
  const m = monacoRef
  if (!m) return null
  const result = await lspRequest<unknown>(model, "textDocument/definition", { position: lspPos(position) })
  const locs = toLocations(m, model, result, byModel.get(model)?.uri)
  return locs.length ? locs : null
}

async function provideReferences(
  model: Model,
  position: Position,
  context: import("monaco-editor").languages.ReferenceContext,
): Promise<import("monaco-editor").languages.Location[] | null> {
  const m = monacoRef
  if (!m) return null
  const result = await lspRequest<unknown>(model, "textDocument/references", {
    position: lspPos(position),
    context: { includeDeclaration: context.includeDeclaration },
  })
  const locs = toLocations(m, model, result, byModel.get(model)?.uri)
  return locs.length ? locs : null
}

/**
 * 重命名：LSP 返回的是 WorkspaceEdit（`changes` 或 `documentChanges`，后者可带版本与文件操作）。
 *
 * 两条实操约束：
 * - **只处理已在工作台打开的文档**：本文件之外的 `changes` 无法凭空核验与撤销，跳过而不是造一堆
 *   看不见的编辑（工作台编辑器是“所见即所得”，跨文件重命名应该由用户逐个文件确认）；
 * - Monaco 的 WorkspaceEdit 只接受 `{ edits: [...] }`（resource + textEdit），因此把本文件的编辑
 *   展成它认的形态，区间用**非扩张**口径（零宽即插入）。
 */
async function provideRenameEdits(
  model: Model,
  position: Position,
  newName: string,
): Promise<import("monaco-editor").languages.WorkspaceEdit | null> {
  const m = monacoRef
  if (!m) return null
  const result = await lspRequest<{ changes?: Record<string, unknown>; documentChanges?: unknown[] }>(model, "textDocument/rename", {
    position: lspPos(position),
    newName,
  })
  if (!result) return null
  const groups: Array<[string, unknown]> = []
  if (result.changes) for (const [uri, list] of Object.entries(result.changes)) groups.push([uri, list])
  if (Array.isArray(result.documentChanges)) {
    for (const raw of result.documentChanges as Array<{ textDocument?: { uri?: unknown }; edits?: unknown }>) {
      const uri = raw?.textDocument?.uri
      if (typeof uri === "string") groups.push([uri, raw.edits])
    }
  }
  const doc = byModel.get(model)
  const edits: Array<{ resource: import("monaco-editor").Uri; textEdit: { range: import("monaco-editor").IRange; text: string }; versionId: number | undefined }> = []
  for (const [uri, list] of groups) {
    if (!doc || !sameUri(uri, doc.uri)) continue
    for (const edit of toTextEdits(list)) {
      edits.push({
        resource: m.Uri.parse(uri),
        textEdit: { range: toRange(model, edit.range, { expandEmpty: false }), text: edit.text },
        versionId: undefined,
      })
    }
  }
  return edits.length ? { edits } : null
}

/** 两个 uri 是否指向同一份文档（解码 + 反斜杠归一后逐字比）。 */
function sameUri(a: string, b: string): boolean {
  const norm = (s: string): string => {
    try {
      return decodeURIComponent(s).replace(/\\/g, "/")
    } catch {
      return s.replace(/\\/g, "/")
    }
  }
  return b !== "" && norm(a) === norm(b)
}

async function provideDocumentFormattingEdits(
  model: Model,
  options: import("monaco-editor").languages.FormattingOptions,
): Promise<import("monaco-editor").languages.TextEdit[]> {
  const result = await lspRequest<unknown>(model, "textDocument/formatting", {
    options: { tabSize: options.tabSize, insertSpaces: options.insertSpaces },
  })
  // 格式化编辑不得扩张零宽区间（扩一格会吃掉一个字符）
  return toTextEdits(result).map((edit) => ({ range: toRange(model, edit.range, { expandEmpty: false }), text: edit.text }))
}

/** 区域格式化（选中一段后格式化）：声明的 `rangeFormatting` 能力对应的 provider。 */
async function provideRangeFormattingEdits(
  model: Model,
  range: import("monaco-editor").IRange,
  options: import("monaco-editor").languages.FormattingOptions,
): Promise<import("monaco-editor").languages.TextEdit[]> {
  const result = await lspRequest<unknown>(model, "textDocument/rangeFormatting", {
    range: {
      start: { line: Math.max(0, range.startLineNumber - 1), character: Math.max(0, range.startColumn - 1) },
      end: { line: Math.max(0, range.endLineNumber - 1), character: Math.max(0, range.endColumn - 1) },
    },
    options: { tabSize: options.tabSize, insertSpaces: options.insertSpaces },
  })
  return toTextEdits(result).map((edit) => ({ range: toRange(model, edit.range, { expandEmpty: false }), text: edit.text }))
}

/* ------------------------------ 文档符号（大纲 / 面板） ------------------------------ */

/**
 * 取服务器视角的文件符号（`textDocument/documentSymbol`）。
 *
 * 为什么要这一步：工作台原本把“有 LSP 的语言”从符号 provider 的选择器里**剔除**，但 LSP 那侧只
 * 注册了补全/悬停/跳转这些，结果 Go / C / Python 这类语言的 Monaco 大纲（Ctrl+Shift+O）**一个符号
 * 都没有**——文档符号得自己接上。两种服务器返回形态（层级 DocumentSymbol / 扁平 SymbolInformation）
 * 都由 `lsp-convert` 归一到同一个中间树。
 *
 * 返回 null 表示「没有服务器 / 请求失败 / 服务器没声明该能力」，调用方回退 tree-sitter 与词法。
 */
export async function lspDocumentSymbols(
  model: Model,
): Promise<{ tree: LspSym[]; outline: import("monaco-editor").languages.DocumentSymbol[]; flat: FlatSym[] } | null> {
  const m = monacoRef
  if (!m) return null
  const raw = await lspRequest<unknown>(model, "textDocument/documentSymbol", {})
  const tree = toLspSymTree(raw)
  if (!tree) return null
  return { tree, outline: toDocumentSymbols(model, tree), flat: toPanelSymbols(tree) }
}
async function provideSignatureHelp(
  model: Model,
  position: Position,
  _token: import("monaco-editor").CancellationToken,
  context: import("monaco-editor").languages.SignatureHelpContext,
): Promise<import("monaco-editor").languages.SignatureHelpResult | null> {
  const result = await lspRequest<{ signatures?: unknown[]; activeSignature?: number; activeParameter?: number }>(
    model,
    "textDocument/signatureHelp",
    {
      position: lspPos(position),
      context: {
        triggerKind: context.triggerKind,
        triggerCharacter: context.triggerCharacter,
        isRetrigger: context.isRetrigger,
      },
    },
  )
  if (!result?.signatures?.length) return null
  const signatures = (result.signatures as Record<string, unknown>[]).map((sig) => ({
    label: typeof sig.label === "string" ? sig.label : "",
    documentation: sig.documentation === undefined ? undefined : toMarkdown(sig.documentation),
    parameters: Array.isArray(sig.parameters)
      ? (sig.parameters as Record<string, unknown>[]).map((p) => ({
          label: typeof p.label === "string" || Array.isArray(p.label) ? (p.label as string | [number, number]) : "",
          documentation: p.documentation === undefined ? undefined : toMarkdown(p.documentation),
        }))
      : [],
  }))
  return {
    value: {
      signatures,
      activeSignature: result.activeSignature ?? 0,
      activeParameter: result.activeParameter ?? 0,
    },
    dispose: () => {
      /* 无资源可释放 */
    },
  }
}

/* ------------------------------ 跨文件跳转与服务器事件 ------------------------------ */

/**
 * 跨文件跳转的落地目标。
 *
 * 两态而不是一个（“根内”与“工作区外”由工作台分别处置）：
 * - `inside`：命中某个已挂载根 → 根内相对路径（普通工程内跳转，行为与以前一致）；
 * - `absPath`：未命中任何根 → 绝对路径（**库文件**：`/usr/include/c++/13/string`、GOROOT 标准库、
 *   rust-src 的 `library/core`、site-packages/typeshed 的 `.pyi`）——早先这种目标直接返回 false，
 *   于是“跳库文件”变成静默失败；现在交给工作台按绝对路径建临时根打开。
 *
 * 另带两样跳转来源信息：`language`（目标语言判定不出来时的提示，如无扩展名的标准库头）与
 * `sourceModel`（复用来源文档的**语言服务器会话**——库文件应由原来那个服务器回答语义问题，
 * 而不是另起一份没有编译参数的实例）。
 */
export interface LspJumpTarget {
  inside?: { rootId: string; path: string }
  absPath?: string
  line: number
  column?: number
  language: string
  sourceModel?: Model | null
}

/** `file://` uri → 本工作台的根 + 相对路径（取最长匹配根；不匹配返回 null）。跨平台：见文件头注释。 */
function mapUri(uri: import("monaco-editor").Uri): { rootId: string; path: string } | null {
  if (uri.scheme !== "file") return null
  const abs = fileUriToAbs(uri)
  if (!abs) return null
  const display = abs.replace(/\\/g, "/")
  const windows = isWindowsPath(abs)
  const target = windows ? display.toLowerCase() : display
  let best: AttachedDoc | null = null
  let bestLen = -1
  for (const doc of docs.values()) {
    if (!doc.rootAbs) continue
    const rootRaw = trimSlash(doc.rootAbs.replace(/\\/g, "/"))
    const root = windows ? rootRaw.toLowerCase() : rootRaw
    if (target !== root && !target.startsWith(`${root}/`)) continue
    if (root.length > bestLen) {
      best = doc
      bestLen = root.length
    }
  }
  if (!best) return null
  // 相对路径取自**原串**：Windows 下用于比较的 target 是小写化的，拿它当路径会把文件名的大小写改掉
  return { rootId: best.rootId, path: display.slice(bestLen + 1) }
}

/** 去尾斜杠（根 `/` 除外）。 */
function trimSlash(p: string): string {
  const t = p.replace(/\/+$/, "")
  return t === "" ? "/" : t
}

/** 登录主机形态的 Windows 路径（`C:/…` 或 `C:\\…`）。 */
function isWindowsPath(p: string): boolean {
  return /^[A-Za-z]:[\\/]/.test(p)
}

/**
 * Monaco 的 `file://` Uri → 本机绝对路径。**跨平台的关键在盘符**：POSIX 的 `file:///a/b` 本身就是
 * 绝对路径（首斜杠要留），Windows 的 `file:///C:/a/b` 里那个前导斜杠要掉（掉完是 `C:/a/b`）。
 */
function fileUriToAbs(uri: import("monaco-editor").Uri): string {
  let path = uri.path ?? ""
  try {
    path = decodeURIComponent(path)
  } catch {
    /* 有非法转义：保留原样 */
  }
  if (/^\/[A-Za-z]:([\\/]|$)/.test(path)) return path.slice(1) // Windows 盘符：去掉前导斜杠
  return path
}

/** 编辑器打开外部 uri 的落地：折算成「根内相对路径」或「绝对路径」后交给 `opener`（工作台决定用哪个根）。 */
function openResource(source: import("monaco-editor").editor.ICodeEditor | null, resource: import("monaco-editor").Uri, selection: unknown): boolean {
  if (!opener) return false
  const pos = selection as { lineNumber?: number; column?: number; startLineNumber?: number; startColumn?: number } | undefined
  const line = pos?.startLineNumber ?? pos?.lineNumber ?? 1
  const column = pos?.startColumn ?? pos?.column ?? 1
  // 跳转来源：Monaco 把**发起跳转的编辑器**一并给出，据此取它的已挂载文档（语言提示 + 会话复用）
  const sourceModel = (source?.getModel?.() ?? null) as Model | null
  const srcDoc = sourceModel ? byModel.get(sourceModel) : undefined
  const origin = { language: srcDoc?.language ?? "", sourceModel }
  const inside = mapUri(resource)
  if (inside) {
    opener({ inside, line, column, ...origin })
    return true
  }
  if (resource.scheme === "file") {
    const abs = fileUriToAbs(resource)
    if (abs) {
      opener({ absPath: abs, line, column, ...origin })
      return true
    }
  }
  // 非 file 协议（如 Java 的 jdt://）没有可打开的本地文件：明确告知，不留「点了没反应」的静默
  notify(`该定义位于非本地文件（${resource.scheme || "未知"} 协议），工作台无法打开`, "warn")
  return false
}

/** 服务器推送：诊断整体替换该文档的 markers（owner 按服务器区分，互不干扰）。 */
function onServerNotify(payload: Record<string, unknown>): void {
  if (String(payload.method ?? "") !== "textDocument/publishDiagnostics") return
  const doc = docs.get(String(payload.docId ?? ""))
  const m = monacoRef
  if (!doc || doc.disposed || !m) return
  const params = (payload.params ?? {}) as { diagnostics?: unknown }
  try {
    m.editor.setModelMarkers(doc.model, doc.markerOwner, toMarkers(m, doc.model, params.diagnostics))
  } catch {
    /* model 已销毁（标签刚关）：忽略 */
  }
}

/**
 * 服务器进程退出：清诊断 + 记日志 + 限频重挂**该服务器的全部文档**。
 *
 * 限频是「每服务器 30s 内只重挂一次」——早期实现把限频与挂载写在同一个循环里，结果是**只有第一个
 * 文档被重挂**（首次进入循环就把配额用掉了），同一服务器上的其它文件于是静默失去语义能力。
 */
function onServerExit(payload: Record<string, unknown>): void {
  const m = monacoRef
  const server = String(payload.server ?? "")
  const code = payload.code === null || payload.code === undefined ? "" : ` code=${String(payload.code)}`
  log(`[${server || "lsp"}] 语言服务器已退出${code}`)
  if (!m) return
  const affected: AttachedDoc[] = []
  for (const doc of [...docs.values()]) {
    if (server && doc.serverId !== server) continue
    try {
      m.editor.setModelMarkers(doc.model, doc.markerOwner, [])
    } catch {
      /* model 已销毁 */
    }
    affected.push(doc)
  }
  if (!server || !affected.length) return
  state.projectRoots.delete(server)
  // 重挂会拉起新进程（能力表可能不同）：清掉该服务器的「method not found」记录
  unsupported.clear(server)
  if (!restartable(server)) {
    log(`[${server}] 短时间内反复退出：本次不自动重连（避免重连风暴）`)
    return
  }
  for (const doc of affected) {
    const { model, rootId, path, language } = doc
    detachDocument(model)
    void attachDocument({ model, rootId, path, language })
  }
}

const lastRestart = new Map<string, number>()
/** 同一服务器 30s 内只自动重连一次（重连覆盖它名下的全部文档）。 */
function restartable(server: string): boolean {
  const now = Date.now()
  const prev = lastRestart.get(server) ?? 0
  if (now - prev < 30000) return false
  lastRestart.set(server, now)
  return true
}

function onServerLog(payload: Record<string, unknown>): void {
  log(String(payload.text ?? ""))
}

function log(text: string): void {
  const line = text.trim()
  if (!line) return
  logTail.push(line)
  if (logTail.length > 50) logTail.shift()
}
