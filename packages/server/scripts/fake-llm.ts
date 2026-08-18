/**
 * 假 OpenAI 兼容 SSE 服务——无真实模型 Key 的端到端自测工具。
 *
 * 背景：Web 前端 / 服务端联调与回归自测（流式渲染、agent_run 执行过程、计划审批等）
 * 依赖模型行为，真实接口不可控且慢。本服务按场景脚本回放 OpenAI `chat/completions`
 * 流式响应（含 tool_calls 分片、reasoning、usage），服务端 provider 无感接入。
 *
 * 用法：
 *   bun run --cwd packages/server fake-llm [场景]        # 场景缺省 = text
 *   端口默认 9801，FAKE_LLM_PORT 覆盖
 *
 * 场景：
 *   text      单轮纯文本回复（链路冒烟）
 *   agent_run 主会话调 agent_run → 子会话两轮（文本 + code_ls 工具 / 多行结论）→
 *             主会话收尾（验证新会话执行过程实时渲染与结果 markdown 换行）
 *   plan      延迟 4s 调 plan 计划审批（waitForChoice 阻塞等用户批准/拒绝）→
 *             批准后主会话收尾（验证计划卡片、选择卡片、审批后继续；延迟窗口用于
 *             先把页面滚离底部再观察审批弹出时是否落底展示计划）
 *   error     每次调用返回 HTTP 500（验证错误呈现与重试耗尽路径）
 *
 * 配套：
 *   服务端指向本服务联调：
 *     GEBAI_LLM_API_BASE=http://127.0.0.1:9801/v1 GEBAI_LLM_API_KEY=test \
 *       GEBAI_LLM_MODEL=fake GEBAI_PORT=3900 bun run --cwd packages/server dev
 *   浏览器打开 http://127.0.0.1:9801/probe 验证到服务端端口的 WS 连通性（标题变为 WS_OK）。
 */

interface Step {
  /** 正文文本（按 8 字符分片流式输出）。 */
  text?: string
  /** 调用前延迟（毫秒）。 */
  delayMs?: number
  /** 工具调用（与 text 可同轮：先文本后工具）。 */
  toolCall?: { id: string; name: string; args: Record<string, unknown> }
}

/** 场景脚本：数组按 chat 调用序号消费；耗尽后回复固定收尾文本（防场景外调用死循环）。 */
const SCENARIOS: Record<string, Step[]> = {
  text: [{ text: "你好，这是 fake-llm 的单轮文本回复。" }],
  agent_run: [
    { toolCall: { id: "c1", name: "agent_run", args: { agents: ["code"], input: "检查目录并总结" } } },
    { text: "子会话第一轮：开始检查目录。", toolCall: { id: "c2", name: "code_ls", args: { path: "." } } },
    // 多行结论：验证 agent_run 结果 markdown 渲染不出现双倍换行
    { text: "分析结论：\n第一行结论\n第二行结论\n第三行结论" },
    { text: "主会话最终回复。" },
  ],
  plan: [
    { delayMs: 4000, toolCall: { id: "c1", name: "plan", args: { title: "演示计划", steps: ["第一步：分析", "第二步：执行", "第三步：验证"] } } },
    { text: "计划已批准，开始执行。" },
  ],
  error: [],
}

const SCENARIO = process.argv[2] && SCENARIOS[process.argv[2]] ? process.argv[2] : "text"
const PORT = Number(process.env.FAKE_LLM_PORT || 9801)
let calls = 0

/** WS 连通性探针页：连到本机 gebai 服务端 /ws，结果写入标题（WS_OK / WS_ERR / WS_CLOSED / WS_TIMEOUT）。 */
const PROBE_HTML = `<!doctype html><title>probe</title><script>
const ws = new WebSocket("ws://127.0.0.1:${process.env.GEBAI_PORT_FOR_PROBE || 3900}/ws")
const setTitle = (t) => { document.title = t }
ws.onopen = () => setTitle("WS_OK")
ws.onerror = () => setTitle("WS_ERR")
ws.onclose = () => setTitle("WS_CLOSED")
setTimeout(() => { if (document.title === "probe") setTitle("WS_TIMEOUT") }, 4000)
</script><p>ws probe → gebai server</p>`

function sse(chunks: string[]): Response {
  const body = chunks.map((c) => `data: ${c}\n\n`).join("") + "data: [DONE]\n\n"
  return new Response(body, { status: 200, headers: { "Content-Type": "text/event-stream" } })
}

Bun.serve({
  port: PORT,
  async fetch(req) {
    const url = new URL(req.url)
    if (url.pathname === "/probe") {
      return new Response(PROBE_HTML, { status: 200, headers: { "Content-Type": "text/html" } })
    }
    if (url.pathname.endsWith("/chat/completions")) {
      if (SCENARIO === "error") {
        console.log(`[fake-llm] call ${calls + 1} -> HTTP 500`)
        return new Response("fake model failure", { status: 500 })
      }
      const script = SCENARIOS[SCENARIO]
      const step = calls < script.length ? script[calls] : { text: "（fake-llm 场景脚本已结束）" }
      calls++
      if (step.delayMs) await new Promise((r) => setTimeout(r, step.delayMs))
      const chunks: string[] = []
      if (step.text) {
        for (const piece of step.text.match(/[\s\S]{1,8}/g) ?? []) {
          chunks.push(JSON.stringify({ choices: [{ delta: { content: piece } }] }))
        }
      }
      if (step.toolCall) {
        chunks.push(
          JSON.stringify({
            choices: [{ delta: { tool_calls: [{ index: 0, id: step.toolCall.id, type: "function", function: { name: step.toolCall.name, arguments: "" } }] } }],
          }),
          JSON.stringify({
            choices: [{ delta: { tool_calls: [{ index: 0, function: { arguments: JSON.stringify(step.toolCall.args) } }] } }],
          }),
          JSON.stringify({ choices: [{ delta: {}, finish_reason: "tool_calls" }] }),
        )
      } else {
        chunks.push(JSON.stringify({ choices: [{ delta: {}, finish_reason: "stop" }] }))
      }
      chunks.push(JSON.stringify({ choices: [], usage: { prompt_tokens: 100, completion_tokens: 20, total_tokens: 120 } }))
      console.log(`[fake-llm] call ${calls} [${SCENARIO}] -> ${step.toolCall ? `tool:${step.toolCall.name}` : `text(${(step.text ?? "").length} chars)`}`)
      return sse(chunks)
    }
    return new Response("not found", { status: 404 })
  },
})
console.log(`[fake-llm] scenario=${SCENARIO} listening on ${PORT}（可用场景: ${Object.keys(SCENARIOS).join(", ")}）`)
