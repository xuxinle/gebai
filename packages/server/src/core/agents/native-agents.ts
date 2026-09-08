/**
 * 多语言子代理发现器（core/agents/native-agents.ts）：扫描 manifest（agent.json）→ 启动
 * 边车进程 → init/tools.list 握手 → 构造标准 SubAgentDef 注册进 SubAgentManager。
 *
 * 发现范围（后者同名覆盖前者）：
 *   1. 内置源 native-agents/（仓库根，按实现语言分目录：python/cpp/rust/…——语言目录下共享
 *      基础框架驱动，每个二级目录一个子代理项目；dist 形态产物同构、二进制形态物化
 *      {GEBAI_HOME}/vendor/native-agents/——安装包预置物化；构建时过滤 venv/编译产物）
 *   2. 用户自建 {GEBAI_HOME}/agents/{name}/agent.json（放一个目录即成一个子代理，任意语言）
 *
 * 设计原则：实现语言对模型透明——子代理 = 工具 + 提示词（能力导向命名与描述），语言仅是
 * 工程组织维度；一种语言可派生任意多个子代理项目（manifest 可选 build 声明编译引导）。
 *
 * manifest（agent.json）字段：
 *   name*         子代理名（[a-z0-9_]+，即 agent_list/agent_load 名；目录名不必相同）
 *   description*  一句话能力描述
 *   protocol*     协议版本（当前 1）
 *   command*      启动命令（字符串数组；占位符：{python}=解析出的解释器、{driver}=driver 脚本绝对路径）
 *   driver        command 占位 {driver} 引用的脚本文件名（相对 manifest 目录）
 *   prompt        系统提示词文件名（相对 manifest 目录，缺省 PROMPT.md）
 *   cwd           工作目录（缺省 manifest 目录；占位 {GEBAI_HOME}）
 *   env           附加环境变量（值支持 {GEBAI_HOME} 占位）
 *   build         编译型语言构建引导（command/windows/unix 平台分支）：command 首元素指向的
 *                可执行文件不存在时先执行（占位符同 command，cwd 为 manifest 目录）
 *
 * 生命周期：进程管理全部委托 AgentSidecar（惰性启动/超时杀进程重启/崩溃自愈/退出清理）。
 * 门控（DESIGN「多语言子代理」）：仅本地形态启用——沙箱启用（服务端部署）或 GEBAI_NATIVE_AGENTS=off
 * 时发现器整体禁用（native 子代理完全不可见），不影响 TS 子代理。
 * 失败安全：manifest 损坏/边车启动失败/握手失败 → 该项跳过并记入 loadErrors（模型经
 * unknownAgentError 可见根因），绝不阻断启动与其他子代理。
 */
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs"
import { basename, dirname, isAbsolute, join } from "node:path"
import { resolveGebaiHome } from "../base/config"
import type { SubAgentDef, Tool } from "../base/types"
import { AgentSidecar, defaultSpawn, nativeAgentsSourceDir, type SidecarSpawnFn } from "./sidecar"

/** 解释器解析（{python} 占位，其他语言 manifest 直接写可执行体路径/命令名）：
 *  GEBAI_PYTHON_DIR（解释器目录或可执行体完整路径；目录时补拼 python.exe/python）
 *  → 仓库根 native-agents/python/venv（语言目录 venv，源码形态）→ PATH（python3/python/py，
 *  Bun.which）。 */
export function resolvePythonCommand(env: Record<string, string> = process.env as Record<string, string>): string[] | null {
  const explicit = String(env.GEBAI_PYTHON_DIR ?? "").trim()
  const absolute: string[] = []
  if (explicit) {
    // 目录形态补拼解释器名（Windows python.exe / 其余 python）；直接指向可执行体则原样
    if (/\.(exe|cmd|bat)$/i.test(explicit) || !existsSync(explicit) || statSync(explicit).isFile()) absolute.push(explicit)
    else absolute.push(join(explicit, process.platform === "win32" ? "python.exe" : "python"))
  }
  for (const vdir of [sourceTreeVenv()]) {
    if (!vdir) continue
    absolute.push(process.platform === "win32" ? join(vdir, "Scripts", "python.exe") : join(vdir, "bin", "python"))
  }
  for (const c of absolute) {
    if (c && isAbsolute(c) && existsSync(c)) return [c]
  }
  for (const name of process.platform === "win32" ? ["python", "py"] : ["python3", "python"]) {
    const found = Bun.which(name)
    if (found) return [found]
  }
  return null
}

/** 仓库根语言目录下的 venv（源码形态）：packages/server/src/core/agents → 仓库根
 *  native-agents/python/venv；目录不存在返回 null（dist/二进制形态不用源码树 venv）。 */
function sourceTreeVenv(): string | null {
  const dir = join(import.meta.dirname, "..", "..", "..", "..", "..", "native-agents", "python", "venv")
  return existsSync(dir) ? dir : null
}

export interface NativeAgentManifest {
  name: string
  description: string
  protocol: number
  command: string[]
  driver?: string
  prompt?: string
  cwd?: string
  env?: Record<string, string>
  /** 编译型语言构建引导：command 首元素指向的可执行文件不存在时先执行（占位符同 command），
   *  产物落盘后正常启动——缺编译器的环境记 loadErrors 不阻断其他子代理。 */
  build?: NativeAgentBuild
}

export interface NativeAgentBuild {
  /** 构建命令（优先于平台分支；缺省取 windows/unix 对应平台项）。 */
  command?: string[]
  windows?: string[]
  unix?: string[]
}

/** 解析并校验 manifest（缺必填字段/非法名/坏 JSON 返回错误文本）。 */
export function parseManifest(raw: string, source: string): { manifest?: NativeAgentManifest; error?: string } {
  let json: Record<string, unknown>
  try {
    json = JSON.parse(raw) as Record<string, unknown>
  } catch (err) {
    return { error: `${source}: agent.json 非法 JSON: ${(err as Error).message}` }
  }
  const name = String(json.name ?? "").trim()
  const description = String(json.description ?? "").trim()
  const protocol = Number(json.protocol ?? 0)
  const command = Array.isArray(json.command) ? json.command.map((c) => String(c)) : null
  if (!/^[a-z0-9_]+$/.test(name)) return { error: `${source}: name 非法（须 [a-z0-9_]+）: ${name}` }
  if (!description) return { error: `${source}: 缺 description` }
  if (protocol !== 1) return { error: `${source}: 不支持的协议版本 ${protocol}（当前支持 1）` }
  if (!command || !command.length || command.some((c) => !c.trim())) return { error: `${source}: command 须为非空字符串数组` }
  return {
    manifest: {
      name,
      description,
      protocol,
      command,
      driver: typeof json.driver === "string" ? json.driver : undefined,
      prompt: typeof json.prompt === "string" ? json.prompt : undefined,
      cwd: typeof json.cwd === "string" ? json.cwd : undefined,
      env: typeof json.env === "object" && json.env !== null ? Object.fromEntries(Object.entries(json.env as Record<string, string>).map(([k, v]) => [k, String(v)])) : undefined,
      build: typeof json.build === "object" && json.build !== null ? parseBuild(json.build as Record<string, unknown>) : undefined,
    },
  }
}

/** build 字段解析：command/windows/unix 均须为非空字符串数组（非法项静默忽略）。 */
function parseBuild(b: Record<string, unknown>): NativeAgentBuild {
  const arr = (v: unknown): string[] | undefined => {
    if (!Array.isArray(v) || !v.length || v.some((c) => typeof c !== "string" || !String(c).trim())) return undefined
    return v.map((c) => String(c))
  }
  const build: NativeAgentBuild = {}
  const command = arr(b.command)
  const windows = arr(b.windows)
  const unix = arr(b.unix)
  if (command) build.command = command
  if (windows) build.windows = windows
  if (unix) build.unix = unix
  return build
}

/** 占位符解析：{python}（解释器命令，数组展开）/{driver}（脚本绝对路径）/{agent_dir}
 *  （manifest 目录）/{lang_dir}（语言目录，即 agent_dir 上一级）/{agent_name}（manifest
 *  name）/{exe}（Windows ".exe"，其余平台空串——跨平台可执行体引用）/{GEBAI_HOME}。 */
export function expandCommand(
  command: string[],
  dir: string,
  manifest: { driver?: string; name?: string },
  pythonCmd: string[] | null,
): { command: string[]; error?: string } {
  const home = resolveGebaiHome()
  const exeSuffix = process.platform === "win32" ? ".exe" : ""
  const out: string[] = []
  for (const part of command) {
    if (part.includes("{driver}")) {
      const driverFile = manifest.driver ? join(dir, manifest.driver) : ""
      if (!driverFile || !existsSync(driverFile)) {
        return { command: [], error: `command 引用 {driver} 但 manifest 未声明可用的 driver 文件（${manifest.driver ?? "未声明"}）` }
      }
      out.push(part.replace(/\{driver\}/g, driverFile))
    } else if (part === "{python}") {
      if (!pythonCmd) return { command: [], error: "command 引用 {python} 但解释器不可解析（GEBAI_PYTHON_DIR / 语言目录 venv / 系统 PATH 均无 python）" }
      out.push(...pythonCmd)
    } else {
      // 路径类占位（agent_dir/lang_dir）替换后统一为平台分隔符（字符串 replace 保留原文风格，
      // 测试断言与跨平台可执行体引用都需归一化）
      out.push(
        normalizePath(
          part
            .replace(/\{GEBAI_HOME\}/g, home)
            .replace(/\{agent_dir\}/g, dir)
            .replace(/\{agent_name\}/g, manifest.name ?? basename(dir))
            .replace(/\{lang_dir\}/g, dirname(dir))
            .replace(/\{exe\}/g, exeSuffix),
        ),
      )
    }
  }
  return { command: out }
}

/** 平台分隔符归一（占位替换后的路径风格统一；非路径 token 不受影响——纯单词/选项不含 / 与 \）。 */
function normalizePath(p: string): string {
  return process.platform === "win32" ? p.replace(/\//g, "\\") : p.replace(/\\/g, "/")
}

/** 平台构建命令选取：command 优先 → windows/unix 对应平台分支 → 无（不构建）。 */
function pickBuildCommand(build?: NativeAgentBuild): string[] | null {
  if (!build) return null
  if (build.command?.length) return build.command
  if (process.platform === "win32" && build.windows?.length) return build.windows
  if (process.platform !== "win32" && build.unix?.length) return build.unix
  return null
}

/** 构建目标：command 首元素展开占位（不含 {python}/{driver}）后为绝对路径时才引导构建
 *  （相对首元素/解释器命令无构建语义）。 */
function resolveCommandTarget(command: string[], dir: string, name?: string): string | undefined {
  const first = command[0] ?? ""
  const expanded = normalizePath(
    first
      .replace(/\{GEBAI_HOME\}/g, resolveGebaiHome())
      .replace(/\{agent_dir\}/g, dir)
      .replace(/\{agent_name\}/g, name ?? basename(dir))
      .replace(/\{lang_dir\}/g, dirname(dir))
      .replace(/\{exe\}/g, process.platform === "win32" ? ".exe" : ""),
  )
  return isAbsolute(expanded) ? expanded : undefined
}

/** 构建引导超时（编译型语言冷编译：cl/rustc/g++ 均远低于此）。 */
const BUILD_TIMEOUT_MS = 300_000

/** 构建引导：command 首元素指向的可执行文件不存在时执行 manifest.build 编译命令（cwd 为
 *  manifest 目录，占位符同 command），产物落盘后返回目标路径；可执行文件已存在/无 build
 *  声明 → 跳过；构建失败抛错（launchNativeAgent 记入 loadErrors，不阻断其他子代理）。 */
export async function ensureBuilt(
  dir: string,
  manifest: NativeAgentManifest,
  opts: { spawn?: SidecarSpawnFn; env?: Record<string, string> } = {},
): Promise<string | null> {
  const buildCmd = pickBuildCommand(manifest.build)
  if (!buildCmd) return null
  const target = resolveCommandTarget(manifest.command, dir, manifest.name)
  if (!target || existsSync(target)) return target ?? null
  const expanded = expandCommand(buildCmd, dir, manifest, null)
  if (expanded.error) throw new Error(`build 命令占位解析失败: ${expanded.error}`)
  const spawn = opts.spawn ?? defaultSpawn
  // process.env 值可 undefined：过滤后拼接（Bun.spawn env 类型要求全 string）
  const proc = spawn(expanded.command, {
    cwd: dir,
    env: { ...Object.fromEntries(Object.entries(process.env).filter(([, v]) => v !== undefined) as [string, string][]), ...(opts.env ?? {}) },
  })
  let out = ""
  const decoder = new TextDecoder()
  const drain = async (stream: ReadableStream<Uint8Array> | undefined) => {
    if (!stream) return
    try {
      const reader = stream.getReader()
      for (;;) {
        const { done, value } = await reader.read()
        if (done) break
        out += decoder.decode(value, { stream: true })
        if (out.length > 200_000) out = out.slice(-100_000)
      }
    } catch { /* 已关闭 */ }
  }
  const timer = setTimeout(() => {
    try {
      proc.kill()
    } catch { /* 已退出 */ }
  }, BUILD_TIMEOUT_MS)
  const codeP = (proc.exitCode ?? Promise.resolve<number | null>(null)) as Promise<number | null>
  const [code] = await Promise.all([codeP, drain(proc.stdout), drain(proc.stderr)]).finally(() => clearTimeout(timer))
  if (!existsSync(target)) {
    const tail = out.split("\n").slice(-30).join("\n")
    throw new Error(`构建引导失败：${expanded.command.join(" ")}（exit ${code ?? "?"}）未产出 ${target}${tail ? `:\n${tail}` : "（无输出）"}`)
  }
  return target
}

/** 边车工具 → 标准 Tool（execute 桥到 sidecar.toolCall；parameters 为驱动侧上报的 JSON Schema
 *  原样透传——多语言驱动自定义其形态）。 */
const TOOL_DEFAULT_TIMEOUT_MS = 300_000 // 工具描述 timeout 缺省 300s：宿主请求超时与此对齐（长任务不会先被宿主杀）

function sidecarTool(sidecar: AgentSidecar, toolName: string, def: { description: string; parameters: { type: string; properties: Record<string, unknown> } }, agentName: string): Tool {
  return {
    name: toolName,
    description: def.description,
    parameters: def.parameters as unknown as Tool["parameters"],
    requiresApproval: true, // 边车工具一律审批（任意代码执行面）
    card: { args: "code", codeField: "code", codeLang: toolName.includes("py") || agentName.includes("python") ? "python" : undefined } as Tool["card"],
    async execute(args) {
      const timeoutMs = typeof args.timeout === "number" && args.timeout > 0 ? Math.min(args.timeout * 1000, 540_000) : TOOL_DEFAULT_TIMEOUT_MS
      const r = await sidecar.toolCall(toolName, args, timeoutMs)
      if (r.error) {
        return { output: `${r.error}\n（${agentName} 边车工具 ${toolName} 失败；边车进程已自动重启，命名空间如丢失请重建）` }
      }
      return { output: r.output || "（无输出）", data: r.data as Record<string, unknown> | undefined }
    },
  }
}

export interface NativeAgentRunnerOptions {
  /** 测试注入：spawn 替身与解释器解析替身。 */
  spawn?: SidecarSpawnFn
  resolvePython?: () => string[] | null
  /** 跳过边车启动握手（单测：只验证 manifest 解析与 def 构造）。 */
  skipHandshake?: boolean
  /** 覆盖发现根目录（测试隔离；缺省 nativeAgentRoots()——内置 + {GEBAI_HOME}/agents）。 */
  roots?: string[]
  env?: Record<string, string>
}

/** 启动一个 native 子代理并握手，返回 SubAgentDef 与边车实例（失败抛错，调用方记 loadErrors）。
 *  边车实例登记进模块级 liveSidecars（进程级共享：签名命中缓存复用时新实例不重启进程；
 *  热加载重扫时同名先 dispose 再拉新、消失的名单由 discoverNativeAgents 对账回收）。 */
export async function launchNativeAgent(
  dir: string,
  manifest: NativeAgentManifest,
  opts: NativeAgentRunnerOptions = {},
): Promise<{ def: SubAgentDef; sidecar: AgentSidecar }> {
  const home = resolveGebaiHome()
  const pythonCmd = opts.resolvePython ? opts.resolvePython() : resolvePythonCommand(opts.env)
  // 编译型语言构建引导：可执行体缺失时先执行 manifest.build（如 rustc 直编 / cl 编译），
  // 产物落盘后正常启动；失败抛错记 loadErrors，不阻断其他子代理
  await ensureBuilt(dir, manifest, { spawn: opts.spawn, env: { GEBAI_HOME: home, ...(manifest.env ?? {}) } })
  const expanded = expandCommand(manifest.command, dir, manifest, pythonCmd)
  if (expanded.error) throw new Error(expanded.error)
  const promptFile = manifest.prompt ? join(dir, manifest.prompt) : join(dir, "PROMPT.md")
  let systemPrompt = ""
  if (existsSync(promptFile)) {
    const raw = readFileSync(promptFile, "utf8")
    // frontmatter（--- description/… ---）剥离：description 已由 manifest 提供，只取正文
    systemPrompt = raw.replace(/^---\r?\n[\s\S]*?\r?\n---\r?\n?/, "").trim()
  }
  if (!systemPrompt) {
    systemPrompt = `你是多语言子代理 ${manifest.name}。${manifest.description}`
  }
  const cwd = manifest.cwd ? manifest.cwd.replace(/\{GEBAI_HOME\}/g, home) : dir
  // 基础环境继承：保留 PATH/SYSTEMROOT 等进程基础变量（Windows 下 python 编解码/subprocess 初始化依赖
  // SYSTEMROOT；极小 env 会让驱动启动即卡死无报错），manifest env 覆盖同名项；GEBAI_AGENT_DIR
  // 供驱动定位子代理项目专属资产（如 python 驱动加载 {agent_dir}/tools.py 合并专属工具）
  const sidecarEnv: Record<string, string> = { ...process.env, GEBAI_HOME: home, GEBAI_AGENT_DIR: dir, ...(manifest.env ?? {}) }
  const sidecar = new AgentSidecar({
    command: () => {
      // 每次启动重新解析占位符（venv 创建后边车重启自动切换解释器）
      const py = opts.resolvePython ? opts.resolvePython() : resolvePythonCommand(opts.env)
      const c = expandCommand(manifest.command, dir, manifest, py)
      if (c.error) throw new Error(c.error)
      return c.command
    },
    cwd: existsSync(cwd) ? cwd : undefined,
    env: sidecarEnv,
    spawn: opts.spawn,
  })
  if (opts.skipHandshake) {
    return { def: { name: manifest.name, description: manifest.description, systemPrompt }, sidecar }
  }
  const info = await sidecar.init()
  if (info.name !== manifest.name) {
    sidecar.dispose("manifest 与边车 init 上报的 name 不一致")
    throw new Error(`边车 init 上报 name=${info.name} 与 manifest name=${manifest.name} 不一致`)
  }
  const tools = (await sidecar.toolsList()) as Array<{ name: string; description: string; parameters: { type: string; properties: Record<string, unknown> } }>
  const toolSet: Record<string, Tool> = {}
  for (const t of tools) toolSet[t.name] = sidecarTool(sidecar, t.name, t, manifest.name)
  const def = { name: manifest.name, description: manifest.description, systemPrompt, tools: toolSet }
  liveSidecars.get(manifest.name)?.dispose("重新启动（热加载重扫）")
  liveSidecars.set(manifest.name, sidecar)
  return { def, sidecar }
}

/** 扫描目录下全部子目录的 agent.json（返回 dir → raw 内容），目录不存在返回空。
 *  跳过语言目录运行时/构建资产：venv/__pycache__（Python）、objs/target（编译中间产物）；
 *  framework 类无 manifest 的共享库目录（如 rust cargo workspace 的 framework crate）自然跳过
 *  （子目录无 agent.json）。 */
export async function scanManifestDirs(roots: string[]): Promise<Array<{ dir: string; raw: string }>> {
  const { readdir } = await import("node:fs/promises")
  const out: Array<{ dir: string; raw: string }> = []
  for (const root of roots) {
    let entries
    try {
      entries = await readdir(root, { withFileTypes: true })
    } catch {
      continue // 目录不存在（如 {GEBAI_HOME}/agents 未建）——正常
    }
    for (const e of entries) {
      if (!e.isDirectory()) continue
      if (["venv", "__pycache__", "objs", "target", ".git"].includes(e.name)) continue
      const manifestPath = join(root, e.name, "agent.json")
      try {
        out.push({ dir: join(root, e.name), raw: readFileSync(manifestPath, "utf8") })
      } catch {
        continue // 子目录无 agent.json：跳过（共享库 crate/其他资产目录）
      }
    }
  }
  return out
}

/** native-agents 源目录们（供发现器与目录签名共用）：内置源按实现语言展开（每个语言目录一个
 *  扫描根——语言目录下二级目录即子代理项目）+ 用户自建 {GEBAI_HOME}/agents。 */
export function nativeAgentRoots(): string[] {
  const roots: string[] = []
  const src = nativeAgentsSourceDir()
  try {
    for (const e of readdirSync(src, { withFileTypes: true })) {
      if (e.isDirectory()) roots.push(join(src, e.name))
    }
  } catch {
    // 源目录不存在（如二进制形态未物化）：只剩用户自建根
  }
  roots.push(join(resolveGebaiHome(), "agents"))
  return roots
}

/** 进程级边车注册表：name → 运行中的 AgentSidecar。 */
const liveSidecars = new Map<string, AgentSidecar>()

/** native 子代理开关：GEBAI_NATIVE_AGENTS=off 显式关闭；沙箱启用（服务端部署形态）同样关闭。 */
export function nativeAgentsEnabled(): boolean {
  const v = String(process.env.GEBAI_NATIVE_AGENTS ?? "").trim().toLowerCase()
  if (v === "off" || v === "false" || v === "0") return false
  // 沙箱启用 = 服务端部署形态：多语言子代理仅限本地使用（用户约定），整体禁用
  const sandbox = String(process.env.GEBAI_SANDBOX ?? "auto").toLowerCase()
  if (sandbox === "on") return false
  return true
}

/** 发现并启动全部 native 子代理（发现器入口）：扫描内置 + 用户自建 manifest，逐个启动握手，
 *  返回成功 defs 与失败清单（name → 错误）；单项失败不阻断其他项（同名去重：用户自建覆盖内置）。 */
export async function discoverNativeAgents(
  opts: NativeAgentRunnerOptions = {},
): Promise<{ defs: SubAgentDef[]; errors: Array<[string, string]> }> {
  const roots = opts.roots ?? nativeAgentRoots()
  const found = await scanManifestDirs(roots)
  const byName = new Map<string, { dir: string; raw: string }>()
  const parsed: Array<{ dir: string; raw: string; name: string }> = []
  for (const f of found) {
    const m = parseManifest(f.raw, f.dir)
    if (m.error) {
      parsed.push({ ...f, name: "" })
      continue
    }
    if (m.manifest) {
      parsed.push({ ...f, name: m.manifest.name })
      byName.set(m.manifest.name, f) // 后扫到的根（用户自建）同名覆盖内置
    }
  }
  const defs: SubAgentDef[] = []
  const errors: Array<[string, string]> = []
  const validDirs = new Set(byName.values().map((f) => f.dir))
  await Promise.all(
    [...byName.entries()].map(async ([name, f]) => {
      const m = parseManifest(f.raw, f.dir)!
      if (!m.manifest) return
      try {
        const { def } = await launchNativeAgent(f.dir, m.manifest, opts)
        defs.push(def)
      } catch (err) {
        errors.push([name, String((err as Error).message || err)])
      }
    }),
  )
  // 坏 manifest（解析失败）也记入 errors（目录名作键）
  for (const p of parsed) {
    if (p.name === "" && !validDirs.has(p.dir)) {
      const m = parseManifest(p.raw, p.dir)
      if (m.error) errors.push([p.dir, m.error])
    }
  }
  return { defs, errors }
}

/** 回收不再存在的边车（热加载重扫后对账：上次名单有、本次没有的 → dispose）。 */
export function disposeNativeAgentsNotIn(names: string[]): void {
  for (const [name, sc] of [...liveSidecars]) {
    if (!names.includes(name)) {
      sc.dispose(`manifest 已移除（${name}）`)
      liveSidecars.delete(name)
    }
  }
}

/** 全部回收（服务关闭/测试收尾用）。 */
export function disposeAllNativeAgents(): void {
  for (const [name, sc] of [...liveSidecars]) {
    sc.dispose("服务关闭")
    liveSidecars.delete(name)
  }
}

/** native 目录签名（热加载）：各根目录（语言目录）子目录（子代理项目）内全部文件（递归 2 层
 *  ——兼容 cargo/gradle 等标准工程布局 src/*.rs）的 路径:mtime 拼接——manifest/驱动脚本/提示词
 *  任一变化即变化；跳过 venv/__pycache__/objs/target 与编译产物（driver 可执行体及中间产物）
 *  ——运行时数据不触发重扫；根全不存在返回空串（与 sub-agents 签名拼接后仍稳定）。 */
export async function nativeAgentsSignature(roots: string[]): Promise<string> {
  const { readdir, stat } = await import("node:fs/promises")
  const SKIP_DIRS = ["venv", "__pycache__", "objs", "target", ".git"]
  const parts: string[] = []
  const walk = async (rel: string, abs: string, depth: number): Promise<void> => {
    let entries
    try {
      entries = await readdir(abs, { withFileTypes: true })
    } catch {
      return
    }
    for (const f of entries) {
      if (f.isFile()) {
        // 编译产物跨平台形态：Windows driver.exe / Linux 与 macOS 无后缀 driver（含中间产物）
        if (/^driver(\.(exe|pdb|obj|o|d|out|bin|so|dylib))?$/.test(f.name)) continue
        const st = await stat(join(abs, f.name)).catch(() => null)
        if (st) parts.push(`${rel}/${f.name}:${st.mtimeMs}`)
      } else if (f.isDirectory() && depth > 0 && !SKIP_DIRS.includes(f.name)) {
        await walk(`${rel}/${f.name}`, join(abs, f.name), depth - 1)
      }
    }
  }
  for (const root of roots) {
    let dirs
    try {
      dirs = await readdir(root, { withFileTypes: true })
    } catch {
      continue
    }
    for (const e of dirs) {
      if (!e.isDirectory()) continue
      if (SKIP_DIRS.includes(e.name)) continue
      await walk(e.name, join(root, e.name), 2)
    }
  }
  return parts.sort().join("|")
}
