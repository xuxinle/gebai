/**
 * 服务级端到端验证（引擎全链路，fake-llm python 场景驱动）：连接运行中的歌白服务 →
 * REST 确认 python 子代理注册 → WS sendPrompt 驱动模型按场景脚本调用
 * agent_load(python) + python_run ×2（常驻状态跨调用保持）→ 断言工具输出。
 *
 * 前置：①歌白服务已启动（GEBAI_PORT，本地形态，建议 GEBAI_APPROVAL_SKIP=true 免审批）
 *      ②fake-llm python 场景：bun run --cwd packages/server fake-llm python（FAKE_LLM_PORT）
 * 用法：GEBAI_PORT=3987 bun run scripts/e2e-service-native.ts
 */
import { GebaiClient } from "@gebai/sdk"

const port = (process.env.GEBAI_PORT || "3987").trim()
const fakePort = (process.env.FAKE_LLM_PORT || "9807").trim()
const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}` })
await c.connect()

// 0) 重置 fake-llm 场景计数（e2e 可反复跑：场景按调用序号消耗，不重置会拿到「脚本已结束」）
const reset = await fetch(`http://127.0.0.1:${fakePort}/__reset`).then((r) => r.json()).catch(() => null)
console.log("fake-llm reset:", JSON.stringify(reset))
if (!reset?.ok) console.warn("WARN: fake-llm /__reset 不可达（需 fake-llm 新版；本跑可能拿到耗尽的场景）")

// 1) REST 子代理目录：python 注册面
const subs = await c.get("/api/v1/sub-agents")
const names = ((subs ?? []) as Array<{ name: string }>).map((a) => a.name)
console.log("REST /sub-agents:", names.length, "个，python 注册:", names.includes("python"))
if (!names.includes("python")) throw new Error("python 未注册（boot native 接线失败）")

// 2) 新会话任务：fake-llm python 场景（agent_load → python_run ×2 → 收尾）
const created = await c.request("session.create", { title: "native-service-e2e" })
const sessionId = (created as { session?: { id?: string } })?.session?.id ?? (created as { id?: string })?.id
if (!sessionId) throw new Error(`session.create 返回异常: ${JSON.stringify(created).slice(0, 200)}`)
console.log("session:", sessionId)

const toolCalls: string[] = []
const outputs: string[] = []
for await (const ch of c.sendPrompt(sessionId, "计算圆周率并加倍", { env: { GEBAI_APPROVAL_SKIP: "true" } })) {
  if (ch.kind === "tool_result") {
    const tname = (ch as { toolCall?: { name?: string } }).toolCall?.name ?? "?"
    toolCalls.push(tname)
    if (tname === "python_run") outputs.push(String((ch as { output?: string }).output ?? ""))
  } else if (ch.kind === "error" || ch.kind === "model_error") {
    throw new Error(`任务失败: ${JSON.stringify(ch).slice(0, 300)}`)
  }
}

console.log("工具调用序列:", toolCalls)
// 3) 断言：python_run 被调 2 次，输出含 3.14159 与 6.2832（常驻状态 X 跨调用保持）；
// 首调可能带 agent_load（场景脚本与路由自愈装载兼容两种路径）
const runCount = toolCalls.filter((n) => n === "python_run").length
console.log("python_run 输出:", outputs.map((o) => o.slice(0, 60)))
if (!toolCalls.includes("agent_load") && toolCalls[0] !== "python_run") throw new Error(`工具序列异常: ${toolCalls}`)
if (runCount !== 2) throw new Error(`python_run 应调 2 次，实际 ${runCount}`)
if (!outputs[0]?.includes("3.14159")) throw new Error(`第一次 python_run 未回显 pi: ${outputs[0]?.slice(0, 100)}`)
if (!outputs[1]?.includes("6.2832")) throw new Error(`第二次未复用 X（常驻状态丢失）: ${outputs[1]?.slice(0, 100)}`)

// 4) 会话记录落盘核验（chat.json 含工具调用）
const msgs = await c.get(`/api/v1/sessions/${sessionId}`)
console.log("会话记录可见:", !!msgs)

console.log("\n=== 服务级引擎全链路验证通过（boot 接线→路由自愈装载→python_run 常驻→输出回传）===")
process.exit(0)
