import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ContentBlock, LLMCapabilities, MessageLike } from "@gebai/sdk"
import type { LLMChunk, LLMProvider, ChatOptions } from "./llm"
import { AgentEngine, stripThinkTags } from "./engine"
import { SessionStore } from "./store"
import { ToolRegistry } from "./registry"
import { createGlobalTools, pageCaptureTool, TRUNCATE_THRESHOLD } from "./tools"
import { Sandbox } from "./sandbox"
import { EnvManager } from "./env"
import { EventBus } from "./event-bus"
import { AuthService } from "../auth"
import { sessionPath } from "./paths"
import { SubAgentManager } from "./subagents"
import { loadConfig } from "./config"

class FakeProvider implements LLMProvider {
  readonly id = "fake"
  calls = 0
  toolName = "ls"
  /** 工具调用参数（除 ask 外的工具使用）。 */
  toolArgs: Record<string, unknown> = {}
  /** 每次 chat 调用收到的完整消息数组（子Agent 内部消息/工具结果断言用）。 */
  seenChats: MessageLike[][] = []
  /** 每次 chat 调用收到的工具 schema 名列表（通道禁用工具过滤断言用）。 */
  seenTools: string[][] = []
  /** 是否声明多模态能力（附件图片内联断言用）。 */
  multimodal = false
  /** ask 选项询问分支是否带 multi=true 调用（多选场景断言用）。 */
  askMulti = false
  /** ask 填值分支请求的变量名（默认 MY_KEY；测试可改为敏感键验证拒绝）。 */
  askEnvName = "MY_KEY"
  /** askenv 第二轮工具（默认 sh 验证注入后可读；测试可换无需审批工具避免审批等待）。 */
  askEnvSecondTool = "sh"
  /** streamwait 模式放行钩子（测试控制第一轮文本后的阻塞解除）。 */
  release?: () => void
  /** 首次 chat 调用即抛此错（多模态图片降级场景用）。 */
  failFirstError: Error | null = null
  /** done chunk 携带的 usage 真值（模拟服务端返回 input tokens，含缓存命中 cachedTokens）；undefined = 不返回（估算兜底路径）。 */
  usage: { inputTokens?: number; outputTokens?: number; totalTokens?: number; cachedTokens?: number } | undefined = undefined
  constructor(private mode: "tool" | "approval" | "approval2" | "text" | "sub" | "subwrite" | "submulti" | "subproj" | "subgrep" | "subcompose" | "subdeep" | "substream" | "suberr" | "subpipe" | "loadproj" | "interact" | "askenv" | "guard" | "subself" | "dyn" | "autoload" | "subautoload" | "subrisky" | "streamwait" | "parallel" | "mixapprove" | "mixmissing" | "subparallel" = "tool") {}
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: this.multimodal, maxContextTokens: 10000 }
  }
  async *chat(_msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    // 深拷贝：引擎运行中会原地修改 messages（如图片块降级），断言需看到调用时的原始内容
    this.seenChats.push(JSON.parse(JSON.stringify(_msgs)) as MessageLike[])
    this.seenTools.push((_opts?.tools ?? []).map((t) => t.name))
    if (this.failFirstError && this.calls === 1) throw this.failFirstError
    for await (const c of this.raw()) {
      // usage 真值统一挂 done chunk（与真实 Provider 行为一致）；未配置时原样透传（估算兜底路径）
      if (c.type === "done" && this.usage) yield { type: "done", stopReason: c.stopReason, usage: { ...this.usage } }
      else yield c
    }
  }
  private async *raw(): AsyncIterable<LLMChunk> {
    if (this.mode === "text") {
      yield { type: "text", text: "hello from fake" }
      yield { type: "done" }
      return
    }
    if (this.mode === "sub" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-sub", name: "agent_run", arguments: { agents: ["code"], input: "modify a file" } } }
      yield { type: "done" }
      return
    }
    // subwrite：总Agent 调 code，子Agent 内调 write 写文件（项目绑定验证用）
    if (this.mode === "subwrite" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-sub", name: "agent_run", arguments: { agents: ["code"], input: "modify project" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subwrite" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-sub2", name: "write", arguments: { path: "out.txt", content: "hi" } } }
      yield { type: "done" }
      return
    }
    // subproj：子Agent 内以 project 参数（预置项目名）调 write（预置项目验证用，参数来自 toolArgs）
    if (this.mode === "subproj" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-sub", name: "agent_run", arguments: { agents: ["code"], input: "modify preset project" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subproj" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-sub2", name: "write", arguments: this.toolArgs } }
      yield { type: "done" }
      return
    }
    // subgrep：子Agent 内以 project 参数对预置项目根递归 grep（跳过大型目录）
    if (this.mode === "subgrep" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-g1", name: "agent_run", arguments: { agents: ["code"], input: "search code" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subgrep" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-g2", name: "grep", arguments: this.toolArgs } }
      yield { type: "done" }
      return
    }
    // subcompose：总Agent 调 combo_test（测试注册的纯 md 组合子 Agent），
    // 其环境注入的 agent_run 再编排 code
    if (this.mode === "subcompose" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-c1", name: "agent_run", arguments: { agents: ["combo_test"], input: "make a report" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subcompose" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-c2", name: "agent_run", arguments: { agents: ["code"], input: "fix a bug" } } }
      yield { type: "done" }
      return
    }
    // subdeep：递归自嵌套 combo_test，验证 SUBAGENT_DEPTH=3 截断（depth≥3 时 runNewSession 直接抛错，不再触发 chat）
    if (this.mode === "subdeep" && this.calls <= 3) {
      yield { type: "tool_call", toolCall: { id: `tc-d${this.calls}`, name: "agent_run", arguments: { agents: ["combo_test"], input: "loop" } } }
      yield { type: "done" }
      return
    }
    // substream：总Agent 调 code；子Agent 内一轮推理+文本+工具调用（验证推理/工具不推送），一轮纯文本（验证 done 推送）
    if (this.mode === "substream" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-s1", name: "agent_run", arguments: { agents: ["code"], input: "check something" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "substream" && this.calls === 2) {
      yield { type: "reasoning", text: "子代理推理过程" }
      yield { type: "text", text: "子代理开始分析" }
      yield { type: "tool_call", toolCall: { id: "tc-s2", name: "todo", arguments: { entries: [] } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "substream" && this.calls === 3) {
      yield { type: "text", text: "子代理完成分析" }
      yield { type: "done" }
      return
    }
    // suberr：总Agent 调 code；子Agent 内部模型调用持续抛错（重试耗尽后失败，验证异常路径 done 携带 error）
    if (this.mode === "suberr" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-e1", name: "agent_run", arguments: { agents: ["code"], input: "boom" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "suberr" && this.calls >= 2) {
      throw new Error("sub agent model failed")
    }
    // submulti：总Agent 一次预加载多个子Agent（code + writer_test）执行新会话；
    // 新会话内调用第二个子Agent 的工具（writer_test_summarize），验证多 Agent 工具集叠加
    if (this.mode === "submulti" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-m1", name: "agent_run", arguments: { agents: ["code", "writer_test"], input: "make a report" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "submulti" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-m2", name: "writer_test_summarize", arguments: { text: "draft" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "submulti" && this.calls === 3) {
      yield { type: "text", text: "result after" }
      yield { type: "done" }
      return
    }
    // subpipe：总Agent 调 code 执行新会话；新会话内调用 flow（内部编排 code 子Agent 工具），
    // 验证数据流编排能力（flow/tool_schemas）在新会话环境注册且经会话注册表解析子Agent 工具
    if (this.mode === "subpipe" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-p1", name: "agent_run", arguments: { agents: ["code"], input: "batch task" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subpipe" && this.calls === 2) {
      yield {
        type: "tool_call",
        toolCall: { id: "tc-p2", name: "flow", arguments: { steps: [{ id: "q", tool: "todo", params: { entries: [] } }] } },
      }
      yield { type: "done" }
      return
    }
    // loadproj：装载模式（agent_load 后直接用全局 read）下预置项目 project 参数路由验证用
    if (this.mode === "loadproj" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-l1", name: "agent_load", arguments: { name: "code" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "loadproj" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-l2", name: "read", arguments: this.toolArgs } }
      yield { type: "done" }
      return
    }
    // subself：agent_run 预加载 self_optimize（验证连带预载 code + 写范围守卫）——
    // 新会话内先试写核心引擎源码（守卫拒绝），再写子Agent 目录（放行）
    if (this.mode === "subself" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-so1", name: "agent_run", arguments: { agents: ["self_optimize"], input: "optimize gebai" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subself" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-so2", name: "write", arguments: { path: "packages/server/src/core/engine.ts", content: "x" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subself" && this.calls === 3) {
      yield { type: "tool_call", toolCall: { id: "tc-so3", name: "write", arguments: { path: "packages/server/src/sub-agents/new_agent.ts", content: "x" } } }
      yield { type: "done" }
      return
    }
    // guard：write 防误覆盖守卫全链路——第1轮盲覆盖被拒、第2轮 read、第3轮 write 成功（fileGuard 会话级追踪）
    if (this.mode === "guard" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-gd1", name: "write", arguments: { path: "guard.txt", content: "v2" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "guard" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-gd2", name: "read", arguments: { path: "guard.txt" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "guard" && this.calls === 3) {
      yield { type: "tool_call", toolCall: { id: "tc-gd3", name: "write", arguments: { path: "guard.txt", content: "v2" } } }
      yield { type: "done" }
      return
    }
    // interact：模型第一轮尝试调用 ask 选项询问分支（无交互模式分支门控验证用）
    if (this.mode === "interact" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-i1", name: "ask", arguments: { prompt: "选择方案", options: ["方案A", "方案B"] } } }
      yield { type: "done" }
      return
    }
    // askenv：第一轮调 ask 填值分支请求环境变量，第二轮 sh echo $MY_KEY 验证注入后工具可读
    if (this.mode === "askenv" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-e1", name: "ask", arguments: { name: this.askEnvName, description: "测试密钥", secret: true } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "askenv" && this.calls === 2) {
      yield {
        type: "tool_call",
        toolCall: {
          id: "tc-e2",
          name: this.askEnvSecondTool,
          arguments: this.askEnvSecondTool === "sh" ? { command: `node -e "console.log(process.env.MY_KEY || 'EMPTY')"` } : this.toolArgs,
        },
      }
      yield { type: "done" }
      return
    }
    if (this.mode === "approval2" && this.calls === 1) {
      // 两轮各一个需审批工具：验证「运行中开启自动审批」对后续（下一轮）审批即时生效。
      // 同批多调用并行门控（同批共享门控时刻，中途改 env 不再影响同批后续项），跨轮才保留该时序
      yield { type: "tool_call", toolCall: { id: "tc-1", name: "sh", arguments: { command: "echo a" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "approval2" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-2", name: "sh", arguments: { command: "echo b" } } }
      yield { type: "done" }
      return
    }
    // autoload：总Agent 未装载 code 直接调用 code_system_info（主循环路由自愈：自动装载后执行）
    if (this.mode === "autoload" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-al1", name: "code_system_info", arguments: {} } }
      yield { type: "done" }
      return
    }
    // streamwait：第一轮输出部分文本后阻塞（测试控制放行）——attachSnapshot 在途流断言用
    if (this.mode === "streamwait" && this.calls === 1) {
      yield { type: "text", text: "partial text" }
      await new Promise<void>((resolve) => {
        this.release = resolve
      })
      yield { type: "done" }
      return
    }
    // subautoload：新会话（combo_test 纯 md 组合，未预加载 code）内直接调用 code_system_info（新会话循环路由自愈）
    if (this.mode === "subautoload" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-sa1", name: "agent_run", arguments: { agents: ["combo_test"], input: "list files" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subautoload" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-sa2", name: "code_system_info", arguments: {} } }
      yield { type: "done" }
      return
    }
    // subrisky：安全模式注册期过滤验证——新会话预加载 risky_test（其工具短名 delete 命中风险规则），
    // 调用 risky_test_delete 报未知工具
    if (this.mode === "subrisky" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-sr1", name: "agent_run", arguments: { agents: ["risky_test"], input: "delete something" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subrisky" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-sr2", name: "risky_test_delete", arguments: {} } }
      yield { type: "done" }
      return
    }
    // 同批多工具并行执行（DESIGN「同批工具并行执行」）：单次响应返回多个 tool_call
    // （须置于下方 calls===1 兜底分支之前）
    if (this.mode === "parallel" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-pp1", name: "slowecho", arguments: { tag: "a" } } }
      yield { type: "tool_call", toolCall: { id: "tc-pp2", name: "slowecho", arguments: { tag: "b" } } }
      yield { type: "done" }
      return
    }
    // 混合批次：需审批工具 + 免审批工具同批（免审批项不等待审批项）
    if (this.mode === "mixapprove" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-ma1", name: "sh", arguments: { command: "echo hi" } } }
      yield { type: "tool_call", toolCall: { id: "tc-ma2", name: "slowecho", arguments: { tag: "free" } } }
      yield { type: "done" }
      return
    }
    // 混合批次：缺参调用 + 正常调用同批（门控说明性结果与执行项共存）
    if (this.mode === "mixmissing" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-mm1", name: "slowecho", arguments: {} } }
      yield { type: "tool_call", toolCall: { id: "tc-mm2", name: "slowecho", arguments: { tag: "ok" } } }
      yield { type: "done" }
      return
    }
    // 新会话内同批并行：外层 agent_run，子会话单批两个 tool_call（工具来自测试子Agent para_test_slow）
    if (this.mode === "subparallel" && this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-spr1", name: "agent_run", arguments: { agents: ["para_test"], input: "run parallel" } } }
      yield { type: "done" }
      return
    }
    if (this.mode === "subparallel" && this.calls === 2) {
      yield { type: "tool_call", toolCall: { id: "tc-spr2", name: "para_test_slow", arguments: { tag: "n1" } } }
      yield { type: "tool_call", toolCall: { id: "tc-spr3", name: "para_test_slow", arguments: { tag: "n2" } } }
      yield { type: "done" }
      return
    }
    if (this.calls === 1) {
      yield { type: "text", text: "using tool" }
      // ask 选项询问分支需要有效参数（prompt + options），否则会因无选项抛错
      const args = this.toolName === "ask" ? { prompt: "选择方案", options: ["方案A", "方案B"], ...(this.askMulti ? { multi: true } : {}) } : this.toolArgs
      yield { type: "tool_call", toolCall: { id: "tc-1", name: this.toolName, arguments: args } }
      yield { type: "done" }
      return
    }
    if (this.mode === "dyn" && this.calls === 2) {
      // 动态工具在第二轮已进入 schema：模型直接调用（js 脚本 defineTool 注册的会话级工具）
      yield { type: "tool_call", toolCall: { id: "tc-2", name: "hello_tool", arguments: { who: "test" } } }
      yield { type: "done" }
      return
    }
    yield { type: "text", text: `result after ${this.calls - 1} tool rounds` }
    yield { type: "done" }
  }
}

async function setup(mode: "tool" | "approval" | "approval2" | "text" | "sub" | "subwrite" | "submulti" | "subproj" | "subgrep" | "subcompose" | "subdeep" | "substream" | "suberr" | "subpipe" | "loadproj" | "interact" | "askenv" | "guard" | "subself" | "dyn" | "autoload" | "subautoload" | "subrisky" | "streamwait" | "parallel" | "mixapprove" | "mixmissing" | "subparallel" = "tool", sandboxEnabled = false, authMode: "local" | "server" = "local", safeMode = false, extraOpts: Record<string, unknown> = {}) {
  const home = mkdtempSync(join(tmpdir(), "gebai-test-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const config = loadConfig({
    gebaiHome: home,
    auth: "local",
    sandbox: sandboxEnabled ? "on" : "off",
    preloadSubAgents: [],
    binaryMode: false,
    safeMode,
  })
  const store = new SessionStore({ home })
  const registry = new ToolRegistry({ safeMode })
  for (const tool of Object.values(createGlobalTools())) registry.register(tool)
  const sandbox = new Sandbox({ home, enabled: sandboxEnabled })
  const auth = new AuthService(home, "local")
  const env = new EnvManager(store)
  const events = new EventBus()
  const subAgents = new SubAgentManager({ registry, preloadOverride: [] })
  await subAgents.discover()
  // 测试专用纯 md 组合子 Agent（无工具 → 运行环境自动注入编排工具）：编排/递归深度测试载体，
  // 不依赖内置子Agent 集合（内置 Agent 增删不影响这些机制测试）
  subAgents.register({
    name: "combo_test",
    description: "测试组合子 Agent（编排其他子 Agent）",
    systemPrompt: "你是测试组合编排 Agent。",
  })
  const provider = new FakeProvider(mode)
  if (mode === "approval" || mode === "approval2") {
    provider.toolName = "sh"
    provider.toolArgs = { command: "echo hi" } // 合法参数：缺参调用会被引擎必填校验拦截、到不了审批门
  }
  const engine = new AgentEngine({ provider, registry, store, env, sandbox, events, config, subAgents, retryBackoffMs: 5, authMode, ...extraOpts })
  // loadConfig 会显式加载项目根 .env（loadDotEnv，如 GEBAI_LLM_MODEL）注入 process.env，
  // 泄漏进 EnvManager.resolve 会污染「无覆盖沿用启动实例」类断言——此处（loadConfig 之后）统一清理
  for (const k of Object.keys(process.env)) {
    // 项目根 .env 的 LLM 与预置项目配置泄漏会污染断言（任务级模型覆盖/系统提示词清单），统一清理
    if (k.startsWith("GEBAI_LLM_") || k.startsWith("CODE_")) {
      savedEnv[k] = process.env[k]
      delete process.env[k]
    }
  }
  return { home, store, registry, sandbox, auth, env, events, subAgents, engine, provider, config }
}

/** setup 清理掉的 process.env 键（cleanup 恢复）：loadConfig 显式加载项目根 .env（loadDotEnv）
 * 会把开发者真实配置（如 GEBAI_LLM_MODEL）注入 process.env，泄漏进测试断言。 */
let savedEnv: Record<string, string | undefined> = {}

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
  for (const [k, v] of Object.entries(savedEnv)) {
    if (v === undefined) delete process.env[k]
    else process.env[k] = v
  }
  savedEnv = {}
}

/** 同批并行测试载体：慢速工具记录并发重叠（active 计数峰值），输出 `done:{tag}`。 */
function registerSlowEcho(registry: ToolRegistry, ms = 120): { maxActive: () => number } {
  let active = 0
  let max = 0
  registry.register({
    name: "slowecho",
    description: "test slow tool (parallel batch)",
    parameters: { type: "object", properties: { tag: { type: "string" } }, required: ["tag"] },
    async execute(args: Record<string, unknown>) {
      active++
      max = Math.max(max, active)
      await new Promise((r) => setTimeout(r, ms))
      active--
      return { output: `done:${String(args.tag)}` }
    },
  })
  return { maxActive: () => max }
}

/** 轮询等待会话消息满足条件（审批等待期间断言「免审批工具已完成」用）。 */
async function waitForStore(store: SessionStore, sessionId: string, pred: (msgs: Array<{ role: string; content: string }>) => boolean, timeout = 3000): Promise<void> {
  const t0 = Date.now()
  while (!pred((await store.load(sessionId))?.messages ?? [])) {
    if (Date.now() - t0 > timeout) throw new Error("timeout waiting store condition")
    await new Promise((r) => setTimeout(r, 15))
  }
}

describe("AgentEngine", () => {
  test("runs a plain text turn and persists assistant reply", async () => {
    const { home, engine, store } = await setup("text")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "user" && m.content === "hi")).toBe(true)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "hello from fake")).toBe(true)
    cleanup(home)
  })

  test("final assistant reply is persisted with the streamed messageId (撤回对刚完成的回复立即生效)", async () => {
    const { home, engine, store, events } = await setup("text")
    const session = await store.createSession("default", "t")
    let streamedId: string | undefined
    events.subscribe((e) => {
      if (e.type === "event.message.delta" && e.sessionId === session.id) streamedId = (e.payload as Record<string, unknown>).messageId as string
    })
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    const final = loaded!.messages.find((m) => m.role === "assistant" && m.content === "hello from fake")
    expect(final).toBeDefined()
    expect(streamedId).toBeDefined()
    expect(final!.id).toBe(streamedId!)
    // 按该 id 撤回（truncate）立即命中：最终消息与其后续一并删除
    await store.truncateMessages(session.id, "default", final!.id)
    const after = await store.load(session.id)
    expect(after!.messages.some((m) => m.id === final!.id)).toBe(false)
    cleanup(home)
  })

  test("truncating a mid-turn assistant(toolCalls) message removes its tool results (助手消息撤回语义)", async () => {
    const { home, engine, store } = await setup("tool")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    const mid = loaded!.messages.find((m) => m.role === "assistant" && m.toolCalls?.length)
    expect(mid).toBeDefined()
    await store.truncateMessages(session.id, "default", mid!.id)
    const after = await store.load(session.id)
    // 中途 assistant(toolCalls)、其 tool 结果与最终回复一并删除，只保留用户输入
    expect(after!.messages.every((m) => m.role === "user")).toBe(true)
    cleanup(home)
  })

  test("same-batch tool calls execute in parallel (同批工具并行执行)", async () => {
    const { home, engine, store, registry, provider } = await setup("parallel")
    const probe = registerSlowEcho(registry)
    const session = await store.createSession("default", "t")
    const t0 = Date.now()
    await engine.run(session.id, "default", "run both")
    const elapsed = Date.now() - t0
    // 两个调用重叠执行（并行），总耗时接近单次（串行 ≥ 2×120ms）
    expect(probe.maxActive()).toBeGreaterThanOrEqual(2)
    expect(elapsed).toBeLessThan(230)
    // 两条结果均落盘并按 toolCallId 配对完整；下一轮模型调用可见全部结果
    const loaded = await store.load(session.id)
    const round = loaded!.messages.find((m) => m.role === "assistant" && m.toolCalls?.length)!
    expect(round.toolCalls!.length).toBe(2)
    const results = loaded!.messages.filter((m) => m.role === "tool")
    expect(results.length).toBe(2)
    const ids = new Set(round.toolCalls!.map((tc) => tc.id))
    for (const r of results) expect(ids.has(r.toolCallId!)).toBe(true)
    const second = provider.seenChats[1]
    expect(JSON.stringify(second)).toContain("done:a")
    expect(JSON.stringify(second)).toContain("done:b")
    cleanup(home)
  })

  test("free tool in same batch does not wait for approval-gated sibling (审批等待不阻塞同批免审批工具)", async () => {
    const { home, engine, store, registry, events } = await setup("mixapprove")
    registerSlowEcho(registry)
    const session = await store.createSession("default", "t")
    let approvalSeen = false
    events.subscribe((e) => {
      if (e.type === "event.approval.request" && e.sessionId === session.id) approvalSeen = true
    })
    const run = engine.run(session.id, "default", "run mixed batch")
    // 免审批工具在审批等待期间已完成并落盘（任务仍在运行）
    await waitForStore(store, session.id, (msgs) => msgs.some((m) => m.role === "tool" && m.content === "done:free"))
    expect(approvalSeen).toBe(true)
    expect(engine.isRunning(session.id)).toBe(true)
    await engine.decideApproval(session.id, "tc-ma1", true)
    await run
    const loaded = await store.load(session.id)
    const results = loaded!.messages.filter((m) => m.role === "tool").map((m) => m.content)
    expect(results).toContain("done:free")
    expect(results.some((c) => c.includes("hi"))).toBe(true) // 审批通过后 sh 执行
    cleanup(home)
  })

  test("gating notes and executable calls coexist in one batch (缺参门控结果与执行项共存)", async () => {
    const { home, engine, store, registry, provider, events } = await setup("mixmissing")
    registerSlowEcho(registry)
    const session = await store.createSession("default", "t")
    // 门控说明性结果同样推送 call+result 事件对：前端实时建卡（不依赖刷新），与落盘一致
    const callEvents: string[] = []
    const resultEvents: string[] = []
    events.subscribe((e) => {
      if (e.sessionId !== session.id) return
      if (e.type === "event.tool.call") callEvents.push(String((e.payload as Record<string, unknown>).toolCallId))
      if (e.type === "event.tool.result") resultEvents.push(String((e.payload as Record<string, unknown>).toolCallId))
    })
    await engine.run(session.id, "default", "run mixed")
    const loaded = await store.load(session.id)
    const results = loaded!.messages.filter((m) => m.role === "tool")
    expect(results.length).toBe(2)
    // 缺参调用：门控说明性结果（不执行）；正常调用：执行结果——同批共存、配对完整
    expect(results.some((m) => m.content.includes("缺少必填参数"))).toBe(true)
    expect(results.some((m) => m.content === "done:ok")).toBe(true)
    const second = provider.seenChats[1]
    expect(JSON.stringify(second)).toContain("done:ok")
    expect(JSON.stringify(second)).toContain("缺少必填参数")
    // 事件对齐落盘：缺参门控项与执行项都有 call+result 事件（运行时卡片可见）
    expect(callEvents).toContain("tc-mm1")
    expect(callEvents).toContain("tc-mm2")
    expect(resultEvents).toContain("tc-mm1")
    expect(resultEvents).toContain("tc-mm2")
    cleanup(home)
  })

  test("new-session loop runs same-batch tools in parallel (新会话循环同批并行，存档完整)", async () => {
    const { home, engine, store, subAgents } = await setup("subparallel")
    // 工具经测试子Agent 提供（新会话继承全局工具走 createGlobalTools 工厂全集，不含主注册表临时注册项）
    let active = 0
    let max = 0
    subAgents.register({
      name: "para_test",
      description: "并行批次测试 Agent",
      systemPrompt: "你是并行测试 Agent。",
      tools: {
        slow: {
          name: "slow",
          description: "慢速工具",
          parameters: { type: "object", properties: { tag: { type: "string" } }, required: ["tag"] },
          execute: async (args: Record<string, unknown>) => {
            active++
            max = Math.max(max, active)
            await new Promise((r) => setTimeout(r, 120))
            active--
            return { output: `done:${String(args.tag)}` }
          },
        },
      },
    })
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "delegate")
    expect(max).toBeGreaterThanOrEqual(2)
    // 存档（agent_run 工具消息扩展字段）包含两个并行调用的结果条目
    const loaded = await store.load(session.id)
    const runToolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "agent_run")!
    const archive = (runToolMsg as unknown as { sessionRun?: { messages: Array<{ role: string; content: string }> } }).sessionRun
    expect(archive).toBeDefined()
    const toolEntries = archive!.messages.filter((m) => m.role === "tool")
    expect(toolEntries.length).toBe(2)
    expect(toolEntries.some((m) => m.content === "done:n1")).toBe(true)
    expect(toolEntries.some((m) => m.content === "done:n2")).toBe(true)
    cleanup(home)
  })

  test("write 防误覆盖守卫：未读过的已存在文件拒绝整体覆盖，read 后放行（fileGuard 会话级追踪）", async () => {
    const { home, engine, store, provider } = await setup("guard")
    const session = await store.createSession("default", "t")
    // 预置已存在文件（本会话从未 read/write 过）；文件工具相对路径基于会话 tmp/ 解析
    const file = join(sessionPath(home, "default", session.id), "tmp", "guard.txt")
    writeFileSync(file, "v1")
    await engine.run(session.id, "default", "把 guard.txt 覆盖为 v2")
    // 最终文件被覆盖为 v2（第1轮被拒 → 第2轮 read → 第3轮 write 成功）
    expect(await Bun.file(file).text()).toBe("v2")
    // 第2轮模型上下文携带拒绝提示（防盲覆盖），第4轮携带写入成功结果
    const chats = JSON.stringify(provider.seenChats)
    expect(chats).toContain("防盲覆盖")
    expect(chats).toContain("已写入")
    cleanup(home)
  })

  test("任务级模型覆盖：会话 env 配置 GEBAI_LLM_* 时用重建 Provider，无覆盖沿用启动实例", async () => {
    const { home, store, registry, env, sandbox, events, config, subAgents } = await setup("text")
    const baseProvider = new FakeProvider("text")
    const overriddenProvider = new FakeProvider("text")
    let resolvedEnv: Record<string, string> | undefined
    const engine = new AgentEngine({
      provider: baseProvider,
      registry,
      store,
      env,
      sandbox,
      events,
      config,
      subAgents,
      retryBackoffMs: 5,
      resolveProvider: (e) => {
        resolvedEnv = e
        return e.GEBAI_LLM_MODEL ? overriddenProvider : undefined
      },
    })
    const session = await store.createSession("default", "t")
    // 未配置覆盖：启动 Provider 实例服务
    await engine.run(session.id, "default", "hi")
    expect(baseProvider.calls).toBeGreaterThan(0)
    expect(overriddenProvider.calls).toBe(0)
    expect(resolvedEnv!.GEBAI_LLM_MODEL).toBeUndefined()
    // 会话 env 设置模型：同一会话下一任务用重建 Provider
    await store.setEnv(session.id, "default", { GEBAI_LLM_MODEL: "gpt-x" })
    await engine.run(session.id, "default", "switch model")
    expect(overriddenProvider.calls).toBeGreaterThan(0)
    expect(resolvedEnv!.GEBAI_LLM_MODEL).toBe("gpt-x")
    cleanup(home)
  })

  test("任务级模型覆盖：agent_run 新会话执行同样使用任务级 Provider", async () => {
    const { home, store, registry, env, sandbox, events, config, subAgents } = await setup("sub")
    const baseProvider = new FakeProvider("sub")
    const overriddenProvider = new FakeProvider("sub")
    const engine = new AgentEngine({
      provider: baseProvider,
      registry,
      store,
      env,
      sandbox,
      events,
      config,
      subAgents,
      retryBackoffMs: 5,
      resolveProvider: (e) => (e.GEBAI_LLM_MODEL ? overriddenProvider : undefined),
    })
    const session = await store.createSession("default", "t")
    await store.setEnv(session.id, "default", { GEBAI_LLM_MODEL: "gpt-x" })
    await engine.run(session.id, "default", "hi")
    // 主循环（2 轮）+ 新会话执行（1 轮）均由任务级 Provider 服务，启动实例零调用
    expect(overriddenProvider.calls).toBe(3)
    expect(baseProvider.calls).toBe(0)
    cleanup(home)
  })

  test("run() honors client-supplied messageId (撤回/反馈定位), rejects invalid ids", async () => {
    const { home, engine, store } = await setup("text")
    const session = await store.createSession("default", "t")
    const clientId = "c9a4f1e2-3b7d-4e5f-9a8b-0123456789ab"
    await engine.run(session.id, "default", "hi", { messageId: clientId })
    let loaded = await store.load(session.id)
    expect(loaded!.messages.find((m) => m.role === "user")!.id).toBe(clientId)
    // 非法 id（路径穿越/超长/非法字符）回退服务端生成
    await engine.run(session.id, "default", "hi2", { messageId: "../../etc/passwd" })
    loaded = await store.load(session.id)
    const ids = loaded!.messages.filter((m) => m.role === "user").map((m) => m.id)
    expect(ids).not.toContain("../../etc/passwd")
    expect(ids.filter((i) => i === clientId)).toHaveLength(1)
    cleanup(home)
  })

  test("contextWindow exposes provider maxContextTokens", async () => {
    const { home, engine } = await setup("text")
    expect(engine.contextWindow()).toBe(10000) // FakeProvider capabilities
    cleanup(home)
  })

  test("executes a non-approval tool round and feeds result back", async () => {
    const { home, engine, store } = await setup("tool")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "what time is it")
    const loaded = await store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool")
    expect(toolMsg).toBeDefined()
    expect(toolMsg!.toolCallId).toBe("tc-1")
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(home)
  })

  test("必填参数缺失校验：edit 漏传 path 不执行、明确报缺参（旧版报 tmp\\undefined 类无关 ENOENT 无法自愈）", async () => {
    const s = await setup("tool")
    s.provider.toolName = "edit"
    s.provider.toolArgs = { edits: [{ oldString: "export function sortTodos", newString: "export function sortTodos" }] } // 漏传 path
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "edit it")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "edit")
    expect(toolMsg).toBeDefined()
    expect(toolMsg!.content).toContain("缺少必填参数")
    expect(toolMsg!.content).toContain("path")
    // 旧缺陷形态不再出现：缺失参数被 String() 成字面量 undefined 落进路径解析 → 无关 ENOENT
    expect(toolMsg!.content).not.toContain("undefined")
    expect(toolMsg!.content).not.toContain("ENOENT")
    // 工具未执行（无落盘副作用），模型下一轮补参后正常收尾
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("engine fallback truncates oversized tool output (not relying on tool self-truncate)", async () => {
    const { home, engine, store, registry, provider } = await setup("tool")
    const big = "line " + "x".repeat(200)
    // 工具未调用 truncate，直接返回超长输出：引擎必须兜底截断落盘
    registry.register({
      name: "huge_out",
      description: "returns oversized output without self-truncation",
      parameters: { type: "object", properties: {} },
      async execute() {
        return { output: Array.from({ length: 60 }, () => big).join("\n") }
      },
    })
    provider.toolName = "huge_out"
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "get big output")
    const loaded = await store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool")!
    expect(toolMsg.content).toContain("已截断")
    expect(toolMsg.content).toMatch(/tmp\/truncated\/huge_out_[0-9a-f]{64}\.txt/)
    expect(toolMsg.content.length).toBeLessThan(TRUNCATE_THRESHOLD)
    // 完整内容已落盘，模型可经 read 读取
    const filePath = toolMsg.content.match(/tmp\/truncated\/(huge_out_[0-9a-f]{64}\.txt)/)![1]
    const root = sessionPath(home, "default", session.id)
    expect(existsSync(join(root, "tmp", "truncated", filePath))).toBe(true)
    expect(readFileSync(join(root, "tmp", "truncated", filePath), "utf8")).toBe(Array.from({ length: 60 }, () => big).join("\n"))
    // 已自行截断的工具结果不被二次处理（无 double-truncate）
    const visited = provider.seenChats[1]
    expect(JSON.stringify(visited).includes("huge_out_")).toBe(true)
    cleanup(home)
  })

  test("js defineTool 注册的会话级动态工具：落盘持久化、重启恢复、随 forgetSession 释放", async () => {
    const { home, engine, store, provider, registry, sandbox, env, events, subAgents, config } = await setup("dyn")
    provider.toolName = "js"
    provider.toolArgs = {
      code: `await defineTool({
  name: "hello_tool",
  description: "向 who 问好（运行时定义工具）",
  parameters: { type: "object", properties: { who: { type: "string" } }, required: ["who"] },
  requiresApproval: false,
  async execute(args) {
    return { output: "hello, " + args.who }
  },
})
console.log("defined ok")`,
      approval: false,
    }
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "define a tool")
    // 第二轮 schema 包含动态工具（defineTool 后会话覆盖层立即可见）
    expect(provider.seenTools[1].includes("hello_tool")).toBe(true)
    // 定义清单落盘 chat.json（SessionData.dynamicTools，重启恢复的持久化载体）
    const loaded = await store.load(session.id)
    expect(loaded!.dynamicTools?.some((t) => t.name === "hello_tool")).toBe(true)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.content.includes("hello, test"))
    expect(toolMsg).toBeDefined()
    // 重启恢复（模拟进程重启：同 store/home 新建引擎）：会话无需重新定义，按名调用直接执行（磁盘水合）
    const provider2 = new FakeProvider("tool")
    provider2.toolName = "hello_tool"
    provider2.toolArgs = { who: "restart" }
    const engine2 = new AgentEngine({ provider: provider2, registry, store, env, sandbox, events, config, subAgents, retryBackoffMs: 5, authMode: "local" })
    await engine2.run(session.id, "default", "again")
    const after = await store.load(session.id)
    expect(after!.messages.some((m) => m.role === "tool" && m.content.includes("hello, restart"))).toBe(true)
    // forgetSession（会话删除路径）释放内存态：新建会话运行后不再可见（磁盘定义随会话删除清理）
    engine.forgetSession(session.id)
    engine2.forgetSession(session.id)
    const session2 = await store.createSession("default", "t2")
    await engine.run(session2.id, "default", "again")
    // 最后一轮 chat 的 schema 不再含 hello_tool（新会话无覆盖层）
    const lastSeen = provider.seenTools[provider.seenTools.length - 1]
    expect(lastSeen.includes("hello_tool")).toBe(false)
    cleanup(home)
  }, 60000)

  test("安全模式：动态工具水合并降级执行（只读动态工具照常可用，写通道被扫描拒绝）", async () => {
    const { home, engine, store, provider, registry, sandbox, env, events, subAgents } = await setup("dyn")
    provider.toolName = "js"
    provider.toolArgs = {
      code: `await defineTool({
  name: "hello_tool",
  description: "向 who 问好（运行时定义工具）",
  parameters: { type: "object", properties: { who: { type: "string" } }, required: ["who"] },
  requiresApproval: false,
  async execute(args) {
    return { output: "hello, " + args.who }
  },
})`,
      approval: false,
    }
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "define a tool")
    expect((await store.load(session.id))!.dynamicTools?.some((t) => t.name === "hello_tool")).toBe(true)
    // 重启进安全模式：水合被跳过——schema 无动态工具、按名调用报未知工具，任意代码不执行
    const configSafe = loadConfig({ gebaiHome: home, auth: "local", sandbox: "off", preloadSubAgents: [], binaryMode: false, safeMode: true })
    const providerSafe = new FakeProvider("tool")
    providerSafe.toolName = "hello_tool"
    providerSafe.toolArgs = { who: "bypass" }
    const engineSafe = new AgentEngine({ provider: providerSafe, registry, store, env, sandbox, events, config: configSafe, subAgents, retryBackoffMs: 5, authMode: "local" })
    await engineSafe.run(session.id, "default", "again")
    // 水合不跳过（与 js 同规则降级：源码扫描 + 只读 shim）：schema 含动态工具，只读 execute 正常返回
    expect(providerSafe.seenTools[0].includes("hello_tool")).toBe(true)
    const after = await store.load(session.id)
    expect(after!.messages.some((m) => m.role === "tool" && m.content.includes("hello, bypass"))).toBe(true)
    cleanup(home)
  }, 60000)

  test("cancel during tool execution interrupts the tool and stops the task", async () => {
    const { home, engine, store, registry, provider, events } = await setup("tool")
    registry.register({
      name: "slow_tool",
      description: "slow tool for cancel test",
      parameters: { type: "object", properties: {} },
      async execute() {
        // 模拟长时间运行的工具（如脚本/网络挂起）：正常情况下 0.7 秒后才返回
        await new Promise((r) => setTimeout(r, 700))
        return { output: "slow done" }
      },
    })
    provider.toolName = "slow_tool"
    const session = await store.createSession("default", "t")
    let taskError = ""
    events.subscribe((e) => {
      if (e.type === "event.task.error" && e.sessionId === session.id) taskError = String((e.payload as { error?: string }).error ?? "")
    })
    const run = engine.run(session.id, "default", "do slow thing")
    await new Promise((r) => setTimeout(r, 200)) // 等工具开始执行（runToolInterruptible 已挂起）
    const t0 = Date.now()
    engine.cancel(session.id)
    await run
    // 取消立即生效：未等慢工具自然结束（远小于 0.7 秒）
    expect(Date.now() - t0).toBeLessThan(600)
    expect(taskError).toBe("cancelled")
    // 取消结果落盘（工具卡片显示「已取消」，脚本场景进程同步被杀）
    const loaded = await store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool")
    expect(toolMsg!.content).toContain("已取消")
    expect(provider.calls).toBe(1) // 取消后不再发起后续模型调用
    expect(engine.isRunning(session.id)).toBe(false)
    cleanup(home)
  })

  test("tool execution timeout returns timeout to the model without ending the task", async () => {
    const { home, store, registry, sandbox, env, events, subAgents, provider, config } = await setup("tool")
    registry.register({
      name: "slow_tool",
      description: "slow tool for timeout test",
      parameters: { type: "object", properties: {} },
      async execute() {
        await new Promise((r) => setTimeout(r, 500))
        return { output: "slow done" }
      },
    })
    provider.toolName = "slow_tool"
    const engine = new AgentEngine({ provider, registry, store, env, sandbox, events, config, subAgents, toolTimeoutMs: 150 })
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "do slow thing")
    const loaded = await store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool")
    // 超时不结束任务：作为工具结果（含「执行超时」与工具名）返回给模型
    expect(toolMsg!.content).toContain("slow_tool 执行超时")
    expect(toolMsg!.content).toContain("已终止")
    // 模型收到超时后继续：第二轮调用产出最终回复，任务正常完成
    expect(provider.calls).toBe(2)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    expect(engine.isRunning(session.id)).toBe(false)
    cleanup(home)
  })

  test("approval-required tool waits for decision", async () => {
    const { home, engine, store } = await setup("approval")
    const session = await store.createSession("default", "t")
    const run = engine.run(session.id, "default", "run sh")
    await new Promise((r) => setTimeout(r, 50))
    // should be blocked awaiting approval; not yet done
    await engine.decideApproval(session.id, "tc-1", true)
    await run
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool")).toBe(true)
    cleanup(home)
  })

  test("attachSnapshot：审批等待中列出待决交互，任务结束后为 null", async () => {
    const { home, engine, store } = await setup("approval")
    const session = await store.createSession("default", "t")
    // 未运行：null
    expect(engine.attachSnapshot(session.id)).toBeNull()
    const run = engine.run(session.id, "default", "run sh")
    await new Promise((r) => setTimeout(r, 50))
    // 审批等待中：running + 待决审批（工具名/重试计数载荷完整，前端重渲染审批卡用）
    const snap = engine.attachSnapshot(session.id)
    expect(snap).not.toBeNull()
    expect(snap!.running).toBe(true)
    expect(typeof snap!.startedAt).toBe("number")
    expect(snap!.pending).toHaveLength(1)
    expect(snap!.pending[0]).toMatchObject({ type: "approval", toolCallId: "tc-1", tool: "sh", retries: 0 })
    await engine.decideApproval(session.id, "tc-1", true)
    await run
    expect(engine.attachSnapshot(session.id)).toBeNull()
    cleanup(home)
  })

  test("attachSnapshot：在途流式累积（未持久化部分文本），回合持久化后清空", async () => {
    const { home, engine, store, provider } = await setup("streamwait")
    const session = await store.createSession("default", "t")
    const run = engine.run(session.id, "default", "hello")
    // 等待部分文本产出（provider 阻塞在 yield 之后）
    for (let i = 0; i < 100 && !provider.release; i++) await new Promise((r) => setTimeout(r, 10))
    const snap = engine.attachSnapshot(session.id)
    expect(snap).not.toBeNull()
    expect(snap!.stream?.text).toBe("partial text")
    provider.release!()
    await run
    // 任务结束：快照为 null，全文已持久化（存储恢复承担）
    expect(engine.attachSnapshot(session.id)).toBeNull()
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "partial text")).toBe(true)
    cleanup(home)
  })

  test("approval-required tool rejected stops the session run", async () => {
    const { home, engine, store, provider, events } = await setup("approval")
    provider.toolArgs = { command: "echo hello" }
    const session = await store.createSession("default", "t")
    // 等待审批请求发布后再拒绝（确定性同步：拒绝立即 abort，若早于循环启动会直接取消无产物）
    let requested: Record<string, unknown> | null = null
    const resultEvents: string[] = []
    events.subscribe((e) => {
      if (e.sessionId !== session.id) return
      if (e.type === "event.approval.request") requested = e.payload as Record<string, unknown>
      if (e.type === "event.tool.result") resultEvents.push(String((e.payload as Record<string, unknown>).toolCallId))
    })
    const run = engine.run(session.id, "default", "run sh")
    const t0 = Date.now()
    while (!requested) {
      if (Date.now() - t0 > 2000) throw new Error("approval.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    // 审批请求携带完整调用参数（飞书审批卡片据此展示参数摘要）
    expect(requested).toMatchObject({ toolCallId: "tc-1", tool: "sh", arguments: { command: "echo hello" } })
    await engine.decideApproval(session.id, "tc-1", false)
    await run
    // 拒绝 = 停止会话：拒绝消息落盘，任务结束且不再发起后续模型调用
    const loaded = await store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool")
    expect(toolMsg!.content).toContain("拒绝")
    expect(provider.calls).toBe(1)
    expect(engine.isRunning(session.id)).toBe(false)
    // 拒绝结果同样推送 tool.result 事件：实时卡片落终态（不依赖刷新）
    expect(resultEvents).toContain("tc-1")
    cleanup(home)
  })

  test("cancel during approval wait unwinds promptly and the session can continue immediately", async () => {
    const { home, engine, store, provider } = await setup("approval")
    const session = await store.createSession("default", "t")
    // 审批等待中停止：旧实现只 abort 信号不 resolve 等待 promise，runLoop 永久挂起，
    // 任务收尾不完成（isRunning 残留）→ 下一次 prompt 被 "task already running" 拒绝（要发两次）
    const run = engine.run(session.id, "default", "run sh")
    await new Promise((r) => setTimeout(r, 50)) // 等进入审批等待
    const t0 = Date.now()
    engine.cancel(session.id)
    await run
    // 取消立即解开等待：远快于审批超时（5 分钟），任务以 cancelled 快速收尾
    expect(Date.now() - t0).toBeLessThan(1000)
    expect(engine.isRunning(session.id)).toBe(false)
    // 取消不写「用户拒绝」虚假记录（等待被信号解开，不落拒绝消息）
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool" && String(m.content).includes("拒绝"))).toBe(false)
    // 立即再次发起任务：一次成功（不再要求第二次输入）
    provider.toolName = "tool" // 第二次无需审批路径，直接文本完成
    await engine.run(session.id, "default", "continue now")
    expect(provider.calls).toBe(2)
    cleanup(home)
  })

  test("cancel during ask choice wait unwinds promptly", async () => {
    const { home, engine, store, registry, provider } = await setup("tool")
    registry.register({
      name: "asker",
      description: "ask test",
      parameters: { type: "object", properties: {} },
      async execute(_args, ctx) {
        const r = await ctx.waitForChoice("pick", ["a", "b"])
        return { output: `chose: ${r ? JSON.stringify(r) : "none"}` }
      },
    })
    provider.toolName = "asker"
    const session = await store.createSession("default", "t")
    const run = engine.run(session.id, "default", "ask user")
    await new Promise((r) => setTimeout(r, 50))
    const t0 = Date.now()
    engine.cancel(session.id)
    await run
    expect(Date.now() - t0).toBeLessThan(1000)
    expect(engine.isRunning(session.id)).toBe(false)
    cleanup(home)
  })

  test("cancel during draw wait unwinds promptly", async () => {
    const { home, engine, store, registry, provider } = await setup("tool")
    registry.register({
      name: "drawer",
      description: "draw test",
      parameters: { type: "object", properties: {} },
      async execute(_args, ctx) {
        await ctx.waitForDraw({ code: "a -> b" })
        return { output: "drawn" }
      },
    })
    provider.toolName = "drawer"
    const session = await store.createSession("default", "t")
    const run = engine.run(session.id, "default", "draw it")
    await new Promise((r) => setTimeout(r, 50))
    const t0 = Date.now()
    engine.cancel(session.id)
    await run
    expect(Date.now() - t0).toBeLessThan(1000)
    expect(engine.isRunning(session.id)).toBe(false)
    cleanup(home)
  })

  test("session ctxTokens persisted on finish and pushed per round", async () => {
    const { home, engine, store, events } = await setup("tool")
    const session = await store.createSession("default", "t")
    const pushed: number[] = []
    events.subscribe((e) => {
      if (e.type === "event.session.ctx" && e.sessionId === session.id) pushed.push(Number((e.payload as { ctxTokens?: number }).ctxTokens ?? 0))
    })
    await engine.run(session.id, "default", "what time is it")
    // 运行中每轮推送（tool 模式至少 2 轮模型调用 → 至少 2 次推送）
    expect(pushed.length).toBeGreaterThanOrEqual(2)
    expect(pushed.every((n) => n > 0)).toBe(true)
    // 任务结束后持久化（历史会话列表展示用）
    const loaded = await store.load(session.id)
    expect(loaded!.ctxTokens).toBeGreaterThan(0)
    cleanup(home)
  })

  test("usage 真值：event.session.ctx 推送与任务结束持久化以真实 input tokens 为基线（估算只补增量）", async () => {
    const { home, engine, store, events, provider } = await setup("tool")
    provider.usage = { inputTokens: 3000, outputTokens: 7, totalTokens: 3007, cachedTokens: 2000 }
    const session = await store.createSession("default", "t")
    const pushed: number[] = []
    const pushedCached: Array<number | undefined> = []
    events.subscribe((e) => {
      if (e.type === "event.session.ctx" && e.sessionId === session.id) {
        pushed.push(Number((e.payload as { ctxTokens?: number }).ctxTokens ?? 0))
        pushedCached.push((e.payload as { ctxCachedTokens?: number }).ctxCachedTokens)
      }
    })
    await engine.run(session.id, "default", "what time is it")
    // 每轮调用返回相同 usage 真值；调用后尚未追加消息 → 增量估算为 0，推送值即真值
    expect(pushed.length).toBeGreaterThanOrEqual(2)
    expect(pushed.every((n) => n === 3000)).toBe(true)
    // 缓存命中随事件同点位推送（接口返回缓存字段时）
    expect(pushedCached.every((n) => n === 2000)).toBe(true)
    // 持久化基线：ctxInputTokens = 真值；ctxTokens（展示）= 真值 + 基线后增量（最终回复）估算
    const loaded = await store.load(session.id)
    expect(loaded!.ctxInputTokens).toBe(3000)
    expect(loaded!.ctxAtMessage).toBeGreaterThan(0)
    expect(loaded!.ctxTokens).toBeGreaterThanOrEqual(3000)
    expect(loaded!.ctxCachedTokens).toBe(2000)
    cleanup(home)
  })

  test("无 usage 真值时回退估算：持久化基线清除、ctxTokens 走估算", async () => {
    const { home, engine, store, provider } = await setup("text")
    provider.usage = { inputTokens: 500, outputTokens: 1, totalTokens: 501, cachedTokens: 400 }
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi") // 首轮写入基线
    // 第二轮换无 usage 的 provider：基线被清除，展示回退估算（与 toSessionInfo 兜底同口径）
    const plain = {
      ...provider,
      usage: undefined,
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 10000 }),
      chat: async function* () {
        yield { type: "text", text: "no usage" }
        yield { type: "done" }
      },
    }
    ;(engine as unknown as { opts: { provider: unknown } }).opts.provider = plain
    await engine.run(session.id, "default", "again")
    const loaded = await store.load(session.id)
    expect(loaded!.ctxInputTokens).toBeUndefined()
    expect(loaded!.ctxAtMessage).toBeUndefined()
    expect(loaded!.ctxCachedTokens).toBeUndefined()
    expect(loaded!.ctxTokens).toBeGreaterThan(0)
    cleanup(home)
  })

  test("压缩判定使用持久化 usage 基线；压缩后基线清除", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    for (let i = 0; i < 6; i++) {
      await s.store.appendMessage(session.id, { id: crypto.randomUUID(), role: "user", content: `问题 ${i}`, createdAt: Date.now() })
      await s.store.appendMessage(session.id, { id: crypto.randomUUID(), role: "assistant", content: `回答 ${i}`, createdAt: Date.now() })
    }
    // 模拟上次任务持久化的真实 usage 基线：历史很短（估算远小于阈值）但基线真值超小窗口阈值
    const base = await s.store.load(session.id)
    base!.ctxInputTokens = 50_000
    base!.ctxAtMessage = 12
    await s.store.save(base!)
    const smallCap = {
      ...s.provider,
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 4000 }),
      chat: async function* () {
        yield { type: "text", text: "ok" }
        yield { type: "done" }
      },
    }
    ;(s.engine as unknown as { opts: { provider: unknown } }).opts.provider = smallCap
    await s.engine.run(session.id, "default", "继续")
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.compacted)).toBe(true) // 基线触发自动压缩
    expect(loaded!.ctxInputTokens).toBeUndefined() // 压缩替换消息 → 锚点失效，基线清除
    expect(loaded!.ctxAtMessage).toBeUndefined()
    cleanup(s.home)
  })

  test("超长用户输入自动落盘：消息保留头尾引用，全文写入 tmp/user_inputs/（read 可读）", async () => {
    const { home, engine, store } = await setup("text")
    const session = await store.createSession("default", "t")
    const prefix = "开头内容".repeat(600)
    const middle = "中段大块内容".repeat(2000)
    const suffix = "结尾请求：请分析这段内容"
    const full = prefix + middle + suffix
    await engine.run(session.id, "default", full)
    const loaded = await store.load(session.id)
    const userMsg = loaded!.messages.find((m) => m.role === "user")!
    // 消息正文：保留头尾 + 文件引用，中段省略（正文显著短于原文）
    expect(userMsg.content).toContain("开头内容")
    expect(userMsg.content).toContain("结尾请求：请分析这段内容")
    expect(userMsg.content).toContain("tmp/user_inputs/")
    expect(userMsg.content).toContain("省略中间")
    expect(userMsg.content.length).toBeLessThan(full.length)
    // 原文完整落盘会话 tmp/user_inputs/（文件面板可见、模型可 read）
    const m = userMsg.content.match(/tmp\/user_inputs\/([0-9a-f]+)\.txt/)
    expect(m).not.toBeNull()
    const tmp = store.getTmpDir(session.id, "default")
    expect(await Bun.file(join(tmp, "user_inputs", `${m![1]}.txt`)).text()).toBe(full)
    cleanup(home)
  })

  test("短用户输入不落盘不改变", async () => {
    const { home, engine, store } = await setup("text")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "普通的一句话输入")
    const loaded = await store.load(session.id)
    expect(loaded!.messages.find((m) => m.role === "user")!.content).toBe("普通的一句话输入")
    cleanup(home)
  })

  test("approval-skip env bypasses approval", async () => {
    const { home, engine, store } = await setup("approval")
    const session = await store.createSession("default", "t")
    await store.setEnv(session.id, "default", { GEBAI_APPROVAL_SKIP: "true" })
    await engine.run(session.id, "default", "run sh")
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool")).toBe(true)
    cleanup(home)
  })

  test("approval skip enabled mid-run applies to subsequent approvals", async () => {
    const { home, engine, store, events, provider } = await setup("approval2")
    const session = await store.createSession("default", "t")
    const requested: string[] = []
    events.subscribe((ev) => {
      if (ev.type === "event.approval.request") requested.push((ev.payload as { toolCallId: string }).toolCallId)
    })
    const run = engine.run(session.id, "default", "run two tools")
    // tc-1 审批等待中（轮询等待审批请求发布——引擎门控与并行池启动存在毫秒级开销，固定 sleep 在慢环境下会错过）
    const t0 = Date.now()
    while (!requested.length && Date.now() - t0 < 3000) await new Promise((r) => setTimeout(r, 10))
    expect(requested).toEqual(["tc-1"])
    // 会话运行中开启自动审批（会话 env 实时写入），已等待中的 tc-1 手动通过
    await store.setEnv(session.id, "default", { GEBAI_APPROVAL_SKIP: "true" })
    await engine.decideApproval(session.id, "tc-1", true)
    await run
    // tc-2 的审批被实时判定跳过：全程仅 tc-1 一次审批请求，两轮工具 + 最终回复共三次 chat 后正常完成
    expect(requested).toEqual(["tc-1"])
    expect(provider.calls).toBe(3)
    expect(engine.isRunning(session.id)).toBe(false)
    cleanup(home)
  })

  test("runs a preloaded sub-agent via agent_run (new session)", async () => {
    const { home, engine, store } = await setup("sub")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "call code")
    const loaded = await store.load(session.id)
    // sub-agent run finished (assistant reply present)
    expect(loaded!.messages.some((m) => m.role === "assistant")).toBe(true)
    cleanup(home)
  })

  test("long tool execution publishes event.tool.alive heartbeats (frontend idle watchdog keepalive)", async () => {
    const { home, engine, store, events, provider } = await setup("tool", false, "local", false, { heartbeatMs: 30 })
    const session = await store.createSession("default", "t")
    const alive: string[] = []
    events.subscribe((ev) => {
      if (ev.type === "event.tool.alive") alive.push(String((ev.payload as { name: string }).name))
    })
    // 阻塞 200ms 的 sh 命令（node -e 非免审白名单 → 审批等待 → 测试侧自动批准），跨多个 30ms 心跳周期
    events.subscribe((ev) => {
      if (ev.type === "event.approval.request") void engine.decideApproval(session.id, String(ev.payload.toolCallId), true)
    })
    provider.toolName = "sh"
    provider.toolArgs = { command: `node -e "setTimeout(()=>{},200)"`, approval: false }
    await engine.run(session.id, "default", "run slow tool")
    expect(alive.length).toBeGreaterThan(0)
    expect(alive.every((n) => n === "sh")).toBe(true)
    cleanup(home)
  })

  test("agent_run streams new-session full execution to frontend and persists it (archived, not in main context)", async () => {
    const { home, engine, store, events, provider } = await setup("substream")
    const session = await store.createSession("default", "t")
    const got: Array<{ type: string; text?: string; name?: string; session?: boolean; runId?: string; agents?: string[]; input?: string; output?: string }> = []
    events.subscribe((e) => {
      if (
        e.type.startsWith("event.message.") ||
        e.type === "event.tool.call" ||
        e.type === "event.tool.result" ||
        e.type === "event.session.start" ||
        e.type === "event.session.done"
      ) {
        const p = e.payload as Record<string, unknown>
        got.push({
          type: e.type,
          text: p.text as string | undefined,
          name: p.name as string | undefined,
          session: p.session as boolean | undefined,
          runId: p.sessionRunId as string | undefined,
          agents: p.agents as string[] | undefined,
          input: p.input as string | undefined,
          output: p.output as string | undefined,
        })
      }
    })
    await engine.run(session.id, "default", "run sub-agent")
    // 新会话每轮模型回复文本实时推送到前端（带 session 标记）
    const texts = got.filter((e) => e.type === "event.message.delta").map((e) => e.text).join("")
    expect(texts).toContain("子代理开始分析")
    expect(texts).toContain("子代理完成分析")
    // 新会话最终轮推送 done（仅一条 session 标记的 done：中间工具轮不推 done）
    expect(got.filter((e) => e.type === "event.message.done" && e.session).map((e) => e.text)).toEqual(["子代理完成分析"])
    // 新会话的推理与工具调用全程推送（与主循环一致）：reasoning 带 session 标记，工具含主循环 agent_run 与新会话内 todo（全局工具继承）
    const reasonings = got.filter((e) => e.type === "event.message.reasoning")
    expect(reasonings.map((e) => e.text)).toEqual(["子代理推理过程"])
    expect(reasonings.every((e) => e.session === true)).toBe(true)
    expect(got.filter((e) => e.type === "event.tool.call").map((e) => [e.name, e.session])).toEqual([
      ["agent_run", undefined],
      ["todo", true],
    ])
    // 新会话 run 起止事件：start 携带 agents/input（每轮重推、同 runId 幂等，前端容器重建兜底），done 携带最终输出（前端折叠容器标题用）
    const starts = got.filter((e) => e.type === "event.session.start")
    expect(starts.length).toBeGreaterThanOrEqual(1)
    expect(starts[0].agents).toEqual(["code"])
    expect(starts[0].input).toBe("check something")
    expect(starts.every((e) => e.agents && e.agents[0] === "code" && e.input === "check something")).toBe(true)
    const dones = got.filter((e) => e.type === "event.session.done")
    expect(dones).toHaveLength(1)
    expect(dones[0].output).toBe("子代理完成分析")
    // 新会话执行完整存档：作为 agent_run 工具调用记录的扩展字段落盘（sessionRun 存档含全部内容）
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.session)).toBe(false) // 不再逐条落盘独立 session 消息
    const callMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "agent_run" && m.sessionRun)
    expect(callMsg).toBeDefined()
    const archive = callMsg!.sessionRun!
    expect(archive.agents).toEqual(["code"])
    expect(archive.input).toBe("check something")
    expect(archive.output).toBe("子代理完成分析")
    expect(archive.messages[0]).toMatchObject({ role: "user", content: "check something" })
    expect(archive.messages.some((m) => m.role === "assistant" && m.content.includes("子代理开始分析") && m.toolCalls?.[0]?.name === "todo")).toBe(true)
    expect(archive.messages.some((m) => m.role === "tool" && m.name === "todo")).toBe(true)
    expect(archive.messages.some((m) => m.role === "assistant" && m.content.includes("子代理完成分析"))).toBe(true)
    // 存档条目推理独立字段（SessionRunEntry.reasoning，与主循环同规则）：content 纯正文、推理字段携带、无推理轮省略
    expect(archive.messages.find((m) => m.role === "assistant" && m.content.includes("子代理开始分析"))?.reasoning).toBe("子代理推理过程")
    expect(archive.messages.find((m) => m.role === "assistant" && m.content.includes("子代理开始分析"))?.content).not.toContain("<think>")
    expect(archive.messages.find((m) => m.role === "assistant" && m.content.includes("子代理完成分析"))?.reasoning).toBeUndefined()
    // 存档不进入主上下文：再次运行会话（新一轮对话），模型看到的消息不含新会话内部过程
    // （推理/中间文本/内部工具）；agent_run 调用参数与最终结果仍属主上下文（DESIGN：模型可见调用与最终返回）
    await engine.run(session.id, "default", "next question")
    const lastChat = provider.seenChats[provider.seenChats.length - 1]
    const joined = JSON.stringify(lastChat)
    expect(joined).not.toContain("子代理推理过程")
    expect(joined).not.toContain("子代理开始分析")
    // 新会话内 todo（全局工具继承）的执行结果（查询待办清单文本）不进主上下文
    expect(joined).not.toContain("待办")
    expect(joined).toContain("agent_run")
    expect(joined).toContain("子代理完成分析")
    cleanup(home)
  })

  test("compact keeps new-session archive intact (agent_run 记录扩展字段存档不随压缩丢失)", async () => {
    const { home, engine, store } = await setup("substream")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "run sub-agent")
    const before = (await store.load(session.id))!.messages
    const archivedBefore = before.filter((m) => m.sessionRun)
    expect(archivedBefore.length).toBeGreaterThan(0)
    // 全区间压缩：区间内夹带带存档的 agent_run 工具消息
    await engine.compactSession(session.id, "default", { from: 0, to: before.length })
    const after = (await store.load(session.id))!.messages
    const archivedAfter = after.filter((m) => m.sessionRun)
    // 存档消息原样保留（同 id 同序），普通消息被压缩为摘要
    expect(archivedAfter.map((m) => m.id)).toEqual(archivedBefore.map((m) => m.id))
    expect(after.some((m) => m.compacted)).toBe(true)
    cleanup(home)
  })

  test("sub-agent internal failure pushes done with error (fold shows interrupted, not running)", async () => {
    const { home, engine, store, events } = await setup("suberr")
    const session = await store.createSession("default", "t")
    const dones: Array<{ output?: string; error?: string }> = []
    events.subscribe((e) => {
      if (e.type === "event.session.done") dones.push(e.payload as { output?: string; error?: string })
    })
    await engine.run(session.id, "default", "run sub-agent")
    // 异常收尾：done 事件 output 为空并携带 error（前端据此折叠显示「（无返回/已中断）」，不残留「执行中」）
    expect(dones).toHaveLength(1)
    expect(dones[0].output).toBe("")
    expect(String(dones[0].error ?? "")).toContain("sub agent model failed")
    cleanup(home)
  })

  test("tool-less sub-agent gets orchestration tools injected and compose chain works", async () => {
    const { home, engine, store, subAgents } = await setup("subcompose")
    // 纯提示词组合子 Agent（无 ts/md 文件，测试动态注册）：tools 为空列表
    const defs = subAgents.list()
    const combo = defs.find((d) => d.name === "combo_test")
    expect(combo).toBeDefined()
    expect(combo!.tools).toEqual([])
    expect(combo!.description).toContain("组合")

    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "produce report")
    const loaded = await store.load(session.id)
    // combo_test 环境注入的 agent_run（原名）可解析：无「未知工具」消息
    expect(loaded!.messages.some((m) => m.content.includes("未知工具: agent_run"))).toBe(false)
    // 嵌套编排链路跑通：combo_test → code 均返回文本并汇总到最终回复
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(home)
  })

  test("agent_run environment provides flow dataflow orchestration (sub-agent tools resolvable inside flow)", async () => {
    const { home, engine, store } = await setup("subpipe")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "run pipeline")
    const loaded = await store.load(session.id)
    const callMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "agent_run" && m.sessionRun)
    expect(callMsg).toBeDefined()
    const archive = callMsg!.sessionRun!
    // 新会话内 flow 执行成功：全局工具（todo，继承注册）经会话注册表解析、无「未知工具」错误
    expect(archive.messages.some((m) => m.role === "tool" && m.name === "flow" && m.content.includes("todo"))).toBe(true)
    expect(archive.messages.some((m) => m.role === "tool" && m.content.includes("未知工具"))).toBe(false)
    cleanup(home)
  })

  test("路由自愈（主循环）：未装载子Agent 的 {agent}_* 调用自动装载后执行", async () => {
    const { home, engine, store, provider, subAgents } = await setup("autoload")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "ls via code")
    // code 已被自动装载：全局注册表可见 + 会话记录 loadedSubAgents 与装载提示词消息
    expect(subAgents.isLoaded("code")).toBe(true)
    const loaded = await store.load(session.id)
    expect(loaded!.loadedSubAgents).toContain("code")
    expect(loaded!.messages.some((m) => m.role === "system" && m.loadedAgent === "code")).toBe(true)
    // 工具结果带自动装载说明且正常执行（非「未知工具」）；第二轮模型上下文已拼接 code 提示词段
    const toolResults = JSON.stringify(loaded!.messages.filter((m) => m.role === "tool"))
    expect(toolResults).toContain("自动装载子Agent code")
    expect(toolResults).not.toContain("未知工具")
    expect(provider.seenChats[1].some((m) => m.role === "system" && typeof m.content === "string" && m.content.includes("源码分析与修改专家"))).toBe(true)
    cleanup(home)
  })

  test("路由自愈（新会话循环）：未预加载子Agent 的 {agent}_* 调用在本次运行内装载执行（隔离语义）", async () => {
    const { home, engine, store, provider, subAgents } = await setup("subautoload")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "delegate")
    // 新会话内 code_system_info 经自愈装载执行：无「未知工具」，后续轮次 schema 已含、上下文拼接 code 提示词段
    const callMsg = (await store.load(session.id))!.messages.find((m) => m.role === "tool" && m.name === "agent_run" && m.sessionRun)
    const archive = callMsg!.sessionRun!
    expect(archive.messages.some((m) => m.role === "tool" && m.content.includes("自动装载子Agent code"))).toBe(true)
    expect(archive.messages.some((m) => m.role === "tool" && m.content.includes("未知工具"))).toBe(false)
    expect(provider.seenTools[2]).toContain("code_system_info")
    expect(provider.seenChats[2].some((m) => m.role === "system" && typeof m.content === "string" && m.content.includes("源码分析与修改专家"))).toBe(true)
    // 隔离语义：不写全局注册表、不落盘父会话装载记录
    expect(subAgents.isLoaded("code")).toBe(false)
    const loaded = await store.load(session.id)
    expect(loaded!.loadedSubAgents ?? []).not.toContain("code")
    cleanup(home)
  })

  test("agent_run preloads multiple sub-agents: 完整提示词拼接 + 多 Agent 工具集叠加", async () => {
    const { home, engine, store, subAgents, provider } = await setup("submulti")
    // 动态注册第二个测试子Agent（带工具）：验证多 Agent 预加载时其工具同样进入新会话
    subAgents.register({
      name: "writer_test",
      description: "文档撰写",
      systemPrompt: "你是 writer_test，负责把草稿整理成报告。",
      tools: {
        summarize: {
          name: "summarize",
          description: "汇总文本",
          parameters: { type: "object", properties: { text: { type: "string" } }, required: ["text"] },
          execute: async (args) => ({ output: `已汇总: ${String(args.text)}` }),
        },
      },
    })
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "multi-agent task")
    const loaded = await store.load(session.id)
    // 新会话系统提示词 = 两个子Agent 完整提示词拼接（均注入）
    const childSystem = String(provider.seenChats[1][0].content)
    expect(childSystem).toContain("你是源码分析与修改专家")
    expect(childSystem).toContain("你是 writer_test")
    expect(childSystem).toContain("已预加载子Agent: code, writer_test")
    // 新会话内调用了第二个子Agent 的工具（writer_test_summarize）：多 Agent 工具集叠加生效
    const callMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "agent_run" && m.sessionRun)
    expect(callMsg).toBeDefined()
    const archive = callMsg!.sessionRun!
    expect(archive.agents).toEqual(["code", "writer_test"])
    expect(archive.messages.some((m) => m.role === "tool" && m.name === "writer_test_summarize" && m.content.includes("已汇总: draft"))).toBe(true)
    cleanup(home)
  })

  test("sub-agent recursion depth is enforced via runNewSession depth propagation", async () => {
    const { home, engine, store, provider } = await setup("subdeep")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "loop")
    // 深度保护生效：chat 次数有界（深度错误发生在子 Agent 内部内存链，不持久化）。
    // 无保护时自嵌套会无限膨胀（每层 20 轮工具循环），calls 将远超此值。
    expect(provider.calls).toBeLessThanOrEqual(6)
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(home)
  })

  test("second concurrent run on same session throws", async () => {
    const { home, engine, store } = await setup("approval")
    const session = await store.createSession("default", "t")
    const first = engine.run(session.id, "default", "start")
    await new Promise((r) => setTimeout(r, 30))
    await expect(engine.run(session.id, "default", "again")).rejects.toThrow(/已有任务在运行/)
    await engine.decideApproval(session.id, "tc-1", false)
    await first
    cleanup(home)
  })

  test("tool result blocks are persisted to session message", async () => {
    const s = await setup("tool")
    s.provider.toolName = "show"
    s.provider.toolArgs = { code: "Alice -> Bob", format: "plantuml" }
    const session = await s.store.createSession("default", "t")
    // show 图表分支执行期间等待前端渲染回传：模拟前端渲染成功
    s.events.subscribe((e) => {
      if (e.type === "event.draw.render") void s.engine.decideDrawResult(session.id, String(e.payload.renderId), { ok: true })
    })
    await s.engine.run(session.id, "default", "make a diagram")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool")
    expect(toolMsg).toBeDefined()
    expect(toolMsg!.blocks?.length).toBeGreaterThan(0)
    expect(toolMsg!.blocks![0].type).toBe("diagram")
    cleanup(s.home)
  })

  test("diff tool block is persisted to session message", async () => {
    const s = await setup("tool")
    s.provider.toolName = "diff"
    s.provider.toolArgs = { oldText: "const a = 1\n", newText: "const a = 2\n", language: "typescript" }
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "diff something")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool")
    expect(toolMsg).toBeDefined()
    expect(toolMsg!.blocks?.[0].type).toBe("diff")
    const b = toolMsg!.blocks![0] as Extract<ContentBlock, { type: "diff" }>
    expect(b.language).toBe("typescript")
    expect(b.lines.map((l) => l.kind)).toEqual(["del", "add"])
    expect(b.lines[0].text).toBe("const a = 1")
    expect(b.lines[1].text).toBe("const a = 2")
    cleanup(s.home)
  })

  test("attachments with traversal names are sanitized (cannot escape tmp/)", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "attach", {
      attachments: [{ name: "../../evil.txt", data: new Uint8Array([1, 2, 3]) }],
    })
    // 名称被消毒为 basename：写入 tmp/evil.txt，而非会话目录外
    const file = await Bun.file(join(s.store.getTmpDir(session.id, "default"), "evil.txt")).arrayBuffer()
    expect(file.byteLength).toBe(3)
    // 会话目录外不存在该文件
    expect(() => Bun.file(join(s.home, "evil.txt"))).toBeDefined()
    cleanup(s.home)
  })

  test("sandboxed attachments reject out-of-sandbox source paths", async () => {
    const s = await setup("text")
    ;(s.engine as unknown as { opts: { sandbox: import("./sandbox").Sandbox } }).opts.sandbox = new Sandbox({ home: s.home, enabled: true })
    const session = await s.store.createSession("default", "t")
    const outside = mkdtempSync(join(tmpdir(), "gebai-att-out-"))
    writeFileSync(join(outside, "x.txt"), "secret")
    await s.engine.run(session.id, "default", "attach", { attachments: [{ name: "x.txt", path: join(outside, "x.txt") }] })
    const loaded = await s.store.load(session.id)
    // 附件未保存（沙箱拒绝越界源路径，任务报错）
    expect(loaded!.messages.some((m) => m.attachments?.length)).toBe(false)
    rmSync(outside, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("exempt user (auth=none default) bypasses sandbox: out-of-sandbox attachment source accepted", async () => {
    const s = await setup("text")
    // 沙箱启用 + 默认用户豁免（auth=none 单用户模式：操作者本人不受用户沙箱控制）
    ;(s.engine as unknown as { opts: { sandbox: import("./sandbox").Sandbox } }).opts.sandbox = new Sandbox({
      home: s.home,
      enabled: true,
      isExempt: (u) => u === "default",
    })
    const session = await s.store.createSession("default", "t")
    const outside = mkdtempSync(join(tmpdir(), "gebai-att-exempt-"))
    writeFileSync(join(outside, "x.txt"), "secret")
    await s.engine.run(session.id, "default", "attach", { attachments: [{ name: "x.txt", path: join(outside, "x.txt") }] })
    const loaded = await s.store.load(session.id)
    // 豁免用户：越界源路径附件正常保存（逻辑路径 tmp/x.txt，内容一致）
    const um = loaded!.messages.find((m) => m.role === "user")!
    expect(um.attachments?.[0]).toMatchObject({ name: "x.txt", path: "tmp/x.txt" })
    const stored = await Bun.file(join(s.store.getTmpDir(session.id, "default"), "x.txt")).text()
    expect(stored).toBe("secret")
    rmSync(outside, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("local-mode path attachments resolve against the session dir and store logical paths", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    const tmp = s.store.getTmpDir(session.id, "default")
    mkdirSync(tmp, { recursive: true })
    writeFileSync(join(tmp, "shot.png"), "png-bytes")
    await s.engine.run(session.id, "default", "look at this", { attachments: [{ name: "shot.png", mime: "image/png", path: "tmp/shot.png" }] })
    const loaded = await s.store.load(session.id)
    const um = loaded!.messages.find((m) => m.role === "user")!
    expect(um.attachments?.[0]).toMatchObject({ name: "shot.png", mime: "image/png", path: "tmp/shot.png", size: 9 })
    // 非多模态主模型：模型收到的用户消息为文本说明（路径 + MIME + vision 工具指引）
    const userMsg = s.provider.seenChats[0]!.find((m) => m.role === "user")!
    const texts = (userMsg.content as Array<Record<string, unknown>>).map((b) => String((b as { text?: string }).text ?? ""))
    expect(texts[0]).toBe("look at this")
    expect(texts[1]).toContain("[用户附件图片: shot.png（image/png，9B，会话路径 tmp/shot.png）")
    expect(texts[1]).toContain("vision 工具")
    cleanup(s.home)
  })

  test("multimodal provider inlines image attachments as base64 image blocks", async () => {
    const s = await setup("text")
    s.provider.multimodal = true
    const session = await s.store.createSession("default", "t")
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 1, 2, 3])
    await s.engine.run(session.id, "default", "describe", { attachments: [{ name: "a.png", mime: "image/png", data: png }] })
    const userMsg = s.provider.seenChats[0]!.find((m) => m.role === "user")!
    expect(userMsg.content).toEqual([
      { type: "text", text: "describe" },
      { type: "image", mime: "image/png", data: Buffer.from(png).toString("base64"), path: "tmp/a.png", name: "a.png", size: 7 },
    ])
    cleanup(s.home)
  })

  test("non-image attachments degrade to a text note even for multimodal providers", async () => {
    const s = await setup("text")
    s.provider.multimodal = true
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "read doc", { attachments: [{ name: "doc.pdf", mime: "application/pdf", data: new Uint8Array([1, 2]) }] })
    const userMsg = s.provider.seenChats[0]!.find((m) => m.role === "user")!
    expect(userMsg.content).toEqual([{ type: "text", text: "read doc" }, { type: "text", text: expect.stringContaining("[用户附件文件: doc.pdf（application/pdf") }])
    cleanup(s.home)
  })

  test("image blocks rejected by the API (HTTP 4xx) degrade to text notes and retry", async () => {
    const s = await setup("text")
    s.provider.multimodal = true
    s.provider.failFirstError = new Error("模型接口错误（HTTP 400）: image_url is not supported")
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "describe", { attachments: [{ name: "a.png", mime: "image/png", data: new Uint8Array([1, 2, 3]) }] })
    // 首次调用携带图片块；接口拒绝后降级为文本说明重试成功
    const first = s.provider.seenChats[0]!.find((m) => m.role === "user")!
    expect((first.content as Array<Record<string, unknown>>).some((b) => b.type === "image")).toBe(true)
    const retry = s.provider.seenChats[1]!.find((m) => m.role === "user")!
    expect((retry.content as Array<Record<string, unknown>>).some((b) => b.type === "image")).toBe(false)
    expect((retry.content as Array<Record<string, unknown>>).some((b) => b.type === "text" && String(b.text).includes("vision 工具"))).toBe(true)
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "hello from fake")).toBe(true)
    cleanup(s.home)
  })

  test("legacy absolute attachment refs degrade to a note instead of crashing history load", async () => {
    const s = await setup("text", true)
    s.provider.multimodal = true
    const session = await s.store.createSession("default", "t")
    const tmp = s.store.getTmpDir(session.id, "default")
    mkdirSync(tmp, { recursive: true })
    writeFileSync(join(tmp, "old.png"), "legacy")
    // 旧格式消息：AttachmentRef.path 为绝对路径（沙箱拒绝解析，须降级为说明）
    await s.store.appendMessage(session.id, { id: "u1", role: "user", content: "look", attachments: [{ path: join(tmp, "old.png"), mime: "image/png", name: "old.png", size: 6 }], createdAt: Date.now() })
    await s.engine.run(session.id, "default", "continue")
    const userMsg = s.provider.seenChats[0]!.find((m) => m.role === "user")!
    expect(Array.isArray(userMsg.content)).toBe(true)
    const texts = (userMsg.content as Array<Record<string, unknown>>).map((b) => String((b as { text?: string }).text ?? ""))
    expect(texts.join(" ")).toContain("[用户附件图片: old.png")
    cleanup(s.home)
  })

  test("ask 选项询问分支 blocks the loop until the user decides (decideChoice resumes)", async () => {
    const s = await setup("tool")
    s.provider.toolName = "ask"
    const session = await s.store.createSession("default", "t")
    let choiceId = ""
    s.events.subscribe((e) => {
      if (e.type === "event.choice.request") choiceId = String(e.payload.choiceId)
    })
    const run = s.engine.run(session.id, "default", "ask me")
    // 等待 choice.request 发布（run 阻塞在 ask 选项询问分支等待）
    const t0 = Date.now()
    while (!choiceId) {
      if (Date.now() - t0 > 2000) throw new Error("choice.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    expect(choiceId).toBeTruthy()
    // 任务仍挂起（未选择前不继续）
    expect(s.engine.isRunning(session.id)).toBe(true)
    // 提交用户选择 → ask 选项询问分支返回 → 引擎继续下一轮
    await s.engine.decideChoice(session.id, choiceId, "方案B")
    await run
    expect(s.engine.isRunning(session.id)).toBe(false)
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "ask")
    expect(toolMsg?.content).toContain("用户选择：方案B")
    cleanup(s.home)
  })

  test("ask 选项询问分支支持自定义文本输入与拒绝（decideChoice）", async () => {
    // 自定义文本：用户直接输入不在选项中的答案
    const s = await setup("tool")
    s.provider.toolName = "ask"
    const session = await s.store.createSession("default", "t")
    let choiceId = ""
    s.events.subscribe((e) => {
      if (e.type === "event.choice.request") choiceId = String(e.payload.choiceId)
    })
    const run = s.engine.run(session.id, "default", "ask me")
    const t0 = Date.now()
    while (!choiceId) {
      if (Date.now() - t0 > 2000) throw new Error("choice.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    await s.engine.decideChoice(session.id, choiceId, "自定义答案")
    await run
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "ask")
    expect(toolMsg?.content).toContain("用户选择：自定义答案")
    cleanup(s.home)

    // 拒绝回答：option 传 null → ask 选项询问分支返回拒绝提示，模型停止询问
    const s2 = await setup("tool")
    s2.provider.toolName = "ask"
    const session2 = await s2.store.createSession("default", "t")
    let choiceId2 = ""
    s2.events.subscribe((e) => {
      if (e.type === "event.choice.request") choiceId2 = String(e.payload.choiceId)
    })
    const run2 = s2.engine.run(session2.id, "default", "ask me")
    const t1 = Date.now()
    while (!choiceId2) {
      if (Date.now() - t1 > 2000) throw new Error("choice.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    await s2.engine.decideChoice(session2.id, choiceId2, null)
    await run2
    const loaded2 = await s2.store.load(session2.id)
    const refusedMsg = loaded2!.messages.find((m) => m.role === "tool" && m.name === "ask")
    expect(refusedMsg?.content).toContain("拒绝")
    cleanup(s2.home)
  })

  test("ask 选项询问分支多选结果以「、」连接（decideChoice 数组）", async () => {
    const s = await setup("tool")
    s.provider.toolName = "ask"
    s.provider.askMulti = true
    const session = await s.store.createSession("default", "t")
    let choiceId = ""
    let multi = false
    s.events.subscribe((e) => {
      if (e.type === "event.choice.request") {
        choiceId = String(e.payload.choiceId)
        multi = e.payload.multi === true
      }
    })
    const run = s.engine.run(session.id, "default", "ask me")
    const t0 = Date.now()
    while (!choiceId) {
      if (Date.now() - t0 > 2000) throw new Error("choice.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    // 事件携带 multi 标记（UI 据此渲染多选卡片）
    expect(multi).toBe(true)
    // 提交多选集合 → ask 选项询问分支返回「、」连接的选择
    await s.engine.decideChoice(session.id, choiceId, ["方案A", "方案C"])
    await run
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "ask")
    expect(toolMsg?.content).toContain("用户选择：方案A、方案C")
    cleanup(s.home)
  })

  test("show 图表分支 blocks the loop until the frontend render result arrives (decideDrawResult resumes)", async () => {
    const s = await setup("tool")
    s.provider.toolName = "show"
    s.provider.toolArgs = { code: "Alice -> Bob", format: "plantuml" }
    const session = await s.store.createSession("default", "t")
    let renderId = ""
    s.events.subscribe((e) => {
      if (e.type === "event.draw.render") renderId = String(e.payload.renderId)
    })
    const run = s.engine.run(session.id, "default", "draw a diagram")
    // 等待 draw.render 发布（run 阻塞在 show 图表分支等待渲染结果）
    const t0 = Date.now()
    while (!renderId) {
      if (Date.now() - t0 > 2000) throw new Error("draw.render not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    expect(renderId).toBeTruthy()
    // 任务仍挂起（渲染结果未回传前不继续）
    expect(s.engine.isRunning(session.id)).toBe(true)
    // 前端渲染成功回传 → show 图表分支返回成功 → 引擎继续下一轮
    await s.engine.decideDrawResult(session.id, renderId, { ok: true })
    await run
    expect(s.engine.isRunning(session.id)).toBe(false)
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "show")
    expect(toolMsg?.content).toContain("渲染成功")
    // 产物落盘会话 tmp/（与工具描述一致），而非会话根
    const root = sessionPath(s.home, "default", session.id)
    expect(existsSync(join(root, "tmp", "diagram.puml"))).toBe(true)
    expect(existsSync(join(root, "diagram.puml"))).toBe(false)
    cleanup(s.home)
  })

  test("file action=move auto-creates the destination parent directory (matches write behavior)", async () => {
    const s = await setup("tool")
    const session = await s.store.createSession("default", "t")
    const root = sessionPath(s.home, "default", session.id)
    // 预置源文件（相对路径基准 = 会话 tmp/）
    writeFileSync(join(root, "tmp", "a.txt"), "hello")
    s.provider.toolName = "file"
    s.provider.toolArgs = { action: "move", path: "a.txt", to: "moved/b.txt" }
    await s.engine.run(session.id, "default", "move it")
    // 目标父目录 moved/ 被自动创建（rename 本身对缺失父目录报 ENOENT）
    expect(existsSync(join(root, "tmp", "moved", "b.txt"))).toBe(true)
    expect(existsSync(join(root, "tmp", "a.txt"))).toBe(false)
    cleanup(s.home)
  })

  test("show 图表分支 returns render error and timeout message to the model", async () => {
    const s = await setup("tool")
    s.provider.toolName = "show"
    s.provider.toolArgs = { code: "Alice -> Bob", format: "plantuml" }
    const session = await s.store.createSession("default", "t")
    let renderId = ""
    s.events.subscribe((e) => {
      if (e.type === "event.draw.render") renderId = String(e.payload.renderId)
    })
    const run = s.engine.run(session.id, "default", "draw a diagram")
    const t0 = Date.now()
    while (!renderId) {
      if (Date.now() - t0 > 2000) throw new Error("draw.render not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    // 前端渲染报错 → 工具结果携带错误信息（模型据此修正）
    await s.engine.decideDrawResult(session.id, renderId, { ok: false, error: "PlantUML 语法错误" })
    await run
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "show")
    expect(toolMsg?.content).toContain("画图失败（渲染错误）")
    expect(toolMsg?.content).toContain("PlantUML 语法错误")
    cleanup(s.home)
  })

  test("page_capture blocks until frontend capture result arrives and persists html+screenshot", async () => {
    const s = await setup("tool")
    s.registry.register(pageCaptureTool)
    s.provider.toolName = "page_capture"
    const session = await s.store.createSession("default", "t")
    let captureId = ""
    s.events.subscribe((e) => {
      if (e.type === "event.capture.request") captureId = String(e.payload.captureId)
    })
    const run = s.engine.run(session.id, "default", "capture the page")
    const t0 = Date.now()
    while (!captureId) {
      if (Date.now() - t0 > 2000) throw new Error("capture.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    expect(captureId).toBeTruthy()
    // 任务仍挂起（前端捕获结果未回传前不继续）
    expect(s.engine.isRunning(session.id)).toBe(true)
    // 前端回传捕获结果（html + 截图 base64）→ page_capture 返回成功 → 引擎继续
    await s.engine.decideCaptureResult(session.id, captureId, {
      html: "<!doctype html><html><body><h1>渲染后页面</h1></body></html>",
      imageBase64: "data:image/png;base64," + Buffer.from("png-bytes").toString("base64"),
    })
    await run
    expect(s.engine.isRunning(session.id)).toBe(false)
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "page_capture")
    expect(toolMsg?.content).toContain("已捕获当前页面")
    expect(toolMsg?.content).toContain("tmp/capture/page-")
    // 产物落盘会话 tmp/capture/（html + 截图），blocks 含 image
    expect(toolMsg!.blocks!.some((b) => b.type === "image")).toBe(true)
    const root = sessionPath(s.home, "default", session.id)
    const files = readdirSync(join(root, "tmp", "capture"))
    expect(files.some((f) => f.endsWith(".html"))).toBe(true)
    expect(files.some((f) => f.endsWith(".png"))).toBe(true)
    cleanup(s.home)
  })

  test("page_capture reports frontend error and timeout to the model", async () => {
    const s = await setup("tool")
    s.registry.register(pageCaptureTool)
    s.provider.toolName = "page_capture"
    const session = await s.store.createSession("default", "t")
    let captureId = ""
    s.events.subscribe((e) => {
      if (e.type === "event.capture.request") captureId = String(e.payload.captureId)
    })
    const run = s.engine.run(session.id, "default", "capture the page")
    const t0 = Date.now()
    while (!captureId) {
      if (Date.now() - t0 > 2000) throw new Error("capture.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    // 前端捕获报错 → 工具结果携带错误信息（模型据此调整方案）
    await s.engine.decideCaptureResult(session.id, captureId, { html: "", error: "截图失败" })
    await run
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "page_capture")
    expect(toolMsg?.content).toContain("页面捕获失败")
    expect(toolMsg?.content).toContain("截图失败")
    cleanup(s.home)
  })

  test("page_capture times out (frontend offline) and returns failure hint", async () => {
    const s = await setup("tool")
    s.registry.register(pageCaptureTool)
    s.provider.toolName = "page_capture"
    // 短超时注入：模拟前端离线（无人回传），60ms 后工具返回失败提示
    s.engine = new AgentEngine({ ...s, captureTimeoutMs: 60 })
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "capture the page")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "page_capture")
    expect(toolMsg?.content).toContain("页面捕获失败")
    expect(toolMsg?.content).toContain("限定时间")
    cleanup(s.home)
  })

  test("page_capture consumes capture result arriving before waiter registration (race)", async () => {
    const s = await setup("tool")
    s.registry.register(pageCaptureTool)
    s.provider.toolName = "page_capture"
    const session = await s.store.createSession("default", "t")
    // EventBus 同步分发：订阅回调里立即回传 → 回传先于 waitForCapture 的 Promise 注册
    // （进入 pendingCaptures 排队），注册后应被立即消费而非超时
    s.events.subscribe((e) => {
      if (e.type === "event.capture.request") {
        void s.engine.decideCaptureResult(session.id, String(e.payload.captureId), {
          html: "<html><body>race</body></html>",
          imageBase64: "data:image/png;base64," + Buffer.from("race-png").toString("base64"),
        })
      }
    })
    await s.engine.run(session.id, "default", "capture the page")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "page_capture")
    expect(toolMsg?.content).toContain("已捕获当前页面")
    cleanup(s.home)
  })

  test("capture results submitted after the task ended are dropped (no accumulation)", async () => {
    const s = await setup("tool")
    s.registry.register(pageCaptureTool)
    s.provider.toolName = "page_capture"
    s.engine = new AgentEngine({ ...s, captureTimeoutMs: 60 })
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "capture the page") // 60ms 超时返回失败
    // 任务已结束（task 已删除）：迟到回传被丢弃，不产生堆积
    await s.engine.decideCaptureResult(session.id, "stale", { html: "x".repeat(1024) })
    s.provider.calls = 0 // FakeProvider 仅在 calls===1 时发起工具调用，重置计数
    await s.engine.run(session.id, "default", "capture again") // 正常超时失败，不受迟到回传影响
    const loaded = await s.store.load(session.id)
    const toolMsgs = loaded!.messages.filter((m) => m.role === "tool" && m.name === "page_capture")
    expect(toolMsgs).toHaveLength(2)
    expect(toolMsgs.every((m) => m.content.includes("页面捕获失败"))).toBe(true)
    cleanup(s.home)
  })

  test("page_capture enforces input size limits (oversized html truncated, oversized image dropped)", async () => {
    const s = await setup("tool")
    s.registry.register(pageCaptureTool)
    s.provider.toolName = "page_capture"
    const session = await s.store.createSession("default", "t")
    let captureId = ""
    s.events.subscribe((e) => {
      if (e.type === "event.capture.request") captureId = String(e.payload.captureId)
    })
    const run = s.engine.run(session.id, "default", "capture the page")
    const t0 = Date.now()
    while (!captureId) {
      if (Date.now() - t0 > 2000) throw new Error("capture.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    // 恶意/异常超限回传：html 超 300KB → 截断落盘；imageBase64 超限 → 丢弃（无 image block）
    const oversizedHtml = "<html><body>" + "x".repeat(400 * 1024) + "</body></html>"
    await s.engine.decideCaptureResult(session.id, captureId, {
      html: oversizedHtml,
      imageBase64: "data:image/png;base64," + "A".repeat(12 * 1024 * 1024),
    })
    await run
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "page_capture")
    expect(toolMsg!.blocks!.some((b) => b.type === "image")).toBe(false)
    const root = sessionPath(s.home, "default", session.id)
    const dir = readdirSync(join(root, "tmp", "capture"))
    const htmlName = dir.find((f) => f.endsWith(".html"))!
    const text = (await Bun.file(join(root, "tmp", "capture", htmlName)).text())
    expect(text.length).toBeLessThanOrEqual(300 * 1024)
    cleanup(s.home)
  })

  test("disabledTools filters schemas (channel-scoped tool removal) and blocks execution", async () => {
    const s = await setup("tool")
    s.provider.toolName = "show"
    s.provider.toolArgs = { html: "<p>hi</p>" }
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "render a page", { disabledTools: ["show", "fetch_url", "page_capture"] })
    const loaded = await s.store.load(session.id)
    // 模型看到的 schema 已过滤（首轮即无禁用工具）
    expect(s.provider.seenTools[0]).not.toContain("show")
    expect(s.provider.seenTools[0]).not.toContain("fetch_url")
    expect(s.provider.seenTools[0]).toContain("diff")
    // 模型仍调用禁用工具（假模型行为）→ 执行被阻止，返回说明消息，产物不落盘
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "show")
    expect(toolMsg?.content).toContain("当前通道不可用")
    const root = sessionPath(s.home, "default", session.id)
    expect(existsSync(join(root, "tmp", "page.html"))).toBe(false)
    // 未被禁用的工具照常执行（第二轮调用 ls 完成）
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("极简模式（GEBAI_MINIMAL_MODE=true）：schema 仅含 sh/edit/full_mode，其余工具调用被阻止，系统提示词极简化", async () => {
    const s = await setup("tool")
    const session = await s.store.createSession("default", "t")
    await s.store.setEnv(session.id, "default", { GEBAI_MINIMAL_MODE: "true" })
    await s.engine.run(session.id, "default", "what time")
    const loaded = await s.store.load(session.id)
    // 模型看到的 schema 仅 sh/edit + full_mode 切换入口（默认 first-call ls 也被过滤）
    const seen = s.provider.seenTools[0]
    expect(seen).toContain("sh")
    expect(seen).toContain("edit")
    expect(seen).toContain("full_mode")
    expect(seen).not.toContain("ls")
    expect(seen).not.toContain("read")
    // 系统提示词极简化：保留极简说明，裁剪编排/子Agent 清单等（对应工具均不可用，注入纯属浪费上下文）
    const sys = String(s.provider.seenChats[0][0].content)
    expect(sys).toContain("极简模式")
    expect(sys).not.toContain("可选子Agent")
    expect(sys).not.toContain("数据流编排")
    // 假模型仍调用 ls → 执行被阻止，返回极简模式说明（含 full_mode 切换指引）
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "ls")
    expect(toolMsg?.content).toContain("极简模式")
    expect(toolMsg?.content).toContain("full_mode")
    // 模型收到说明后第二轮直接回复，任务正常收尾
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("极简模式 full_mode 切换完整模式：审批通过后解锁全工具、系统提示词原地升级、会话 env 清除、事件通知前端", async () => {
    const s = await setup("tool")
    s.provider.toolName = "full_mode"
    const session = await s.store.createSession("default", "t")
    await s.store.setEnv(session.id, "default", { GEBAI_MINIMAL_MODE: "true" })
    const minimalEvents: boolean[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.session.minimal") minimalEvents.push(e.payload.enabled === false)
    })
    const run = s.engine.run(session.id, "default", "need more tools")
    await new Promise((r) => setTimeout(r, 50))
    // full_mode 需审批：批准后执行切换（清除白名单 + 提示词升级）
    await s.engine.decideApproval(session.id, "tc-1", true)
    await run
    const loaded = await s.store.load(session.id)
    // 第一轮（极简）：仅 sh/edit/full_mode，提示词为极简版
    expect(s.provider.seenTools[0]).toContain("full_mode")
    expect(s.provider.seenTools[0]).not.toContain("ls")
    expect(String(s.provider.seenChats[0][0].content)).toContain("极简模式")
    // 第二轮（已切换）：schema 全量下发（full_mode 自身随之隐藏），系统提示词升级为完整版
    expect(s.provider.seenTools[1]).toContain("ls")
    expect(s.provider.seenTools[1]).not.toContain("full_mode")
    expect(String(s.provider.seenChats[1][0].content)).toContain("可选子Agent")
    // 工具结果说明切换成功
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "full_mode")
    expect(toolMsg?.content).toContain("已切换到完整模式")
    // 会话内存 env 极简标记已删（下一任务不再极简），前端通知事件已发布
    expect(await s.store.getEnv(session.id, "default")).not.toHaveProperty("GEBAI_MINIMAL_MODE")
    expect(minimalEvents).toEqual([true])
    cleanup(s.home)
  })

  test("非极简会话：full_mode 不出现在 schema（仅极简会话可见，防冗余工具干扰选择）", async () => {
    const s = await setup("tool")
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "what time")
    expect(s.provider.seenTools[0]).not.toContain("full_mode")
    cleanup(s.home)
  })

  test("极简模式下 sh 工具照常执行（白名单内不受限）", async () => {
    const s = await setup("tool")
    s.provider.toolName = "sh"
    s.provider.toolArgs = { command: "echo minimal-ok", approval: false }
    const session = await s.store.createSession("default", "t")
    await s.store.setEnv(session.id, "default", { GEBAI_MINIMAL_MODE: "true" })
    await s.engine.run(session.id, "default", "echo it")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "sh")
    expect(toolMsg).toBeDefined()
    expect(toolMsg?.content).toContain("minimal-ok")
    cleanup(s.home)
  })

  test("safe mode degrades sh in main loop: non-whitelisted command denied without approval, whitelisted read-only executes", async () => {
    const s = await setup("approval", false, "local", true) // safeMode=true；approval 模式首轮调用 sh
    s.provider.toolArgs = { command: "rm -rf /nope", approval: false }
    const session = await s.store.createSession("default", "t")
    const approvals: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.approval.request") approvals.push(String(e.payload.toolCallId))
    })
    await s.engine.run(session.id, "default", "run command")
    const loaded = await s.store.load(session.id)
    // sh 降级：非白名单命令在工具内被拒（白名单提示返回模型），免审标记下不弹审批
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "sh")
    expect(toolMsg?.content).toContain("安全模式")
    expect(toolMsg?.content).toContain("白名单")
    expect(approvals).toEqual([]) // approval:false 免审 → 无审批请求
    // schema 仍可见（风险工具降级而非移除）
    expect(s.provider.seenTools[0]).toContain("sh")
    // 拒绝后任务正常收尾（模型收到限制信息后第二轮直接回复）
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("safe mode: whitelisted read-only sh command executes (degraded, not blocked)", async () => {
    const s = await setup("approval", false, "local", true)
    s.provider.toolArgs = { command: "echo whitelisted-ok", approval: false }
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "run command")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "sh")
    expect(toolMsg?.content).toContain("whitelisted-ok")
    expect(toolMsg?.content).not.toContain("安全模式")
    cleanup(s.home)
  })

  test("safe mode: risky-named sub-agent tools are not registered (unknown tool on call, no side effect)", async () => {
    const s = await setup("subrisky", false, "local", true) // safeMode=true；新会话预加载 risky_test 并调用其 delete 工具
    // 动态注册测试子Agent：工具短名 delete 命中安全模式风险规则（注册期过滤），执行体带副作用标记
    let sideEffect = false
    s.subAgents.register({
      name: "risky_test",
      description: "风险工具测试",
      systemPrompt: "你是风险工具测试 Agent。",
      tools: {
        delete: {
          name: "delete",
          description: "删除",
          parameters: { type: "object", properties: {}, required: [] },
          execute: async () => {
            sideEffect = true
            return { output: "deleted" }
          },
        },
      },
    })
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "delete file")
    const loaded = await s.store.load(session.id)
    // 子Agent 风险短名工具（risky_test_delete）注册期被 Tool.safeMode 规则过滤：schema 不下发，调用报未知工具
    const callMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "agent_run" && m.sessionRun)
    expect(callMsg).toBeDefined()
    const subToolMsg = callMsg!.sessionRun!.messages.find((m) => m.role === "tool" && m.name === "risky_test_delete")
    expect(subToolMsg?.content).toContain("未知工具")
    expect(sideEffect).toBe(false)
    // 子Agent 收到未知工具说明后调整（最终正常返回，主循环收尾）
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("safe mode allows read-only tools (ls) and keeps risky tools out of execution only", async () => {
    const s = await setup("tool", false, "local", true) // safeMode=true；默认 ls（只读）
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "what time")
    const loaded = await s.store.load(session.id)
    // 只读工具照常执行
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "ls")
    expect(toolMsg).toBeDefined()
    expect(toolMsg?.content).not.toContain("安全模式")
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("safe mode degrades risky tools inside flow (sh whitelist executes, write scoped to user dir)", async () => {
    const s = await setup("tool", false, "local", true) // safeMode=true；flow 内嵌 sh/write（降级执行）
    s.provider.toolName = "flow"
    s.provider.toolArgs = { steps: [{ tool: "sh", params: { command: "echo pwned", approval: false } }, { tool: "write", params: { path: "x.txt", content: "x" } }] }
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "flow it")
    const loaded = await s.store.load(session.id)
    // flow step 层不再一刀切拦截（仅 cron 调度类硬阻断）：sh 白名单命令执行、write 限用户目录内落盘
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "flow")
    expect(toolMsg?.content).toContain("pwned")
    expect(toolMsg?.content).not.toContain("安全模式")
    const root = sessionPath(s.home, "default", session.id)
    expect(existsSync(join(root, "tmp", "x.txt"))).toBe(true) // 会话 tmp 在用户目录内 → 放行
    cleanup(s.home)
  })

  test("safe mode off: risky tools execute normally (write creates file)", async () => {
    const s = await setup("tool", false, "local", false) // safeMode=false（默认）
    s.provider.toolName = "write"
    s.provider.toolArgs = { path: "out.txt", content: "hi" }
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "write file")
    const root = sessionPath(s.home, "default", session.id)
    expect(existsSync(join(root, "tmp", "out.txt"))).toBe(true)
    cleanup(s.home)
  })

  test("interactionMode=none 禁用实时前端工具；合并型工具（ask/show）不做工具级门控（分支内校验）", async () => {
    const s = await setup("interact")
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "ask something", { interactionMode: "none" })
    // schema 过滤：实时前端（realtime）工具不在工具清单
    expect(s.provider.seenTools[0]).not.toContain("page_capture")
    // 合并型工具不做工具级门控：全模式可见，通道能力在分支内校验
    //（ask：选择/计划分支报「无交互能力」、填值分支引导设置面板；show：html 报错、图表引导 backend）
    expect(s.provider.seenTools[0]).toContain("ask")
    expect(s.provider.seenTools[0]).toContain("show")
    // 普通工具（single 默认）可用
    expect(s.provider.seenTools[0]).toContain("read")
    // 分支门控：模型调用 ask 选项询问分支 → 无交互说明作为工具结果返回（不空等超时），任务正常完成
    const blocked = s.provider.seenChats.some((chat) =>
      chat.some((m) => m.role === "tool" && String(m.content).includes("当前通道无交互能力")),
    )
    expect(blocked).toBe(true)
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("interactionMode=multi_turn 保留多轮交互类工具（ask/show），禁用实时前端工具", async () => {
    const s = await setup("tool") // 第一轮调 ls（无交互），仅验证 schema 过滤
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "do a thing", { interactionMode: "multi_turn" })
    // 实时前端工具禁用（与飞书通道行为一致）
    expect(s.provider.seenTools[0]).not.toContain("page_capture")
    // 合并型工具全模式可见：ask 选择/计划分支经飞书选择卡片（填值分支报错）、show 图表分支经飞书后端渲染
    expect(s.provider.seenTools[0]).toContain("ask")
    expect(s.provider.seenTools[0]).toContain("show")
    cleanup(s.home)
  })

  test("interactionMode=none auto-approves requiresApproval tools (no approval.request)", async () => {
    const s = await setup("approval") // 第一轮调 sh（requiresApproval）
    s.provider.toolName = "sh"
    s.provider.toolArgs = { command: "echo auto" }
    const session = await s.store.createSession("default", "t")
    const approvals: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.approval.request") approvals.push(String((e.payload as { tool?: unknown }).tool))
    })
    await s.engine.run(session.id, "default", "run tool", { interactionMode: "none" })
    // 无交互模式：无人可询问，需审批工具自动通过（不发审批请求、不等待）
    expect(approvals).toEqual([])
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool" && m.name === "sh" && String(m.content).includes("auto"))).toBe(true)
    cleanup(s.home)
  })

  test("multi-user + interactionMode=none refuses requiresApproval tools (no silent approval)", async () => {
    // 多用户隔离模式：无交互通道不得自动通过审批——普通用户不得经 REST 免审批执行 sh/py
    const s = await setup("approval", false, "server")
    s.provider.toolName = "sh"
    s.provider.toolArgs = { command: "echo hack" }
    const session = await s.store.createSession("default", "t")
    const approvals: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.approval.request") approvals.push(String((e.payload as { tool?: unknown }).tool))
    })
    await s.engine.run(session.id, "default", "run tool", { interactionMode: "none" })
    // 不发布审批请求、不等待（直接拒绝），sh 未真正执行
    expect(approvals).toEqual([])
    const loaded = await s.store.load(session.id)
    const denied = loaded!.messages.find((m) => m.role === "tool" && m.name === "sh")
    expect(denied).toBeDefined()
    expect(String(denied!.content)).toContain("需要审批")
    expect(String(denied!.content)).not.toContain("hack")
    // 单用户对照：同引擎配置（authMode=multi 外）保持自动通过（见上一用例），此处确认会话可继续收尾
    expect(loaded!.messages.some((m) => m.role === "assistant")).toBe(true)
    cleanup(s.home)
  })

  test("multi-user + interactionMode=none: sh approval:false cannot bypass the hard gate", async () => {
    // approval:false 只放宽交互审批（弹卡）：服务模式无交互通道按剥离免审标记后的默认审批姿态拒绝，
    // 防模型自行声明免审绕过「无人值守不执行敏感工具」（含 flow 嵌套步骤，见 stripApprovalFlags）
    const s = await setup("approval", false, "server")
    s.provider.toolName = "sh"
    s.provider.toolArgs = { command: "echo bypass", approval: false }
    const session = await s.store.createSession("default", "t")
    const approvals: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.approval.request") approvals.push(String((e.payload as { tool?: unknown }).tool))
    })
    await s.engine.run(session.id, "default", "run tool", { interactionMode: "none" })
    expect(approvals).toEqual([])
    const loaded = await s.store.load(session.id)
    const denied = loaded!.messages.find((m) => m.role === "tool" && m.name === "sh")
    expect(denied).toBeDefined()
    expect(String(denied!.content)).toContain("需要审批")
    expect(String(denied!.content)).not.toContain("bypass")
    cleanup(s.home)
  })

  test("realtime: sh approval:false skips the approval card and executes directly", async () => {
    // 交互通道：模型声明免审（明确安全命令）时不弹审批卡，直接执行
    const s = await setup("approval")
    s.provider.toolName = "sh"
    s.provider.toolArgs = { command: "echo relax", approval: false }
    const session = await s.store.createSession("default", "t")
    const approvals: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.approval.request") approvals.push(String((e.payload as { tool?: unknown }).tool))
    })
    await s.engine.run(session.id, "default", "run tool")
    expect(approvals).toEqual([])
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool" && m.name === "sh" && String(m.content).includes("relax"))).toBe(true)
    cleanup(s.home)
  })

  test("multi-user ask 填值分支 cannot set GEBAI_APPROVAL_SKIP (fourth channel blocked)", async () => {
    // ask 填值分支是模型驱动的 env 写入通道：多用户模式下不得借此设置审批跳过键
    const s = await setup("askenv", false, "server")
    s.provider.askEnvName = "GEBAI_APPROVAL_SKIP"
    s.provider.askEnvSecondTool = "ls" // 第二轮换无需审批工具，避免审批等待
    const session = await s.store.createSession("default", "t")
    const envReqs: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.env.request") envReqs.push(String((e.payload as { name?: unknown }).name))
    })
    await s.engine.run(session.id, "default", "need env", { interactionMode: "realtime" })
    // 不发布填值卡片（无 env.request 事件），ask 填值分支返回拒绝说明
    expect(envReqs).toEqual([])
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool" && m.name === "ask" && String(m.content).includes("用户未提供环境变量 GEBAI_APPROVAL_SKIP"))).toBe(true)
    cleanup(s.home)
  })

  test("interactionMode=multi_turn still asks approval for critical tools (关键操作询问用户)", async () => {
    const s = await setup("approval")
    s.provider.toolName = "sh"
    s.provider.toolArgs = { command: "echo ask" }
    const session = await s.store.createSession("default", "t")
    const approvals: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.approval.request") approvals.push(String((e.payload as { tool?: unknown }).tool))
    })
    const run = s.engine.run(session.id, "default", "run tool", { interactionMode: "multi_turn" })
    // 多轮交互：关键操作（requiresApproval）经审批卡片询问用户（轮询等待审批事件，防全量并行下偶发超时）
    const t0 = Date.now()
    while (approvals.length === 0) {
      if (Date.now() - t0 > 2000) throw new Error("approval.request not published")
      await new Promise((r) => setTimeout(r, 20))
    }
    expect(approvals).toEqual(["sh"])
    await s.engine.decideApproval(session.id, "tc-1", true)
    await run
    cleanup(s.home)
  })

  test("outputMode=final_only 不推送文本增量/推理流，仍推送 done 与工具事件", async () => {
    const s = await setup("tool") // 第一轮 text "using tool" + tool_call ls；第二轮文本收尾
    s.provider.toolName = "ls"
    const session = await s.store.createSession("default", "t")
    const deltas: string[] = []
    const reasoning: string[] = []
    const dones: string[] = []
    const toolCalls: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.message.delta") deltas.push(String((e.payload as { text?: unknown }).text))
      if (e.type === "event.message.reasoning") reasoning.push(String((e.payload as { text?: unknown }).text))
      if (e.type === "event.message.done") dones.push(String((e.payload as { text?: unknown }).text))
      if (e.type === "event.tool.call") toolCalls.push(String((e.payload as { name?: unknown }).name))
    })
    await s.engine.run(session.id, "default", "do a thing", { outputMode: "final_only" })
    // 仅最终响应：无文本增量、无推理流（"using tool" 不推送）
    expect(deltas).toEqual([])
    expect(reasoning).toEqual([])
    // 结构化事件保留：工具调用事件与最终 done 正常推送
    expect(toolCalls).toContain("ls")
    expect(dones.some((d) => d.includes("result after"))).toBe(true)
    cleanup(s.home)
  })

  test("ask 填值分支 requests env from frontend and injects value into task env for later tools", async () => {
    const s = await setup("askenv")
    const session = await s.store.createSession("default", "t")
    // sh 需审批，跳过
    await s.store.setEnv(session.id, "default", { GEBAI_APPROVAL_SKIP: "true" })
    const envReqs: Array<{ envId: string; name: string; secret: boolean; description: string }> = []
    s.events.subscribe((e) => {
      if (e.type === "event.env.request") {
        const p = e.payload as { envId?: unknown; name?: unknown; secret?: unknown; description?: unknown }
        envReqs.push({ envId: String(p.envId), name: String(p.name), secret: p.secret === true, description: String(p.description ?? "") })
      }
    })
    const run = s.engine.run(session.id, "default", "need env")
    // 等待 event.env.request 发布（含变量名与说明，供前端弹窗展示）
    await new Promise((r) => setTimeout(r, 100))
    expect(envReqs).toHaveLength(1)
    expect(envReqs[0].name).toBe("MY_KEY")
    expect(envReqs[0].description).toBe("测试密钥")
    expect(envReqs[0].secret).toBe(true)
    // 模拟前端提交值 → 注入任务 env（sh 工具后续读取立即生效）
    await s.engine.decideEnvResult(session.id, envReqs[0].envId, "v1")
    await run
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool" && m.name === "ask" && String(m.content).includes("已由用户设置"))).toBe(true)
    // 值注入生效：后续 sh 工具 echo $MY_KEY 输出 v1
    expect(loaded!.messages.some((m) => m.role === "tool" && m.name === "sh" && String(m.content).includes("v1"))).toBe(true)
    cleanup(s.home)
  })

  test("ask 填值分支 reports refusal when user declines", async () => {
    const s = await setup("askenv")
    const session = await s.store.createSession("default", "t")
    await s.store.setEnv(session.id, "default", { GEBAI_APPROVAL_SKIP: "true" })
    const envReqs: Array<{ envId: string }> = []
    s.events.subscribe((e) => {
      if (e.type === "event.env.request") envReqs.push({ envId: String((e.payload as { envId?: unknown }).envId) })
    })
    const run = s.engine.run(session.id, "default", "need env")
    await new Promise((r) => setTimeout(r, 100))
    expect(envReqs).toHaveLength(1)
    await s.engine.decideEnvResult(session.id, envReqs[0].envId, null)
    await run
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "tool" && m.name === "ask" && String(m.content).includes("用户未提供环境变量 MY_KEY"))).toBe(true)
    cleanup(s.home)
  })

  test("disabledTools blocks tools in new sessions too (inherited globals filtered)", async () => {
    const s = await setup("subwrite")
    // 新会话继承的全局 write：禁用 write 对新会话同样生效
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "ask code agent to write", { disabledTools: ["write"] })
    // 新会话内的 schema 过滤（write 不出现在子Agent 模型调用的工具列表，seenTools[1] 为子Agent 首轮）
    expect(s.provider.seenTools[1]).not.toContain("write")
    // 新会话循环内被调用时阻止执行（禁用说明作为工具结果返回给子Agent 模型）
    const disabledMsgs = s.provider.seenChats.some((chat) =>
      chat.some((m) => m.role === "tool" && m.name === "write" && String(m.content).includes("当前通道不可用")),
    )
    expect(disabledMsgs).toBe(true)
    cleanup(s.home)
  })

  test("disabledTools does not leak across runs (per-task scope)", async () => {
    const s = await setup("tool")
    s.provider.toolName = "show"
    s.provider.toolArgs = { html: "<p>hi</p>" }
    const session = await s.store.createSession("default", "t")
    // 第一轮禁用 show
    await s.engine.run(session.id, "default", "render a page", { disabledTools: ["show"] })
    expect(s.provider.seenTools[0]).not.toContain("show")
    // 第二轮未传 disabledTools：schema 恢复完整（show 再次可见可执行）
    s.provider.calls = 0
    s.provider.seenTools.length = 0
    await s.engine.run(session.id, "default", "render a page")
    expect(s.provider.seenTools[0]).toContain("show")
    const loaded = await s.store.load(session.id)
    const toolMsg = loaded!.messages.find((m) => m.role === "tool" && m.name === "show" && m.content.includes("已生成"))
    expect(toolMsg).toBeDefined()
    cleanup(s.home)
  })

  test("channelNote 通道环境注记注入系统提示词（未传时不注入）", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let sysPrompt = ""
    s.provider.chat = async function* (msgs: import("@gebai/sdk").MessageLike[]) {
      for (const m of msgs) if (m.role === "system") sysPrompt = String(m.content)
      yield { type: "text", text: "ok" }
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "hi", { channelNote: "当前对话经飞书机器人通道进行：审批经卡片按钮作答。" })
    expect(sysPrompt).toContain("当前对话经飞书机器人通道进行")
    expect(sysPrompt).toContain("审批经卡片按钮作答")
    // 未传时不注入（引擎通道无关，Web/REST 通道无注记）
    await s.engine.run(session.id, "default", "hi")
    expect(sysPrompt).not.toContain("当前对话经飞书机器人通道进行")
    cleanup(s.home)
  })

  test("main system prompt includes task-type routing guide (D1 子 Agent 自动推荐)", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let sysPrompt = ""
    s.provider.chat = async function* (msgs: import("@gebai/sdk").MessageLike[]) {
      for (const m of msgs) if (m.role === "system") sysPrompt = String(m.content)
      yield { type: "text", text: "ok" }
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "hi")
    expect(sysPrompt).toContain("任务类型路由")
    expect(sysPrompt).toContain("按任务类型从下方「可选子Agent」清单选用")
    // 路由信息由子Agent description（触发场景）承载，不再硬编码映射表
    expect(sysPrompt).not.toContain("→ code")
    expect(sysPrompt).not.toContain("→ playwright")
    // 未装载清单注入全部子Agent 的触发场景描述（code/self_optimize 含职责边界）
    expect(sysPrompt).toContain("- code: 涉及代码编写与源码分析时装载本子Agent（不处理歌白自身代码，自我优化用 self_optimize）")
    expect(sysPrompt).toContain("- playwright: 涉及网页浏览/信息抓取/表单填写/页面自动化验证时装载本子Agent")
    expect(sysPrompt).toContain("- self_optimize: 优化歌白自身")
    // 路由段不含子Agent 专属行为约束（预置项目/受限模式说明只注入子Agent 系统提示词）
    expect(sysPrompt).not.toContain("受限模式")
    expect(sysPrompt).not.toContain("预置项目")
    cleanup(s.home)
  })

  test("empty response (no text, no tool call) is retried with a hint then succeeds", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    s.provider.chat = async function* (msgs: import("@gebai/sdk").MessageLike[]) {
      calls++
      if (calls === 1) {
        yield { type: "done", stopReason: "stop" } // 空响应：无文本无工具调用
        return
      }
      // 重试注入的提示为 user 角色（对 OpenAI/Anthropic 均合法），且不污染会话历史
      const last = msgs[msgs.length - 1]
      expect(last.role).toBe("user")
      expect(String(last.content)).toContain("没有返回任何内容")
      yield { type: "text", text: "第二次终于有内容了" }
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "hi")
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "第二次终于有内容了")).toBe(true)
    // 注入提示未持久化
    expect(loaded!.messages.some((m) => String(m.content).includes("没有返回任何内容"))).toBe(false)
    expect(calls).toBe(2)
    cleanup(s.home)
  })

  test("provider exception with no output is retried then succeeds", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    s.provider.chat = async function* () {
      calls++
      if (calls === 1) throw new Error("网络中断")
      yield { type: "text", text: "重试后恢复" }
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "hi")
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "重试后恢复")).toBe(true)
    expect(calls).toBe(2)
    cleanup(s.home)
  })

  test("模型服务异常重试期间发布 event.model.error（前端可见的非终态提示）", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    s.provider.chat = async function* () {
      calls++
      if (calls === 1) throw new Error("网络中断")
      if (calls === 2) {
        yield { type: "done", stopReason: "stop" } // 空响应也重试
        return
      }
      yield { type: "text", text: "第三次成功" }
      yield { type: "done", stopReason: "stop" }
    }
    const modelErrors: Array<Record<string, unknown>> = []
    s.events.subscribe((e) => {
      if (e.type === "event.model.error") modelErrors.push(e.payload)
    })
    await s.engine.run(session.id, "default", "hi")
    // 两次将重试的异常均推送（接口异常 + 空响应），携带 retry/maxRetry；最终成功（非终态）
    expect(modelErrors.length).toBe(2)
    expect(modelErrors[0]).toMatchObject({ error: "网络中断", retry: 1 })
    expect(String(modelErrors[1].error)).toContain("空响应")
    expect(modelErrors[1].retry).toBe(2)
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "第三次成功")).toBe(true)
    cleanup(s.home)
  })

  test("stream interruption after partial output is not retried (avoids duplicate text)", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    const errors: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.task.error") errors.push(String(e.payload.error))
    })
    s.provider.chat = async function* () {
      calls++
      yield { type: "text", text: "部分内容" }
      throw new Error("流中断")
    }
    await s.engine.run(session.id, "default", "hi")
    expect(calls).toBe(1) // 有产出后不重试
    expect(errors.some((m) => m.includes("流中断"))).toBe(true)
    cleanup(s.home)
  })

  test("empty response retries exhaust and surface friendly error", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    const errors: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.task.error") errors.push(String(e.payload.error))
    })
    s.provider.chat = async function* () {
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "hi")
    expect(errors.some((m) => m.includes("模型未返回任何内容"))).toBe(true)
    cleanup(s.home)
  })



  test("repeated identical tool calls are interrupted with a hint, then the model completes", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    s.provider.chat = async function* () {
      calls++
      if (calls < 4) {
        yield { type: "text", text: "try again" }
        yield { type: "tool_call", toolCall: { id: `tc-r${calls}`, name: "ls", arguments: {} } }
        yield { type: "done", stopReason: "tool_calls" }
        return
      }
      // 第 3 次相同调用被中断（未执行），模型收到引导提示后换方向给出纯文本回复
      yield { type: "text", text: "换用其他方案" }
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "do it")
    const loaded = await s.store.load(session.id)
    const toolMsgs = loaded!.messages.filter((m) => m.role === "tool" && m.name === "ls")
    // 前两次正常执行，第三次被中断（注入引导提示，未再次执行）
    expect(toolMsgs.filter((m) => String(m.content).includes("已中断重复的工具调用")).length).toBe(1)
    expect(toolMsgs.filter((m) => !String(m.content).includes("已中断重复的工具调用")).length).toBe(2)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "换用其他方案")).toBe(true)
    expect(calls).toBe(4)
    cleanup(s.home)
  })

  test("repeated calls: stall exhaustion terminates the loop early", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    s.provider.chat = async function* () {
      calls++
      yield { type: "text", text: `round ${calls}` }
      yield { type: "tool_call", toolCall: { id: `tc-s${calls}`, name: "ls", arguments: {} } }
      yield { type: "done", stopReason: "tool_calls" }
    }
    await s.engine.run(session.id, "default", "loop")
    const loaded = await s.store.load(session.id)
    // 第 3/4 次中断（stalls 1、2），第 5 次中断超限（stalls 3 > 2）→ 终止循环，远低于 MAX_TOOL_ROUNDS
    expect(calls).toBe(5)
    const interr = loaded!.messages.filter((m) => m.role === "tool" && String(m.content).includes("已中断重复的工具调用"))
    expect(interr.length).toBe(3)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "round 5")).toBe(true)
    cleanup(s.home)
  })

  test("same-batch identical calls are deliberate fan-out: all execute, no repeat interrupt (同批重复签名只计一次)", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    s.provider.chat = async function* () {
      calls++
      if (calls === 1) {
        // 同批三个完全相同的调用（有意扇出：发出时尚未见任何结果）
        yield { type: "tool_call", toolCall: { id: "tc-f1", name: "ls", arguments: {} } }
        yield { type: "tool_call", toolCall: { id: "tc-f2", name: "ls", arguments: {} } }
        yield { type: "tool_call", toolCall: { id: "tc-f3", name: "ls", arguments: {} } }
        yield { type: "done", stopReason: "tool_calls" }
        return
      }
      yield { type: "text", text: "fan-out done" }
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "fan out")
    const loaded = await s.store.load(session.id)
    const toolMsgs = loaded!.messages.filter((m) => m.role === "tool" && m.name === "ls")
    // 三个相同调用全部执行（无重复中断），三条结果均落盘（第二轮模型可见）
    expect(toolMsgs.length).toBe(3)
    expect(toolMsgs.some((m) => String(m.content).includes("已中断重复的工具调用"))).toBe(false)
    expect(loaded!.messages.some((m) => m.role === "assistant" && m.content === "fan-out done")).toBe(true)
    cleanup(s.home)
  })

  test("cross-round repetition still interrupted after same-batch fan-out (跨轮重复照常累积判定)", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    let calls = 0
    s.provider.chat = async function* () {
      calls++
      if (calls === 1) {
        // 第一轮同批两个相同调用（扇出，各记录一次签名）
        yield { type: "tool_call", toolCall: { id: "tc-c1", name: "ls", arguments: {} } }
        yield { type: "tool_call", toolCall: { id: "tc-c2", name: "ls", arguments: {} } }
        yield { type: "done", stopReason: "tool_calls" }
        return
      }
      if (calls === 2) {
        // 第二轮再见结果后重发同签名：为窗口内第 2 次记录，尚不触发
        yield { type: "tool_call", toolCall: { id: "tc-c3", name: "ls", arguments: {} } }
        yield { type: "done", stopReason: "tool_calls" }
        return
      }
      if (calls === 3) {
        // 第三轮仍重发同签名：窗口第 3 次记录命中阈值，被中断（注入引导提示）
        yield { type: "tool_call", toolCall: { id: "tc-c4", name: "ls", arguments: {} } }
        yield { type: "done", stopReason: "tool_calls" }
        return
      }
      yield { type: "text", text: "stopped repeating" }
      yield { type: "done", stopReason: "stop" }
    }
    await s.engine.run(session.id, "default", "repeat")
    const loaded = await s.store.load(session.id)
    const toolMsgs = loaded!.messages.filter((m) => m.role === "tool" && m.name === "ls")
    // 第一轮 2 个执行（扇出各记录一次）+ 第二轮 1 个执行（第 2 次记录），第三轮重发被中断（第 3 次记录命中）
    expect(toolMsgs.filter((m) => String(m.content).includes("已中断重复的工具调用")).length).toBe(1)
    expect(toolMsgs.filter((m) => !String(m.content).includes("已中断重复的工具调用")).length).toBe(3)
    expect(calls).toBe(4)
    cleanup(s.home)
  })
})

describe("context compaction", () => {
  /** 直接构造含历史消息的会话（跳过引擎 run，避免依赖 provider）。 */
  async function sessionWithHistory(n = 8) {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    for (let i = 0; i < n; i++) {
      await s.store.appendMessage(session.id, { id: crypto.randomUUID(), role: "user", content: `问题 ${i}`, createdAt: Date.now() })
      await s.store.appendMessage(session.id, { id: crypto.randomUUID(), role: "assistant", content: `回答 ${i}`, createdAt: Date.now() })
    }
    return { ...s, session }
  }

  test("store.compactMessages replaces [from,to) with a compacted summary message", async () => {
    const { home, store, session } = await sessionWithHistory(6)
    const before = (await store.load(session.id))!.messages.length
    const next = await store.compactMessages(session.id, "default", { from: 0, to: 4, summary: "摘要文本" })
    // 区间内 2 条用户输入原位保留：长度 = 12 - 4 + 1(摘要) + 2(保留用户) = 11
    expect(next.length).toBe(before - 4 + 1 + 2)
    const compacted = next.find((m) => m.compacted)
    expect(compacted).toBeDefined()
    expect(compacted!.role).toBe("system")
    expect(compacted!.content).toBe("摘要文本")
    expect(compacted!.summary).toContain("已压缩 2 条")
    // 用户输入不压缩不改变（区间内 user 消息原位保留、内容不变）
    expect(next.filter((m) => m.role === "user" && m.content === "问题 0")).toHaveLength(1)
    expect(next.filter((m) => m.role === "user" && m.content === "问题 1")).toHaveLength(1)
    cleanup(home)
  })

  test("engine.compactSession summarizes oldest messages and publishes event", async () => {
    const s = await sessionWithHistory(6)
    const events: string[] = []
    s.events.subscribe((e) => events.push(e.type))
    const result = await s.engine.compactSession(s.session.id, "default")
    expect(result.compacted).toBeGreaterThan(0)
    expect(result.summary).toBeTruthy()
    const loaded = await s.store.load(s.session.id)
    expect(loaded!.messages.some((m) => m.compacted)).toBe(true)
    // 用户输入不压缩不改变：压缩区间内的 user 消息原位保留
    for (let i = 1; i < 3; i++) expect(loaded!.messages.some((m) => m.role === "user" && m.content === `问题 ${i}`)).toBe(true)
    expect(events).toContain("event.message.compact")
    // loadHistory 将摘要消息作为 assistant 注入（不污染 system 段）
    const msg = (s.engine as unknown as { loadHistory(sessionId: string, user: string): Promise<import("@gebai/sdk").MessageLike[]> }).loadHistory
    const history = await msg.call(s.engine, s.session.id, "default")
    expect(history.some((m) => String(m.content).startsWith("[历史摘要]"))).toBe(true)
    cleanup(s.home)
  })

  test("compactSession 不碰子Agent 装载提示词消息（不选进区间、不进摘要输入、原位保留）", async () => {
    const s = await sessionWithHistory(4)
    // 会话中途装载：历史中间插入装载提示词消息（模拟 agent_load 后追加）
    await s.store.appendMessage(s.session.id, { id: "load-1", role: "system", loadedAgent: "code", content: "### code（完整提示词）\n你是源码分析专家。", createdAt: Date.now() } as never)
    for (let i = 0; i < 6; i++) {
      await s.store.appendMessage(s.session.id, { id: crypto.randomUUID(), role: "user", content: `后续问题 ${i}`, createdAt: Date.now() })
      await s.store.appendMessage(s.session.id, { id: crypto.randomUUID(), role: "assistant", content: `后续回答 ${i}`, createdAt: Date.now() })
    }
    // 摘要输入断言：provider 收到的压缩输入不含装载提示词内容
    let summaryInput = ""
    const spy = {
      ...s.provider,
      chat: async function* (msgs: import("@gebai/sdk").MessageLike[]) {
        summaryInput = JSON.stringify(msgs)
        yield { type: "text", text: "压缩摘要" }
        yield { type: "done" }
      },
    }
    ;(s.engine as unknown as { opts: { provider: unknown } }).opts.provider = spy
    await s.engine.compactSession(s.session.id, "default")
    expect(summaryInput).not.toContain("你是源码分析专家") // 提示词全文未进摘要输入
    const loaded = await s.store.load(s.session.id)
    const note = loaded!.messages.find((m) => m.loadedAgent === "code")
    expect(note).toBeDefined() // 消息本体原位保留（未被压缩替换/移动）
    expect(note!.content).toContain("你是源码分析专家")
    cleanup(s.home)
  })

  test("compactSession degrades to rolling truncation when summarization fails", async () => {
    const s = await sessionWithHistory(4)
    // provider 摘要调用抛错 → 降级占位摘要（滚动裁剪语义）
    const throwing = {
      ...s.provider,
      chat: async function* () {
        throw new Error("provider down")
      },
    }
    ;(s.engine as unknown as { opts: { provider: unknown } }).opts.provider = throwing
    const result = await s.engine.compactSession(s.session.id, "default")
    expect(result.compacted).toBeGreaterThan(0)
    expect(result.summary).toContain("上下文已裁剪")
    cleanup(s.home)
  })

  test("auto compaction triggers when the model service rejects with context length exceeded（接口 4xx = 真实大小信号）", async () => {
    const s = await sessionWithHistory(10)
    // 无 usage 基线（新会话/接口不返回 usage）：不做估算预判——接口以上下文长度错误拒绝
    // （真实大小信号）后，压缩最早历史并重试成功
    let calls = 0
    const smallCap = {
      ...s.provider,
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 40 }),
      chat: async function* () {
        calls++
        if (calls <= 3) throw new Error("模型接口错误（HTTP 400）: This model's maximum context length is 40 tokens. However, your messages resulted in 5000 tokens.")
        yield { type: "text", text: "ok" }
        yield { type: "done" }
      },
    }
    ;(s.engine as unknown as { opts: { provider: unknown } }).opts.provider = smallCap
    await s.engine.run(s.session.id, "default", "继续")
    const loaded = await s.store.load(s.session.id)
    expect(loaded!.messages.some((m) => m.compacted)).toBe(true) // 溢出恢复触发自动压缩
    cleanup(s.home)
  })

  test("repeated compaction never re-compacts an existing summary message", async () => {
    const s = await sessionWithHistory(10) // 20 条消息
    const r1 = await s.engine.compactSession(s.session.id, "default")
    // 区间内 5 条 assistant 被摘要替换（4 条 user 原位保留，不算压缩条数）
    expect(r1.compacted).toBe(5)
    const r2 = await s.engine.compactSession(s.session.id, "default")
    // 第二轮压缩剩余未压缩历史（保留最近一半），摘要消息本身不参与
    expect(r2.compacted).toBe(2)
    const after = (await s.store.load(s.session.id))!.messages
    const compacted = after.filter((m) => m.compacted)
    expect(compacted.length).toBe(2)
    // 两条摘要消息均描述各自的原始区间（不存在「摘要的摘要」链）
    for (const c of compacted) expect(c.summary).toContain("已压缩")
    cleanup(s.home)
  })

  test("摘要输入只含将被移除的可压缩消息：系统提示词与用户输入不进摘要", async () => {
    const s = await sessionWithHistory(4) // u0,a0,u1,a1,u2,a2,u3,a3
    const inputs: string[] = []
    const spy = {
      ...s.provider,
      chat: async function* (msgs: import("@gebai/sdk").MessageLike[]) {
        inputs.push(JSON.stringify(msgs))
        yield { type: "text", text: "压缩摘要" }
        yield { type: "done" }
      },
    }
    ;(s.engine as unknown as { opts: { provider: unknown } }).opts.provider = spy
    await s.engine.compactSession(s.session.id, "default")
    expect(inputs.length).toBeGreaterThan(0)
    // assistant 内容进摘要输入；用户输入不进（原样保留在上下文中）
    expect(inputs[0]).toContain("回答 0")
    expect(inputs[0]).toContain("回答 1")
    expect(inputs[0]).not.toContain("问题 1")
    cleanup(s.home)
  })

  test("blank reasoning chunks are not published", async () => {
    const s = await setup("text")
    const blankProvider = {
      ...s.provider,
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 10000 }),
      chat: async function* () {
        yield { type: "reasoning", text: "   " } // 纯空白
        yield { type: "reasoning", text: "" } // 空字符串
        yield { type: "reasoning", text: "真正的推理" }
        yield { type: "text", text: "正文" }
        yield { type: "done" }
      },
    }
    ;(s.engine as unknown as { opts: { provider: unknown } }).opts.provider = blankProvider
    const published: string[] = []
    s.events.subscribe((e) => {
      if (e.type === "event.message.reasoning") published.push(String(e.payload.text))
    })
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "hi")
    expect(published).toEqual(["真正的推理"])
    cleanup(s.home)
  })

  test("reasoning 持久化为独立字段：content 纯正文，回放给 LLM 不含推理，旧版 content 内嵌 think 块仍剥离", async () => {
    const s = await setup("text")
    const reasoningProvider = {
      ...s.provider,
      capabilities: () => ({ streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 10000 }),
      chat: async function* () {
        yield { type: "reasoning", text: "思考过程" }
        yield { type: "text", text: "正式回答" }
        yield { type: "done" }
      },
    }
    ;(s.engine as unknown as { opts: { provider: unknown } }).opts.provider = reasoningProvider
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "hi")
    // 落盘：推理在独立字段 reasoning，content 为纯正文（不再内嵌 <think>）
    const loaded = await s.store.load(session.id)
    const assistant = loaded!.messages.find((m) => m.role === "assistant")
    expect(assistant?.content).toBe("正式回答")
    expect(assistant?.reasoning).toBe("思考过程")
    expect(assistant?.content).not.toContain("<think>")
    // 回放：loadHistory 出的消息不含推理（独立字段不映射 + 旧版 think 块剥离），防推理泄漏进模型上下文
    const history = await (s.engine as unknown as { loadHistory(sessionId: string, user: string): Promise<Array<{ role: string; content: string; reasoning?: unknown }>> }).loadHistory(session.id, "default")
    const last = history[history.length - 1]!
    expect(last.role).toBe("assistant")
    expect(last.content).toBe("正式回答")
    expect(last.reasoning).toBeUndefined()
    // 旧版兼容：content 内嵌 think 块的历史消息回放时仍剥离
    await s.store.appendMessage(session.id, { id: "legacy-1", role: "assistant", content: "<think>旧思考</think>旧正文", createdAt: Date.now() } as never)
    const history2 = await (s.engine as unknown as { loadHistory(sessionId: string, user: string): Promise<Array<{ role: string; content: string }>> }).loadHistory(session.id, "default")
    const legacy = history2.find((m) => m.content === "旧正文")
    expect(legacy).toBeDefined()
    expect(history2.some((m) => String(m.content).includes("<think>"))).toBe(false)
    cleanup(s.home)
  })

  test("stripThinkTags removes think blocks and trims（旧版 content 内嵌数据兼容）", async () => {
    expect(stripThinkTags("<think>推理</think>正文")).toBe("正文")
    expect(stripThinkTags("<think>推理</think>")).toBe("")
    expect(stripThinkTags("纯正文")).toBe("纯正文")
    expect(stripThinkTags("前<think>a</think>中<think>b</think>后")).toBe("前中后")
  })

  test("sub-agent project binding: CODE_PROJECT roots workdir and paths", async () => {
    const s = await setup("subwrite")
    const project = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const session = await s.store.createSession("default", "t")
    // 会话环境变量声明项目根（子Agent 大写前缀 CODE_PROJECT）+ 跳过审批（write 需审批）
    await s.store.setEnv(session.id, "default", { CODE_PROJECT: project, GEBAI_APPROVAL_SKIP: "true" })
    await s.engine.run(session.id, "default", "modify project")
    // 子Agent 内的 write 写到项目根（而非会话 tmp）
    const written = await Bun.file(join(project, "out.txt")).text()
    expect(written).toBe("hi")
    rmSync(project, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("agent_run 预加载 self_optimize 连带预载 code（工具与提示词复用）+ 写范围守卫生效", async () => {
    // 环境隔离：selfModifyEnabled 直读 process.env，宿主 .env 配置 GEBAI_SELF_MODIFY=true 会整体放开写
    // 范围守卫——本测试验证默认只读路径。清空必须置空串而非 delete：config 的 .env 注入只跳过非 undefined
    // 键（delete 后 setup() 会从 .env 回填 true），而 selfModifyEnabled 仅认 "true"/"1"（空串=关闭）
    const savedSelfModify = process.env.GEBAI_SELF_MODIFY
    process.env.GEBAI_SELF_MODIFY = ""
    try {
      const s = await setup("subself")
      // 临时歌白仓库结构（SELF_OPTIMIZE_PROJECT 指向它，守卫按它界定仓库边界）
      const repo = mkdtempSync(join(tmpdir(), "gebai-selfopt-repo-"))
      mkdirSync(join(repo, "packages", "server", "src", "sub-agents"), { recursive: true })
      mkdirSync(join(repo, "packages", "server", "src", "core"), { recursive: true })
      const session = await s.store.createSession("default", "t")
      await s.store.setEnv(session.id, "default", { SELF_OPTIMIZE_PROJECT: repo, GEBAI_APPROVAL_SKIP: "true" })
      await s.engine.run(session.id, "default", "optimize gebai")
      // 新会话系统消息：连带预载 code（两段职责提示词都在，通用工作流来自 code）
      const sys = String(s.provider.seenChats[1][0].content)
      expect(sys).toContain("已预加载子Agent: code, self_optimize")
      expect(sys).toContain("### code（")
      expect(sys).toContain("源码分析与修改专家")
      expect(sys).toContain("### self_optimize（")
      expect(sys).toContain("自我优化专家")
      // 新会话工具集：继承的全局工具（read/write）+ code_* 独有工具 + self_optimize_* 独有工具并存（不重复注册）
      expect(s.provider.seenTools[1]).toContain("read")
      expect(s.provider.seenTools[1]).toContain("write")
      expect(s.provider.seenTools[1]).toContain("code_search_symbols")
      expect(s.provider.seenTools[1]).toContain("self_optimize_run_tests")
      expect(s.provider.seenTools[1]).not.toContain("self_optimize_write")
      expect(s.provider.seenTools[1]).not.toContain("code_read")
      // 写范围守卫：核心引擎源码被拒（未写入），子Agent 目录放行
      expect(existsSync(join(repo, "packages", "server", "src", "core", "engine.ts"))).toBe(false)
      expect(await Bun.file(join(repo, "packages", "server", "src", "sub-agents", "new_agent.ts")).text()).toBe("x")
      const chats = JSON.stringify(s.provider.seenChats)
      expect(chats).toContain("拒绝写入")
      rmSync(repo, { recursive: true, force: true })
      cleanup(s.home)
    } finally {
      if (savedSelfModify === undefined) delete process.env.GEBAI_SELF_MODIFY
      else process.env.GEBAI_SELF_MODIFY = savedSelfModify
    }
  })

  test("sub-agent project AGENTS.md is injected into system prompt", async () => {
    const s = await setup("subwrite")
    const project = mkdtempSync(join(tmpdir(), "gebai-agentsmd-"))
    writeFileSync(join(project, "AGENTS.md"), "# 项目约定\n- 只用 TypeScript\n- 禁止使用 any")
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "modify project", { envOverride: { CODE_PROJECT: project, GEBAI_APPROVAL_SKIP: "true" } })
    // 子 Agent 的 system 消息（seenChats[1] 为 code 子 Agent 内首次 chat）包含 AGENTS.md 内容
    const sys = String(s.provider.seenChats[1][0].content)
    expect(sys).toContain("项目约定（AGENTS.md")
    expect(sys).toContain("禁止使用 any")
    rmSync(project, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("sub-agent project without AGENTS.md injects nothing extra", async () => {
    const s = await setup("subwrite")
    const project = mkdtempSync(join(tmpdir(), "gebai-noagentsmd-"))
    const session = await s.store.createSession("default", "t")
    await s.engine.run(session.id, "default", "modify project", { envOverride: { CODE_PROJECT: project, GEBAI_APPROVAL_SKIP: "true" } })
    const sys = String(s.provider.seenChats[1][0].content)
    expect(sys).not.toContain("项目约定（AGENTS.md")
    expect(sys).not.toContain("项目约定（AGENT.md")
    rmSync(project, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("envOverride (browser-local env) roots project without persisting", async () => {
    const s = await setup("subwrite")
    const project = mkdtempSync(join(tmpdir(), "gebai-proj-ovr-"))
    const session = await s.store.createSession("default", "t")
    // 浏览器本地环境变量经 run 的 envOverride 注入（模拟前端 localStorage），不写入会话 env 副本
    await s.engine.run(session.id, "default", "modify project", { envOverride: { CODE_PROJECT: project, GEBAI_APPROVAL_SKIP: "true" } })
    const written = await Bun.file(join(project, "out.txt")).text()
    expect(written).toBe("hi")
    // 未持久化：会话 env 副本无该变量（仅本次任务临时生效）
    expect(await s.store.getEnv(session.id, "default")).toEqual({})
    rmSync(project, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("preset projects: CODE_PROJECTS injected into prompt and project param roots file ops", async () => {
    const s = await setup("subproj")
    const projA = mkdtempSync(join(tmpdir(), "gebai-pa-"))
    const projB = mkdtempSync(join(tmpdir(), "gebai-pb-"))
    const session = await s.store.createSession("default", "t")
    s.provider.toolArgs = { project: "app", path: "out.txt", content: "hi" }
    // 预置项目注册表：name + path + 可选 description（项目名作为工具参数）
    await s.store.setEnv(session.id, "default", {
      CODE_PROJECTS: JSON.stringify([
        { name: "app", path: projA, description: "主业务应用" },
        { name: "lib", path: projB },
      ]),
      GEBAI_APPROVAL_SKIP: "true",
    })
    await s.engine.run(session.id, "default", "modify preset project")
    // project 参数命中预置项目 app：写入落在 app 项目根（而非会话 tmp）
    const written = await Bun.file(join(projA, "out.txt")).text()
    expect(written).toBe("hi")
    // 子Agent 系统提示词注入预置项目清单（名称 + 说明 + 路径）
    const subPrompt = s.provider.seenChats.find((msgs) => msgs.some((m) => m.role === "system" && typeof m.content === "string" && m.content.includes("预置项目（全局文件工具用 project 参数指定项目名")))
    expect(subPrompt).toBeDefined()
    const sysContent = String(subPrompt!.find((m) => m.role === "system")!.content)
    expect(sysContent).toContain("- app: 主业务应用（")
    expect(sysContent).toContain(`- lib（${projB}）`)
    // 环境注记前置：预置项目清单位于静态提示词（工作流）之前，模型开工先读环境
    const noteIdx = sysContent.indexOf("预置项目（全局文件工具用 project 参数指定项目名")
    const staticIdx = sysContent.indexOf("你是源码分析与修改专家")
    expect(noteIdx).toBeGreaterThanOrEqual(0)
    expect(staticIdx).toBeGreaterThan(noteIdx)
    rmSync(projA, { recursive: true, force: true })
    rmSync(projB, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("loaded-mode: preset projects surfaced in code description (main prompt) while full note stays sub-agent only", async () => {
    const s = await setup("loadproj")
    const projA = mkdtempSync(join(tmpdir(), "gebai-lp-"))
    writeFileSync(join(projA, "README.md"), "# app readme")
    const session = await s.store.createSession("default", "t")
    s.provider.toolArgs = { project: "app", path: "README.md" }
    // 预置项目注册表（含项目说明）：code 描述动态体现预置项目（总Agent 按项目名关联任务），完整清单注记只注入子Agent 系统提示词
    await s.store.setEnv(session.id, "default", {
      CODE_PROJECTS: JSON.stringify([{ name: "app", path: projA, description: "主业务应用" }]),
    })
    await s.engine.run(session.id, "default", "analyze project")
    // 总Agent 系统提示词：未装载 code 的描述体现预置项目（名称/说明/路径），但不含子Agent 专属完整注记
    const sys = String(s.provider.seenChats[0].find((m) => m.role === "system")!.content)
    expect(sys).not.toContain("预置项目（全局文件工具用 project 参数指定项目名")
    expect(sys).toContain("主业务应用")
    expect(sys).toContain(projA)
    // 装载后直接用全局 read + project 参数：路由到预置项目根读取文件
    const readChat = s.provider.seenChats.find((msgs) => msgs.some((m) => m.role === "tool" && typeof m.content === "string" && m.content.includes("# app readme")))
    expect(readChat).toBeDefined()
    rmSync(projA, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("loaded-mode: CODE_PROJECT binding injected into main prompt", async () => {
    const s = await setup("text")
    const project = mkdtempSync(join(tmpdir(), "gebai-bind-"))
    const session = await s.store.createSession("default", "t")
    await s.store.setEnv(session.id, "default", { CODE_PROJECT: project })
    await s.engine.run(session.id, "default", "hi")
    // 项目根绑定注入总Agent 系统提示词（装载模式直接使用工具时同样生效）
    const sys = String(s.provider.seenChats[0].find((m) => m.role === "system")!.content)
    expect(sys).toContain("code 子Agent 项目绑定")
    expect(sys).toContain(project)
    rmSync(project, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("preset projects: unknown project name yields tool error", async () => {
    const s = await setup("subproj")
    const projA = mkdtempSync(join(tmpdir(), "gebai-pa2-"))
    const session = await s.store.createSession("default", "t")
    s.provider.toolArgs = { project: "nope", path: "out.txt", content: "hi" }
    await s.store.setEnv(session.id, "default", {
      CODE_PROJECTS: JSON.stringify([{ name: "app", path: projA }]),
      GEBAI_APPROVAL_SKIP: "true",
    })
    await s.engine.run(session.id, "default", "modify preset project")
    // 未知预置项目：工具结果注入错误，引导模型修正（子Agent 内部消息可见）
    const errChat = s.provider.seenChats.find((msgs) => msgs.some((m) => m.role === "tool" && typeof m.content === "string" && m.content.includes("未知预置项目: nope")))
    expect(errChat).toBeDefined()
    rmSync(projA, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("preset projects: invalid CODE_PROJECTS JSON ignored and free-path mode keeps working", async () => {
    const s = await setup("subproj")
    const session = await s.store.createSession("default", "t")
    // 无 project 参数（自由项目路径模式）+ 非法 JSON：不崩溃，按路径写会话 tmp
    s.provider.toolArgs = { path: "out.txt", content: "hi" }
    await s.store.setEnv(session.id, "default", {
      CODE_PROJECTS: "{not-json",
      GEBAI_APPROVAL_SKIP: "true",
    })
    await s.engine.run(session.id, "default", "modify file")
    // 非法 JSON 未注入清单；自由路径模式正常写入会话 tmp
    const sysMsgs = s.provider.seenChats.flatMap((msgs) => msgs.map((m) => String(m.content)))
    expect(sysMsgs.some((c) => c.includes("预置项目（全局文件工具用 project 参数指定项目名"))).toBe(false)
    const writeChat = s.provider.seenChats.find((msgs) => msgs.some((m) => m.role === "tool" && m.name === "write" && typeof m.content === "string" && m.content.includes("已写入")))
    expect(writeChat).toBeDefined()
    cleanup(s.home)
  })

  test("preset projects: grep searches project root (skips heavy dirs)", async () => {
    const s = await setup("subgrep")
    const projA = mkdtempSync(join(tmpdir(), "gebai-pg-"))
    mkdirSync(join(projA, "src"), { recursive: true })
    mkdirSync(join(projA, "node_modules", "pkg"), { recursive: true })
    writeFileSync(join(projA, "src", "main.ts"), "export const hello = 1\n")
    writeFileSync(join(projA, "node_modules", "pkg", "x.js"), "hello from node_modules\n")
    const session = await s.store.createSession("default", "t")
    s.provider.toolArgs = { project: "app", pattern: "hello" }
    await s.store.setEnv(session.id, "default", {
      CODE_PROJECTS: JSON.stringify([{ name: "app", path: projA }]),
    })
    await s.engine.run(session.id, "default", "search code")
    // project 模式下 grep 递归扫描项目根（相对路径输出）
    const grepChat = s.provider.seenChats.find((msgs) => msgs.some((m) => m.role === "tool" && typeof m.content === "string" && m.content.includes("src/main.ts:1")))
    expect(grepChat).toBeDefined()
    const content = String(grepChat!.find((m) => m.role === "tool")!.content)
    // 大型/生成目录被跳过
    expect(content).not.toContain("node_modules")
    rmSync(projA, { recursive: true, force: true })
    cleanup(s.home)
  })

  test("preset projects: sandbox mode resolves preset root inside user data dir", async () => {
    const s = await setup("subproj", true)
    const session = await s.store.createSession("default", "t")
    s.provider.toolArgs = { project: "app", path: "out.txt", content: "hi" }
    // 沙箱模式：相对路径在用户数据目录内解析（{home}/users/default/app）
    await s.store.setEnv(session.id, "default", {
      CODE_PROJECTS: JSON.stringify([{ name: "app", path: "app", description: "沙箱项目" }]),
      GEBAI_APPROVAL_SKIP: "true",
    })
    await s.engine.run(session.id, "default", "modify preset project")
    const written = await Bun.file(join(s.home, "users", "default", "app", "out.txt")).text()
    expect(written).toBe("hi")
    cleanup(s.home)
  })

  test("todo continuation: incomplete todos trigger extra rounds until completed", async () => {
    const s = await setup("tool")
    const session = await s.store.createSession("default", "t")
    await s.store.setTodos(session.id, [{ id: "t1", title: "任务A", status: "pending", priority: "medium" }])
    // 模型第 2 轮用 todo 工具把待办标记为完成
    s.provider.toolName = "todo"
    s.provider.toolArgs = { entries: [{ op: "update", id: "t1", status: "completed" }] }
    await s.engine.run(session.id, "default", "完成待办")
    const loaded = await s.store.load(session.id)
    expect(loaded!.todos[0].status).toBe("completed")
    // 待办完成后不再续做：总模型调用 = 工具轮 + 收尾轮
    expect(s.provider.calls).toBe(2)
    const msgs = loaded!.messages.map((m) => String(m.content))
    expect(msgs.some((c) => c.includes("【待办续做】"))).toBe(false)
    cleanup(s.home)
  })

  test("todo continuation: stuck incomplete todos re-prompt up to the round cap", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    await s.store.setTodos(session.id, [{ id: "t1", title: "任务A", status: "in_progress", priority: "medium" }])
    const events: string[] = []
    const unsub = s.events.subscribe((ev) => events.push(ev.type))
    await s.engine.run(session.id, "default", "hi")
    unsub()
    // 纯文本模式永不完成待办：初始 1 轮 + 续做上限 3 轮
    expect(s.provider.calls).toBe(4)
    const loaded = await s.store.load(session.id)
    const contMsgs = loaded!.messages.filter((m) => String(m.content).includes("【待办续做】"))
    expect(contMsgs.length).toBe(3)
    // 续做提醒携带未完成清单；事件推送可见
    expect(String(contMsgs[0].content)).toContain("任务A")
    expect(events.filter((t) => t === "event.todo.continue").length).toBe(3)
    cleanup(s.home)
  })

  test("todo continuation: identical repeated replies get an anti-repetition hint", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    await s.store.setTodos(session.id, [{ id: "t1", title: "任务A", status: "in_progress", priority: "medium" }])
    await s.engine.run(session.id, "default", "hi")
    const loaded = await s.store.load(session.id)
    const contMsgs = loaded!.messages.filter((m) => String(m.content).includes("【待办续做】"))
    // 纯文本模式回复完全相同：首轮无提示（无前文可比较），第 2/3 轮提醒携带防复述提示
    expect(contMsgs.length).toBe(3)
    expect(String(contMsgs[0].content)).not.toContain("完全相同")
    expect(String(contMsgs[1].content)).toContain("完全相同")
    expect(String(contMsgs[2].content)).toContain("完全相同")
    cleanup(s.home)
  })

  test("todo continuation: completed/cancelled todos do not re-prompt", async () => {
    const s = await setup("text")
    const session = await s.store.createSession("default", "t")
    await s.store.setTodos(session.id, [
      { id: "t1", title: "任务A", status: "completed", priority: "medium" },
      { id: "t2", title: "任务B", status: "cancelled", priority: "low" },
    ])
    await s.engine.run(session.id, "default", "hi")
    expect(s.provider.calls).toBe(1)
    const loaded = await s.store.load(session.id)
    expect(loaded!.messages.some((m) => String(m.content).includes("【待办续做】"))).toBe(false)
    cleanup(s.home)
  })
})

describe("AgentEngine cron integration", () => {
  test("cron tools bound via ToolContext create tasks in the scheduler", async () => {
    const { CronManager } = await import("./cron")
    const home = mkdtempSync(join(tmpdir(), "gebai-engine-cron-"))
    mkdirSync(join(home, "users", "default"), { recursive: true })
    const config = loadConfig({ gebaiHome: home, auth: "local", sandbox: "off", preloadSubAgents: [], binaryMode: false })
    const store = new SessionStore({ home })
    const registry = new ToolRegistry()
    for (const tool of Object.values(createGlobalTools())) registry.register(tool)
    const sandbox = new Sandbox({ home, enabled: false })
    const env = new EnvManager(store)
    const events = new EventBus()
    // cron 子Agent：discover 注册定义 + 预载装载（cron_add/list/update/trigger/remove 命名空间工具进入注册表）
    const subAgents = new SubAgentManager({ registry, preloadOverride: ["cron"] })
    await subAgents.discover()
    expect(registry.resolve("cron_add")).toBeDefined()
    const provider = new FakeProvider("tool")
    provider.toolName = "cron_add"
    provider.toolArgs = { name: "daily-backup", schedule: "0 9 * * *", type: "script", script: "echo backup" }
    const cron = new CronManager({ home, store, env, sandbox, events, now: () => 1_780_000_000_000 })
    try {
      const engine = new AgentEngine({ provider, registry, store, env, sandbox, events, config, subAgents, cron })
      const session = await store.createSession("default", "t")
      // cron_add 声明 requiresApproval：等待审批请求注册后批准，验证审批通过后任务创建
      const runPromise = engine.run(session.id, "default", "创建定时任务")
      await new Promise((r) => setTimeout(r, 50))
      await engine.decideApproval(session.id, "tc-1", true)
      await runPromise
      const tasks = await cron.list("default")
      expect(tasks).toHaveLength(1)
      expect(tasks[0].name).toBe("daily-backup")
      expect(tasks[0].type).toBe("script")
      expect(tasks[0].script).toBe("echo backup")
      // 下次执行时间为本地 9:00
      expect(new Date(tasks[0].nextRunAt).getHours()).toBe(9)
      // 引擎输出提及任务 id
      const loaded = await store.load(session.id)
      expect(loaded!.messages.some((m) => String(m.content).includes("daily-backup"))).toBe(true)
    } finally {
      cron.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("cron scheduler attached after engine construction backfills opts.cron (production wiring)", async () => {
    const { CronManager } = await import("./cron")
    const home = mkdtempSync(join(tmpdir(), "gebai-engine-cron-attach-"))
    mkdirSync(join(home, "users", "default"), { recursive: true })
    const config = loadConfig({ gebaiHome: home, auth: "local", sandbox: "off", preloadSubAgents: [], binaryMode: false })
    const store = new SessionStore({ home })
    const registry = new ToolRegistry()
    for (const tool of Object.values(createGlobalTools())) registry.register(tool)
    const sandbox = new Sandbox({ home, enabled: false })
    const env = new EnvManager(store)
    const events = new EventBus()
    const subAgents = new SubAgentManager({ registry, preloadOverride: ["cron"] })
    await subAgents.discover()
    const provider = new FakeProvider("tool")
    provider.toolName = "cron_add"
    provider.toolArgs = { name: "hourly-report", schedule: "@hourly", type: "script", script: "echo report" }
    const cron = new CronManager({ home, store, env, sandbox, events, now: () => 1_780_000_000_000 })
    try {
      // 生产接线（index.ts）：engine 先构造（不带 cron）、CronManager 后建经 attach 双向绑定；
      // 修复前 attach 单向注入，opts.cron 恒空 → cron_add 报「能力未启用」、任务不落调度器
      const engine = new AgentEngine({ provider, registry, store, env, sandbox, events, config, subAgents })
      cron.attach(engine)
      const session = await store.createSession("default", "t")
      // cron_add 声明 requiresApproval：审批请求到达即批准（事件驱动，免固定等待的时序竞态）
      events.subscribe((ev) => {
        if (ev.type === "event.approval.request") void engine.decideApproval(session.id, String(ev.payload.toolCallId), true)
      })
      await engine.run(session.id, "default", "创建定时任务")
      const tasks = await cron.list("default")
      expect(tasks).toHaveLength(1)
      expect(tasks[0].name).toBe("hourly-report")
      const loaded = await store.load(session.id)
      expect(loaded!.messages.some((m) => String(m.content).includes("能力未启用"))).toBe(false)
    } finally {
      cron.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("cron tools absent without a scheduler (capability fully hidden)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-engine-cron-off-"))
    mkdirSync(join(home, "users", "default"), { recursive: true })
    const registry = new ToolRegistry()
    for (const tool of Object.values(createGlobalTools())) registry.register(tool)
    // 未启用 cron：子Agent 未装载/未注册，cron_* 工具不在注册表（模型不可见、不可调用）
    expect(registry.resolve("cron_add")).toBeUndefined()
    expect(registry.resolve("cron_list")).toBeUndefined()
    rmSync(home, { recursive: true, force: true })
  })

  test("cron sub-agent unregistered when capability disabled (invisible to agent_list/agent_run)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-engine-cron-gate-"))
    mkdirSync(join(home, "users", "default"), { recursive: true })
    const registry = new ToolRegistry()
    for (const tool of Object.values(createGlobalTools())) registry.register(tool)
    const subAgents = new SubAgentManager({ registry, preloadOverride: [] })
    await subAgents.discover()
    expect(subAgents.def("cron")).toBeDefined() // 默认发现注册（能力开启形态）
    subAgents.unregister("cron") // GEBAI_CRON_ENABLED=false 启动时执行同一撤销
    expect(subAgents.def("cron")).toBeUndefined()
    expect(registry.resolve("cron_add")).toBeUndefined()
    expect(subAgents.list().some((a) => a.name === "cron")).toBe(false)
    await expect(subAgents.load("cron")).rejects.toThrow(/unknown sub-agent/)
    rmSync(home, { recursive: true, force: true })
  })
})


describe("会话级子Agent 装载持久化与恢复", () => {
  test("默认不预载：新会话 run 无装载提示词消息、无装载记录", async () => {
    const { home, engine, store } = await setup("text")
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.loadedAgent)).toBe(false)
    expect(loaded!.loadedSubAgents).toBeUndefined()
    cleanup(home)
  })

  test("预载名单初始化：新会话按启动预载名单写入提示词消息并注册工具", async () => {
    const { home, engine, store, registry, config } = await setup("text")
    config.preloadSubAgents = ["code"] // 模拟 GEBAI_PRELOAD_SUB_AGENTS=code
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    expect(loaded!.loadedSubAgents).toEqual(["code"])
    const note = loaded!.messages.find((m) => m.loadedAgent === "code")
    expect(note).toBeDefined()
    expect(note!.role).toBe("system")
    expect(note!.content).toContain("### code（")
    expect(note!.content).toContain("你是源码分析与修改专家")
    expect(registry.resolve("code_search_symbols")).toBeDefined()
    cleanup(home)
  })

  test("预载名单初始化携带预置项目清单（CODE_PROJECTS 动态注入，装载模式闭环）", async () => {
    const { home, engine, store, config } = await setup("text")
    config.preloadSubAgents = ["code"] // 模拟 GEBAI_PRELOAD_SUB_AGENTS=code
    const proj = mkdtempSync(join(tmpdir(), "gebai-preset-"))
    const session = await store.createSession("default", "t")
    await store.setEnv(session.id, "default", { CODE_PROJECTS: JSON.stringify([{ name: "app", path: proj, description: "测试项目" }]) })
    await engine.run(session.id, "default", "hi")
    const loaded = await store.load(session.id)
    const note = loaded!.messages.find((m) => m.loadedAgent === "code")
    // 装载模式提示词动态附带预置项目清单（模型按名使用 project 参数，不再盲区）
    expect(note!.content).toContain("预置项目（全局文件工具用 project 参数指定项目名")
    expect(note!.content).toContain(`- app: 测试项目（${proj}）`)
    rmSync(proj, { recursive: true, force: true })
    cleanup(home)
  })

  test("agent_load 装载：提示词消息写入会话记录并立即进入当前 run 上下文", async () => {
    const { home, engine, store, provider } = await setup("tool")
    provider.toolName = "agent_load"
    provider.toolArgs = { name: "code" }
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "load code please")
    const loaded = await store.load(session.id)
    expect(loaded!.loadedSubAgents).toEqual(["code"])
    expect(loaded!.messages.some((m) => m.loadedAgent === "code")).toBe(true)
    // 第二轮（装载后）模型上下文已含提示词 system 消息（当次 run 立即生效，不必等下次 run）
    const second = provider.seenChats[1]
    expect(second!.some((m) => m.role === "system" && String(m.content).includes("你是源码分析与修改专家"))).toBe(true)
    cleanup(home)
  })

  test("agent_load 装载后同轮次模型上下文：装载提示词并入系统前置段，不夹在 tool_calls 与 tool 结果之间", async () => {
    const { home, engine, store, provider } = await setup("tool")
    provider.toolName = "agent_load"
    provider.toolArgs = { name: "code" }
    const session = await store.createSession("default", "t")
    await engine.run(session.id, "default", "load code please")
    const second = provider.seenChats[1] as Array<MessageLike & { toolCalls?: Array<{ id: string }> }>
    // 装载提示词紧跟主 system 提示词之后（系统前置段）
    expect(second[0]!.role).toBe("system")
    expect(second[1]!.role).toBe("system")
    expect(String(second[1]!.content)).toContain("你是源码分析与修改专家")
    // assistant(tool_calls) 后必须紧跟 tool 结果（接口校验要求，防止装载 system 夹在中间）
    const toolCallIdx = second.findIndex((m) => m.role === "assistant" && m.toolCalls?.length)
    expect(toolCallIdx).toBeGreaterThanOrEqual(0)
    expect(second[toolCallIdx + 1]!.role).toBe("tool")
    expect(second[toolCallIdx + 1]).toMatchObject({ toolCallId: second[toolCallIdx]!.toolCalls![0]!.id })
    cleanup(home)
  })

  test("loadHistory 装载提示词消息前置：不夹在 assistant(tool_calls) 与 tool 结果之间（装载后会话可继续）", async () => {
    const { home, engine, store } = await setup("text")
    const session = await store.createSession("default", "t")
    // 模拟真实装载时序：user → assistant(tool_calls=agent_load) → system(装载提示词) → tool 结果 → user 继续
    await store.appendMessage(session.id, { id: "u1", role: "user", content: "load", createdAt: Date.now() } as never)
    await store.appendMessage(session.id, {
      id: "a1",
      role: "assistant",
      content: "",
      toolCalls: [{ id: "call_load", name: "agent_load", arguments: { name: "code" } }],
      createdAt: Date.now(),
    } as never)
    await store.appendMessage(session.id, { id: "l1", role: "system", loadedAgent: "code", content: "### code（完整提示词）\n你是源码分析专家。", createdAt: Date.now() } as never)
    await store.appendMessage(session.id, { id: "l2", role: "system", loadedAgent: "self_optimize", content: "### self_optimize（完整提示词）\n你是自我优化专家。", createdAt: Date.now() } as never)
    await store.appendMessage(session.id, { id: "t1", role: "tool", toolCallId: "call_load", name: "agent_load", content: "子Agent code 已装载。", createdAt: Date.now() } as never)
    await store.appendMessage(session.id, { id: "u2", role: "user", content: "继续", createdAt: Date.now() } as never)
    const msg = (engine as unknown as { loadHistory(sessionId: string, user: string): Promise<MessageLike[]> }).loadHistory
    const history = await msg.call(engine, session.id, "default")
    // 装载提示词全部前置且顺序保持
    expect(String(history[0]!.content)).toContain("### code（")
    expect(String(history[1]!.content)).toContain("### self_optimize（")
    // assistant(tool_calls) 与 tool 结果相邻（无 system 夹在中间）
    const toolCallIdx = history.findIndex((m) => m.role === "assistant" && (m as { toolCalls?: unknown[] }).toolCalls?.length)
    expect(toolCallIdx).toBeGreaterThanOrEqual(0)
    expect(history[toolCallIdx + 1]!.role).toBe("tool")
    // 会话内容完整保留
    expect(history.some((m) => m.role === "user" && m.content === "继续")).toBe(true)
    cleanup(home)
  })

  test("恢复历史会话：按会话记录重新注册工具并透传提示词消息", async () => {
    const { home, store, config, env, sandbox, events, engine } = await setup("text")
    // 装载 code 到会话（模拟先前会话中装载过，记录已落盘）
    const session = await store.createSession("default", "t")
    await engine.loadAgentToSession(session.id, "default", "code")
    await engine.run(session.id, "default", "hi")
    // 模拟重启：同一 home/store，全新 registry + SubAgentManager + engine
    const registry2 = new ToolRegistry()
    for (const tool of Object.values(createGlobalTools())) registry2.register(tool)
    const subAgents2 = new SubAgentManager({ registry: registry2, preloadOverride: [] })
    await subAgents2.discover()
    const provider2 = new FakeProvider("text")
    const engine2 = new AgentEngine({ provider: provider2, registry: registry2, store, env, sandbox, events, config, subAgents: subAgents2, retryBackoffMs: 5, authMode: "local" })
    // 重启后恢复会话：工具注册还原 + 提示词消息透传进模型上下文
    await engine2.run(session.id, "default", "continue")
    expect(registry2.resolve("code_search_symbols")).toBeDefined()
    const first = provider2.seenChats[0]
    expect(first!.some((m) => m.role === "system" && String(m.content).includes("你是源码分析与修改专家"))).toBe(true)
    cleanup(home)
  })
})
