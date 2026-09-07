/**
 * 多语言子代理发现器（core/agents/native-agents.ts）：扫描 manifest（agent.json）→ 启动
 * 边车进程 → init/tools.list 握手 → 构造标准 SubAgentDef 注册进 SubAgentManager。
 *
 * 发现范围（后者同名覆盖前者）：
 *   1. 内置源 native-agents/（源码形态为仓库内目录，dist 形态产物同目录，二进制形态
 *      {GEBAI_HOME}/vendor/native-agents/——安装包预置物化）
 *   2. 用户自建 {GEBAI_HOME}/agents/{name}/agent.json（放一个目录即成一个子代理，任意语言）
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
 *
 * 生命周期：进程管理全部委托 AgentSidecar（惰性启动/超时杀进程重启/崩溃自愈/退出清理）。
 * 门控（DESIGN「多语言子代理」）：仅本地形态启用——沙箱启用（服务端部署）或 GEBAI_NATIVE_AGENTS=off
 * 时发现器整体禁用（native 子代理完全不可见），不影响 TS 子代理。
 * 失败安全：manifest 损坏/边车启动失败/握手失败 → 该项跳过并记入 loadErrors（模型经
 * unknownAgentError 可见根因），绝不阻断启动与其他子代理。
 */
import { existsSync, readFileSync, statSync } from "node:fs"
import { isAbsolute, join } from "node:path"
import { resolveGebaiHome } from "../base/config"
import type { SubAgentDef, Tool } from "../base/types"
import { AgentSidecar, nativeAgentsSourceDir, type SidecarSpawnFn } from "./sidecar"

/** 解释器解析（{python} 占位，其他语言 manifest 直接写可执行体路径/命令名）：
 *  GEBAI_PYTHON_DIR（解释器目录或可执行体完整路径；目录时补拼 python.exe/python）
 *  → {GEBAI_HOME}/venv → PATH（python3/python/py，Bun.which）。 */
export function resolvePythonCommand(env: Record<string, string> = process.env as Record<string, string>): string[] | null {
  const home = resolveGebaiHome()
  const explicit = String(env.GEBAI_PYTHON_DIR ?? "").trim()
  const absolute: string[] = []
  if (explicit) {
    // 目录形态补拼解释器名（Windows python.exe / 其余 python）；直接指向可执行体则原样
    if (/\.(exe|cmd|bat)$/i.test(explicit) || !existsSync(explicit) || statSync(explicit).isFile()) absolute.push(explicit)
    else absolute.push(join(explicit, process.platform === "win32" ? "python.exe" : "python"))
  }
  absolute.push(process.platform === "win32" ? join(home, "venv", "Scripts", "python.exe") : join(home, "venv", "bin", "python"))
  for (const c of absolute) {
    if (c && isAbsolute(c) && existsSync(c)) return [c]
  }
  for (const name of process.platform === "win32" ? ["python", "py"] : ["python3", "python"]) {
    const found = Bun.which(name)
    if (found) return [found]
  }
  return null
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
    },
  }
}

/** 占位符解析：{python}（解释器命令，数组展开）/{driver}（脚本绝对路径）/{GEBAI_HOME}。 */
export function expandCommand(
  command: string[],
  dir: string,
  manifest: { driver?: string },
  pythonCmd: string[] | null,
): { command: string[]; error?: string } {
  const home = resolveGebaiHome()
  const out: string[] = []
  for (const part of command) {
    if (part.includes("{driver}")) {
      const driverFile = manifest.driver ? join(dir, manifest.driver) : ""
      if (!driverFile || !existsSync(driverFile)) {
        return { command: [], error: `command 引用 {driver} 但 manifest 未声明可用的 driver 文件（${manifest.driver ?? "未声明"}）` }
      }
      out.push(part.replace(/\{driver\}/g, driverFile))
    } else if (part === "{python}") {
      if (!pythonCmd) return { command: [], error: "command 引用 {python} 但解释器不可解析（GEBAI_PYTHON_DIR / {GEBAI_HOME}/venv / 系统 PATH 均无 python）" }
      out.push(...pythonCmd)
    } else {
      out.push(part.replace(/\{GEBAI_HOME\}/g, home))
    }
  }
  return { command: out }
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
  // SYSTEMROOT；极小 env 会让驱动启动即卡死无报错），manifest env 覆盖同名项
  const sidecarEnv: Record<string, string> = { ...process.env, GEBAI_HOME: home, ...(manifest.env ?? {}) }
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

/** 扫描目录下全部子目录的 agent.json（返回 dir → raw 内容），目录不存在返回空。 */
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
      const manifestPath = join(root, e.name, "agent.json")
      try {
        out.push({ dir: join(root, e.name), raw: readFileSync(manifestPath, "utf8") })
      } catch {
        continue // 子目录无 agent.json：跳过（目录里可能有别的资产）
      }
    }
  }
  return out
}

/** native-agents 源目录们（内置 + 用户自建；供发现器与目录签名共用）。 */
export function nativeAgentRoots(): string[] {
  return [nativeAgentsSourceDir(), join(resolveGebaiHome(), "agents")]
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

/** native 目录签名（热加载）：各根目录子目录内全部文件（递归 1 层）的 路径:mtime 拼接——
 *  manifest/驱动脚本/提示词任一变化即变化；根全不存在返回空串（与 sub-agents 签名拼接后仍稳定）。 */
export async function nativeAgentsSignature(roots: string[]): Promise<string> {
  const { readdir, stat } = await import("node:fs/promises")
  const parts: string[] = []
  for (const root of roots) {
    let dirs
    try {
      dirs = await readdir(root, { withFileTypes: true })
    } catch {
      continue
    }
    for (const e of dirs) {
      if (!e.isDirectory()) continue
      const sub = join(root, e.name)
      let files
      try {
        files = await readdir(sub, { withFileTypes: true })
      } catch {
        continue
      }
      for (const f of files) {
        if (!f.isFile()) continue
        const st = await stat(join(sub, f.name)).catch(() => null)
        if (st) parts.push(`${e.name}/${f.name}:${st.mtimeMs}`)
      }
    }
  }
  return parts.sort().join("|")
}
