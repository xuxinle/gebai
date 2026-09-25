/**
 * local_infer 子Agent 的数据访问层：路径解析、档位定义、模型清单，
 * 以及**服务状态文件 / 启动记录 / 日志**的读写（进程管理的唯一事实来源）。
 *
 * 状态文件布局（子项目 infer/ 内，全部可重建、不入库）：
 *   run/server-<port>.json   正在运行的服务实例（PID/端口/档位/模型/日志路径/argv）
 *   bench/reports/launch-<stamp>.json  历史启动记录（run-server.ps1 写）
 *   bench/reports/server-<stamp>.log   服务 stdout/stderr
 *   bench/runs/<job-id>/      批量推理任务的输入、结果与进度
 */
import { closeSync, existsSync, mkdirSync, openSync, readFileSync, readSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs"
import { isAbsolute, join, resolve, win32 } from "node:path"

// ── 路径解析 ──────────────────────────────────────────────────────────────

/** 绝对路径判定（跨平台）：POSIX 绝对路径与 Windows 盘符/UNC 形态都算绝对——环境变量配置的路径
 *  常在 Windows 与 WSL/容器之间共享，只在当前平台判定会把 `C:\x` 拼成 `<cwd>/C:\x` 这样的无意义路径
 *  （后续访问必失败，且错误信息指向一个并不存在的怪路径）。 */
export function isAbsolutePath(p: string): boolean {
  return isAbsolute(p) || win32.isAbsolute(p)
}

/** 子项目根：LOCAL_INFER_HOME 优先；dev 模式按模块路径推导（src/agents/local_infer → 仓库根/infer）。 */
export function inferHome(env: Record<string, string>): string {
  const h = env.LOCAL_INFER_HOME
  if (h) return isAbsolutePath(h) ? h : resolve(process.cwd(), h)
  return resolve(import.meta.dirname, "..", "..", "..", "..", "..", "infer")
}

/** 运行档位定义文件。 */
export function profilesPath(home: string): string {
  return join(home, "config", "profiles.json")
}

/** 模型权重目录（{GEBAI_HOME}/resources/models/infer）。 */
export function modelsDir(home: string, env: Record<string, string>): string {
  const dir = env.LOCAL_INFER_MODELS_DIR
  if (dir) return isAbsolutePath(dir) ? dir : resolve(process.cwd(), dir)
  return resolve(home, "..", "resources", "models", "infer")
}

/** 服务端口（LOCAL_INFER_PORT 或 8080）。 */
export function defaultPort(env: Record<string, string>): number {
  const p = Number(env.LOCAL_INFER_PORT ?? 8080)
  return Number.isFinite(p) && p > 0 ? p : 8080
}

/** OpenAI 兼容端点基址。 */
export function baseUrl(port: number, host = "127.0.0.1"): string {
  return `http://${host}:${port}`
}

export interface InferProfile {
  model?: string
  ctx?: number
  n_cpu_moe?: number | null
  n_gpu_layers?: number
  parallel?: number
  spec_type?: string | null
}

export interface InferProfiles {
  engine_dir: string
  default_profile: string
  default_model?: string
  profiles: Record<string, InferProfile>
}

/** 读取档位定义（缺失或损坏时返回 null，由调用方给出可操作的提示）。 */
export function loadProfiles(home: string): InferProfiles | null {
  const p = profilesPath(home)
  if (!existsSync(p)) return null
  try {
    const raw = JSON.parse(readFileSync(p, "utf-8")) as InferProfiles
    if (!raw || typeof raw !== "object" || !raw.profiles) return null
    return raw
  } catch {
    return null
  }
}

/** 列出模型目录下的 GGUF（含大小），按名称排序。 */
export function listModels(dir: string): Array<{ name: string; gb: number; incomplete: boolean }> {
  if (!existsSync(dir)) return []
  const out: Array<{ name: string; gb: number; incomplete: boolean }> = []
  for (const f of readdirSync(dir)) {
    if (!/\.gguf(\.incomplete)?$/i.test(f)) continue
    try {
      const st = statSync(join(dir, f))
      if (!st.isFile()) continue
      out.push({ name: f, gb: st.size / 1024 ** 3, incomplete: f.endsWith(".incomplete") })
    } catch {
      /* 忽略不可读条目 */
    }
  }
  return out.sort((a, b) => a.name.localeCompare(b.name))
}

/** 解析某档位实际会用的模型文件名（档位未指定时取全局默认）。 */
export function profileModel(p: InferProfiles, profile: string): string {
  return p.profiles[profile]?.model ?? p.default_model ?? ""
}

// ── 目录约定 ──────────────────────────────────────────────────────────────

/** 启动记录与日志目录。 */
export function reportsDir(home: string): string {
  return join(home, "bench", "reports")
}

/** 批量推理任务目录（每个任务一个子目录）。 */
export function runsDir(home: string): string {
  return join(home, "bench", "runs")
}

/** 运行态状态目录（服务实例状态文件）。 */
export function stateDir(home: string): string {
  return join(home, "run")
}

/** 引擎安装根目录（`infer/vendor/<engine-id>/`；矩阵与安装状态见 engines.ts）。 */
export function vendorDir(home: string): string {
  return join(home, "vendor")
}

/** 某个引擎的安装目录。 */
export function engineDir(home: string, id: string): string {
  return join(vendorDir(home), id)
}

/** 下载缓存目录（未完成/待解压的归档；不入库）。 */
export function downloadCacheDir(home: string): string {
  return join(vendorDir(home), ".cache")
}

/** 某端口的服务实例状态文件（一个端口一份，多实例并存互不覆盖）。 */
export function serverStatePath(home: string, port: number): string {
  return join(stateDir(home), `server-${port}.json`)
}

// ── 服务实例状态 ──────────────────────────────────────────────────────────

/** 正在运行（或曾运行）的服务实例记录：run-server.ps1 启动时写入、停止时删除。 */
export interface ServerState {
  pid: number
  port: number
  profile: string
  model: string
  host?: string
  exe?: string
  /** 服务 stdout 日志（llama-server 的全部输出）。 */
  log?: string
  /** 服务 stderr 日志（Windows 后台启动时单独落盘）。 */
  err_log?: string
  started_at?: string
  argv?: string[]
  engine?: string
}

/** 写入服务实例状态（目录不存在时创建）。 */
export function writeServerState(home: string, st: ServerState): string {
  const p = serverStatePath(home, st.port)
  mkdirSync(stateDir(home), { recursive: true })
  writeFileSync(p, `${JSON.stringify({ ...st, written_at: new Date().toISOString() }, null, 2)}\n`, "utf-8")
  return p
}

/** 删除服务实例状态（停止服务后调用；文件不存在不算错）。 */
export function clearServerState(home: string, port: number): boolean {
  const p = serverStatePath(home, port)
  if (!existsSync(p)) return false
  try {
    rmSync(p, { force: true })
    return true
  } catch {
    return false
  }
}

/** 读出全部服务实例状态（按端口排序；损坏文件跳过，不阻断其它实例）。 */
export function readServerStates(home: string): ServerState[] {
  const dir = stateDir(home)
  if (!existsSync(dir)) return []
  const out: ServerState[] = []
  for (const f of readdirSync(dir)) {
    if (!/^server-\d+\.json$/.test(f)) continue
    try {
      const j = JSON.parse(readFileSync(join(dir, f), "utf-8")) as ServerState
      if (j && typeof j.port === "number" && typeof j.pid === "number") out.push(j)
    } catch {
      /* 损坏则跳过 */
    }
  }
  return out.sort((a, b) => a.port - b.port)
}

// ── 历史启动记录 ──────────────────────────────────────────────────────────

export interface LaunchRecord {
  started_at?: string
  profile?: string
  model?: string
  n_cpu_moe?: number
  spec_type?: string | null
  threads?: number
  port?: number
  exe?: string
  /** 引擎 id（引擎矩阵标识，如 linux-cpu-x64；旧记录可能没有）。 */
  engine?: string
  /** 启动方式：launcher（TS 跨平台直启）/ script（run-server.ps1）。 */
  launch_mode?: string
  argv?: string[]
  log?: string
}

/** 读最近 N 条启动记录（新的在前）。 */
export function readLaunchRecords(home: string, limit = 1): LaunchRecord[] {
  const dir = reportsDir(home)
  if (!existsSync(dir)) return []
  const files = readdirSync(dir)
    .filter((f) => f.startsWith("launch-") && f.endsWith(".json"))
    .sort()
    .reverse()
    .slice(0, Math.max(0, limit))
  const out: LaunchRecord[] = []
  for (const f of files) {
    try {
      out.push(JSON.parse(readFileSync(join(dir, f), "utf-8")) as LaunchRecord)
    } catch {
      /* 记录损坏则跳过 */
    }
  }
  return out
}

// ── 日志访问 ──────────────────────────────────────────────────────────────

export interface LogFile {
  path: string
  name: string
  kind: "out" | "err"
  size: number
  mtime: number
}

/** 列出服务日志（新的在前）：server-<stamp>.log 与 server-<stamp>.log.err。 */
export function listServerLogs(home: string, limit = 10): LogFile[] {
  const dir = reportsDir(home)
  if (!existsSync(dir)) return []
  const out: LogFile[] = []
  for (const f of readdirSync(dir)) {
    if (!/^server-.*\.log(\.err)?$/.test(f)) continue
    try {
      const st = statSync(join(dir, f))
      if (!st.isFile()) continue
      out.push({ path: join(dir, f), name: f, kind: f.endsWith(".err") ? "err" : "out", size: st.size, mtime: st.mtimeMs })
    } catch {
      /* 忽略不可读条目 */
    }
  }
  return out.sort((a, b) => b.mtime - a.mtime).slice(0, Math.max(0, limit))
}

/** 读文件尾部若干行（大日志不整文件读入内存：从尾部按块回退，最多读 maxBytes）。 */
export function tailLines(file: string, lines: number, maxBytes = 256 * 1024): { text: string; truncated: boolean } {
  if (!existsSync(file)) return { text: "", truncated: false }
  const st = statSync(file)
  const want = Math.max(1, lines)
  let size = st.size
  // 目标：至少覆盖 want 行 + 余量；从 8KB 起步按 4 倍扩张，上限 maxBytes
  let chunk = Math.min(Math.max(8 * 1024, want * 200), maxBytes)
  let text = ""
  let truncated = false
  for (;;) {
    const start = Math.max(0, size - chunk)
    const len = size - start
    const buf = Buffer.allocUnsafe(len)
    const fd = openSync(file, "r")
    try {
      readSync(fd, buf, 0, len, start)
    } finally {
      closeSync(fd)
    }
    text = buf.toString("utf-8")
    const got = text.split(/\r?\n/).length - 1
    if (start === 0) {
      truncated = false
      break
    }
    if (got >= want + 1 || chunk >= maxBytes) {
      truncated = chunk >= maxBytes
      break
    }
    chunk = Math.min(chunk * 4, maxBytes)
  }
  const all = text.split(/\r?\n/)
  // 尾部读取通常从行中间开始：首行只在读到文件开头时可信，否则丢弃
  const safe = size === 0 || chunk >= size ? all : all.slice(1)
  return { text: safe.slice(-want).join("\n"), truncated }
}

// ── 批量任务记录 ──────────────────────────────────────────────────────────

/** 批量任务状态文件的快照（jobs 工具读取）。 */
export interface BatchJobState {
  job_id: string
  state: "running" | "done" | "failed" | "cancelled" | "interrupted"
  phase?: string
  port: number
  model?: string
  concurrency: number
  total: number
  done: number
  failed: number
  skipped?: number
  structured?: boolean
  started_at: string
  updated_at: string
  finished_at?: string
  /** 结果落盘路径（JSONL，每行一条）。 */
  results_file?: string
  /** 输入副本（JSONL）。 */
  items_file?: string
  pid?: number
  error?: string
}

export interface BatchJobEntry extends BatchJobState {
  dir: string
}

/** 列出批量任务（新的在前）。 */
export function listBatchJobs(home: string, limit = 20): BatchJobEntry[] {
  const dir = runsDir(home)
  if (!existsSync(dir)) return []
  const out: BatchJobEntry[] = []
  for (const name of readdirSync(dir)) {
    const jp = join(dir, name, "job.json")
    if (!existsSync(jp)) continue
    try {
      const j = JSON.parse(readFileSync(jp, "utf-8")) as BatchJobState
      if (j && j.job_id) out.push({ ...j, dir: join(dir, name) })
    } catch {
      /* 损坏则跳过 */
    }
  }
  return out.sort((a, b) => String(b.started_at).localeCompare(String(a.started_at))).slice(0, Math.max(0, limit))
}

/** 读单个批量任务状态。 */
export function readBatchJob(home: string, jobId: string): BatchJobEntry | null {
  const jp = join(runsDir(home), jobId, "job.json")
  if (!existsSync(jp)) return null
  try {
    const j = JSON.parse(readFileSync(jp, "utf-8")) as BatchJobState
    return j && j.job_id ? { ...j, dir: join(runsDir(home), jobId) } : null
  } catch {
    return null
  }
}
