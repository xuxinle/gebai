/**
 * 真实模型端到端验证：子会话运行（subsession_run）在**真实 LLM** 下的行为——模型是否按新语义正确调用
 * （隔离/继承形态、agents 预加载、多任务并发、报告合入与汇总），并顺带探测前端静态资源可用性（UI 验证前置）。
 *
 * 用法：bun run --cwd packages/server e2e:subsession:real
 *   对已启动的实例实测（不自起服务）：E2E_REAL_EXTERNAL=1 E2E_REAL_PORT=3999 bun run e2e:subsession:real
 * 用独立 GEBAI_HOME（临时目录，不污染真实数据）与独立端口；模型配置沿用环境/.env（GEBAI_LLM_*）。
 */
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { GebaiClient } from "../../sdk/src/client"


const REPO = join(import.meta.dir, "../../..")
const HOME = mkdtempSync(join(tmpdir(), "gebai-e2e-real-"))
const logFile = join(tmpdir(), "gebai-e2e-real-service.log")

let passed = 0
const failures: string[] = []
const check = (name: string, cond: boolean, detail = ""): void => {
  if (cond) {
    passed++
    console.log(`  ✓ ${name}`)
  } else {
    failures.push(name)
    console.log(`  ✗ ${name}${detail ? ` —— ${detail}` : ""}`)
  }
}
const section = (t: string): void => console.log(`\n=== ${t} ===`)

const port = Number(process.env.E2E_REAL_PORT || 3989)
/** 外部模式（E2E_REAL_EXTERNAL=1）：不自起服务，直接实测已启动的实例（如新端口验证实例）。 */
const EXTERNAL = process.env.E2E_REAL_EXTERNAL === "1"

const service = EXTERNAL
  ? null
  : Bun.spawn(["bun", "--preload", "./scripts/build-env-embed.ts", "./src/index.ts"], {
      cwd: join(REPO, "packages/server"),
      env: {
        ...process.env,
        GEBAI_HOME: HOME,
        GEBAI_PORT: String(port),
        GEBAI_AUTH: "local",
        GEBAI_SANDBOX: "off",
        GEBAI_CRON_ENABLED: "false",
        GEBAI_FEISHU_BOT_ENABLED: "false",
        GEBAI_LOG_LEVEL: "warn",
      },
      stdout: Bun.file(logFile),
      stderr: Bun.file(logFile),
    })

const base = `http://127.0.0.1:${port}`
async function waitReady(timeoutMs = 60000): Promise<void> {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    try {
      if ((await fetch(`${base}/api/v1/sub-agents`)).ok) return
    } catch {
      /* 继续等 */
    }
    if (Date.now() > deadline) throw new Error(`服务未就绪（日志 ${logFile}）`)
    await new Promise((r) => setTimeout(r, 300))
  }
}

const cleanup = (): void => {
  try {
    service?.kill()
  } catch {
    /* 已退出 */
  }
  if (!EXTERNAL) rmSync(HOME, { recursive: true, force: true })
}

interface StoredMsg {
  role?: string
  content?: unknown
  name?: string
  engineNote?: string
  arguments?: Record<string, unknown>
  subSessionArchive?: { runId: string; agents: string[]; input: string; output?: string; messages: Array<{ name?: string }> }
  subSessionMerged?: { runId: string; name: string }
}

try {
  section("启动服务（真实模型）")
  await waitReady()
  const client = new GebaiClient({ baseUrl: base })
  await client.connect()
  const users = EXTERNAL ? [] : await Array.fromAsync(new Bun.Glob("users/*").scan({ cwd: HOME, onlyFiles: false }))
  const user = EXTERNAL ? "(外部实例)" : ((users[0] ?? "users/admin").replace(/\\/g, "/").split("/")[1] ?? "admin")
  const model = process.env.GEBAI_LLM_MODEL ?? "(未配置)"
  console.log(`  服务就绪 ${base}（${EXTERNAL ? "外部模式" : `用户 ${user}`}，模型 ${model}）`)

  const root = await fetch(`${base}/`).catch(() => null)
  const html = root ? await root.text() : ""
  check("前端页面可访问（UI 验证前置）", !!root?.ok && html.includes("<html"), `status=${root?.status ?? "ERR"}`)
  // 前端产物为新版（子会话容器类名/dist 已重建）——避免“后端新版 + 前端旧版”的错位验证
  const mainAsset = html.match(/assets\/(main-[^"]+\.js)/)?.[1]
  const bundle = mainAsset ? await (await fetch(`${base}/assets/${mainAsset}`)).text().catch(() => "") : ""
  check("前端产物含子会话容器（subsession-run）", bundle.includes("subsession-run"), mainAsset ?? "未匹配到主产物")

  // 会话消息经协议读取（REST/WS），不依赖实例的 GEBAI_HOME 布局——自起与外部两种模式通用
  const loadMsgs = async (sid: string): Promise<StoredMsg[]> => {
    const s = (await client.getSession(sid).catch(() => null)) as { messages?: StoredMsg[] } | null
    return s?.messages ?? []
  }

  const runTask = async (title: string, prompt: string): Promise<{ sid: string; chunks: Array<Record<string, unknown>>; text: string }> => {
    const created = (await client.request("session.create", { title })) as { session?: { id?: string }; id?: string }
    const sid = created?.session?.id ?? created?.id
    if (!sid) throw new Error("session.create 失败")
    const chunks: Array<Record<string, unknown>> = []
    for await (const ch of client.sendPrompt(sid, prompt, { env: { GEBAI_APPROVAL_SKIP: "true" } })) chunks.push(ch as unknown as Record<string, unknown>)
    const text = chunks.filter((c) => c.kind === "text" && c.subSession !== true).map((c) => String(c.text ?? "")).join("")
    return { sid, chunks, text }
  }
  const errorsOf = (chunks: Array<Record<string, unknown>>): string[] => chunks.filter((c) => c.kind === "error" || c.kind === "model_error").map((c) => String(c.error ?? ""))

  /* ---------- R1 隔离子会话 + 子Agent 预加载（真实任务） ---------- */
  section("R1 真实模型：隔离子会话委托 explore 子Agent（结果经工具结果交付）")
  const r1 = await runTask(
    "real-isolated",
    "请用 subsession_run 派一个**隔离上下文**的子会话（inherit_context 不传或 false），预加载 explore 子Agent（agents: [\"explore\"]），让它搜索服务端代码里注册 subsession_run 工具的位置（给出文件:行号），然后把它返回的结论原样转述给我。不要自己直接搜索，必须通过 subsession_run 委托。",
  )
  check("无模型错误", errorsOf(r1.chunks).length === 0, errorsOf(r1.chunks).join(" | ").slice(0, 200))
  const r1Call = r1.chunks.find((c) => c.kind === "tool_call" && (c.toolCall as { name?: string })?.name === "subsession_run")
  const r1Args = (r1Call?.toolCall as { arguments?: Record<string, unknown> } | undefined)?.arguments ?? {}
  console.log(`  模型调用参数: ${JSON.stringify(r1Args).slice(0, 220)}`)
  check("模型调用 subsession_run", !!r1Call)
  check("模型选了隔离形态（未继承上下文）", r1Args.inherit_context !== true, JSON.stringify(r1Args.inherit_context))
  check("模型按需预加载 explore 子Agent", Array.isArray(r1Args.agents) ? r1Args.agents.includes("explore") : false, JSON.stringify(r1Args.agents))
  const r1msgs = await loadMsgs(r1.sid)
  const r1Archive = r1msgs.find((m) => m.name === "subsession_run")?.subSessionArchive
  check("落盘：工具消息携带子会话过程存档", !!r1Archive, `${(r1Archive?.messages ?? []).length} 条过程消息`)
  check("子会话内真实执行了只读检索工具（全局或 explore 专属）", (r1Archive?.messages ?? []).some((m) => ["read", "grep", "glob", "ls", "explore_search_symbols", "explore_analyze"].includes(String(m.name ?? ""))), (r1Archive?.messages ?? []).map((m) => m.name).filter(Boolean).join(","))
  check("子会话结论指出工具定义位置（core/tools/agent.ts）", /agent\.ts/.test(String(r1Archive?.output ?? "")), String(r1Archive?.output ?? "").slice(0, 100))
  check("隔离子会话报告回传父会话（结果非空）", String(r1Archive?.output ?? "").length > 20, String(r1Archive?.output ?? "").slice(0, 120))
  check("父会话最终回复非空（转述子会话结论）", r1.text.trim().length > 20, r1.text.slice(0, 120))
  console.log(`  子会话结论: ${String(r1Archive?.output ?? "").replace(/\s+/g, " ").slice(0, 200)}`)

  /* ---------- R2 继承子会话并行 + 自动合入 ---------- */
  section("R2 真实模型：继承上下文并行子会话（两个子会话，报告自动合入）")
  const r2 = await runTask(
    "real-fork",
    "用 subsession_run 的 inherit_context:true 一次派生两个子会话（subsessions 数组，名分别叫 甲、乙）：甲负责回答「17 × 23 等于多少」并给出计算过程，乙负责回答「100 除以 7 的前 5 位小数」并给出计算过程。两个子会话的最终报告都要落到我这里，然后汇总成一句话给我。必须真的派生子会话，不要自己直接算。",
  )
  check("无模型错误", errorsOf(r2.chunks).length === 0, errorsOf(r2.chunks).join(" | ").slice(0, 200))
  const r2Call = r2.chunks.find((c) => c.kind === "tool_call" && (c.toolCall as { name?: string })?.name === "subsession_run")
  const r2Args = (r2Call?.toolCall as { arguments?: Record<string, unknown> } | undefined)?.arguments ?? {}
  console.log(`  模型调用参数: ${JSON.stringify(r2Args).slice(0, 260)}`)
  check("模型选了继承形态（inherit_context:true）", r2Args.inherit_context === true, JSON.stringify(r2Args.inherit_context))
  check("多任务并发形态（subsessions 数组）或单任务多次调用", Array.isArray(r2Args.subsessions) || !!r2Call)
  const r2Starts = r2.chunks.filter((c) => c.kind === "subsession_start")
  check("子会话 start 事件 ≥2（并行派生）", r2Starts.length >= 2, `${r2Starts.length} 个`)
  const r2msgs = await loadMsgs(r2.sid)
  const r2Merges = r2msgs.filter((m) => m.subSessionMerged)
  check("落盘：合并消息 ≥2（报告自动合入父会话）", r2Merges.length >= 2, r2Merges.map((m) => m.subSessionMerged?.name).join(","))
  check("合并消息为 user + engineNote=subsession（模型兼容形态）", r2Merges.every((m) => m.role === "user" && m.engineNote === "subsession"))
  const r2Text = r2.text + JSON.stringify(r2Merges.map((m) => String(m.content)))
  check("汇总含两个子会话结论（391 / 14.285…）", r2Text.includes("391") && /14\.28|14,28/.test(r2Text), r2Text.replace(/\s+/g, " ").slice(0, 200))
  console.log(`  合并消息: ${r2Merges.map((m) => String(m.content).replace(/\s+/g, " ").slice(0, 90)).join(" || ")}`)
  console.log(`  父会话最终回复: ${r2.text.replace(/\s+/g, " ").slice(0, 200)}`)

  /* ---------- R3 异步后台 + bg_task 控制（父会话不阻塞） ---------- */
  section("R3 真实模型：异步后台子会话（async:true + bg_task 控制）")
  const r3 = await runTask(
    "real-async",
    "用 subsession_run 并发模式做三件事：①传 async:true 与 inherit_context:true 后台派生一个子会话（input：计算 12×12 并给出过程）；②紧接着调 bg_task action=list 查看它的状态；③再调 bg_task action=wait 等它完成并取回结果（timeout 给 30）；最后把结果汇总给我。必须真的用 async:true，不要在调用里等它完成。",
  )
  check("无模型错误", errorsOf(r3.chunks).length === 0, errorsOf(r3.chunks).join(" | ").slice(0, 200))
  const r3Args = ((r3.chunks.find((c) => c.kind === "tool_call" && (c.toolCall as { name?: string })?.name === "subsession_run")?.toolCall as { arguments?: Record<string, unknown> } | undefined)?.arguments) ?? {}
  console.log(`  模型调用参数: ${JSON.stringify(r3Args).slice(0, 220)}`)
  check("模型选了异步运行（async:true）", r3Args.async === true, JSON.stringify(r3Args.async))
  const r3Start = r3.chunks.filter((c) => c.kind === "tool_result" && (c.toolCall as { name?: string })?.name === "subsession_run" && c.subSession !== true).map((c) => String(c.output ?? "")).join("\n")
  check("异步启动立即返回 runId（s 前缀）+ 后台说明", /子会话已后台启动/.test(r3Start) && /runId: s[0-9a-f]{8}/.test(r3Start), r3Start.slice(0, 140))
  const r3Bg = r3.chunks.filter((c) => c.kind === "tool_result" && (c.toolCall as { name?: string })?.name === "bg_task" && c.subSession !== true).map((c) => String(c.output ?? ""))
  check("父会话用 bg_task 查进度（list/status）", r3Bg.length >= 1 && /s[0-9a-f]{8}/.test(r3Bg.join("\n")), r3Bg.map((t) => t.slice(0, 60)).join(" | "))
  check("bg_task wait 取回异步子会话结果（144）", r3Bg.some((t) => t.includes("144")), r3Bg.map((t) => t.replace(/\s+/g, " ").slice(0, 80)).join(" | "))
  const r3msgs = await loadMsgs(r3.sid)
  const mergedWith144 = r3msgs.find((m) => m.subSessionMerged && (String(m.content).includes("144") || String(m.subSessionArchive?.output ?? "").includes("144")))
  const bgWithArchive = r3msgs.find((m) => m.name === "bg_task" && !!m.subSessionArchive)
  check("异步子会话完成：报告合入父会话（或 bg_task 携带存档）", !!mergedWith144 || !!bgWithArchive, mergedWith144 ? String(mergedWith144.subSessionMerged?.name) : bgWithArchive ? "bg_task 存档" : "未找到")
  console.log(`  异步汇总: ${r3.text.replace(/\s+/g, " ").slice(0, 180)}`)

  section("汇总")
  console.log(`  通过 ${passed} 项断言${failures.length ? `，失败 ${failures.length} 项` : ""}`)
  for (const f of failures) console.log(`   - ${f}`)
  console.log(`  服务日志 ${logFile}`)
  cleanup()
  process.exit(failures.length ? 1 : 0)
} catch (err) {
  console.error("\n[e2e-real] 失败:", err)
  console.error(`服务日志见 ${logFile}`)
  cleanup()
  process.exit(1)
}
