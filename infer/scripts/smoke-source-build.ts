/**
 * smoke-source-build —— 内网源码编译的**一条命令冒烟**（跨平台，不需要 PowerShell、不需要 GEBAI 服务在跑）。
 *
 * 设计原则：**零重复实现**。本脚本直接调用 local_infer 子Agent 的真实工具
 * （sources / source_build / engines / start / status / generate / batch / stop），
 * 因此它验证的就是模型走的那条路径——脚本与产品不会漂移。
 *
 * 流程（每步都做**真实断言**，不是只看输出）：
 *   1. 源码发现     sources(list)      —— 至少一个可编译候选
 *   2. 工具链与后端 sources(toolchain) —— 选定后端就绪（缺什么给出补齐办法）
 *   3. 编译安装     source_build       —— 后台编译 + 实时进度；产物 exe 落盘可执行
 *   4. 引擎可见     engines(list)      —— 新引擎出现在「已安装」里
 *   5. 启动服务     start              —— /health 通、/props 有 n_ctx
 *   6. 结构化推理   generate(schema)   —— 返回可解析且满足必填字段的 JSON
 *   7. 批量提交     batch(可关)        —— results.jsonl 行数与成功数达标
 *   8. 清理         stop               —— 端口释放（--keep 可保留）
 *
 * 退出码：0 全通过 / 1 有失败项（可接 CI 或开机自检）。
 *
 * 用法：见 --help（或用同目录的 smoke-source-build.sh / .ps1 包装）。
 */
import { existsSync, mkdirSync, readFileSync, statSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { tools } from "../../packages/agents/src/agents/local_infer/local_infer"

// ── 参数 ──────────────────────────────────────────────────────────────────

interface Opts {
  device: string
  engineId: string
  jobs: number
  port: number
  model: string
  profile: string
  items: number
  skipBuild: boolean
  clean: boolean
  keep: boolean
  timeoutSec: number
  help: boolean
}

function parseArgs(argv: string[]): Opts {
  const o: Opts = {
    device: "cpu", // CPU 是最普适的后端（GPU 后端编译更久；要测 GPU 用 --device cuda）
    engineId: "",
    jobs: 0,
    port: 19180,
    model: "",
    profile: "cpu-small",
    items: 2,
    skipBuild: false,
    clean: false,
    keep: false,
    timeoutSec: 1800,
    help: false,
  }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    const val = (): string => argv[++i] ?? ""
    if (a === "--help" || a === "-h") o.help = true
    else if (a === "--device") o.device = val()
    else if (a === "--engine-id") o.engineId = val()
    else if (a === "--jobs") o.jobs = Number(val()) || 0
    else if (a === "--port") o.port = Number(val()) || 19180
    else if (a === "--model") o.model = val()
    else if (a === "--profile") o.profile = val()
    else if (a === "--items") o.items = Math.max(0, Number(val()) || 0)
    else if (a === "--timeout") o.timeoutSec = Number(val()) || 1800
    else if (a === "--skip-build") o.skipBuild = true
    else if (a === "--clean") o.clean = true
    else if (a === "--keep") o.keep = true
  }
  return o
}

const HELP = `smoke-source-build —— 内网源码编译冒烟（调用 local_infer 真实工具）

用法: bun run infer/scripts/smoke-source-build.ts [选项]
      bash  infer/scripts/smoke-source-build.sh  [选项]
      pwsh -File infer/scripts/smoke-source-build.ps1 [选项]

选项:
  --device <cpu|cuda|vulkan|metal|rocm>  编译后端（缺省 cpu——最普适且最快）
  --engine-id <id>                       目标引擎 id（缺省 smoke-<平台>-<后端>；同 id 重跑=幂等跳过编译）
  --jobs <n>                             并行编译任务数（缺省 = CPU 核数的 75%）
  --port <n>                             服务端口（缺省 19180）
  --model <文件名>                       模型（缺省自动挑模型目录里的第一个 GGUF；优先小模型）
  --profile <名>                         运行档位（缺省 cpu-small）
  --items <n>                            批量提交条目数（缺省 2；0 = 跳过批量）
  --timeout <秒>                         等待编译完成的超时（缺省 1800）
  --skip-build                           跳过编译，直接用已安装的引擎（验证下游链路）
  --clean                                **强制全量重编**（删掉已有构建目录；CPU 约 5 分钟、CUDA/Vulkan 更久）——
                                         用于真实验证编译能力（缺省是增量：热构建目录下几秒完成）
  --keep                                 结束后保留服务运行（缺省自动 stop）
  -h, --help                             显示本帮助

前置:
  · 源码：把 llama.cpp 源码目录或归档放进 {GEBAI_HOME}/resources/src/（或用 LOCAL_INFER_SOURCE_DIRS 指定）；
    联网机器可先拉：bun run resources:download --only="src/**"
  · 模型：{GEBAI_HOME}/resources/models/infer/ 下至少一个 GGUF（缺小模型可用 model_fetch preset=qwen2.5-0.5b）
  · 工具链：cmake + ninja/make + 编译器（CUDA 还需 nvcc）——第 2 步会如实报缺什么、怎么补
`

// ── 输出 ──────────────────────────────────────────────────────────────────

let failures = 0
const checks: Array<{ name: string; ok: boolean; detail: string }> = []

function section(title: string): void {
  console.log(`\n${"─".repeat(78)}\n${title}\n${"─".repeat(78)}`)
}

function check(name: string, ok: boolean, detail = ""): boolean {
  checks.push({ name, ok, detail })
  if (ok) console.log(`  [OK]   ${name}${detail ? ` —— ${detail}` : ""}`)
  else {
    failures++
    console.log(`  [FAIL] ${name}${detail ? ` —— ${detail}` : ""}`)
  }
  return ok
}

function info(line: string): void {
  console.log(`         ${line}`)
}

function fmtSecs(ms: number): string {
  return `${(ms / 1000).toFixed(1)}s`
}

// ── 工具上下文（与引擎注入的形状一致，但不依赖 GEBAI 服务） ────────────────

function makeCtx(inferRoot: string, repoRoot: string): ToolContext {
  const workdir = join(tmpdir(), "gebai-smoke-source-build")
  mkdirSync(workdir, { recursive: true })
  const env: Record<string, string> = {
    ...(process.env as Record<string, string>),
    LOCAL_INFER_HOME: inferRoot,
    ...(process.env.LOCAL_INFER_MODELS_DIR ? {} : { LOCAL_INFER_MODELS_DIR: join(repoRoot, "resources", "models", "infer") }),
  }
  return {
    user: "smoke",
    sessionId: "smoke-source-build",
    workdir,
    sessionWorkdir: workdir,
    home: repoRoot,
    env,
    sandboxed: false,
    resolvePath: (p: string) => (p.startsWith("/") || /^[A-Za-z]:[\\/]/.test(p) ? p : join(workdir, p)),
    readFile: async (p: string) => Bun.file(p).text(),
    readBinaryFile: async (p: string) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p: string, c: string) => void (await Bun.write(p, c)),
    writeBinaryFile: async (p: string, d: Uint8Array) => void (await Bun.write(p, d)),
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    async runCommand(cmd: string, opts?: { workdir?: string; timeoutMs?: number; env?: Record<string, string> }) {
      const shell = process.platform === "win32" ? ["cmd", "/c", cmd] : ["bash", "-lc", cmd]
      const proc = Bun.spawn(shell, {
        cwd: opts?.workdir ?? workdir,
        env: { ...env, ...(opts?.env ?? {}) },
        stdout: "pipe",
        stderr: "pipe",
      })
      const timer = opts?.timeoutMs ? setTimeout(() => proc.kill(), opts.timeoutMs) : undefined
      const [stdout, stderr] = await Promise.all([new Response(proc.stdout).text(), new Response(proc.stderr).text()])
      const code = await proc.exited
      if (timer) clearTimeout(timer)
      return { stdout, stderr, code }
    },
    uploadAttachment: async () => "",
    publish: () => {},
    projects: [],
    resolveProjectPath: () => repoRoot,
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
  } as unknown as ToolContext
}

type ToolMap = Record<string, { execute: (a: Record<string, unknown>, c: ToolContext) => Promise<{ output: string; data?: unknown }> }>
const T = tools as unknown as ToolMap

async function call(name: string, args: Record<string, unknown>, ctx: ToolContext): Promise<{ output: string; data?: unknown }> {
  const tool = T[name]
  if (!tool) throw new Error(`工具不存在：${name}（可用：${Object.keys(T).join(", ")}）`)
  return await tool.execute(args, ctx)
}

const d = <X>(r: { data?: unknown }): X => (r.data ?? {}) as X

// ── 主流程 ────────────────────────────────────────────────────────────────

async function main(): Promise<number> {
  const opts = parseArgs(process.argv.slice(2))
  if (opts.help) {
    console.log(HELP)
    return 0
  }

  const scriptsDir = dirname(new URL(import.meta.url).pathname)
  const inferRoot = resolve(scriptsDir, "..")
  const repoRoot = resolve(inferRoot, "..")
  const ctx = makeCtx(inferRoot, repoRoot)
  // --skip-build 且未显式给 id 时，自动挑一个已安装引擎（否则默认 smoke-* 还没编出来，start 必然找不到）
  let engineId = opts.engineId || `smoke-${process.platform}-${opts.device}`
  if (opts.skipBuild && !opts.engineId) {
    // --skip-build 且未显式给 id：自动挑一个本机现成可用的引擎（否则默认 smoke-* 还没编出来，start 必然找不到）
    const eng0 = await call("engines", { action: "list" }, ctx)
    const pick = d<{ installed_ids?: string[] }>(eng0).installed_ids?.[0]
    if (pick) {
      engineId = pick
      console.log(`自动选用已安装引擎：${engineId}（可用 --engine-id 指定）`)
    }
  }

  console.log(`smoke-source-build —— 内网源码编译冒烟`)
  console.log(`  推理子项目: ${inferRoot}`)
  console.log(`  引擎 id:    ${engineId}（后端 ${opts.device}${opts.skipBuild ? "，跳过编译" : opts.clean ? "，全量重编" : "，增量编译"}）`)
  console.log(`  端口:       ${opts.port}${opts.keep ? "（结束后保留服务）" : "（结束后自动停止）"}`)

  // ── 1. 源码发现 ────────────────────────────────────────────────────────
  section("[1/8] 源码发现（sources）")
  const srcRes = await call("sources", { action: "list" }, ctx)
  console.log(srcRes.output)
  const srcData = d<{ count: number; candidates: Array<{ id: string; kind: string; path: string; peek?: { has_vendor_deps: boolean } }> }>(srcRes)
  if (!check("发现至少一个可编译源码候选", srcData.count > 0, srcData.count ? `${srcData.count} 个` : "无")) {
    info("获取源码：联网机器执行 bun run resources:download --only=\"src/**\"，把 resources/src/ 带入内网")
    return finish()
  }
  // 优先 llama.cpp 候选；归档缺 vendor/ 时给出明确警告（离线受限，但不阻断——用户可能已联网）
  const cand = srcData.candidates.find((c) => c.kind === "llama_cpp") ?? srcData.candidates[0]
  info(`选用候选：${cand.id}（${cand.kind}）`)
  if (cand.peek && !cand.peek.has_vendor_deps) {
    info("注意：该归档未发现 vendor/ 依赖——纯离线环境编译可能因缺依赖失败；建议换用完整源码包")
  }

  // ── 2. 工具链与后端 ────────────────────────────────────────────────────
  section("[2/8] 工具链与后端就绪（sources action=toolchain）")
  const tcRes = await call("sources", { action: "toolchain" }, ctx)
  console.log(tcRes.output)
  const tcData = d<{ ready: string[]; toolchain: { devices: Record<string, { ready: boolean; missing: string[] }> } }>(tcRes)
  const dev = tcData.toolchain?.devices?.[opts.device]
  if (!check(`后端 ${opts.device} 可编译`, Boolean(dev?.ready), dev ? (dev.ready ? "就绪" : `缺 ${dev.missing.join("、")}`) : "未知后端")) {
    return finish()
  }

  // ── 3. 编译安装 ────────────────────────────────────────────────────────
  section("[3/8] 编译安装（source_build）")
  let exe = ""
  if (opts.skipBuild) {
    info("已指定 --skip-build：跳过编译，改用已安装引擎")
  } else {
    const t0 = Date.now()
    const bg = await call(
      "source_build",
      { action: "build", source: cand.id, engine_id: engineId, device: opts.device, jobs: opts.jobs, clean: opts.clean, background: true },
      ctx,
    )
    const buildId = d<{ build_id: string }>(bg).build_id
    info(`编译任务：${buildId}（后台执行，实时进度如下）`)
    const seen = new Set<string>()
    let state = "running"
    let err = ""
    while (Date.now() - t0 < opts.timeoutSec * 1000) {
      await Bun.sleep(5000)
      const st = await call("source_build", { action: "status", build_id: buildId }, ctx)
      const sd = d<{ state: string; steps: Array<{ phase: string; message: string }>; exe?: string; error?: string }>(st)
      for (const s of sd.steps ?? []) {
        const key = `${s.phase}|${s.message}`
        if (seen.has(key)) continue
        seen.add(key)
        info(`[${s.phase}] ${s.message.slice(0, 150)}`)
      }
      state = sd.state
      err = sd.error ?? ""
      if (state !== "running") {
        exe = sd.exe ?? ""
        break
      }
    }
    const secs = fmtSecs(Date.now() - t0)
    if (state === "running") {
      check("编译在超时内完成", false, `超过 ${opts.timeoutSec}s 仍在跑（日志：source_build action=log build_id=${buildId}）`)
      return finish()
    }
    if (!check(`编译成功（${secs}）`, state === "done", state === "done" ? `引擎 ${engineId}` : err.split("\n")[0])) {
      info(`日志尾部：local_infer_source_build(action="log", build_id="${buildId}")`)
      return finish()
    }
    if (!check("产物可执行文件存在", Boolean(exe) && existsSync(exe), exe || "未报告 exe")) return finish()
    try {
      const size = statSync(exe).size
      info(`exe: ${exe}（${(size / 1024 / 1024).toFixed(2)} MB）`)
    } catch {
      /* 已断言存在 */
    }
  }

  // ── 4. 引擎可见性 ──────────────────────────────────────────────────────
  section("[4/8] 引擎可见性（engines）")
  if (!opts.skipBuild) {
    const eng = await call("engines", { action: "list" }, ctx)
    const visible = eng.output.includes(engineId)
    check(`新引擎 ${engineId} 出现在引擎清单里`, visible, visible ? "可被 start 的 engine 参数引用" : "未出现（检查 .engine.json 标记）")
  } else {
    info("跳过（--skip-build）")
  }

  // ── 5. 启动服务 ────────────────────────────────────────────────────────
  section("[5/8] 启动服务（start）")
  // 端口预检：占用先停掉（冒烟脚本要独占该端口）
  const preStatus = await call("status", { port: opts.port }, ctx)
  if (/监听中/.test(preStatus.output)) {
    info(`端口 ${opts.port} 已被占用，先停止其占用进程`)
    await call("stop", { port: opts.port }, ctx)
    await Bun.sleep(1000)
  }
  // 模型选择：显式 > 小模型优先 > 目录内第一个 GGUF
  let model = opts.model
  if (!model) {
    const modelsRes = await call("models", {}, ctx)
    const names = [...modelsRes.output.matchAll(/([A-Za-z0-9._-]+\.gguf)(?!\.incomplete)/g)].map((m) => m[1])
    model = names.find((n) => /0\.[5-9]B|1B|1\.5B|2B|3B/i.test(n)) ?? names[0] ?? ""
    if (names.length) info(`模型：自动选用 ${model}（目录内 ${names.length} 个 GGUF；可用 --model 指定）`)
  }
  if (!check("模型文件可用", Boolean(model), model || "模型目录内无 GGUF——可 local_infer_model_fetch(preset=\"qwen2.5-0.5b\")")) return finish()

  const startRes = await call(
    "start",
    { profile: opts.profile, port: opts.port, model, engine: engineId },
    ctx,
  )
  console.log(startRes.output)
  const ready = /服务就绪/.test(startRes.output)
  if (!check("服务就绪", ready)) return finish()

  // /health 与 /props 直连复验（不只看工具自述）
  const base = `http://127.0.0.1:${opts.port}`
  let nCtx = 0
  let slots = 0
  try {
    const h = await fetch(`${base}/health`, { signal: AbortSignal.timeout(5000) })
    const p = await fetch(`${base}/props`, { signal: AbortSignal.timeout(5000) })
    const pj = (await p.json()) as { default_generation_settings?: { n_ctx?: number }; total_slots?: number }
    nCtx = pj.default_generation_settings?.n_ctx ?? 0
    slots = pj.total_slots ?? 0
    check("直连 /health 与 /props 可用", h.ok && nCtx > 0, `n_ctx=${nCtx} slots=${slots}`)
  } catch (e) {
    check("直连 /health 与 /props 可用", false, (e as Error).message)
  }

  // ── 6. 结构化推理 ──────────────────────────────────────────────────────
  section("[6/8] 结构化推理（generate + JSON Schema）")
  const schema = {
    type: "object",
    required: ["text"],
    properties: { text: { type: "string", minLength: 2 } },
  }
  const genRes = await call(
    "generate",
    {
      prompt: "用一句 JSON 说明：内网环境下用本机源码编译出的引擎也能正常推理（字段 text）。",
      schema,
      max_tokens: 160,
      temperature: 0,
      port: opts.port,
    },
    ctx,
  )
  console.log(genRes.output)
  const gen = d<{ ok: boolean; json?: { text?: string }; json_errors?: string[]; decode_tps?: number; elapsed_ms?: number }>(genRes)
  const jsonOk = gen.ok === true && typeof gen.json?.text === "string" && gen.json.text.length >= 2 && !(gen.json_errors?.length)
  check("结构化输出可解析且满足必填字段", jsonOk, jsonOk ? `"${gen.json?.text}"` : `json_errors=${JSON.stringify(gen.json_errors)}`)
  if (jsonOk) info(`实测解码 ${gen.decode_tps?.toFixed(1) ?? "?"} t/s ｜ 耗时 ${((gen.elapsed_ms ?? 0) / 1000).toFixed(2)}s`)

  // ── 7. 批量提交 ────────────────────────────────────────────────────────
  section(`[7/8] 批量提交（batch${opts.items ? ` × ${opts.items}` : " 已跳过"}）`)
  if (opts.items > 0) {
    const jobId = `smoke-src-${new Date().toISOString().replace(/[-:T]/g, "").slice(0, 14)}`
    const batchRes = await call(
      "batch",
      {
        job_id: jobId,
        items: Array.from({ length: opts.items }, (_, i) => ({
          id: `s${i + 1}`,
          prompt: `第 ${i + 1} 条：用一句 JSON 说明源码编译引擎的一个用途（字段 text）。`,
        })),
        schema,
        concurrency: 1,
        max_tokens: 160,
        temperature: 0,
        port: opts.port,
      },
      ctx,
    )
    console.log(batchRes.output)
    const b = d<{ ok: number; failed: number; results_file?: string }>(batchRes)
    check(`批量 ${opts.items} 条全部成功`, b.ok === opts.items && b.failed === 0, `成功 ${b.ok} / 失败 ${b.failed}`)
    if (b.results_file && existsSync(b.results_file)) {
      const lines = readFileSync(b.results_file, "utf-8").trim().split("\n").filter(Boolean)
      const okLines = lines.filter((l) => JSON.parse(l).ok === true).length
      check("产物 results.jsonl 行数与成功数一致", lines.length === opts.items && okLines === opts.items, `${lines.length} 行（成功 ${okLines}）`)
      info(`产物：${b.results_file}`)
    } else {
      check("产物 results.jsonl 落盘", false, "未报告结果文件")
    }
  } else {
    info("已指定 --items 0：跳过批量")
  }

  // ── 8. 清理 ────────────────────────────────────────────────────────────
  section("[8/8] 清理")
  if (opts.keep) {
    info(`已指定 --keep：服务保留在 ${base}（停止：local_infer_stop(port=${opts.port})）`)
  } else {
    const stopRes = await call("stop", { port: opts.port }, ctx)
    console.log(stopRes.output)
    await Bun.sleep(1200)
    let released = false
    try {
      await fetch(`${base}/health`, { signal: AbortSignal.timeout(2000) })
    } catch {
      released = true
    }
    check("服务已停止且端口释放", released, released ? `:${opts.port} 已释放` : `:${opts.port} 仍在响应（--keep？）`)
  }

  return finish()
}

function finish(): number {
  section("汇总")
  for (const c of checks) console.log(`  ${c.ok ? "✓" : "✗"} ${c.name}${c.detail ? ` —— ${c.detail}` : ""}`)
  const passed = checks.filter((c) => c.ok).length
  console.log("")
  if (failures === 0) {
    console.log(`冒烟全部通过 ✓（${passed}/${checks.length} 项）`)
    return 0
  }
  console.log(`冒烟有 ${failures} 项失败 ✗（通过 ${passed}/${checks.length}）`)
  return 1
}

process.exit(await main())
