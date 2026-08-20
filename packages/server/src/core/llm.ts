import type { LLMCapabilities, MessageLike } from "@gebai/sdk"
import { repairToolPairing } from "./store"

export interface LLMToolDef {
  name: string
  description: string
  parameters: Record<string, unknown>
}

/** 服务端 usage 真值（接口返回的 token 计数；服务端/网关不返回时为 undefined → 引擎走估算兜底）。 */
export interface LLMUsage {
  inputTokens?: number
  outputTokens?: number
  totalTokens?: number
}

export interface LLMChunk {
  type: "text" | "reasoning" | "tool_call" | "done"
  text?: string
  toolCall?: { id: string; name: string; arguments: Record<string, unknown> }
  /** 工具参数不是合法 JSON 时携带原始片段（引擎据此回传模型让其修正，而非静默以 {} 执行）。 */
  toolArgsError?: string
  stopReason?: string
  /** usage 真值（挂在 done chunk 上，一次调用一个值）。 */
  usage?: LLMUsage
}

export interface ChatOptions {
  tools?: LLMToolDef[]
  signal?: AbortSignal
  maxTokens?: number
  /** 本次调用的额外请求体参数（如 reasoning_effort 推理强度），优先级高于 ProviderConfig.extraParams。 */
  extraParams?: Record<string, unknown>
}

export interface LLMProvider {
  readonly id: string
  chat(messages: MessageLike[], opts?: ChatOptions): AsyncIterable<LLMChunk>
  capabilities(): LLMCapabilities
}

export type ApiKind = "openai" | "responses" | "anthropic"

/** 模型接口未配置错误（apiBase 缺失）：配置类错误无需重试，引擎据此直接失败并给出配置入口指引。 */
export class LLMConfigError extends Error {}

function assertApiConfigured(config: ProviderConfig): void {
  if (!config.apiBase) {
    throw new LLMConfigError(
      "模型接口地址未配置：请在「设置 → 环境变量」配置 GEBAI_LLM_API_BASE（及 GEBAI_LLM_API_KEY / GEBAI_LLM_MODEL，随消息生效、不落盘），或配置服务端环境变量后重启",
    )
  }
}

export interface ProviderConfig {
  apiKind: ApiKind
  apiBase: string
  apiKey: string
  model: string
  maxContextTokens: number
  multimodal: boolean
  /** 每次请求固定的额外请求体参数（如 reasoning_effort），可被 ChatOptions.extraParams 覆盖。 */
  extraParams?: Record<string, unknown>
}

/**
 * 解析额外模型接口参数（JSON 字符串，如 `{"reasoning_effort":"high"}`）：
 * 必须是 JSON 对象（非数组/标量），否则抛错提示检查配置。
 */
export function parseExtraParams(raw: string | undefined): Record<string, unknown> {
  if (!raw) return {}
  let parsed: unknown
  try {
    parsed = JSON.parse(raw)
  } catch {
    throw new Error(`GEBAI_LLM_EXTRA_PARAMS 不是合法 JSON: ${raw}`)
  }
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    throw new Error(`GEBAI_LLM_EXTRA_PARAMS 必须是 JSON 对象（如 {"reasoning_effort":"high"}）`)
  }
  return parsed as Record<string, unknown>
}

/** Parse an SSE byte stream into `data:` payload strings.
 *  事件分隔与行结束按 SSE 规范兼容 `\n`/`\r\n`/`\r` 三种形态（部分网关以 CRLF 行结尾，
 *  只按 `\n\n` 切分会永远切不出事件边界、整轮静默无输出）；多行 `data:` 按 `\n` join。 */
export async function* parseSSE(stream: ReadableStream<Uint8Array>, signal?: AbortSignal): AsyncGenerator<string> {
  const reader = stream.getReader()
  const decoder = new TextDecoder()
  let buf = ""
  try {
    for (;;) {
      if (signal?.aborted) break
      const { done, value } = await reader.read()
      if (done) break
      buf += decoder.decode(value, { stream: true }).replace(/\r\n/g, "\n").replace(/\r/g, "\n")
      let idx: number
      while ((idx = buf.indexOf("\n\n")) >= 0) {
        const raw = buf.slice(0, idx)
        buf = buf.slice(idx + 2)
        let data = ""
        for (const line of raw.split("\n")) {
          if (!line.startsWith("data:")) continue
          const v = line.slice(5).replace(/^ /, "")
          data = data ? `${data}\n${v}` : v
        }
        if (data) yield data
      }
    }
    const rest = buf.trim()
    if (rest.startsWith("data:")) yield rest.slice(5).trim()
  } finally {
    // 消费方提前退出（空闲超时 iter.return/取消）：cancel 通知上游中止传输，防连接悬挂累积
    void reader.cancel().catch(() => {})
    reader.releaseLock()
  }
}

/**
 * fetch 带重试（模型接口健壮性）：仅网络错误、429、5xx 重试（指数退避，最多 retries 次）；
 * 其余 4xx 与 AbortError 立即返回/抛出，不重试。请求体为字符串可安全复用。
 */
async function fetchRetry(url: string, init: RequestInit, retries = 2): Promise<Response> {
  for (let attempt = 0; ; attempt++) {
    let res: Response
    try {
      res = await fetch(url, init)
    } catch (err) {
      if ((err as Error).name === "AbortError") throw err
      if (attempt >= retries) throw err
      await sleep(500 * 2 ** attempt, init.signal) // 退避期间可被取消（abort 立即中断）
      continue
    }
    if (res.ok || (res.status !== 429 && res.status < 500)) return res
    if (attempt >= retries) return res // 重试耗尽：返回响应，由调用方构造含状态码的错误
    await sleep(500 * 2 ** attempt, init.signal)
  }
}

function sleep(ms: number, signal?: AbortSignal | null): Promise<void> {
  if (signal?.aborted) return Promise.reject(new DOMException("Aborted", "AbortError"))
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      signal?.removeEventListener("abort", onAbort)
      resolve()
    }, ms)
    const onAbort = () => {
      clearTimeout(timer)
      reject(new DOMException("Aborted", "AbortError"))
    }
    signal?.addEventListener("abort", onAbort, { once: true })
  })
}

function openaiTool(t: LLMToolDef): Record<string, unknown> {
  return { type: "function", function: { name: t.name, description: t.description, parameters: t.parameters } }
}

function anthropicTool(t: LLMToolDef): Record<string, unknown> {
  return { name: t.name, description: t.description, input_schema: t.parameters }
}

/**
 * 统一内部多模态内容块 → OpenAI 兼容格式：
 * `{ type: "image", mime, data }`（base64 数据）→ `{ type: "image_url", image_url: { url: data URL } }`；
 * 其余块原样透传（text 等已为 OpenAI 原生结构）。
 */
function toOpenAIContentBlock(b: Record<string, unknown>): Record<string, unknown> {
  if (b.type === "image" && typeof b.mime === "string" && typeof b.data === "string") {
    return { type: "image_url", image_url: { url: `data:${b.mime};base64,${b.data}` } }
  }
  return b
}

function toOpenAIMessages(msgs: MessageLike[]): Array<Record<string, unknown>> {
  const out: Array<Record<string, unknown>> = []
  for (const m of repairToolPairing(msgs, { flushTail: true })) {
    if (m.role === "tool") {
      const msg: Record<string, unknown> = { role: "tool", tool_call_id: m.toolCallId, content: typeof m.content === "string" ? m.content : "" }
      if (m.name) msg.name = m.name
      out.push(msg)
    } else if (m.role === "assistant" && m.toolCalls) {
      const calls = m.toolCalls.map((tc) => ({
        id: tc.id,
        type: "function",
        function: { name: tc.name, arguments: typeof tc.arguments === "string" ? tc.arguments : JSON.stringify(tc.arguments) },
      }))
      const msg: Record<string, unknown> = { role: "assistant", content: typeof m.content === "string" ? (m.content || null) : "", tool_calls: calls }
      if (m.toolCalls[0]?.name) msg.name = m.toolCalls[0].name
      out.push(msg)
    } else {
      const content =
        typeof m.content === "string" ? m.content : (Array.isArray(m.content) ? m.content.map(toOpenAIContentBlock) : "")
      out.push({ role: m.role, content })
    }
  }
  return out
}

/** 统一内部多模态内容块 → Anthropic 格式：`{ type: "image", mime, data }` → `{ type: "image", source: { base64 } }`。 */
function toAnthropicContentBlock(b: Record<string, unknown>): Record<string, unknown> {
  if (b.type === "image" && typeof b.mime === "string" && typeof b.data === "string") {
    return { type: "image", source: { type: "base64", media_type: b.mime, data: b.data } }
  }
  if (b.type === "text" && typeof b.text === "string") return { type: "text", text: b.text }
  return b
}

function toAnthropicMessages(msgs: MessageLike[]): Array<Record<string, unknown>> {
  const out: Array<Record<string, unknown>> = []
  for (const m of repairToolPairing(msgs, { flushTail: true })) {
    if (m.role === "tool") {
      out.push({ role: "user", content: [{ type: "tool_result", tool_use_id: m.toolCallId, content: typeof m.content === "string" ? m.content : "" }] })
    } else if (m.role === "assistant" && m.toolCalls) {
      out.push({
        role: "assistant",
        content: [
          { type: "text", text: typeof m.content === "string" ? m.content : "" },
          ...m.toolCalls.map((tc) => ({
            type: "tool_use",
            id: tc.id,
            name: tc.name,
            input: typeof tc.arguments === "string" ? {} : tc.arguments,
          })),
        ],
      })
    } else if (m.role === "system") {
      // system messages folded by the caller; ignore here to keep ordering valid
    } else {
      // 文本内容统一为块数组（与 tool_result/tool_use 分支同构），使相邻同角色消息可合并
      const content: unknown =
        typeof m.content === "string"
          ? (m.content ? [{ type: "text", text: m.content }] : "")
          : (Array.isArray(m.content) ? m.content.map(toAnthropicContentBlock) : "")
      out.push({ role: m.role, content })
    }
  }
  // 相邻同角色合并（Anthropic 要求 user/assistant 交替）：配对修复在 tool_result 后可能紧跟 user 文本、
  // 历史装载的连续 user/assistant 消息同理——content 数组拼接为单条消息
  const merged: Array<Record<string, unknown>> = []
  for (const m of out) {
    const prev = merged[merged.length - 1]
    if (prev && prev.role === m.role && Array.isArray(prev.content) && Array.isArray(m.content)) {
      prev.content = [...(prev.content as unknown[]), ...(m.content as unknown[])]
    } else {
      merged.push(m)
    }
  }
  return merged
}

function capabilities(config: ProviderConfig): LLMCapabilities {
  return {
    streaming: true,
    toolCalling: true,
    multimodal: config.multimodal,
    maxContextTokens: config.maxContextTokens,
  }
}

/**
 * 统一内部消息 → OpenAI Responses API `input` 格式：
 * 普通消息原样透传；assistant 工具调用拆为独立 `function_call` item（后跟 `function_call_output` tool 消息），
 * 多模态图片块转 `image_url`（同 Chat Completions）。
 */
function toResponsesInput(msgs: MessageLike[]): Array<Record<string, unknown>> {
  const out: Array<Record<string, unknown>> = []
  for (const m of repairToolPairing(msgs, { flushTail: true })) {
    if (m.role === "tool") {
      out.push({ type: "function_call_output", call_id: m.toolCallId, output: typeof m.content === "string" ? m.content : "" })
    } else if (m.role === "assistant" && m.toolCalls) {
      const content =
        typeof m.content === "string" ? (m.content || "") : (Array.isArray(m.content) ? m.content.map(toOpenAIContentBlock) : "")
      out.push({ role: "assistant", content })
      for (const tc of m.toolCalls) {
        out.push({
          type: "function_call",
          call_id: tc.id,
          name: tc.name,
          arguments: typeof tc.arguments === "string" ? tc.arguments : JSON.stringify(tc.arguments),
        })
      }
    } else {
      const content =
        typeof m.content === "string" ? m.content : (Array.isArray(m.content) ? m.content.map(toOpenAIContentBlock) : "")
      out.push({ role: m.role, content })
    }
  }
  return out
}

function responsesTool(t: LLMToolDef): Record<string, unknown> {
  return { type: "function", name: t.name, description: t.description, parameters: t.parameters }
}

export function createProvider(config: ProviderConfig): LLMProvider {
  if (config.apiKind === "anthropic") return new AnthropicProvider(config)
  if (config.apiKind === "responses") return new ResponsesProvider(config)
  return new OpenAIProvider(config)
}

const API_KINDS: ApiKind[] = ["openai", "responses", "anthropic"]

/**
 * 任务级主模型配置覆盖：从会话/任务 env 提取 `GEBAI_LLM_*` 覆盖启动配置
 * （模型/接口地址/密钥/类型/上下文预算/多模态声明；`GEBAI_LLM_EXTRA_PARAMS` 由引擎单独经 ChatOptions 合并，不在此列）。
 * 无任何覆盖键时返回 null（调用方沿用启动 Provider 实例，避免无谓重建）。
 */
export function modelEnvOverrides(env: Record<string, string> | undefined): Partial<ProviderConfig> | null {
  if (!env) return null
  const out: Partial<ProviderConfig> = {}
  let any = false
  if (env.GEBAI_LLM_MODEL) {
    out.model = env.GEBAI_LLM_MODEL
    any = true
  }
  if (env.GEBAI_LLM_API_BASE) {
    out.apiBase = env.GEBAI_LLM_API_BASE
    any = true
  }
  if (env.GEBAI_LLM_API_KEY) {
    out.apiKey = env.GEBAI_LLM_API_KEY
    any = true
  }
  if (env.GEBAI_LLM_API_KIND && API_KINDS.includes(env.GEBAI_LLM_API_KIND as ApiKind)) {
    out.apiKind = env.GEBAI_LLM_API_KIND as ApiKind
    any = true
  }
  const maxCtx = Number(env.GEBAI_LLM_MAX_CONTEXT)
  if (env.GEBAI_LLM_MAX_CONTEXT !== undefined && Number.isFinite(maxCtx) && maxCtx > 0) {
    out.maxContextTokens = maxCtx
    any = true
  }
  if (env.GEBAI_LLM_MULTIMODAL !== undefined) {
    out.multimodal = env.GEBAI_LLM_MULTIMODAL === "true"
    any = true
  }
  return any ? out : null
}

/** 启动配置 + 任务级覆盖合并：无覆盖时返回原配置引用（调用方可据此直接复用启动 Provider）。 */
export function applyModelEnvOverrides(base: ProviderConfig, env: Record<string, string> | undefined): ProviderConfig {
  const o = modelEnvOverrides(env)
  return o ? { ...base, ...o } : base
}

/**
 * 任务级视觉（多模态）Provider 解析：`GEBAI_VISION_*` env 覆盖启动视觉配置；
 * 未配置视觉模型时回落到声明多模态能力的主模型（含任务级 `GEBAI_LLM_MULTIMODAL` 覆盖）。
 * 返回 null 表示视觉不可用。
 */
export function resolveVisionProvider(mainCfg: ProviderConfig, visionCfg: ProviderConfig | null, env: Record<string, string> | undefined): LLMProvider | null {
  const main = applyModelEnvOverrides(mainCfg, env)
  const vModel = env?.GEBAI_VISION_MODEL || visionCfg?.model
  if (!vModel) return main.multimodal ? createProvider(main) : null
  const maxCtx = Number(env?.GEBAI_VISION_MAX_CONTEXT)
  const extra = parseExtraParamsSafe(env?.GEBAI_VISION_EXTRA_PARAMS)
  const merged: ProviderConfig = {
    apiKind: (env?.GEBAI_VISION_API_KIND && API_KINDS.includes(env.GEBAI_VISION_API_KIND as ApiKind) ? env.GEBAI_VISION_API_KIND : (visionCfg?.apiKind || main.apiKind)) as ApiKind,
    apiBase: env?.GEBAI_VISION_API_BASE || visionCfg?.apiBase || main.apiBase,
    apiKey: env?.GEBAI_VISION_API_KEY || visionCfg?.apiKey || main.apiKey,
    model: vModel,
    maxContextTokens: (env?.GEBAI_VISION_MAX_CONTEXT !== undefined && Number.isFinite(maxCtx) && maxCtx > 0 ? maxCtx : visionCfg?.maxContextTokens) || main.maxContextTokens,
    multimodal: true,
    extraParams: extra || visionCfg?.extraParams,
  }
  return createProvider(merged)
}

/** 任务级额外模型接口参数解析（GEBAI_LLM_EXTRA_PARAMS/GEBAI_VISION_EXTRA_PARAMS）：非法 JSON 静默忽略，不阻塞任务。 */
function parseExtraParamsSafe(raw: string | undefined): Record<string, unknown> | undefined {
  if (raw === undefined) return undefined
  try {
    return parseExtraParams(raw)
  } catch {
    return undefined
  }
}

class OpenAIProvider implements LLMProvider {
  readonly id = "openai"
  constructor(private config: ProviderConfig) {}

  capabilities(): LLMCapabilities {
    return capabilities(this.config)
  }

  async *chat(msgs: MessageLike[], opts: ChatOptions = {}): AsyncIterable<LLMChunk> {
    assertApiConfigured(this.config)
    const body: Record<string, unknown> = {
      model: this.config.model,
      messages: toOpenAIMessages(msgs),
      stream: true,
      // 末 chunk 返回 usage 真值（prompt/completion/total tokens；服务端不支持时忽略 → 估算兜底）
      stream_options: { include_usage: true },
    }
    if (opts.tools?.length) body.tools = opts.tools.map(openaiTool)
    if (opts.maxTokens) body.max_tokens = opts.maxTokens
    // 额外模型接口参数：Provider 级固定参数 + 本次调用参数（后者优先），顶层合并进请求体
    Object.assign(body, this.config.extraParams, opts.extraParams)
    const res = await fetchRetry(`${this.config.apiBase}/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: `Bearer ${this.config.apiKey}` },
      body: JSON.stringify(body),
      signal: opts.signal,
    })
    if (!res.ok || !res.body) {
      const detail = (await res.text()).slice(0, 200)
      throw new Error(`模型接口错误（HTTP ${res.status}）: ${detail || res.statusText}`)
    }
    // Aggregate tool-call fragments by index: id/name/arguments arrive across chunks.
    const toolCallsByIndex = new Map<number, { id: string; name: string; args: string }>()
    // usage 真值在 finish_reason 之后的末 chunk（include_usage）到达：记录 finish_reason 并延迟 done 产出，把 usage 一并挂到 done
    let pendingStop: string | undefined
    let usage: LLMUsage | undefined
    for await (const data of parseSSE(res.body, opts.signal)) {
      if (data === "[DONE]") break
      let parsed: Record<string, unknown>
      try {
        parsed = JSON.parse(data)
      } catch {
        continue
      }
      const u = pickUsage(parsed.usage as Record<string, unknown> | undefined)
      if (u) usage = u
      const choice = (parsed.choices as Array<Record<string, unknown>>)?.[0]
      if (!choice) continue
      const delta = (choice.delta as Record<string, unknown>) || {}
      const content = delta.content as string | undefined
      if (content) {
        // DeepSeek 思考标记泄漏（自部署接口未分离）：整轮作废，抛错由引擎层重试（不净化保留）
        yield { type: "text", text: content }
      }
      // 推理内容（如 DeepSeek 的 reasoning_content）：泄漏标记同样作废整轮（用户实测两字段均可能出现）
      const reasoning = delta.reasoning_content as string | undefined
      if (reasoning) {
        yield { type: "reasoning", text: reasoning }
      }
      const tcs = delta.tool_calls as Array<Record<string, unknown>> | undefined
      if (tcs) {
        for (const tc of tcs) {
          const idx = Number(tc.index ?? 0)
          const cur = toolCallsByIndex.get(idx) || { id: "", name: "", args: "" }
          if (tc.id) cur.id = tc.id as string
          const fn = (tc.function as Record<string, unknown>) || {}
          if (fn.name) cur.name = fn.name as string
          if (typeof fn.arguments === "string") cur.args += fn.arguments
          toolCallsByIndex.set(idx, cur)
        }
      }
      if ((choice.finish_reason as string) === "stop" || (choice.finish_reason as string) === "tool_calls") {
        pendingStop = choice.finish_reason as string
      }
    }
    if (pendingStop !== undefined) {
      yield { type: "done", stopReason: pendingStop, ...(usage ? { usage } : {}) }
    } else {
      // 异常断流（无 finish_reason 即连接结束）：补 done 保持三家语义一致（下游终止信号不缺位）
      yield { type: "done", stopReason: "stop" }
    }
    for (const tc of toolCallsByIndex.values()) {
      if (!tc.name) continue
      const pa = parseArgs(tc.args)
      yield { type: "tool_call", toolCall: { id: tc.id, name: tc.name, arguments: pa.args }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
    }
  }
}

class ResponsesProvider implements LLMProvider {
  readonly id = "responses"
  constructor(private config: ProviderConfig) {}

  capabilities(): LLMCapabilities {
    return capabilities(this.config)
  }

  async *chat(msgs: MessageLike[], opts: ChatOptions = {}): AsyncIterable<LLMChunk> {
    assertApiConfigured(this.config)
    const body: Record<string, unknown> = {
      model: this.config.model,
      input: toResponsesInput(msgs),
      stream: true,
    }
    if (opts.tools?.length) body.tools = opts.tools.map(responsesTool)
    if (opts.maxTokens) body.max_output_tokens = opts.maxTokens
    // 额外模型接口参数：Provider 级固定参数 + 本次调用参数（后者优先），顶层合并进请求体
    Object.assign(body, this.config.extraParams, opts.extraParams)
    const base = this.config.apiBase.replace(/\/+$/, "")
    const res = await fetchRetry(`${base}/responses`, {
      method: "POST",
      headers: { "Content-Type": "application/json", Authorization: `Bearer ${this.config.apiKey}` },
      body: JSON.stringify(body),
      signal: opts.signal,
    })
    if (!res.ok || !res.body) {
      const detail = (await res.text()).slice(0, 200)
      throw new Error(`模型接口错误（HTTP ${res.status}）: ${detail || res.statusText}`)
    }
    // 函数调用按 item_id 聚合：call_id/name 由 output_item.added 提供，参数由 arguments delta 累积
    const calls = new Map<string, { callId: string; name: string; args: string }>()
    let done = false
    for await (const data of parseSSE(res.body, opts.signal)) {
      let evt: Record<string, unknown>
      try {
        evt = JSON.parse(data)
      } catch {
        continue
      }
      const type = evt.type as string
      if (type === "response.output_item.added") {
        const item = evt.item as Record<string, unknown> | undefined
        if (item?.type === "function_call" && typeof item.id === "string") {
          calls.set(item.id, { callId: String(item.call_id ?? ""), name: String(item.name ?? ""), args: "" })
        }
      } else if (type === "response.function_call_arguments.delta") {
        const cur = calls.get(String(evt.item_id))
        if (cur && typeof evt.delta === "string") cur.args += evt.delta
      } else if (type === "response.function_call_arguments.done") {
        const cur = calls.get(String(evt.item_id))
        if (cur) {
          if (typeof evt.arguments === "string") cur.args = evt.arguments
          const pa = parseArgs(cur.args)
          yield { type: "tool_call", toolCall: { id: cur.callId, name: cur.name, arguments: pa.args }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
          calls.delete(String(evt.item_id))
        }
      } else if (type === "response.output_text.delta") {
        if (typeof evt.delta === "string") {
          // DeepSeek 思考标记泄漏（自部署接口未分离）：整轮作废，抛错由引擎层重试（不净化保留）
          yield { type: "text", text: evt.delta }
        }
      } else if (type === "response.reasoning_summary_text.delta" || type === "response.reasoning_text.delta") {
        if (typeof evt.delta === "string") {
          yield { type: "reasoning", text: evt.delta }
        }
      } else if (type === "response.completed") {
        const resp = evt.response as Record<string, unknown> | undefined
        const output = ((resp?.output as Array<Record<string, unknown>> | undefined) || [])
        let stopReason = "stop"
        for (let i = output.length - 1; i >= 0; i--) {
          const fr = output[i]?.finish_reason as string | undefined
          if (fr) {
            stopReason = fr
            break
          }
        }
        done = true
        // usage 真值（input/output/total tokens）：与 stopReason 同挂 done chunk
        const usage = pickUsage(resp?.usage as Record<string, unknown> | undefined)
        yield { type: "done", stopReason, ...(usage ? { usage } : {}) }
      } else if (type === "response.failed" || type === "error") {
        const err = (evt.error as Record<string, unknown>) || evt
        throw new Error(`模型接口错误: ${String(err?.message ?? "流式响应失败")}`)
      }
    }
    // 兜底：completed 未触发（异常断流）时清掉尚未产出的函数调用并补 done
    for (const c of calls.values()) {
      if (!c.name) continue
      const pa = parseArgs(c.args)
      yield { type: "tool_call", toolCall: { id: c.callId, name: c.name, arguments: pa.args }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
    }
    if (!done) yield { type: "done", stopReason: "stop" }
  }
}

class AnthropicProvider implements LLMProvider {
  readonly id = "anthropic"
  constructor(private config: ProviderConfig) {}

  capabilities(): LLMCapabilities {
    return capabilities(this.config)
  }

  async *chat(msgs: MessageLike[], opts: ChatOptions = {}): AsyncIterable<LLMChunk> {
    assertApiConfigured(this.config)
    const system = msgs.filter((m) => m.role === "system").map((m) => String(m.content)).join("\n")
    const body: Record<string, unknown> = {
      model: this.config.model,
      messages: toAnthropicMessages(msgs),
      max_tokens: opts.maxTokens || 4096,
      stream: true,
    }
    if (system) body.system = system
    if (opts.tools?.length) body.tools = opts.tools.map(anthropicTool)
    // 额外模型接口参数：Provider 级固定参数 + 本次调用参数（后者优先），顶层合并进请求体
    Object.assign(body, this.config.extraParams, opts.extraParams)
    const base = this.config.apiBase.replace(/\/+$/, "")
    const res = await fetchRetry(`${base}/v1/messages`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "x-api-key": this.config.apiKey,
        "anthropic-version": "2023-06-01",
      },
      body: JSON.stringify(body),
      signal: opts.signal,
    })
    if (!res.ok || !res.body) {
      const detail = (await res.text()).slice(0, 200)
      throw new Error(`模型接口错误（HTTP ${res.status}）: ${detail || res.statusText}`)
    }
    let currentTool: { id: string; name: string; args: string } | null = null
    let doneYielded = false
    // usage 真值：message_start 的 input_tokens（含 system 与工具 schema，即「真上下文」）+ message_delta 的 output_tokens
    let inputTokens: number | undefined
    for await (const data of parseSSE(res.body, opts.signal)) {
      let evt: Record<string, unknown>
      try {
        evt = JSON.parse(data)
      } catch {
        continue
      }
      const type = evt.type as string
      if (type === "message_start") {
        const u = pickUsage((evt.message as Record<string, unknown> | undefined)?.usage as Record<string, unknown> | undefined)
        if (u?.inputTokens !== undefined) inputTokens = u.inputTokens
      } else if (type === "content_block_start") {
        const block = evt.content_block as Record<string, unknown> | undefined
        if (block?.type === "tool_use") {
          currentTool = { id: block.id as string, name: block.name as string, args: "" }
        }
      } else if (type === "content_block_delta") {
        const delta = evt.delta as Record<string, unknown> | undefined
        if (delta?.type === "text_delta" && delta.text) {
          // DeepSeek 思考标记泄漏：整轮作废，抛错由引擎层重试（不净化保留）
          yield { type: "text", text: delta.text as string }
        } else if (delta?.type === "thinking_delta" && delta.thinking) {
          yield { type: "reasoning", text: delta.thinking as string }
        } else if (delta?.type === "input_json_delta" && currentTool) currentTool.args += delta.partial_json as string
      } else if (type === "content_block_stop") {
        if (currentTool) {
          const pa = parseArgs(currentTool.args)
          yield { type: "tool_call", toolCall: { id: currentTool.id, name: currentTool.name, arguments: pa.args }, ...(pa.error ? { toolArgsError: pa.error } : {}) }
          currentTool = null
        }
      } else if (type === "message_delta") {
        const delta = evt.delta as Record<string, unknown> | undefined
        const out = pickUsage(evt.usage as Record<string, unknown> | undefined)?.outputTokens
        const usage = inputTokens !== undefined || out !== undefined ? { inputTokens, outputTokens: out } : undefined
        if (delta?.stop_reason) {
          doneYielded = true
          yield { type: "done", stopReason: delta.stop_reason as string, ...(usage ? { usage } : {}) }
        }
      }
    }
    // 兜底 done（恰好一次）：正常流 message_delta 已产出则跳过；异常断流补终止信号
    if (!currentTool && !doneYielded) yield { type: "done", stopReason: "end_turn" }
  }
}

function parseArgs(raw: string): { args: Record<string, unknown>; error?: string } {
  if (!raw) return { args: {} }
  try {
    return { args: JSON.parse(raw) }
  } catch {
    return { args: {}, error: raw.slice(0, 500) }
  }
}

/** 从接口返回的 usage 对象提取统一结构（OpenAI prompt_tokens/completion_tokens 与 Responses/Anthropic input_tokens/output_tokens 两种命名）。 */
function pickUsage(u: Record<string, unknown> | undefined): LLMUsage | undefined {
  if (!u) return undefined
  const num = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? v : undefined)
  const inputTokens = num(u.input_tokens) ?? num(u.prompt_tokens)
  const outputTokens = num(u.output_tokens) ?? num(u.completion_tokens)
  const totalTokens = num(u.total_tokens)
  if (inputTokens === undefined && outputTokens === undefined && totalTokens === undefined) return undefined
  return { inputTokens, outputTokens, totalTokens }
}

/** 图片扩展名 → MIME（视觉/多模态图片支持白名单，Anthropic 仅接受 png/jpeg/gif/webp）。 */
export const VISION_IMAGE_MIME: Record<string, string> = {
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
}

/**
 * 构造多模态用户消息内容块（统一内部格式：文本 + base64 图片），
 * `provider.chat()` 按接口规范转换（OpenAI `image_url` / Anthropic `image`）。
 * 供视觉工具（vision）等直接携带图片调用模型。
 */
export function imageMessageBlocks(text: string, mime: string, base64: string): Array<Record<string, unknown>> {
  return [{ type: "text", text }, { type: "image", mime, data: base64 }]
}
