/**
 * 服务级端到端验证：子会话运行（subsession_run）全链路——**真实歌白服务进程**（REST/WS/会话落盘/
 * 子Agent 发现/工具注册表全接线）+ 脚本化假模型（按「最后一条非通知用户消息 + 已有工具结果」分流，
 * 与 engine 单测同构的分流逻辑，但走真实 HTTP/WS 链路）。
 *
 * 覆盖：①同步隔离子会话（事件标记/过程存档/隔离）②同步继承子会话（fork 上下文 + 报告自动合入）
 * ③异步运行（bg_task s 前缀 + subsession_merge 阶段性合入）④待办运行内隔离 ⑤门禁与参数校验回传
 * ⑥空 agents 通用子会话 ⑦递归深度上限（进程树）。
 *
 * 用法：bun run --cwd packages/server scripts/e2e-subsession.ts
 * 自带假模型与临时 GEBAI_HOME（不污染真实数据），结束自动清理；服务日志在系统临时目录 gebai-e2e-service.log。
 */
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { GebaiClient } from "../../sdk/src/client"

const FAKE_PORT = Number(process.env.E2E_FAKE_PORT || 9811)
const PORT = Number(process.env.E2E_PORT || 3987)
const REPO = join(import.meta.dir, "../../..")
const HOME = mkdtempSync(join(tmpdir(), "gebai-e2e-sub-"))

let passed = 0
const failures: string[] = []
function check(name: string, cond: boolean, detail = ""): void {
  if (cond) {
    passed++
    console.log(`  ✓ ${name}`)
  } else {
    failures.push(name)
    console.log(`  ✗ ${name}${detail ? ` —— ${detail}` : ""}`)
  }
}
const section = (title: string): void => console.log(`\n=== ${title} ===`)

/* ---------- 脚本化假模型 ---------- */

interface Seen {
  sys: string
  lastUser: string
  toolNames: string[]
  hasToolResult: boolean
  assistantToolNames: string[]
  joined: string
}
const seen: Seen[] = []

type ToolCall = { id: string; name: string; args: Record<string, unknown> }
type Step = { text?: string; toolCall?: ToolCall }
type RawMsg = { role?: string; content?: unknown; name?: string }

const lastToolText = (msgs: RawMsg[]): string => {
  for (let i = msgs.length - 1; i >= 0; i--) if (msgs[i].role === "tool") return String(msgs[i].content ?? "")
  return ""
}
const lastUser = (msgs: RawMsg[]): string => {
  for (let i = msgs.length - 1; i >= 0; i--) if (msgs[i].role === "user") return String(msgs[i].content ?? "")
  return ""
}
/** 最后一条「非引擎通知」用户消息（合入/感知通知也是 user 角色、以【开头，不能当任务输入）。 */
const taskInput = (msgs: RawMsg[]): string => {
  for (let i = msgs.length - 1; i >= 0; i--) {
    if (msgs[i].role !== "user") continue
    const c = String(msgs[i].content ?? "")
    if (!c.startsWith("【")) return c
  }
  return ""
}
const hasToolMsg = (msgs: RawMsg[]): boolean => msgs.some((m) => m.role === "tool")
const toolNamesOf = (msgs: RawMsg[]): string[] => msgs.filter((m) => m.role === "tool").map((m) => String(m.name ?? ""))
const anyToolNamed = (msgs: RawMsg[], name: string): boolean => toolNamesOf(msgs).includes(name)

/** 场景分流：与 engine 单测同构（按任务输入 + 已有工具结果判定所处阶段）。 */
function decide(msgs: RawMsg[]): Step {
  const u = taskInput(msgs)
  const r8 = lastToolText(msgs).match(/s[0-9a-f]{8}/)?.[0] ?? "s-missing"

  /* S1 同步隔离子会话 */
  if (u === "S1" && !anyToolNamed(msgs, "subsession_run")) return { toolCall: { id: "c1", name: "subsession_run", args: { input: "S1-child", agents: ["code"] } } }
  if (u === "S1-child") {
    if (!hasToolMsg(msgs)) return { toolCall: { id: "c1", name: "ls", args: { path: "." } } }
    return { text: "S1 子会话结论：目录已检查，共 3 个文件。" }
  }
  if (u === "S1") return { text: "S1 父会话收尾。" }

  /* S2 同步继承子会话（fork 并行 + 自动合入） */
  if (u === "S2" && !anyToolNamed(msgs, "subsession_run"))
    return { toolCall: { id: "c1", name: "subsession_run", args: { subsessions: [{ name: "左路", input: "S2-A" }, { name: "右路", input: "S2-B" }], inherit_context: true } } }
  if (u === "S2-A") return { text: "S2-A 报告：方案A可行且成本低。" }
  if (u === "S2-B") return { text: "S2-B 报告：方案B风险较高。" }
  if (u === "S2") return { text: "S2 父会话收尾。" }

  /* S3 异步 + subsession_merge + bg_task */
  if (u === "S3" && !anyToolNamed(msgs, "subsession_run")) return { toolCall: { id: "c1", name: "subsession_run", args: { input: "S3-child", inherit_context: true, async: true } } }
  if (u === "S3-child") {
    if (anyToolNamed(msgs, "subsession_merge")) return { text: "S3 子会话最终报告：长任务已完成。" }
    return { toolCall: { id: "c1", name: "subsession_merge", args: { content: "S3 阶段性成果：已定位问题点。" } } }
  }
  if (u === "S3" && anyToolNamed(msgs, "bg_task")) return { text: "S3 父会话收尾。" }
  if (u === "S3" && anyToolNamed(msgs, "subsession_run")) return { toolCall: { id: "c2", name: "bg_task", args: { action: "wait", id: r8, timeout: 20 } } }

  /* S4 待办运行内隔离 */
  if (u === "S4" && !anyToolNamed(msgs, "todo")) return { toolCall: { id: "c1", name: "todo", args: { entries: [{ op: "add", title: "父任务A" }] } } }
  if (u === "S4" && !anyToolNamed(msgs, "subsession_run")) return { toolCall: { id: "c2", name: "subsession_run", args: { input: "S4-child" } } }
  if (u === "S4-child") {
    if (!anyToolNamed(msgs, "todo")) return { toolCall: { id: "c1", name: "todo", args: { entries: [{ op: "add", title: "子任务B" }] } } }
    return { text: "S4 子会话完成。" }
  }
  // 父会话收尾前先查一次自己的待办（验证子会话清单未回流）
  if (u === "S4" && toolNamesOf(msgs).filter((x) => x === "todo").length < 2) return { toolCall: { id: "c3", name: "todo", args: { entries: [] } } }
  if (u === "S4") return { text: "S4 父会话收尾。" }

  /* S5 门禁与参数校验 */
  if (u === "S5") {
    const n = toolNamesOf(msgs).filter((x) => x === "subsession_run").length
    if (n === 0) return { toolCall: { id: "c1", name: "subsession_run", args: { subsessions: Array.from({ length: 9 }, (_, i) => ({ input: `x${i}` })) } } }
    if (n === 1) return { toolCall: { id: "c2", name: "subsession_run", args: { input: "与多任务形态互斥", subsessions: [{ input: "y" }] } } }
    if (n === 2) return { toolCall: { id: "c3", name: "subsession_run", args: { input: "未知子Agent", agents: ["nonexistent_agent_zz"] } } }
    return { text: "S5 父会话收尾。" }
  }

  /* S6 空 agents 通用子会话 */
  if (u === "S6" && !anyToolNamed(msgs, "subsession_run")) return { toolCall: { id: "c1", name: "subsession_run", args: { input: "S6-child" } } }
  if (u === "S6-child") return { text: "S6 通用子会话完成。" }
  if (u === "S6") return { text: "S6 父会话收尾。" }

  /* S7 递归深度（进程树） */
  if (u === "S7" && !anyToolNamed(msgs, "subsession_run")) return { toolCall: { id: "c1", name: "subsession_run", args: { input: "S7-1" } } }
  if (u.startsWith("S7-")) {
    const n = Number(u.slice(3))
    if (!anyToolNamed(msgs, "subsession_run")) return { toolCall: { id: `c${n}`, name: "subsession_run", args: { input: `S7-${n + 1}` } } }
    return { text: `S7-${n} 子会话完成（第 ${n} 层）。` }
  }
  if (u === "S7") return { text: "S7 父会话收尾。" }

  return { text: `（未匹配场景的收尾回复：${u.slice(0, 40)}）` }
}

const fakeServer = Bun.serve({
  port: FAKE_PORT,
  async fetch(req) {
    if (!new URL(req.url).pathname.endsWith("/chat/completions")) return new Response("not found", { status: 404 })
    const body = (await req.json()) as { messages?: RawMsg[]; tools?: Array<{ name?: string; function?: { name?: string } }> }
    const msgs = body.messages ?? []
    const toolNames = (body.tools ?? []).map((t) => t.function?.name ?? t.name ?? "")
    const step = decide(msgs)
    seen.push({
      sys: String(msgs[0]?.content ?? ""),
      lastUser: lastUser(msgs),
      toolNames,
      hasToolResult: hasToolMsg(msgs),
      assistantToolNames: toolNamesOf(msgs),
      joined: JSON.stringify(msgs),
    })
    const chunks: string[] = []
    if (step.text) for (const piece of step.text.match(/[\s\S]{1,12}/g) ?? []) chunks.push(JSON.stringify({ choices: [{ delta: { content: piece } }] }))
    if (step.toolCall) {
      chunks.push(JSON.stringify({ choices: [{ delta: { tool_calls: [{ index: 0, id: step.toolCall.id, type: "function", function: { name: step.toolCall.name, arguments: "" } }] } }] }))
      chunks.push(JSON.stringify({ choices: [{ delta: { tool_calls: [{ index: 0, function: { arguments: JSON.stringify(step.toolCall.args) } }] } }] }))
      chunks.push(JSON.stringify({ choices: [{ delta: {}, finish_reason: "tool_calls" }] }))
    } else {
      chunks.push(JSON.stringify({ choices: [{ delta: {}, finish_reason: "stop" }] }))
    }
    chunks.push(JSON.stringify({ choices: [], usage: { prompt_tokens: 120, completion_tokens: 30, total_tokens: 150 } }))
    return new Response(chunks.map((c) => `data: ${c}\n\n`).join("") + "data: [DONE]\n\n", { status: 200, headers: { "Content-Type": "text/event-stream" } })
  },
})
console.log(`[e2e] fake-llm on ${FAKE_PORT}, gebai home=${HOME}`)

/* ---------- 启动真实歌白服务（独立 HOME/端口，指向本脚本的假模型） ---------- */

const logFile = join(tmpdir(), "gebai-e2e-service.log")
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
    GEBAI_LLM_MODEL: "fake-e2e",
    GEBAI_LLM_MAX_CONTEXT: "512000",
    GEBAI_LOG_LEVEL: "warn",
  },
  stdout: Bun.file(logFile),
  stderr: Bun.file(logFile),
})

const base = `http://127.0.0.1:${PORT}`
async function waitReady(timeoutMs = 60000): Promise<void> {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    try {
      const res = await fetch(`${base}/api/v1/sub-agents`)
      if (res.ok) return
    } catch {
      /* 未就绪：继续轮询 */
    }
    if (Date.now() > deadline) throw new Error(`服务未在 ${timeoutMs}ms 内就绪（日志 ${logFile}）`)
    await new Promise((r) => setTimeout(r, 300))
  }
}

const cleanup = (): void => {
  try {
    service.kill()
  } catch {
    /* 已退出 */
  }
  fakeServer.stop(true)
  rmSync(HOME, { recursive: true, force: true })
}

interface StoredMsg {
  role?: string
  content?: unknown
  name?: string
  engineNote?: string
  subSession?: boolean
  subSessionArchive?: { runId: string; agents: string[]; input: string; output?: string; subsession?: { name: string; model?: string }; messages: Array<{ role?: string; name?: string; content?: string }> }
  subSessionMerged?: { runId: string; name: string; model?: string }
}

try {
  section("启动服务")
  await waitReady()
  const client = new GebaiClient({ baseUrl: base })
  await client.connect()
  const users = await Array.fromAsync(new Bun.Glob("users/*").scan({ cwd: HOME, onlyFiles: false }))
  const user = (users[0] ?? "users/admin").replace(/\\/g, "/").split("/")[1] ?? "admin"
  console.log(`  服务就绪 ${base}（用户 ${user}）`)

  const events: Array<{ type: string; payload: Record<string, unknown> }> = []
  client.onEvent((ev) => events.push({ type: ev.type, payload: ev.payload as Record<string, unknown> }))

  const sessionFile = (sid: string): string => join(HOME, "users", user, "sessions", sid.slice(0, 2), sid.slice(2, 4), sid, "chat.json")
  const loadDoc = async (sid: string): Promise<{ messages?: StoredMsg[]; todos?: Array<{ title?: string }> }> =>
    (await Bun.file(sessionFile(sid)).json().catch(() => ({}))) as { messages?: StoredMsg[]; todos?: Array<{ title?: string }> }
  const loadMsgs = async (sid: string): Promise<StoredMsg[]> => (await loadDoc(sid)).messages ?? []
  const loadTodos = async (sid: string): Promise<Array<{ title?: string }>> => (await loadDoc(sid)).todos ?? []

  const runScenario = async (prompt: string): Promise<{ sid: string; chunks: Array<Record<string, unknown>> }> => {
    const created = (await client.request("session.create", { title: `e2e-${prompt}` })) as { session?: { id?: string }; id?: string }
    const sid = created?.session?.id ?? created?.id
    if (!sid) throw new Error(`session.create 返回异常: ${JSON.stringify(created).slice(0, 200)}`)
    const chunks: Array<Record<string, unknown>> = []
    for await (const ch of client.sendPrompt(sid, prompt, { env: { GEBAI_APPROVAL_SKIP: "true" } })) chunks.push(ch as unknown as Record<string, unknown>)
    return { sid, chunks }
  }
  /** 工具结果文本：scope=parent 只取父会话层（排除子会话过程事件），sub 只取子会话层。 */
  const toolResults = (chunks: Array<Record<string, unknown>>, name: string, scope: "parent" | "sub" = "parent"): string[] =>
    chunks
      .filter((c) => c.kind === "tool_result" && (c.toolCall as { name?: string } | undefined)?.name === name && (scope === "sub") === (c.subSession === true))
      .map((c) => String(c.output ?? ""))
  const errors = (chunks: Array<Record<string, unknown>>): string[] => chunks.filter((c) => c.kind === "error" || c.kind === "model_error").map((c) => String(c.error ?? ""))
  const isSubChat = (s: Seen): boolean => s.sys.includes("【并行子会话】") || s.sys.includes("你正在一个子会话中执行任务")
  const chatOf = (pred: (s: Seen) => boolean): Seen | undefined => [...seen].reverse().find(pred)

  /* ---------- S1 同步隔离子会话 ---------- */
  section("S1 同步隔离子会话（spawn：结果经工具结果交付 + 过程存档）")
  const s1 = await runScenario("S1")
  check("无模型错误", errors(s1.chunks).length === 0, errors(s1.chunks).join(";"))
  const s1Start = s1.chunks.find((c) => c.kind === "subsession_start")
  const s1Meta = s1Start?.subSessionMeta as { input?: string; agents?: string[] } | undefined
  check("event.subsession.start 携带子会话元信息", !!s1Start && s1Meta?.input === "S1-child" && (s1Meta?.agents ?? []).join() === "code", JSON.stringify(s1Meta))
  const s1RunId = String(s1Start?.subSessionId ?? "")
  check("runId 为 s 前缀（bg_task 分发用）", /^s[0-9a-f]{8}$/.test(s1RunId), s1RunId)
  const subChunks = s1.chunks.filter((c) => c.subSession === true)
  check("子会话过程事件带 subSession + subSessionId", subChunks.length > 0 && subChunks.every((c) => c.subSessionId === s1RunId), `${subChunks.length} 条`)
  check("子会话内部工具调用（ls）实时可见", subChunks.some((c) => c.kind === "tool_call" && (c.toolCall as { name?: string })?.name === "ls"))
  const s1Done = s1.chunks.find((c) => c.kind === "subsession_done")?.subSessionMeta as { output?: string } | undefined
  check("subsession_done 带最终输出", String(s1Done?.output ?? "").includes("S1 子会话结论"), String(s1Done?.output ?? "").slice(0, 60))
  const s1Tool = toolResults(s1.chunks, "subsession_run").join("\n")
  check("隔离形态：最终结果作为工具结果返回", s1Tool.includes("S1 子会话结论"), s1Tool.slice(0, 120))
  const s1msgs = await loadMsgs(s1.sid)
  const s1Archive = s1msgs.find((m) => m.name === "subsession_run")?.subSessionArchive
  check("落盘：subsession_run 工具消息携带过程存档", !!s1Archive && String(s1Archive.output).includes("S1 子会话结论"), JSON.stringify(s1Archive?.agents))
  check("落盘：存档含子会话全过程（ls 调用）", !!s1Archive?.messages.some((m) => m.name === "ls"), `${s1Archive?.messages.length ?? 0} 条`)
  check("落盘：父会话不逐条落盘子会话过程", !s1msgs.some((m) => m.subSession === true))
  check("落盘：无自动合入（隔离形态）", !s1msgs.some((m) => m.subSessionMerged))
  const s1Child = chatOf((s) => s.sys.includes("你正在一个子会话中执行任务"))
  check("子会话系统提示词为隔离开场白（含预加载名单）", !!s1Child?.sys.includes("已预加载子Agent: code"))
  check("隔离形态：子会话上下文不含父会话输入", !!s1Child && !s1Child.joined.includes('"content":"S1"'), s1Child?.joined.slice(0, 80))
  check("隔离形态：子会话工具面含全局工具与编排工具", !!s1Child?.toolNames.includes("read") && !!s1Child?.toolNames.includes("ls") && !!s1Child?.toolNames.includes("subsession_run"), (s1Child?.toolNames ?? []).join(",").slice(0, 120))
  check("同步运行不注入 subsession_merge", !s1Child?.toolNames.includes("subsession_merge"))

  /* ---------- S2 同步继承子会话（fork + 自动合入） ---------- */
  section("S2 同步继承子会话（fork：并行 fan-out + 报告自动合入父会话）")
  const s2 = await runScenario("S2")
  check("无模型错误", errors(s2.chunks).length === 0, errors(s2.chunks).join(";"))
  const starts = s2.chunks.filter((c) => c.kind === "subsession_start")
  check("两个子会话各自 start（带名字）", starts.length >= 2 && starts.every((c) => String((c.subSessionMeta as { subsession?: string })?.subsession ?? "")), starts.map((c) => String((c.subSessionMeta as { subsession?: string })?.subsession)).join(","))
  const s2msgs = await loadMsgs(s2.sid)
  const merges = s2msgs.filter((m) => m.subSessionMerged)
  check("落盘：两条合并消息（左路/右路）", merges.length === 2 && merges.map((m) => m.subSessionMerged?.name).join() === "左路,右路", merges.map((m) => m.subSessionMerged?.name).join())
  check("合并消息为 user + engineNote=subsession", merges.every((m) => m.role === "user" && m.engineNote === "subsession"))
  check("合并消息携带过程存档（含 subsession 标识）", merges.every((m) => !!m.subSessionArchive?.subsession?.name))
  check("合并消息内容含报告与自描述头", merges.every((m) => String(m.content).includes("【子会话") && String(m.content).includes("报告")))
  check("event.subsession.merged 推送 ≥2 条", events.filter((e) => e.type === "event.subsession.merged").length >= 2)
  const s2Chats = seen.filter((s) => s.sys.includes("【并行子会话】"))
  check("fork：子会话系统提示词含父会话提示词 + 并行附注", s2Chats.length >= 2 && s2Chats.every((s) => s.sys.includes("歌白智能体")))
  check("fork：子会话上下文含父会话历史与任务指令", s2Chats.every((s) => s.joined.includes('"content":"S2"')) && s2Chats.some((s) => s.lastUser === "S2-A"))
  const s2Final = [...seen].reverse().find((s) => !isSubChat(s) && s.joined.includes("S2-A 报告") && s.joined.includes("S2-B 报告"))
  check("父会话下一轮上下文可见两份报告（合入生效）", !!s2Final, (s2Final?.lastUser ?? "未找到含报告的父会话请求").slice(0, 60))
  check("合入以 user 角色进父上下文（思考类模型兼容）", !!s2Final && s2Final.joined.includes("已合并") && /"role":"user","content":"【子会话/.test(s2Final.joined.replace(/\s+/g, "")))
  check("fork 工具面与父会话同构（全局 + 编排）", s2Chats.every((s) => s.toolNames.includes("read")))

  /* ---------- S3 异步运行（bg_task + subsession_merge） ---------- */
  section("S3 异步子会话（bg_task s 前缀 + subsession_merge 阶段性合入）")
  const s3 = await runScenario("S3")
  check("无模型错误", errors(s3.chunks).length === 0, errors(s3.chunks).join(";"))
  const s3Start = toolResults(s3.chunks, "subsession_run").join("\n")
  check("异步启动：立即返回 runId 与后台说明", s3Start.includes("子会话已后台启动") && /runId: s[0-9a-f]{8}/.test(s3Start), s3Start.slice(0, 120))
  const s3msgs = await loadMsgs(s3.sid)
  const interim = s3msgs.find((m) => String(m.content).includes("阶段性合入"))
  check("阶段性成果合入父会话（subsession_merge 交出）", !!interim && String(interim.content).includes("S3 阶段性成果"))
  check("阶段性合入不带过程存档（子会话仍在执行）", !!interim && interim.subSessionArchive === undefined)
  const finalMerge = s3msgs.find((m) => m.subSessionMerged && String(m.content).includes("S3 子会话最终报告"))
  check("最终报告自动合入（含过程存档）", !!finalMerge && !!finalMerge.subSessionArchive?.output)
  const s3Wait = toolResults(s3.chunks, "bg_task").join("\n")
  check("bg_task wait 取回最终结果与进度", s3Wait.includes("S3 子会话最终报告") && /s[0-9a-f]{8}/.test(s3Wait), s3Wait.slice(0, 120))
  check("落盘：bg_task 结果携带完整存档（历史回放）", s3msgs.filter((m) => m.name === "bg_task").some((m) => !!m.subSessionArchive?.output))
  const s3Child = chatOf((s) => s.sys.includes("【并行子会话】") && s.toolNames.includes("subsession_merge"))
  check("异步运行注入 subsession_merge（同步不注入）", !!s3Child, (s3Child?.toolNames ?? []).join(",").slice(0, 140))
  const mergeResult = s3.chunks.find((c) => c.subSession === true && c.kind === "tool_result" && (c.toolCall as { name?: string })?.name === "subsession_merge")
  check("subsession_merge 结果回执可见（父会话新进展）", !!mergeResult && String(mergeResult.output ?? "").includes("父会话"), String(mergeResult?.output ?? "").slice(0, 100))
  check("完整形态：阶段性 + 最终合入均广播事件", events.filter((e) => e.type === "event.subsession.merged" && String(e.payload.name ?? "") === "s1").length >= 2)

  /* ---------- S4 待办运行内隔离 ---------- */
  section("S4 待办运行内隔离（不落盘/不回流/不继承）")
  const s4 = await runScenario("S4")
  check("无模型错误", errors(s4.chunks).length === 0, errors(s4.chunks).join(";"))
  const s4Child = chatOf((s) => s.sys.includes("你正在一个子会话中执行任务") && s.joined.includes("S4-child"))
  check("子会话上下文不含父会话待办", !!s4Child && !s4Child.joined.includes("父任务A"))
  const s4ChildTodo = toolResults(s4.chunks, "todo", "sub")
  check("子会话 todo 只见自己的清单", s4ChildTodo.some((t) => t.includes("子任务B")) && s4ChildTodo.every((t) => !t.includes("父任务A")), s4ChildTodo.map((t) => t.slice(0, 40)).join(" | "))
  const s4ParentTodo = toolResults(s4.chunks, "todo").join("\n")
  check("父会话待办不被子会话污染（不回流）", s4ParentTodo.includes("父任务A") && !s4ParentTodo.includes("子任务B"), s4ParentTodo.slice(0, 140))
  const todoEvents = events.filter((e) => e.type === "event.todo.update" && e.payload.subSession === true)
  check("子会话待办事件带 subSession 标记（前端不覆盖父面板）", todoEvents.length > 0 && !!todoEvents[0].payload.subSessionId, `${todoEvents.length} 条`)
  const todoDisk = await loadTodos(s4.sid)
  check("落盘：父待办清单只含父任务", JSON.stringify(todoDisk).includes("父任务A") && !JSON.stringify(todoDisk).includes("子任务B"), JSON.stringify(todoDisk).slice(0, 140))

  /* ---------- S5 门禁与参数校验 ---------- */
  section("S5 门禁与参数校验（超限/形态互斥/未知名回到模型可修正）")
  const s5 = await runScenario("S5")
  check("无模型错误（校验失败不中断任务）", errors(s5.chunks).length === 0, errors(s5.chunks).join(";"))
  const s5Results = toolResults(s5.chunks, "subsession_run")
  check("子会话数超限被拒（含引导）", s5Results.some((t) => t.includes("数量超限")), s5Results[0]?.slice(0, 100))
  check("两形态互斥校验回传", s5Results.some((t) => t.includes("二选一")), s5Results[1]?.slice(0, 100))
  check("未知子Agent 附因回传", s5Results.some((t) => t.includes("未知子Agent") || t.includes("nonexistent_agent_zz")), s5Results[2]?.slice(0, 120))

  /* ---------- S6 空 agents 通用子会话 ---------- */
  section("S6 空 agents 通用子会话（不加载任何子Agent）")
  const s6 = await runScenario("S6")
  const s6Child = chatOf((s) => s.sys.includes("你正在一个子会话中执行任务") && s.joined.includes("S6-child"))
  check("开场白标注未预加载子Agent", !!s6Child?.sys.includes("未预加载子Agent"))
  check("工具面只有全局工具与编排（无子Agent 独有工具）", !!s6Child && s6Child.toolNames.includes("read") && !s6Child.toolNames.some((t) => t.startsWith("code_")), (s6Child?.toolNames ?? []).join(",").slice(0, 140))
  check("通用子会话可正常完成并把结果交回", toolResults(s6.chunks, "subsession_run").join().includes("S6 通用子会话完成"))

  /* ---------- S7 递归深度（进程树） ---------- */
  section("S7 递归深度上限（子会话内再派生，depth ≤ 3）")
  const s7 = await runScenario("S7")
  check("无模型错误", errors(s7.chunks).length === 0, errors(s7.chunks).join(";"))
  check("三层子会话依次启动（进程树）", s7.chunks.filter((c) => c.kind === "subsession_start").length >= 3, `${s7.chunks.filter((c) => c.kind === "subsession_start").length} 个 start`)
  const nested = toolResults(s7.chunks, "subsession_run", "sub")
  check("第四层派生被深度上限拒绝", nested.some((t) => t.includes("深度超限")), nested.map((t) => t.slice(0, 90)).join(" | "))
  const s7msgs = await loadMsgs(s7.sid)
  check("嵌套子会话存档递归挂载（历史回放可展开）", JSON.stringify(s7msgs.find((m) => m.name === "subsession_run")?.subSessionArchive ?? {}).includes("subSessionArchive"))

  /* ---------- 汇总 ---------- */
  section("汇总")
  console.log(`  通过 ${passed} 项断言${failures.length ? `，失败 ${failures.length} 项` : ""}`)
  if (failures.length) {
    console.log("  失败清单:")
    for (const f of failures) console.log(`   - ${f}`)
  }
  console.log(`  模型调用累计 ${seen.length} 次；服务日志 ${logFile}`)
  cleanup()
  process.exit(failures.length ? 1 : 0)
} catch (err) {
  console.error("\n[e2e] 失败:", err)
  console.error(`服务日志见 ${logFile}`)
  cleanup()
  process.exit(1)
}
