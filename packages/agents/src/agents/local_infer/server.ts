/**
 * local_infer 的【推理进程管理】工具层：服务状态（含实例列表与 PID 存活校验）、
 * 启停与重启、服务日志查看、基准测试、GGUF 结构解析。
 *
 * 三层分工（单一事实来源）：
 *   * paths.ts  —— 路径/档位/模型/状态文件/日志的读写；
 *   * launcher.ts —— **跨平台进程管理**（参数组装、脱离进程树启动、就绪轮询、进程/端口命令）；
 *   * 本文件   —— 命令组织、结果解释与人读输出；launcher 的命令纯函数在这里再导出，兼容既有引用。
 *
 * 启动默认走 launcher（TS 直接 spawn，不需要 PowerShell，Linux/macOS 同样可用）；
 * `mode: "script"`（或环境变量 LOCAL_INFER_LAUNCH=script）回退到 scripts/run-server.ps1——仅 Windows 兼容路径。
 */
import { existsSync, mkdirSync, writeFileSync } from "node:fs"
import { join, dirname } from "node:path"
import type { Tool, ToolContext, ToolSchema } from "@gebai/sdk"
import { probe } from "./api"
import {
  SERVER_NAME,
  buildServerArgs,
  defaultThreads,
  isListening,
  killAllCmd as launcherKillAllCmd,
  killPid as launcherKillPid,
  launcherEnv,
  listProcsCmd as launcherListProcsCmd,
  parsePortOwner,
  pidAlive as launcherPidAlive,
  portOwnerCmd,
  resolveEngineExe,
  spawnServerAsync,
  waitReady,
  type LaunchResult,
} from "./launcher"
import {
  type InferProfile,
  type InferProfiles,
  type LaunchRecord,
  type LogFile,
  type ServerState,
  baseUrl,
  clearServerState,
  defaultPort,
  inferHome,
  isAbsolutePath,
  listModels,
  listServerLogs,
  loadProfiles,
  modelsDir,
  profileModel,
  profilesPath,
  readLaunchRecords,
  readServerStates,
  reportsDir,
  stateDir,
  tailLines,
  vendorDir,
  writeServerState,
} from "./paths"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

// ── 外部命令封装（实现在 launcher.ts，这里再导出以兼容既有引用） ──────────
// 平台分支（taskkill/netstat vs kill/ss）的**唯一实现**已迁到 launcher.ts（跨平台进程管理）；
// 本文件只保留 mode=script 需要的 pwsh/scriptRun，以及名字/签名不同的两个兼容包装。

/** script 模式用的 PowerShell：PowerShell 7 优先（脚本含中文，Windows PowerShell 5.1 按 GBK 读会乱码），回退 powershell。 */
export function pwshCmd(platform: NodeJS.Platform = process.platform): string {
  return platform === "win32" ? "pwsh -NoProfile" : "pwsh"
}

/** 组装 `pwsh -File <infer>/scripts/<script> <args>` 调用串。 */
export function scriptRun(home: string, script: string, args: string[], platform: NodeJS.Platform = process.platform): string {
  const file = join(home, "scripts", script)
  return `${pwshCmd(platform)} -ExecutionPolicy Bypass -File "${file}" ${args.join(" ")}`
}

// 平台命令（探活/终止/端口占用）的**唯一实现已迁到 launcher.ts**（跨平台进程管理）——
// 与启动、就绪轮询放在一起，避免同一份平台分支散落两处。这里再导出以兼容既有引用与测试；
// 名字/签名不同的两个（killAllCmd / listProcsCmd 在 launcher 里带进程名参数）用包装保持旧签名。

export { isListening, killPidCmd, pidAliveCmd, pidAliveVerdict, portOwnerCmd, type LaunchResult } from "./launcher"
/** 端口占用者 PID 解析（launcher.parsePortOwner 的旧名）。 */
export { parsePortOwner as parsePortOwnerPid } from "./launcher"

/** 兼容包装：终止全部 llama-server（launcher 版为 killAllCmd(name, platform)）。 */
export function killAllCmd(platform: NodeJS.Platform = process.platform): string {
  return launcherKillAllCmd(SERVER_NAME, platform)
}

/** 兼容包装：列出 llama-server 进程（人读用）。 */
export function listProcsCmd(platform: NodeJS.Platform = process.platform): string {
  return launcherListProcsCmd(SERVER_NAME, platform)
}

/** 兼容包装：查询端口监听情况（launcher 版名为 portOwnerCmd）。 */
export function portListenCmd(port: number, platform: NodeJS.Platform = process.platform): string {
  return portOwnerCmd(port, platform)
}

// ── 日志错误特征 ──────────────────────────────────────────────────────────

/** 常见失败特征（用于 logs 工具的「疑似错误」摘要；命中的都是排查时真正有用的行）。 */
const ERROR_PATTERNS: Array<{ re: RegExp; label: string }> = [
  { re: /failed to load model/i, label: "模型加载失败" },
  { re: /error loading model/i, label: "模型加载错误" },
  { re: /CUDA error|CUDA out of memory/i, label: "CUDA 错误/显存不足" },
  { re: /out of memory/i, label: "内存不足" },
  { re: /address already in use|EADDRINUSE|bind.*failed/i, label: "端口被占用" },
  { re: /exiting due to|exiting\.\.\./i, label: "引擎主动退出" },
  { re: /assertion|terminate called/i, label: "断言/异常终止" },
  { re: /unknown argument|invalid argument/i, label: "参数不被引擎接受" },
  { re: /no such file or directory|not found/i, label: "文件缺失" },
]

/** 从日志尾部文本里挑出疑似错误行（去重后最多 5 条）。 */
export function suspectedErrors(text: string): string[] {
  const hits: string[] = []
  for (const line of text.split(/\r?\n/)) {
    const t = line.trim()
    if (!t) continue
    for (const p of ERROR_PATTERNS) {
      if (p.re.test(t)) {
        hits.push(`${p.label}：${t.slice(0, 200)}`)
        break
      }
    }
    if (hits.length >= 5) break
  }
  return hits
}

// ── 公共小工具 ────────────────────────────────────────────────────────────

/** 最近 N 条启动记录：paths.readLaunchRecords 按文件位次占额，一条损坏即白占一个名额——
 *  这里先过取若干再截断，使「最新一条损坏」不至于让「最近启动」/日志定位整体空白。 */
export function latestLaunches(home: string, limit = 1): LaunchRecord[] {
  return readLaunchRecords(home, Math.max(0, limit) + 7).slice(0, Math.max(0, limit))
}

/** 相对时间（状态输出用；无法解析则原样返回）。 */
function agoLabel(iso?: string): string {
  if (!iso) return "?"
  const t = Date.parse(iso)
  if (!Number.isFinite(t)) return iso
  const s = Math.max(0, Math.round((Date.now() - t) / 1000))
  if (s < 60) return `${s} 秒前`
  if (s < 3600) return `${Math.round(s / 60)} 分钟前`
  if (s < 86400) return `${Math.round(s / 3600)} 小时前`
  return `${Math.round(s / 86400)} 天前`
}

/** 缺省档位名（环境变量优先，其次 profiles 的 default_profile）。 */
function activeProfile(prof: InferProfiles | null, env: Record<string, string>): string {
  return env.LOCAL_INFER_PROFILE ?? prof?.default_profile ?? "fast"
}

/** 检查子项目根，缺失时返回可操作的提示文本。 */
function homeGuard(ctx: ToolContext): { home: string; error?: string } {
  const home = inferHome(ctx.env)
  if (!existsSync(home)) {
    return { home, error: `未找到本地推理子项目：${home}（可用 LOCAL_INFER_HOME 指定）` }
  }
  return { home }
}

/** 探测某端口的服务实例：/health 可用性 + /props 的 n_ctx 与 slot 数。 */
async function probePort(port: number, timeoutMs = 3000): Promise<{ ok: boolean; text: string; slots?: number }> {
  const p = await probe(baseUrl(port), { timeoutMs })
  if (!p.ok) return { ok: false, text: `不可用（${p.error ?? "探测失败"}）` }
  const bits: string[] = [`HTTP ${p.health_status}`]
  if (p.n_ctx) bits.push(`n_ctx=${p.n_ctx}`)
  if (p.total_slots) bits.push(`slots=${p.total_slots}`)
  if (p.model_path) bits.push(`model=${p.model_path.split(/[\\/]/).pop()}`)
  const text = `${bits.join("  ")}${p.error ? `  （${p.error}）` : ""}`
  return { ok: true, text, slots: p.total_slots }
}

/** 端口占用者 PID（用于旧版启动脚本未写状态文件时补齐）。 */
async function discoverPidByPort(ctx: ToolContext, port: number, platform: NodeJS.Platform): Promise<number | undefined> {
  const r = await ctx.runCommand(portListenCmd(port, platform), { timeoutMs: 20000 })
  return parsePortOwner(r.stdout, platform)
}

/** 启动后补齐状态文件：脚本已写则不覆盖；未写则按端口反查 PID 与日志路径补写。 */
async function ensureServerState(
  ctx: ToolContext,
  home: string,
  port: number,
  platform: NodeJS.Platform,
  info: { profile: string; model: string },
): Promise<string | undefined> {
  const existing = readServerStates(home).find((s) => s.port === port)
  const pid = await discoverPidByPort(ctx, port, platform)
  const launch = latestLaunches(home, 1)[0]
  const logs = listServerLogs(home, 4)
  const out = logs.find((l) => l.kind === "out")
  const err = logs.find((l) => l.kind === "err")
  if (existing && (!pid || existing.pid === pid)) {
    // 脚本已写且 PID 一致：只补日志路径（旧脚本可能未带）
    if (!existing.log && out) {
      writeServerState(home, { ...existing, log: out.path, err_log: err?.path ?? existing.err_log })
      return `已补齐状态文件日志路径（PID ${existing.pid}）`
    }
    return undefined
  }
  if (!pid) {
    // 服务在跑但查不到 PID（权限或平台差异）：写入状态时保留未知 PID 提示，交由 status/stop 处理
    writeServerState(home, {
      pid: 0,
      port,
      profile: info.profile,
      model: info.model,
      host: "127.0.0.1",
      log: out?.path,
      err_log: err?.path,
      started_at: new Date().toISOString(),
      argv: launch?.argv,
      engine: launch?.exe,
      launch_mode: "script",
    } as ServerStateX)
    return `服务已就绪但未能反查 PID（已写状态文件占位，status 会提示）`
  }
  writeServerState(home, {
    pid,
    port,
    profile: info.profile,
    model: info.model,
    host: "127.0.0.1",
    log: out?.path,
    err_log: err?.path,
    started_at: new Date().toISOString(),
    argv: launch?.argv,
    engine: launch?.exe,
    launch_mode: "script",
  } as ServerStateX)
  return `已写入服务状态文件：${stateDir(home)}/server-${port}.json（PID ${pid}）`
}

// ── status ────────────────────────────────────────────────────────────────

/** status 执行体（restart 也复用其探测逻辑，故独立成函数）。 */
export async function doStatus(args: Record<string, unknown>, ctx: ToolContext, platform: NodeJS.Platform = process.platform): Promise<string> {
  const home = inferHome(ctx.env)
  const port = Number(args.port ?? defaultPort(ctx.env))
  const lines: string[] = []

  if (!existsSync(home)) {
    return `未找到本地推理子项目：${home}\n可通过 LOCAL_INFER_HOME 指定子项目根，或在仓库中初始化 infer/。`
  }
  lines.push(`子项目根: ${home}`)

  const prof = loadProfiles(home)
  if (prof) {
    const active = activeProfile(prof, ctx.env)
    const model = profileModel(prof, active)
    lines.push(`档位: ${active}（可用: ${Object.keys(prof.profiles).join(", ")}）`)
    lines.push(`模型: ${model || "（档位未指定）"}`)
    const p = prof.profiles[active]
    if (p) {
      lines.push(
        `  参数: ctx=${p.ctx ?? "-"} n_gpu_layers=${p.n_gpu_layers ?? "-"} n_cpu_moe=${p.n_cpu_moe ?? "-"} parallel=${p.parallel ?? "-"} spec=${p.spec_type ?? "off"}`,
      )
    }
    lines.push(`引擎目录: ${join(home, prof.engine_dir)}`)
  } else {
    lines.push("档位定义缺失或损坏：config/profiles.json")
  }

  // 实例列表（状态文件 = 精确 PID/日志依据）
  const states = readServerStates(home)

  // 引擎：从状态文件读启动方式与引擎（launcher 模式记引擎 id，script 模式记 exe/目录名），
  // 再探一次「现在还能不能用」（引擎可能被删/vendor 被清）——这决定重启能不能成。
  const st0 = states[0] as ServerStateX | undefined
  const engineId = engineIdFromState(st0)
  const resolved = resolveEngineExe({ home, engineId, engineDir: prof?.engine_dir, platform })
  lines.push("", "── 引擎 ──")
  lines.push(
    `当前: ${st0?.engine ?? "（状态文件未记录——旧版脚本或尚未启动）"}   启动方式: ${st0?.launch_mode ?? "（未记录，按旧脚本计）"}`,
  )
  if (resolved.exe) {
    lines.push(`可执行文件: ${resolved.exe}（${resolved.via === "engine" ? "已安装引擎" : "档位 engine_dir"}）`)
  } else {
    lines.push(`可执行文件: **未找到**——${resolved.note ?? ""}`)
    lines.push(`缺引擎时：local_infer_engines 看可下载项 → local_infer_engine_fetch 安装（启动仍需先解决引擎）`)
  }
  if (prof?.engine_dir) lines.push(`档位 engine_dir: ${join(home, prof.engine_dir)}`)

  lines.push("", "── 实例（服务状态文件） ──")
  if (!states.length) {
    lines.push("（无状态文件：服务未由本工具/脚本启动，或用旧版启动脚本）")
  }
  for (const s of states) {
    const alive = s.pid > 0 ? await launcherPidAlive(s.pid, ctx, platform) : false
    const log = s.log ? s.log.split(/[\\/]/).pop() : "-"
    lines.push(
      `:${s.port}  PID ${s.pid}${s.pid > 0 ? (alive ? "（存活）" : "（**已退出**）") : "（未知）"}  档位 ${s.profile}  模型 ${s.model}`,
    )
    lines.push(`     启动 ${s.started_at ?? "?"}（${agoLabel(s.started_at)}）  日志 ${log}`)
    if (s.pid > 0 && !alive) {
      lines.push(`     ⚠ 状态文件陈旧（进程已退出，可能崩溃）——看日志排查，或用 stop 清理状态文件`)
      if (s.log) {
        const tail = tailLines(s.log, 12)
        const errs = suspectedErrors(tail.text)
        if (errs.length) lines.push(`     疑似错误：${errs[0]}`)
      }
    }
  }

  // 进程
  const ps = await ctx.runCommand(listProcsCmd(platform), { timeoutMs: 20000 })
  lines.push("", "── 进程 ──")
  // 僵尸进程（POSIX：已退出但父进程未回收）不应被当成「在跑」——标注出来，否则排查时会被误导
  const procLines = ps.stdout.trim().split("\n").filter(Boolean)
  const zombie = procLines.filter((l) => /<defunct>/.test(l))
  const liveProcs = procLines.filter((l) => !/<defunct>/.test(l))
  lines.push(liveProcs.length ? liveProcs.slice(0, 3).join("\n") : "未运行 llama-server（无活动进程）")
  if (zombie.length) {
    lines.push(`（另有 ${zombie.length} 个僵尸条目：进程已退出、等待父进程回收，不占端口也不占用算力，父进程退出后自动消失）`)
  }

  // 端口 + 探测
  const net = await ctx.runCommand(portListenCmd(port, platform), { timeoutMs: 20000 })
  const listening = isListening(net.stdout)
  lines.push("", "── 端口 ──")
  lines.push(`:${port} ${listening ? "监听中" : "未监听"}`)
  const owner = parsePortOwner(net.stdout, platform)
  if (listening && owner) lines.push(`占用者 PID: ${owner}`)

  if (listening) {
    const pr = await probePort(port)
    lines.push(`探测: ${pr.text}`)
    if (pr.ok) {
      lines.push(`OpenAI 兼容端点: ${baseUrl(port)}/v1/chat/completions`)
      if (pr.slots) lines.push(`并发上限（slot 数）: ${pr.slots} —— 批量提交的 concurrency 不要超过它`)
    }
  }

  // GPU
  const smi = await ctx.runCommand(
    `nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu,power.draw --format=csv,noheader`,
    { timeoutMs: 30000 },
  )
  lines.push("", "── GPU ──")
  lines.push(smi.code === 0 ? smi.stdout.trim() : `nvidia-smi 不可用：${smi.stderr.trim() || "未安装驱动"}`)

  // 最近启动记录
  const launch = latestLaunches(home, 1)[0]
  if (launch) {
    lines.push("", "── 最近启动 ──")
    lines.push(`${launch.started_at ?? "?"}  档位 ${launch.profile ?? "?"}  端口 ${launch.port ?? "?"}`)
    if (launch.exe) lines.push(`可执行文件: ${launch.exe}`)
    if (launch.argv) lines.push(`argv: ${launch.argv.join(" ")}`)
  }
  return lines.join("\n")
}

const status: Tool = {
  name: "status",
  // 安全模式下不提供：会执行系统命令（tasklist/netstat/nvidia-smi）并请求本地端点
  safeMode: false,
  description:
    "查看本地推理引擎运行状态：服务实例列表（状态文件里的 PID，逐个校验存活）、进程、端口监听与占用者、/health 与 /props 探测（n_ctx、slot 数=并发上限）、GPU 显存与利用率、当前档位与模型、最近一次启动记录。只读，不改变任何状态。",
  parameters: schema({
    port: { type: "number", description: "服务端口（缺省 LOCAL_INFER_PORT 或 8080）" },
  }),
  async execute(args, ctx) {
    return { output: await doStatus(args, ctx) }
  },
}

// ── models ────────────────────────────────────────────────────────────────

const models: Tool = {
  name: "models",
  safeMode: true, // 仅读文件（模型目录与档位定义），安全模式可用
  description: "列出本地推理可用的模型文件（GGUF，含大小与是否下载完成）与全部运行档位（含推荐档位与参数）。只读。",
  parameters: schema({}),
  async execute(_args, ctx) {
    const g = homeGuard(ctx)
    if (g.error) return { output: g.error }
    const home = g.home
    const dir = modelsDir(home, ctx.env)
    const lines: string[] = [`模型目录: ${dir}`, ""]

    const list = listModels(dir)
    if (!list.length) {
      lines.push("（无 GGUF 文件）")
    } else {
      for (const m of list) {
        lines.push(`  ${m.name}  ${m.gb.toFixed(2)} GB${m.incomplete ? "  [未下载完成]" : ""}`)
      }
    }

    const prof = loadProfiles(home)
    lines.push("", "── 运行档位 ──")
    if (!prof) {
      lines.push("档位定义缺失或损坏：config/profiles.json")
    } else {
      lines.push(`默认档位: ${prof.default_profile}`)
      for (const [k, v] of Object.entries(prof.profiles)) {
        lines.push(
          `  ${k}${k === prof.default_profile ? "（默认）" : ""}: 模型=${v.model ?? prof.default_model ?? "-"} ctx=${v.ctx ?? "-"} n_cpu_moe=${v.n_cpu_moe ?? "-"} parallel=${v.parallel ?? "-"}`,
        )
      }
    }
    return { output: lines.join("\n") }
  },
}

// ── start ─────────────────────────────────────────────────────────────────

/** 启动方式：launcher=TS 直接 spawn llama-server（缺省，跨平台、不依赖 PowerShell）；script=run-server.ps1（Windows 兼容回退）。 */
export type LaunchMode = "launcher" | "script"

/** profiles.json 里档位的全部字段（paths.InferProfile 只声明了服务必需的子集，这里补齐档位实际用到的高级项）。 */
interface LaunchProfile extends InferProfile {
  threads?: number
  flash_attn?: string
  cache_type_k?: string
  cache_type_v?: string
  batch?: number
  ubatch?: number
  extra_args?: string[]
  /** 该档位优选引擎 id（engines.json 的引擎标识；缺省用已安装引擎或 engine_dir）。 */
  engine?: string
}

/** 状态文件里的扩展字段（paths.ServerState 保持精简，扩展字段用交叉类型断言读写）。 */
type ServerStateX = ServerState & { launch_mode?: LaunchMode }

/** llama-server 的模型别名缺省值（OpenAI 端点 /v1/models 里显示的名字）。
 *  仅当档位未指定 `alias` 且无法从模型名推导时使用——别名是客户端选模型的键（如 GEBAI_LLM_ROUTES 的 model 字段），
 *  固定写死会让小模型也报成 35B 的名字，误导使用者。 */
const SERVER_ALIAS = "agentworld-35b-a3b"

/** 别名解析：档位 `alias` > 模型文件名去扩展名 > 缺省别名。 */
export function resolveAlias(profileAlias: unknown, model: string): string {
  if (typeof profileAlias === "string" && profileAlias.trim()) return profileAlias.trim()
  const base = model.split(/[\\/]/).pop() ?? model
  const stripped = base.replace(/\.gguf$/i, "").replace(/-UD-.*$/i, "").replace(/-(Q\d[^.]*|IQ\d[^.]*)$/i, "")
  return stripped && stripped.length >= 3 ? stripped.toLowerCase() : SERVER_ALIAS
}

/** 就绪等待预算（大模型首次加载 10-20s，冷盘/大上下文可能更久）。 */
const READY_TIMEOUT_MS = 180_000

/** 输出尾部固定提示（推理型模型的正文与思维链）。 */
const INFER_TIP = "提示：推理型模型的输出正文在 content、思维链在 reasoning_content，两者都要看。"

/** 启动方式解析：显式 mode 参数 > 环境变量 LOCAL_INFER_LAUNCH > 缺省 launcher。
 *  `forced` 表示用户明确要求（此时不做 Windows 的自动回退，见 doStart）。 */
export function resolveLaunch(o: { mode?: unknown; env: Record<string, string> }): { mode: LaunchMode; forced: boolean; reason: string } {
  const fromArg = typeof o.mode === "string" ? o.mode.trim().toLowerCase() : ""
  if (fromArg === "script" || fromArg === "launcher") return { mode: fromArg, forced: true, reason: `mode 参数：${fromArg}` }
  const fromEnv = (o.env.LOCAL_INFER_LAUNCH ?? "").trim().toLowerCase()
  if (fromEnv === "script" || fromEnv === "launcher") return { mode: fromEnv, forced: true, reason: `环境变量 LOCAL_INFER_LAUNCH=${fromEnv}` }
  return { mode: "launcher", forced: false, reason: "缺省 launcher（跨平台，不需要 PowerShell）" }
}

/** 时间戳（yyyyMMdd-HHmmss，与 run-server.ps1 的日志/记录命名一致）。 */
function stampNow(d = new Date()): string {
  const p = (n: number): string => String(n).padStart(2, "0")
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`
}

/** 写启动记录（bench/reports/launch-<stamp>.json，与 run-server.ps1 同构：
 *  status 的「最近启动」与事后审计/回放都读它）。 */
function writeLaunchRecord(home: string, rec: LaunchRecord): string | undefined {
  try {
    const dir = reportsDir(home)
    mkdirSync(dir, { recursive: true })
    const p = join(dir, `launch-${stampNow()}.json`)
    writeFileSync(p, `${JSON.stringify(rec, null, 2)}\n`, "utf-8")
    return p
  } catch {
    return undefined // 记录失败不该影响服务启动
  }
}

/** 状态文件里的引擎标识：launcher 模式写引擎 id；旧脚本（以及本文件补写状态文件的分支）会写成 exe 全路径——
 *  末级是 llama-server[.exe] 时取上级目录名（= 引擎目录 / engine id，等价于 run-server.ps1 的
 *  `Split-Path -Leaf (Split-Path -Parent $exe)`）。 */
export function engineIdFromState(s?: ServerStateX): string | undefined {
  const raw = s?.engine?.trim()
  if (!raw) return undefined
  const parts = raw.split(/[\\/]/).filter(Boolean)
  const last = parts[parts.length - 1]
  if (last && parts.length >= 2 && /^llama-server(\.exe)?$/i.test(last)) return parts[parts.length - 2]
  return last
}

/** 从已安装布局反推引擎 id：exe 相对 `vendor/` 的一级目录名就是引擎 id（矩阵 id = 安装目录名）。
 *  例：`vendor/linux-cpu-x64/llama-b11175/llama-server` → `linux-cpu-x64`。不在 vendor/ 下则返回 undefined。 */
export function inferEngineIdFromExe(home: string, exe: string): string | undefined {
  const vendor = vendorDir(home)
  const norm = (p: string) => p.replace(/\\/g, "/").replace(/\/+$/, "")
  const v = norm(vendor)
  const e = norm(exe)
  if (!e.startsWith(`${v}/`)) return undefined
  const seg = e.slice(v.length + 1).split("/").filter(Boolean)
  // 一级目录即引擎 id（.cache 等隐藏目录不算）
  return seg[0] && !seg[0].startsWith(".") ? seg[0] : undefined
}

/** 路径末级名字（跨平台：Windows 反斜杠与 POSIX 斜杠都认）。 */
function baseName(p: string): string {
  const parts = p.split(/[\\/]/).filter(Boolean)
  return parts[parts.length - 1] ?? p
}

/** 档位里的附加参数拆解：-bs / --cont-batching 由 buildServerArgs 的专用开关产出（避免重复），其余原样透传。 */
function splitExtraArgs(raw: string[] | undefined): { extraArgs: string[]; contBatching: boolean; backendSampling: boolean } {
  const list = Array.isArray(raw) ? raw : []
  return {
    extraArgs: list.filter((a) => a !== "--cont-batching" && a !== "-bs" && a !== "--backend-sampling"),
    contBatching: list.includes("--cont-batching"),
    backendSampling: list.includes("-bs") || list.includes("--backend-sampling"),
  }
}

/** script 模式：调用 run-server.ps1（Windows 兼容回退；行为与引入 launcher 之前一致）。 */
async function startViaScript(
  home: string,
  profile: string,
  port: number,
  model: string,
  args: Record<string, unknown>,
  ctx: ToolContext,
  platform: NodeJS.Platform,
  note?: string,
): Promise<string> {
  const argv: string[] = [`-Profile "${profile}"`, `-Port ${port}`, "-Background", "-NoWait"]
  if (args.model != null) argv.push(`-Model "${args.model}"`)
  if (args.n_cpu_moe != null) argv.push(`-NCpuMoe ${Number(args.n_cpu_moe)}`)

  // 启动脚本完全脱离进程树并立即返回（否则工具会挂在长驻服务进程的句柄上）；就绪在这里轮询。
  const r = await ctx.runCommand(scriptRun(home, "run-server.ps1", argv, platform), { timeoutMs: 60000, workdir: home })
  const out = (r.stdout + (r.stderr ? `\n${r.stderr}` : "")).trim()
  const head = note ? `${note}\n` : ""
  if (r.code !== 0) return `${head}启动失败（exit ${r.code}）：\n${out}`

  const wr = await waitReady(baseUrl(port), { timeoutMs: READY_TIMEOUT_MS, intervalMs: 2000 })
  const notes: string[] = []
  if (wr.ok) {
    const st = await ensureServerState(ctx, home, port, platform, { profile, model })
    if (st) notes.push(st)
  }
  const tail = wr.ok
    ? `服务就绪 ${baseUrl(port)}（加载 ${wr.secs}s，OpenAI 兼容端点 /v1/chat/completions）`
    : `等待 ${wr.secs}s 仍未就绪（${wr.error ?? "未知原因"}）——大模型首次加载可能更久，用 local_infer_status 复查进程与日志（local_infer_logs 看日志尾部）`
  return [`${head}启动方式: script（scripts/run-server.ps1）`, out, "", tail, ...notes, "", INFER_TIP].join("\n")
}

/** start 执行体（restart 复用）：launcher 模式直接 spawn llama-server；script 模式回退 run-server.ps1。 */
export async function doStart(args: Record<string, unknown>, ctx: ToolContext, platform: NodeJS.Platform = process.platform): Promise<string> {
  const g = homeGuard(ctx)
  if (g.error) return g.error
  const home = g.home
  const plan = resolveLaunch({ mode: args.mode, env: ctx.env })
  const prof = loadProfiles(home)
  if (!prof) return `档位定义缺失或损坏：${profilesPath(home)}`

  const profile = args.profile != null ? String(args.profile) : activeProfile(prof, ctx.env)
  const p = prof.profiles[profile] as LaunchProfile | undefined
  if (!p) return `未知档位: ${profile}（可用: ${Object.keys(prof.profiles).join(", ")}）`
  const port = Number(args.port ?? defaultPort(ctx.env))
  const model = String(args.model ?? profileModel(prof, profile))

  if (plan.mode === "script") {
    return await startViaScript(home, profile, port, model, args, ctx, platform)
  }

  // ── launcher 模式：找引擎 → 组装参数 → 脱离进程树启动 → 写状态文件 → 轮询就绪 ──
  const engineId = args.engine != null && String(args.engine).trim() ? String(args.engine).trim() : p.engine
  const resolved = resolveEngineExe({ home, engineId, engineDir: prof.engine_dir, platform })
  if (!resolved.exe) {
    // Windows 上 run-server.ps1 是既有兼容路径：没装引擎但仍想用旧脚本时自动回退（显式 mode=launcher 则不回退）
    if (platform === "win32" && !plan.forced) {
      return await startViaScript(home, profile, port, model, args, ctx, platform, `launcher 模式无可用引擎（${resolved.note ?? ""}），回退 run-server.ps1 兼容路径。`)
    }
    return [
      "没有可用的 llama-server 可执行文件，未启动。",
      `引擎解析: ${resolved.note ?? "未知"}`,
      "",
      "下一步：",
      "  1) local_infer_engines —— 查看本机平台可下载/可用的引擎（矩阵按 linux/win32/darwin × cpu/cuda/vulkan/metal 列全）；",
      "  2) local_infer_engine_fetch —— 安装选定引擎（下载 + 校验 + 解压到 vendor/<engine-id>/）；",
      "  3) 再 local_infer_start（仍失败就看 local_infer_logs 的尾部与「疑似错误」）。",
      "（临时可用 mode=\"script\" 走 Windows 的 run-server.ps1 兼容路径，但它需要 Windows + PowerShell + 预解压的 engine_dir。）",
    ].join("\n")
  }

  const modelPath = isAbsolutePath(model) ? model : join(modelsDir(home, ctx.env), model)
  if (!existsSync(modelPath)) {
    return `找不到模型文件：${modelPath}（模型目录 ${modelsDir(home, ctx.env)}；缺模型时先补齐权重或用 model 参数指定绝对路径）`
  }

  // 端口预检：已在监听时直接说清楚（否则 spawn 出来的引擎会在日志里报 address already in use，白等一轮）
  const pre = await ctx.runCommand(portOwnerCmd(port, platform), { timeoutMs: 20000 })
  if (isListening(pre.stdout)) {
    const owner = parsePortOwner(pre.stdout, platform)
    return `端口 :${port} 已被占用${owner ? `（PID ${owner}）` : ""}——先 local_infer_stop（或 stop 指定 port），或换一个 port 参数。`
  }

  const split = splitExtraArgs(p.extra_args)
  const argv = buildServerArgs({
    modelPath,
    port,
    ctx: p.ctx,
    ngl: p.n_gpu_layers,
    ncmoe: args.n_cpu_moe != null ? Number(args.n_cpu_moe) : (p.n_cpu_moe ?? null),
    parallel: p.parallel,
    threads: p.threads ?? defaultThreads(),
    batch: p.batch,
    ubatch: p.ubatch,
    flashAttn: p.flash_attn,
    cacheTypeK: p.cache_type_k,
    cacheTypeV: p.cache_type_v,
    alias: resolveAlias((p as { alias?: unknown }).alias, model),
    extraArgs: split.extraArgs,
    contBatching: split.contBatching,
    backendSampling: split.backendSampling,
  })

  const logPath = join(reportsDir(home), `server-${stampNow()}.log`)
  let res: LaunchResult
  try {
    res = await spawnServerAsync({ exe: resolved.exe, args: argv, logPath, cwd: home, env: launcherEnv(platform) })
  } catch (e) {
    return `启动失败：${(e as Error).message}\n\n排查：上面的日志尾部就是引擎自己的报错（参数不被接受/显存不足/端口冲突等）；也可用 local_infer_logs 复查与状态确认。`
  }

  // 引擎标识：显式/档位指定的优先；否则从**安装布局**反推——vendor/<engine-id>/ 下的一级目录名才是引擎 id
  //（引擎归档内部还会套一层发行版目录，如 vendor/linux-cpu-x64/llama-b11175/llama-server，
  // 取 exe 的直接父目录名会得到归档内目录名 llama-b11175，与矩阵 id 不符、导致 status 报「未安装」）
  const engine = engineId ?? inferEngineIdFromExe(home, resolved.exe) ?? baseName(dirname(resolved.exe))
  const startedAt = new Date().toISOString()
  writeLaunchRecord(home, {
    started_at: startedAt,
    profile,
    model: baseName(model),
    n_cpu_moe: args.n_cpu_moe != null ? Number(args.n_cpu_moe) : (p.n_cpu_moe ?? undefined),
    spec_type: p.spec_type ?? null,
    threads: p.threads ?? defaultThreads(),
    port,
    exe: resolved.exe,
    engine,
    launch_mode: "launcher",
    argv: res.argv,
    log: logPath,
  })
  const state: ServerStateX = {
    pid: res.pid,
    port,
    profile,
    model: baseName(modelPath),
    host: "127.0.0.1",
    exe: resolved.exe,
    log: logPath,
    started_at: startedAt,
    argv: res.argv,
    engine,
    launch_mode: "launcher",
  }
  writeServerState(home, state)

  const wr = await waitReady(baseUrl(port), {
    timeoutMs: READY_TIMEOUT_MS,
    intervalMs: 2000,
    isAlive: () => launcherPidAlive(res.pid, ctx, platform),
  })
  const head = [
    `启动方式: launcher（TS 直接 spawn，脱离当前进程树；${plan.reason}）`,
    `PID ${res.pid}  引擎 ${engine ?? "?"}（${resolved.via === "engine" ? "已安装引擎" : "目录约定/engine_dir"}）`,
    ...(resolved.note ? [`引擎解析: ${resolved.note}`] : []),
    `日志 ${logPath}`,
    `命令: ${res.argv.join(" ")}`,
  ]
  const tail = wr.ok
    ? `服务就绪 ${baseUrl(port)}（加载 ${wr.secs}s，OpenAI 兼容端点 /v1/chat/completions）`
    : `服务进程已启动但 ${wr.secs}s 内未就绪（${wr.error ?? "未知原因"}）——大模型首次加载可能更久，用 local_infer_status 复查进程与 local_infer_logs 看日志尾部`
  return [...head, "", tail, "", INFER_TIP].join("\n")
}

const start: Tool = {
  name: "start",
  safeMode: false, // 安全模式下不提供：会拉起 GPU 服务进程
  description:
    "按档位启动本地推理服务（OpenAI 兼容端点，默认 127.0.0.1:8080）。会占用整张 GPU（模型约 13GB 显存，加载约 10-20 秒），启动前请确认没有其他 GPU 任务。启动成功后就绪并写入服务状态文件（run/server-<port>.json），供 status/stop/logs 精确管理。" +
    "启动方式：缺省 mode=launcher——TS 直接 spawn llama-server 并脱离当前进程树（跨平台，Linux/macOS/Windows 均不需 PowerShell，日志重定向到 bench/reports/server-<时间戳>.log）；mode=script 走 scripts/run-server.ps1（仅 Windows 的兼容回退，环境变量 LOCAL_INFER_LAUNCH=script 可全局强制）。" +
    "launcher 模式下找不到引擎时不直接报错：会告诉你用 local_infer_engines 看可下载项、local_infer_engine_fetch 安装。需审批。",
  requiresApproval: true,
  parameters: schema({
    profile: { type: "string", description: "运行档位名（缺省用配置的默认档，推荐 fast）" },
    model: { type: "string", description: "模型文件名（缺省用档位指定的模型；也可直接给绝对路径）" },
    port: { type: "number", description: "服务端口（缺省 LOCAL_INFER_PORT 或 8080）" },
    n_cpu_moe: { type: "number", description: "留在 CPU 的专家层数（0=全部专家进显存，性能最优；缺省用档位值）" },
    engine: { type: "string", description: "引擎 id（engines.json 的引擎标识，如 linux-cpu-x64；缺省用档位的 engine 字段，再缺省用 vendor/ 下唯一已安装引擎）" },
    mode: { type: "string", description: "启动方式：launcher（缺省，TS 直接 spawn，跨平台、不需 PowerShell）/ script（调用 scripts/run-server.ps1，仅 Windows 兼容回退）" },
  }, []),
  async execute(args, ctx) {
    return { output: await doStart(args, ctx) }
  },
}
// ── stop ──────────────────────────────────────────────────────────────────

/** 停掉若干实例并清理状态文件（按 PID 精确终止，不留孤儿进程）。 */
async function stopPidSet(
  ctx: ToolContext,
  home: string,
  targets: ServerState[],
  platform: NodeJS.Platform,
): Promise<string[]> {
  const out: string[] = []
  for (const s of targets) {
    const r = await launcherKillPid(s.pid, ctx, platform)
    const cleaned = clearServerState(home, s.port)
    const verdict = r.ok ? "已停止" : `停止命令未成功（可能已自行退出）：${r.output}`
    out.push(`:${s.port}  PID ${s.pid}  ${verdict}${cleaned ? "（状态文件已清理）" : ""}`)
  }
  return out
}

/** stop 执行体（restart 复用）。 */
export async function doStop(args: Record<string, unknown>, ctx: ToolContext, platform: NodeJS.Platform = process.platform): Promise<string> {
  const g = homeGuard(ctx)
  if (g.error) return g.error
  const home = g.home
  const states = readServerStates(home)
  const lines: string[] = []

  // ① 显式 PID
  if (args.pid != null) {
    const pid = Number(args.pid)
    const r = await launcherKillPid(pid, ctx, platform)
    const matched = states.filter((s) => s.pid === pid)
    for (const s of matched) clearServerState(home, s.port)
    lines.push(`PID ${pid}：${r.ok ? "已停止" : `停止命令未成功（可能已不存在）：${r.output}`}`)
    if (matched.length) lines.push(`清理状态文件：${matched.map((s) => `:${s.port}`).join(", ")}`)
    else lines.push("（无匹配的实例状态文件——该 PID 可能不是本工具启动的服务）")
    return await withGpuTail(ctx, lines, true)
  }

  // ② 按端口（优先状态文件里的精确 PID，退化到端口占用者）
  if (args.port != null) {
    const port = Number(args.port)
    const st = states.find((s) => s.port === port)
    if (st && st.pid > 0) {
      return await withGpuTail(ctx, await stopPidSet(ctx, home, [st], platform), true)
    }
    const owner = await discoverPidByPort(ctx, port, platform)
    if (!owner) {
      if (st) {
        clearServerState(home, st.port)
        lines.push(`:${port} 端口未监听（状态文件陈旧，已清理）`)
      } else {
        lines.push(`:${port} 未被占用，无需停止。`)
      }
      return lines.join("\n")
    }
    const r = await launcherKillPid(owner, ctx, platform)
    clearServerState(home, port)
    lines.push(`:${port} 占用者 PID ${owner}：${r.ok ? "已停止" : `停止命令未成功：${r.output}`}（状态文件已清理）`)
    return await withGpuTail(ctx, lines, true)
  }

  // ③ 按档位匹配实例
  if (args.profile != null) {
    const profile = String(args.profile)
    const matched = states.filter((s) => s.profile === profile)
    if (matched.length) {
      return await withGpuTail(ctx, await stopPidSet(ctx, home, matched, platform), true)
    }
    if (states.length) {
      return `档位 ${profile} 没有运行中的实例。当前实例：\n${states.map((s) => `  :${s.port} PID ${s.pid} 档位 ${s.profile}`).join("\n")}`
    }
    lines.push(`无实例状态文件（服务可能由旧版脚本启动）——退化为停止全部 llama-server。`)
  }

  // ④ 全部（缺省 / all=true）
  const r = await ctx.runCommand(killAllCmd(platform), { timeoutMs: 30000 })
  const cleared = states.map((s) => (clearServerState(home, s.port) ? `:${s.port}` : undefined)).filter(Boolean)
  const out = (r.stdout || r.stderr || "").trim()
  lines.push(`停止全部 llama-server：${r.code === 0 ? out || "已执行" : `exit ${r.code}：${out || "未发现运行中的进程"}`}`)
  if (cleared.length) lines.push(`清理状态文件：${cleared.join(", ")}`)
  return await withGpuTail(ctx, lines, true)
}

/** 停止后附一行显存占用（确认释放；查不到 GPU 就静默跳过）。 */
async function withGpuTail(ctx: ToolContext, lines: string[], show: boolean): Promise<string> {
  if (show) {
    const smi = await ctx.runCommand(`nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader`, { timeoutMs: 20000 })
    if (smi.code === 0 && smi.stdout.trim()) lines.push(`显存: ${smi.stdout.trim()}`)
  }
  lines.push("提示：显存释放有数秒延迟（进程退出后驱动回收），随即用 local_infer_status 复查。")
  return lines.join("\n")
}

const stop: Tool = {
  name: "stop",
  safeMode: false, // 安全模式下不提供：会终止进程
  description:
    "停止本地推理服务并释放显存。可按 PID、按端口、按档位（profile）精确停止（用服务状态文件里的 PID 终止进程树并清理状态文件），或缺省停止全部 llama-server。需审批。",
  requiresApproval: true,
  parameters: schema({
    pid: { type: "number", description: "直接停止指定 PID" },
    port: { type: "number", description: "停止该端口上的服务实例（优先用状态文件的 PID，退化到端口占用者）" },
    profile: { type: "string", description: "停止该档位对应的全部实例（按状态文件匹配）" },
    all: { type: "boolean", description: "停止全部 llama-server 并清理所有状态文件（与不传参数同义）" },
  }, []),
  async execute(args, ctx) {
    return { output: await doStop(args, ctx) }
  },
}

// ── restart ───────────────────────────────────────────────────────────────

/** restart 执行体：先按端口 stop（无实例则跳过），再按参数 start。 */
export async function doRestart(args: Record<string, unknown>, ctx: ToolContext, platform: NodeJS.Platform = process.platform): Promise<string> {
  const g = homeGuard(ctx)
  if (g.error) return g.error
  const home = g.home
  const port = Number(args.port ?? defaultPort(ctx.env))

  const stopOut = await doStop({ port }, ctx, platform)
  const startOut = await doStart(args, ctx, platform)
  const prof = loadProfiles(home)
  const profile = args.profile != null ? String(args.profile) : activeProfile(prof, ctx.env)
  return [
    `重启 :${port} → 档位 ${profile}（llama-server 不支持热切换，换档位/换模型必须重启）`,
    "",
    "── 1/2 停止 ──",
    stopOut,
    "",
    "── 2/2 启动 ──",
    startOut,
  ].join("\n")
}

const restart: Tool = {
  name: "restart",
  safeMode: false, // 安全模式下不提供：会终止并重启服务进程
  description:
    "重启本地推理服务并切换档位/模型/专家卸载层数（llama-server 不支持热切换，换档位或换模型必须重启）。语义：先停止该端口上的实例，再按给定参数启动并等待就绪（缺省 launcher 模式：TS 直接 spawn，跨平台；mode=script 走 run-server.ps1）。会占用整张 GPU。需审批。",
  requiresApproval: true,
  parameters: schema({
    profile: { type: "string", description: "目标运行档位名（缺省用配置的默认档）" },
    model: { type: "string", description: "目标模型文件名（缺省用档位指定的模型）" },
    port: { type: "number", description: "服务端口（缺省 LOCAL_INFER_PORT 或 8080）" },
    n_cpu_moe: { type: "number", description: "留在 CPU 的专家层数（0=全部专家进显存）" },
    engine: { type: "string", description: "引擎 id（缺省用档位的 engine 字段或唯一已安装引擎）" },
    mode: { type: "string", description: "启动方式：launcher（缺省）/ script（Windows 的 run-server.ps1 兼容路径）" },
  }, []),
  async execute(args, ctx) {
    return { output: await doRestart(args, ctx) }
  },
}

// ── logs ──────────────────────────────────────────────────────────────────

function fmtSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 ** 2) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 ** 2).toFixed(2)} MB`
}

/** logs 执行体。 */
export async function doLogs(args: Record<string, unknown>, ctx: ToolContext): Promise<string> {
  const g = homeGuard(ctx)
  if (g.error) return g.error
  const home = g.home
  const kind = String(args.kind ?? "both") as "out" | "err" | "both"
  const linesN = Math.max(1, Math.min(5000, Number(args.lines ?? 80)))
  const grep = args.grep != null ? String(args.grep) : undefined
  const all = listServerLogs(home, 50)

  if (!all.length) return `无服务日志（${join(home, "bench", "reports")} 下没有 server-*.log）——服务尚未启动过，或日志被清理。`

  const lines: string[] = [`日志目录: ${join(home, "bench", "reports")}`, ""]
  if (args.list_only) {
    lines.push("── 日志文件（新 → 旧） ──")
    for (const f of all.slice(0, 20)) {
      lines.push(`  ${f.name}  [${f.kind}]  ${fmtSize(f.size)}  ${new Date(f.mtime).toISOString()}`)
    }
    return lines.join("\n")
  }

  // 端口定位：优先状态文件里记录的日志；否则取最新日志（见下）
  const st = args.port != null ? readServerStates(home).find((s) => s.port === Number(args.port)) : undefined
  const picked: LogFile[] = []
  const byPath = (p?: string): LogFile | undefined => (p && existsSync(p) ? all.find((f) => f.path === p) ?? { path: p, name: p.split(/[\\/]/).pop() ?? p, kind: p.endsWith(".err") ? "err" : "out", size: 0, mtime: 0 } : undefined)
  if (st) {
    const o = kind !== "err" ? byPath(st.log) : undefined
    const e = kind !== "out" ? byPath(st.err_log) ?? all.find((f) => f.kind === "err") : undefined
    if (o) picked.push(o)
    if (e && e.path !== o?.path) picked.push(e)
    if (picked.length) lines.push(`实例 :${st.port}（PID ${st.pid}，档位 ${st.profile}）`)
  }
  if (!picked.length) {
    // 未指定实例：主日志取最新的 **stdout**（.log.err 只是重定向的副产物，同毫秒写入时
    // 按 mtime 排序会把它当成「最新日志」，导致读到的全是空 stderr 而误导排查）
    const stdoutLogs = all.filter((f) => f.kind === "out")
    const primary = kind === "err" ? all.find((f) => f.kind === "err") : (stdoutLogs[0] ?? all[0])
    if (!primary) return `没有 kind=${kind} 的日志文件（可用 out/err/both）。`
    picked.push(primary)
    if (kind === "both") {
      const e = all.find((f) => f.kind === "err")
      if (e && e.path !== primary.path) picked.push(e)
    }
    lines.push(`（未指定实例，取最新日志）`)
  }
  lines.push("")

  for (const f of picked) {
    lines.push(`── ${f.name}  [${f.kind}]  ${fmtSize(f.size)}  ${f.mtime ? new Date(f.mtime).toISOString() : "?"} ──`)
    lines.push(`路径: ${f.path}`)
    const tail = tailLines(f.path, linesN)
    let body = tail.text
    if (grep) {
      const needle = grep.toLowerCase()
      const hits = body.split(/\r?\n/).filter((l) => l.toLowerCase().includes(needle))
      lines.push(`grep "${grep}"：尾部窗口内命中 ${hits.length} 行`)
      body = hits.join("\n")
    }
    lines.push(tail.truncated ? `（日志较大，仅取尾部窗口；需要更早内容请用 grep 缩小范围）` : "")
    lines.push(body.trim() ? body : "（无内容）")
    const errs = suspectedErrors(tail.text)
    if (errs.length) {
      lines.push("", "疑似错误：")
      for (const e of errs) lines.push(`  - ${e}`)
    }
    lines.push("")
  }
  lines.push("说明：本工具只读日志尾部快照、不做流式跟随（跟随会一直挂住会话）；服务运行中可反复调用获取新内容。")
  return lines.join("\n").replace(/\n{3,}/g, "\n\n")
}

const logs: Tool = {
  name: "logs",
  safeMode: true, // 仅读日志文件（尾部按块回退），安全模式可用
  description:
    "查看本地推理服务的日志（llama-server 的 stdout/stderr）：尾部若干行、关键字过滤、错误特征摘要、日志文件清单。用 port 可定位具体实例的日志（依据服务状态文件）。只读，不改变服务状态；非流式（一次取快照，跟随会挂住会话）。",
  parameters: schema({
    port: { type: "number", description: "服务端口（定位该实例的日志；缺省取最新日志）" },
    kind: { type: "string", description: "日志类型：out（stdout）/err（stderr）/both（缺省 both）" },
    lines: { type: "number", description: "尾部行数（缺省 80，上限 5000）" },
    grep: { type: "string", description: "大小写不敏感的子串过滤（只在尾部窗口内过滤）" },
    list_only: { type: "boolean", description: "只列出日志文件清单，不读内容" },
  }, []),
  async execute(args, ctx) {
    return { output: await doLogs(args, ctx) }
  },
}

// ── bench / inspect（沿用既有实现） ───────────────────────────────────────

const bench: Tool = {
  name: "bench",
  safeMode: false, // 安全模式下不提供：会执行基准脚本并占用整张 GPU
  description:
    "对本地推理引擎跑基准测试（llama-bench：预填充与解码吞吐），结果落 bench/reports/ 并返回控制台表。耗时较长（每次加载模型 10-20 秒，多上下文多轮更久）。需审批。",
  requiresApproval: true,
  parameters: schema({
    model: { type: "string", description: "模型文件名（缺省用默认档位的模型）" },
    n_cpu_moe: { type: "number", description: "留在 CPU 的专家层数（0=全 GPU）" },
    prompt_tokens: { type: "string", description: "预填充测试的 token 数，逗号分隔（缺省 512,4096）" },
    gen_tokens: { type: "number", description: "解码测试生成 token 数（缺省 128）" },
    reps: { type: "number", description: "每项重复次数（缺省 2，越大越稳）" },
  }, []),
  async execute(args, ctx) {
    const g = homeGuard(ctx)
    if (g.error) return { output: g.error }
    const home = g.home
    const argv: string[] = []
    if (args.model != null) argv.push(`-Model "${args.model}"`)
    if (args.n_cpu_moe != null) argv.push(`-NCpuMoe ${Number(args.n_cpu_moe)}`)
    if (args.prompt_tokens != null) argv.push(`-PromptTokens ${String(args.prompt_tokens)}`)
    if (args.gen_tokens != null) argv.push(`-GenTokens ${Number(args.gen_tokens)}`)
    if (args.reps != null) argv.push(`-Reps ${Number(args.reps)}`)

    const r = await ctx.runCommand(scriptRun(home, "bench.ps1", argv), { timeoutMs: 900000, workdir: home })
    const out = (r.stdout + (r.stderr ? `\n${r.stderr}` : "")).trim()
    return { output: `基准测试（exit ${r.code}）：\n${out}` }
  },
}

const inspect: Tool = {
  name: "inspect",
  safeMode: false, // 安全模式下不提供：会拉起 python 解析进程
  description:
    "解析 GGUF 模型文件的结构：架构、层数/专家数等关键元数据、张量分类统计（专家/注意力/嵌入各占多少）、每层专家字节数（用于显存规划）、是否含 MTP 头。支持未下载完成的文件（只读文件头，无需下完）。只读。",
  parameters: schema({
    model: { type: "string", description: "模型文件名（缺省解析模型目录下第一个 GGUF）" },
    json_out: { type: "string", description: "可选：把完整结构导出为该路径的 JSON（相对子项目根）" },
  }, []),
  async execute(args, ctx) {
    const g = homeGuard(ctx)
    if (g.error) return { output: g.error }
    const home = g.home
    const dir = modelsDir(home, ctx.env)

    let target: string | undefined
    if (args.model != null) {
      const cand = join(dir, String(args.model))
      if (!existsSync(cand)) {
        const avail = listModels(dir).map((m) => m.name)
        return { output: `模型不存在: ${cand}\n可用: ${avail.length ? avail.join(", ") : "（无）"}` }
      }
      target = cand
    } else {
      const first = listModels(dir)[0]
      if (!first) return { output: `模型目录为空：${dir}` }
      target = join(dir, first.name)
    }

    const argv = [`"${target}"`]
    if (args.json_out != null) argv.push(`--json "${join(home, String(args.json_out))}"`)
    const r = await ctx.runCommand(`python "${join(home, "scripts", "inspect-gguf.py")}" ${argv.join(" ")}`, {
      timeoutMs: 300000,
      workdir: home,
      env: { PYTHONIOENCODING: "utf-8" },
    })
    const out = (r.stdout + (r.stderr ? `\n${r.stderr}` : "")).trim()
    return { output: out || `解析无输出（exit ${r.code}）` }
  },
}

export const tools: Record<string, Tool> = { status, models, start, stop, restart, logs, bench, inspect }
export const requiresApproval: Record<string, boolean> = { start: true, stop: true, restart: true, bench: true }
export const preload = false
