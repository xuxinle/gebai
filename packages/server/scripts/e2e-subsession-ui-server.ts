/**
 * UI 验证支撑服务：假模型（子会话运行场景）+ 歌白服务常驻——供 playwright 打开页面实测
 * 子会话折叠容器与合入通知条的真实渲染（前端 e2e 的「后端」）。
 *
 * 场景（按任务输入分流，供页面手输/脚本 fill）：
 *   S1 → 主会话调 subsession_run（隔离 + 预加载 code）→ 子会话内 code_ls → 结论 → 主会话收尾
 *   S2 → 主会话调 subsession_run（inherit_context 双任务甲/乙）→ 报告自动合入 → 主会话收尾
 *
 * 用法：bun run --cwd packages/server scripts/e2e-subsession-ui-server.ts
 * 端口：服务 GEBAI_PORT（默认 3991）、假模型 E2E_FAKE_PORT（默认 9812）；Ctrl+C 结束。
 */
import { mkdtempSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

const FAKE_PORT = Number(process.env.E2E_FAKE_PORT || 9812)
const PORT = Number(process.env.E2E_UI_PORT || 3991)
const REPO = join(import.meta.dir, "../../..")
const HOME = process.env.E2E_UI_HOME || mkdtempSync(join(tmpdir(), "gebai-e2e-ui-"))

type RawMsg = { role?: string; content?: unknown; name?: string }
type Step = { text?: string; toolCall?: { id: string; name: string; args: Record<string, unknown> } }
const toolNamesOf = (msgs: RawMsg[]): string[] => msgs.filter((m) => m.role === "tool").map((m) => String(m.name ?? ""))
/** 最后一条「非引擎通知」用户消息的位置（合入/感知通知也是 user 角色、以【开头）。 */
const lastTaskUserIdx = (msgs: RawMsg[]): number => {
  for (let i = msgs.length - 1; i >= 0; i--) {
    if (msgs[i].role !== "user") continue
    if (!String(msgs[i].content ?? "").startsWith("【")) return i
  }
  return -1
}
/** 本任务（最后一条任务输入之后）是否已出现该工具结果——跨任务历史（同会话多次演练）不污染判定。 */
const afterTaskTool = (msgs: RawMsg[], n: string): boolean => {
  const i = lastTaskUserIdx(msgs)
  return i < 0 ? false : msgs.slice(i + 1).some((m) => m.role === "tool" && String(m.name) === n)
}
const afterTaskAnyTool = (msgs: RawMsg[]): boolean => {
  const i = lastTaskUserIdx(msgs)
  return i < 0 ? false : msgs.slice(i + 1).some((m) => m.role === "tool")
}
const taskInput = (msgs: RawMsg[]): string => {
  for (let i = msgs.length - 1; i >= 0; i--) {
    if (msgs[i].role !== "user") continue
    const c = String(msgs[i].content ?? "")
    if (!c.startsWith("【")) return c
  }
  return ""
}

function decide(msgs: RawMsg[]): Step {
  const u = taskInput(msgs)
  if (u === "S1" && !afterTaskTool(msgs, "subsession_run")) return { toolCall: { id: "c1", name: "subsession_run", args: { input: "S1-child", agents: ["code"] } } }
  if (u === "S1-child") {
    if (!afterTaskAnyTool(msgs)) return { toolCall: { id: "c1", name: "ls", args: { path: "." } } }
    return { text: "S1 子会话结论：目录已检查，共 3 个文件。\n第二行结论。\n第三行结论。" }
  }
  if (u === "S1") return { text: "S1 父会话收尾回复。" }
  if (u === "S2" && !afterTaskTool(msgs, "subsession_run"))
    return { toolCall: { id: "c1", name: "subsession_run", args: { inherit_context: true, subsessions: [{ name: "甲", input: "S2-A" }, { name: "乙", input: "S2-B" }] } } }
  if (u === "S2-A") return { text: "S2-A 报告：方案A可行且成本低。" }
  if (u === "S2-B") return { text: "S2-B 报告：方案B风险较高。" }
  if (u === "S2") return { text: "S2 父会话收尾回复。" }
  return { text: `（fake 收尾：${u.slice(0, 30)}）` }
}

Bun.serve({
  port: FAKE_PORT,
  async fetch(req) {
    if (!new URL(req.url).pathname.endsWith("/chat/completions")) return new Response("not found", { status: 404 })
    const body = (await req.json()) as { messages?: RawMsg[] }
    const step = decide(body.messages ?? [])
    const chunks: string[] = []
    const text = step.text ?? ""
    // 分片 + 轻微延迟：让前端走真实流式渲染路径
    for (const piece of text.match(/[\s\S]{1,10}/g) ?? []) chunks.push(JSON.stringify({ choices: [{ delta: { content: piece } }] }))
    if (step.toolCall) {
      chunks.push(JSON.stringify({ choices: [{ delta: { tool_calls: [{ index: 0, id: step.toolCall.id, type: "function", function: { name: step.toolCall.name, arguments: "" } }] } }] }))
      chunks.push(JSON.stringify({ choices: [{ delta: { tool_calls: [{ index: 0, function: { arguments: JSON.stringify(step.toolCall.args) } }] } }] }))
      chunks.push(JSON.stringify({ choices: [{ delta: {}, finish_reason: "tool_calls" }] }))
    } else {
      chunks.push(JSON.stringify({ choices: [{ delta: {}, finish_reason: "stop" }] }))
    }
    chunks.push(JSON.stringify({ choices: [], usage: { prompt_tokens: 100, completion_tokens: 20, total_tokens: 120 } }))
    const encoder = new TextEncoder()
    const stream = new ReadableStream({
      async start(controller) {
        for (const c of chunks) {
          controller.enqueue(encoder.encode(`data: ${c}\n\n`))
          await new Promise((r) => setTimeout(r, 20))
        }
        controller.enqueue(encoder.encode("data: [DONE]\n\n"))
        controller.close()
      },
    })
    return new Response(stream, { status: 200, headers: { "Content-Type": "text/event-stream" } })
  },
})

const service = Bun.spawn(["bun", "--preload", "./scripts/build-env-embed.ts", "./src/index.ts"], {
  cwd: join(REPO, "packages/server"),
  env: {
    ...process.env,
    GEBAI_HOME: HOME,
    GEBAI_PORT: String(PORT),
    GEBAI_AUTH: "local",
    GEBAI_SANDBOX: "off",
    GEBAI_CRON_ENABLED: "false",
    GEBAI_LLM_API_BASE: `http://127.0.0.1:${FAKE_PORT}/v1`,
    GEBAI_LLM_API_KEY: "test",
    GEBAI_LLM_MODEL: "fake-ui",
    GEBAI_LOG_LEVEL: "warn",
  },
  stdout: "inherit",
  stderr: "inherit",
})

console.log(`[ui-e2e] home=${HOME}`)
console.log(`[ui-e2e] 页面地址 http://127.0.0.1:${PORT}/ （页面输入 S1 / S2 触发场景）`)

const stop = (): void => {
  service.kill()
  process.exit(0)
}
process.on("SIGINT", stop)
process.on("SIGTERM", stop)
await service.exited.catch(() => {})
