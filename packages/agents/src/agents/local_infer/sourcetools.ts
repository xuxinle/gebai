/**
 * local_infer 的【源码发现与自动编译】工具层：内网/离线环境下把 `resources/src/` 里的源码就地编译成引擎。
 *
 * 与另两层的关系：
 *   * `sourcefind.ts`/`sourcebuild.ts` 是能力实现（发现 + 编译）；
 *   * 本文件只做参数解析、进度/状态落盘、人读输出与结构化 data（与 tasks.ts 的做法一致）；
 *   * 编译产物走 `vendor/<engine-id>/` + `.engine.json` 标记 —— 与下载安装的引擎**同一落点与格式**，
 *     因此 `local_infer_start` / `status` / `engines` 无需区分来源。
 *
 * 两个工具：
 *   * `sources`（只读）：发现清单 / 工具链与设备就绪 / 单候选详情；
 *   * `source_build`：编译安装（可后台，状态可查、可取消）。
 */
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import type { Tool, ToolContext, ToolSchema } from "@gebai/sdk"
import { inferHome, reportsDir, stateDir, tailLines } from "./paths"
import {
  detectToolchain,
  discoverSources,
  findSource,
  type SourceCandidate,
  type ToolchainInfo,
} from "./sourcefind"
import { BUILD_DEVICES, buildFromSource, defaultJobs, type BuildDevice, type BuildStep } from "./sourcebuild"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

// ── 编译任务状态（后台执行的落盘形态） ────────────────────────────────────

interface BuildJobState {
  build_id: string
  source: string
  engine_id: string
  device: string
  jobs: number
  offline: boolean
  clean: boolean
  state: "running" | "done" | "failed" | "cancelled"
  phase: string
  steps: Array<{ phase: string; message: string; at: string }>
  started_at: string
  updated_at: string
  finished_at?: string
  log?: string
  exe?: string
  error?: string
  owner_pid: number
}

function buildDirOf(home: string): string {
  return join(stateDir(home), "builds")
}

function buildStatePath(home: string, id: string): string {
  return join(buildDirOf(home), `${id.replace(/[^A-Za-z0-9._-]+/g, "-")}.json`)
}

function writeBuildState(home: string, st: BuildJobState): void {
  const dir = buildDirOf(home)
  mkdirSync(dir, { recursive: true })
  writeFileSync(buildStatePath(home, st.build_id), `${JSON.stringify(st, null, 2)}\n`, "utf-8")
}

function readBuildStates(home: string): BuildJobState[] {
  const dir = buildDirOf(home)
  if (!existsSync(dir)) return []
  const out: BuildJobState[] = []
  for (const f of readdirSync(dir)) {
    if (!f.endsWith(".json")) continue
    try {
      const j = JSON.parse(readFileSync(join(dir, f), "utf-8")) as BuildJobState
      if (j?.build_id) out.push(j)
    } catch {
      /* 损坏则跳过 */
    }
  }
  return out.sort((a, b) => String(b.started_at).localeCompare(String(a.started_at)))
}

function readBuildState(home: string, id: string): BuildJobState | null {
  const p = buildStatePath(home, id)
  if (!existsSync(p)) return null
  try {
    return JSON.parse(readFileSync(p, "utf-8")) as BuildJobState
  } catch {
    return null
  }
}

/** 取消标记路径（编译层在阶段之间检查；写入即表示“阶段结束就停”）。 */
function cancelPath(home: string, id: string): string {
  return join(buildDirOf(home), `${id.replace(/[^A-Za-z0-9._-]+/g, "-")}.cancel`)
}

function stamp(): string {
  const d = new Date()
  const p = (n: number) => String(n).padStart(2, "0")
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`
}

// ── 输出格式化 ────────────────────────────────────────────────────────────

function fmtGB(bytes?: number): string {
  if (!bytes) return "-"
  return bytes > 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(2)} GB` : `${(bytes / 1024 ** 2).toFixed(1)} MB`
}

function candidateLines(c: SourceCandidate): string[] {
  const kindTag = c.kind === "llama_cpp" ? "llama.cpp" : c.kind
  const lines = [`  ${c.id}  [${kindTag}]  ${c.archive ? `归档 ${c.archive.format} ${fmtGB(c.archive.bytes)}` : `目录 ${fmtGB(c.bytes)}`}`]
  lines.push(`      路径: ${c.path}`)
  if (c.version) lines.push(`      版本: ${c.version}`)
  if (c.evidence.length) lines.push(`      依据: ${c.evidence.slice(0, 3).join("；")}`)
  if (c.peek) {
    const pk = c.peek
    lines.push(
      `      归档预览: ${pk.entries} 个条目 ｜ CMakeLists: ${pk.has_cmake ? "有" : "无"} ｜ vendor/ 依赖: ${
        pk.has_vendor_deps ? "有（离线编译可行）" : "**未发现**（离线编译会尝试联网取依赖）"
      }`,
    )
    if (pk.top_level.length) lines.push(`      顶层: ${pk.top_level.slice(0, 6).join(", ")}`)
    for (const n of pk.notes.slice(0, 2)) lines.push(`      · ${n}`)
  }
  return lines
}

function toolchainLines(tc: ToolchainInfo): string[] {
  const lines: string[] = ["工具链（内网编译的可行性依据）："]
  const tool = (label: string, v?: string, p?: string) => {
    lines.push(`  ${label.padEnd(7)} ${v ? (p ? `${v}  → ${p}` : v) : "缺失"}`)
  }
  tool("cmake", tc.cmake, tc.paths?.cmake)
  tool("ninja", tc.ninja, tc.paths?.ninja)
  tool("make", tc.make, tc.paths?.make)
  tool("cc", tc.cc, tc.paths?.cc)
  tool("cxx", tc.cxx, tc.paths?.cxx)
  tool("nvcc", tc.nvcc, tc.paths?.nvcc)
  tool("hipcc", tc.hipcc, tc.paths?.hipcc)
  tool("python", tc.python, tc.paths?.python)
  lines.push("", "设备后端就绪：")
  for (const [dev, v] of Object.entries(tc.devices)) {
    const mark = v.ready ? "✓ 可编译" : `✗ 缺 ${v.missing.join("、") || "依赖"}`
    lines.push(`  ${dev.padEnd(7)} ${mark}`)
    for (const n of v.notes.slice(0, 1)) lines.push(`          ${n}`)
  }
  if (tc.offline_hints.length) {
    lines.push("", "内网补齐提示：")
    for (const h of tc.offline_hints) lines.push(`  · ${h}`)
  }
  return lines
}

// ── sources（只读） ───────────────────────────────────────────────────────

const sources: Tool = {
  name: "sources",
  safeMode: true, // 只读：发现/探测/预览，不落地不编译
  description:
    "发现本机可编译的推理源码（内网/离线场景的主力：把 llama.cpp 源码或源码归档放进 {GEBAI_HOME}/resources/src/，" +
    "或用 LOCAL_INFER_SOURCE_DIRS 指定目录）并给出编译可行性依据（工具链与设备后端就绪、归档内是否自带 vendor/ 依赖）。" +
    "action=list 列候选（目录/归档；归档只预览不解压）；action=toolchain 列工具链与各设备就绪 + 内网补齐办法；" +
    "action=inspect 看单个候选详情。只读，不改变任何状态；编译用 local_infer_source_build。",
  parameters: schema({
    action: { type: "string", enum: ["list", "toolchain", "inspect"], description: "操作（缺省 list）" },
    id: { type: "string", description: "候选 id 或路径（inspect 用；也支持直接给目录/归档的绝对路径）" },
  }),
  outputSchema: schema({
    action: { type: "string" },
    count: { type: "number" },
    candidates: { type: "array", items: { type: "object" } },
    roots: { type: "array", items: { type: "object" } },
    toolchain: { type: "object" },
  }),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    if (!existsSync(home)) return { output: `未找到本地推理子项目：${home}（可用 LOCAL_INFER_HOME 指定）` }
    const action = String(args.action ?? "list")

    if (action === "toolchain") {
      const tc = await detectToolchain(ctx)
      const ready = BUILD_DEVICES.filter((d) => tc.devices[d]?.ready)
      const lines = toolchainLines(tc)
      lines.push("", `可编译后端：${ready.length ? ready.join("、") : "无（先按上面的提示补齐工具链）"}`)
      lines.push(`下一步：local_infer_source_build(source="<候选 id>", device="${ready[0] ?? "cpu"}")`)
      return { output: lines.join("\n"), data: { action, toolchain: tc, ready } }
    }

    if (action === "inspect") {
      const ref = args.id != null ? String(args.id) : ""
      if (!ref) return { output: "inspect 需要 id（候选 id 或目录/归档路径）——先用 action=\"list\" 查看候选清单。" }
      const found = await findSource(home, ctx.env, ref, { ctx })
      if (!found.candidate) return { output: found.error ?? `未找到源码：${ref}` }
      const c = found.candidate
      const tc = await detectToolchain(ctx)
      const ready = BUILD_DEVICES.filter((d) => tc.devices[d]?.ready)
      const lines = [`源码候选：${c.id}`, "", ...candidateLines(c), "", `所在根: ${c.root}`]
      const offlineRisk = c.peek && !c.peek.has_vendor_deps
      lines.push(
        "",
        offlineRisk
          ? "离线可行性：**有风险**——归档内未发现 vendor/ 依赖，离线编译会尝试联网取依赖。建议换用含 vendor 的完整源码包（git clone --recursive 或官方源码归档）。"
          : c.kind === "llama_cpp"
            ? "离线可行性：看起来可行（源码自带 vendor/ 依赖或为完整源码目录）。"
            : "离线可行性：该候选不是 llama.cpp 源码，编译需自备构建方式（本工具按 llama.cpp 预设编译）。",
      )
      lines.push(`可编译后端：${ready.length ? ready.join("、") : "无"}`)
      return { output: lines.join("\n"), data: { action, candidate: c, toolchain: tc, ready, offline_risk: Boolean(offlineRisk) } }
    }

    // list
    const res = await discoverSources(home, ctx.env, { ctx })
    const lines: string[] = [
      `源码发现（本机 ${process.platform}/${process.arch}）：${res.candidates.length} 个候选`,
      "",
      "发现根（按优先级）：",
    ]
    for (const s of res.scanned) {
      lines.push(`  ${s.exists ? "✓" : "✗"} ${s.root}${s.exists ? `（${s.entries} 个条目）` : "（不存在）"}`)
    }
    lines.push("", "候选：")
    if (!res.candidates.length) {
      lines.push("  （无）——把 llama.cpp 源码目录或源码归档放进上面的资源目录，或用 LOCAL_INFER_SOURCE_DIRS 指定路径。")
      lines.push("  联网机器下载源码归档：bun run resources:download --only=\"src/**\"（清单已登记 src/llama.cpp-<tag>.tar.gz）")
    } else {
      for (const c of res.candidates) lines.push(...candidateLines(c), "")
      lines.push("编译：local_infer_source_build(source=\"<候选 id>\", device=\"cpu\"|\"cuda\"|\"vulkan\"|...；先 action=\"toolchain\" 看后端就绪)")
    }
    if (res.notes.length) {
      lines.push("", "提示：")
      for (const n of res.notes) lines.push(`  · ${n}`)
    }
    lines.push("", `LOCAL_INFER_SOURCE_DIRS 当前${ctx.env.LOCAL_INFER_SOURCE_DIRS ? "：" + ctx.env.LOCAL_INFER_SOURCE_DIRS : "未配置（可选，额外发现根，路径分隔符分隔）"}`)
    return {
      output: lines.join("\n"),
      data: { action: "list", count: res.candidates.length, candidates: res.candidates, roots: res.scanned, notes: res.notes },
    }
  },
}

// ── source_build（编译安装） ──────────────────────────────────────────────

async function runBuild(
  args: Record<string, unknown>,
  ctx: ToolContext,
  home: string,
): Promise<{ output: string; data?: unknown }> {
  const ref = args.source != null ? String(args.source) : ""
  if (!ref) {
    return { output: "source_build 需要 source（候选 id 或源码目录/归档路径）——先用 local_infer_sources 查看候选清单。" }
  }
  // 先校验纯参数（不动盘、不探测），再解析源码——参数错误应最先暴露
  const device = (args.device != null ? String(args.device) : "cpu") as BuildDevice
  if (!BUILD_DEVICES.includes(device)) {
    return { output: `未知设备后端 "${device}"——可用：${BUILD_DEVICES.join("、")}` }
  }
  const found = await findSource(home, ctx.env, ref, { ctx })
  const candidate = found.candidate
  if (!candidate) return { output: found.error ?? `未找到源码：${ref}` }
  // 引擎 id：显式 > 与矩阵同名的平台档（linux-cpu-x64 之类）；已被占用则加 -src 后缀（不覆盖已有安装）
  const explicitId = args.engine_id != null ? String(args.engine_id).trim() : ""
  const baseId = `${process.platform}-${device}-${process.arch === "arm64" ? "arm64" : "x64"}`
  const engineId = explicitId || baseId
  const jobs = Number(args.jobs ?? 0) || defaultJobs()
  const offline = args.offline !== false // 缺省即离线优先（内网场景；显式 false 才允许 FetchContent 联网）
  const clean = args.clean === true
  const background = args.background === true

  const startedAt = new Date().toISOString()
  const buildId = `build-${candidate.id.replace(/[^A-Za-z0-9._-]+/g, "-")}-${stamp()}`
  const st: BuildJobState = {
    build_id: buildId,
    source: candidate.id,
    engine_id: engineId,
    device,
    jobs,
    offline,
    clean,
    state: "running",
    phase: "extract",
    steps: [],
    started_at: startedAt,
    updated_at: startedAt,
    owner_pid: process.pid,
  }
  writeBuildState(home, st)
  rmSync(cancelPath(home, buildId), { force: true })

  const note = (phase: string, message: string) => {
    st.phase = phase
    st.steps.push({ phase, message, at: new Date().toISOString() })
    st.updated_at = new Date().toISOString()
    writeBuildState(home, st)
  }

  const cancelled = () => existsSync(cancelPath(home, buildId))

  const onStep = async (s: BuildStep) => {
    if (cancelled()) throw new Error("已按取消标记中止（阶段之间检查）")
    note(s.phase, s.message)
  }

  const execute = async (): Promise<{
    ok: boolean
    engine?: { id: string; exe: string; dir?: string } & Record<string, unknown>
    error?: string
    logPath?: string
  }> => {
    try {
      const r = await buildFromSource({
        home,
        candidate,
        spec: { engine_id: engineId, device, jobs, clean, offline, extra_cmake_args: (args.extra_cmake_args as string[]) ?? undefined },
        ctx,
        onStep,
      })
      if (r.ok && r.engine) {
        st.state = "done"
        st.phase = "done"
        st.exe = r.engine.exe
        st.log = r.logPath
        st.finished_at = new Date().toISOString()
        st.updated_at = st.finished_at
        writeBuildState(home, st)
        return { ok: true, engine: r.engine, logPath: r.logPath }
      }
      st.state = cancelled() ? "cancelled" : "failed"
      st.phase = "failed"
      st.error = r.error ?? "编译失败"
      st.log = r.logPath
      st.finished_at = new Date().toISOString()
      st.updated_at = st.finished_at
      writeBuildState(home, st)
      return { ok: false, error: r.error, logPath: r.logPath }
    } catch (e) {
      const msg = (e as Error).message ?? String(e)
      st.state = cancelled() ? "cancelled" : "failed"
      st.phase = "failed"
      st.error = msg
      st.finished_at = new Date().toISOString()
      st.updated_at = st.finished_at
      writeBuildState(home, st)
      return { ok: false, error: msg }
    }
  }

  const header = [
    `源码编译安装：${candidate.id}（${candidate.kind === "llama_cpp" ? "llama.cpp" : candidate.kind}）`,
    `目标引擎 ${engineId} ｜ 后端 ${device} ｜ 并行 ${jobs} ｜ 离线 ${offline ? "是（缺依赖立即失败）" : "否"}${clean ? " ｜ 清理重建" : ""}`,
    `源码: ${candidate.path}`,
    `任务 id: ${buildId}`,
  ].join("\n")

  if (background) {
    // 分离执行：与后台批次同款做法（不绑会话取消；进程存活即继续）
    void execute()
    return {
      output: [
        header,
        "",
        "[已后台启动] 编译在服务进程内继续（耗时：CPU 后端数分钟，CUDA/Vulkan 后端可能十几分钟到数十分钟）。",
        `查进度：local_infer_source_build(action="status", build_id="${buildId}")`,
        `看日志：local_infer_source_build(action="log", build_id="${buildId}")`,
        `取消：local_infer_source_build(action="cancel", build_id="${buildId}")（阶段之间生效，不会打断正在跑的 cmake）`,
      ].join("\n"),
      data: { build_id: buildId, state: "running", engine_id: engineId, device, background: true },
    }
  }

  const r = await execute()
  const steps = st.steps.map((s) => `  [${s.phase}] ${s.message}`)
  if (r.ok && r.engine) {
    return {
      output: [
        header,
        "",
        ...steps,
        "",
        `✓ 编译完成：引擎 ${engineId}`,
        `  可执行文件: ${r.engine.exe}`,
        `  日志: ${r.logPath ?? "-"}`,
        "",
        "下一步：",
        `  · 启动服务：local_infer_start(engine="${engineId}", profile="${device === "cpu" ? "cpu-small" : "fast"}")`,
        "  · 确认引擎：local_infer_engines(action=\"list\")（已安装列表里会带 build 来源信息）",
      ].join("\n"),
      data: { build_id: buildId, state: "done", engine: r.engine, log: r.logPath, steps: st.steps },
    }
  }
  return {
    output: [header, "", ...steps, "", `✗ 编译未完成：${r.error ?? "未知原因"}`, r.logPath ? `日志: ${r.logPath}（错误信息里已附尾部若干行）` : ""].join("\n"),
    data: { build_id: buildId, state: st.state, error: r.error, log: r.logPath, steps: st.steps },
  }
}

/** 状态输出：含属主存活判定（服务重启后不算「在跑」）。 */
async function statusOf(home: string, id: string, ctx: ToolContext): Promise<{ output: string; data?: unknown }> {
  const st = readBuildState(home, id)
  if (!st) {
    const list = readBuildStates(home)
    return {
      output: `未找到编译任务 ${id}。${list.length ? `已知任务：${list.map((s) => s.build_id).join("、")}` : "（暂无编译任务）"}`,
    }
  }
  let live = st.state === "running"
  if (live && st.owner_pid !== process.pid) {
    const r = await ctx.runCommand(
      process.platform === "win32" ? `tasklist /FI "PID eq ${st.owner_pid}" /FO CSV /NH` : `kill -0 ${st.owner_pid} 2>/dev/null`,
      { timeoutMs: 15000 },
    )
    const alive = process.platform === "win32" ? new RegExp(`"${st.owner_pid}"`).test(r.stdout) : r.code === 0
    if (!alive) {
      live = false
      st.state = st.state === "running" ? "failed" : st.state
      st.error = st.error ?? `执行进程已退出（PID ${st.owner_pid}）——服务重启或进程被杀，编译结果不可知`
      st.finished_at = st.finished_at ?? new Date().toISOString()
      writeBuildState(home, st)
    }
  }
  const lines = [
    `编译任务 ${st.build_id}`,
    `状态 ${st.state}${live ? "（进行中）" : ""} ｜ 阶段 ${st.phase} ｜ 源码 ${st.source} ｜ 引擎 ${st.engine_id} ｜ 后端 ${st.device} ｜ 并行 ${st.jobs}`,
    `起于 ${st.started_at}${st.finished_at ? ` ｜ 结束于 ${st.finished_at}` : ""}`,
  ]
  if (st.exe) lines.push(`产物: ${st.exe}`)
  if (st.log) lines.push(`日志: ${st.log}`)
  if (st.error) lines.push(`错误: ${st.error.split("\n").slice(0, 12).join("\n  ")}`)
  if (st.steps.length) {
    lines.push("", "步骤：")
    for (const s of st.steps.slice(-8)) lines.push(`  [${s.phase}] ${s.message}`)
  }
  return { output: lines.join("\n"), data: { ...st, live } }
}

const source_build: Tool = {
  name: "source_build",
  safeMode: false, // 会调用编译器并写 vendor/ 落盘
  requiresApproval: true,
  description:
    "把发现的源码**就地编译**成推理引擎（内网/离线部署的主力：预编译包需要外网，这里只依赖本机源码 + 工具链）。" +
    "流程：解压(如需) → 工具链与设备门禁 → cmake 配置 → 编译 → 定位 llama-server → 安装到 vendor/<engine-id>/ → 写引擎标记" +
    "（与下载安装的引擎同一落点与格式，因此 local_infer_start/status 无需区分来源）。" +
    "action=build（缺省）编译；background=true 立即返回 build_id（CPU 数分钟、CUDA 等十几分钟以上，建议后台）；" +
    "action=status 看进度/存活；action=log 看构建日志尾部；action=list 列历史；action=cancel 取消（阶段之间生效）。" +
    "offline 缺省 true（内网：缺依赖立即失败并给出补齐办法，不挂死网络）。",
  parameters: schema({
    action: { type: "string", enum: ["build", "status", "log", "list", "cancel"], description: "操作（缺省 build）" },
    source: { type: "string", description: "候选 id 或源码目录/归档路径（build 用；先 local_infer_sources 查看）" },
    engine_id: { type: "string", description: "目标引擎 id（缺省 <平台>-<后端>-<架构>，如 linux-cpu-x64；同名已安装时请显式换个 id 避免覆盖）" },
    device: { type: "string", enum: [...BUILD_DEVICES], description: "后端（缺省 cpu；cuda/vulkan/metal/rocm 需对应工具链）" },
    jobs: { type: "number", description: "并行编译任务数（缺省 = CPU 核数的 75%）" },
    clean: { type: "boolean", description: "删除已有构建目录后重配（缺省 false，增量构建）" },
    offline: { type: "boolean", description: "离线模式（缺省 true）：禁止联网取依赖；显式 false 才允许 FetchContent 联网" },
    extra_cmake_args: { type: "array", items: { type: "string" }, description: "额外 CMake 参数（如 -DCMAKE_CUDA_ARCHITECTURES=89）；排在预设之后，可覆盖同名项" },
    background: { type: "boolean", description: "build 是否后台执行（缺省 false 阻塞等待；长编译建议 true）" },
    build_id: { type: "string", description: "status/log/cancel 的任务 id" },
    lines: { type: "number", description: "log 动作的尾部行数（缺省 60）" },
  }, ["action"]),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    if (!existsSync(home)) return { output: `未找到本地推理子项目：${home}（可用 LOCAL_INFER_HOME 指定）` }
    const action = String(args.action ?? "build")

    if (action === "build") return await runBuild(args, ctx, home)

    if (action === "status") {
      const id = args.build_id != null ? String(args.build_id) : ""
      if (!id) return { output: "status 需要 build_id（用 action=\"list\" 查看）" }
      return await statusOf(home, id, ctx)
    }

    if (action === "log") {
      const id = args.build_id != null ? String(args.build_id) : ""
      const st = id ? readBuildState(home, id) : null
      if (!st) return { output: `未找到编译任务 ${id || "(未给 build_id)"}` }
      if (!st.log || !existsSync(st.log)) {
        const dir = reportsDir(home)
        return { output: `任务 ${id} 尚未产生构建日志（阶段 ${st.phase}）；日志目录：${dir}` }
      }
      const tail = tailLines(st.log, Number(args.lines ?? 60))
      return {
        output: `构建日志：${st.log}（尾部 ${Number(args.lines ?? 60)} 行${tail.truncated ? "，已截断" : ""}）\n\n${tail.text}`,
        data: { build_id: id, log: st.log, phase: st.phase, state: st.state },
      }
    }

    if (action === "list") {
      const list = readBuildStates(home)
      if (!list.length) return { output: `暂无编译任务记录。编译：local_infer_source_build(source="<候选 id>", device="cpu")` }
      const lines = [`编译任务（${list.length} 个，新→旧）：`, ""]
      for (const s of list) {
        const live = s.state === "running" && s.owner_pid === process.pid ? "进行中" : s.state
        lines.push(`  ${s.build_id}  [${live}]  ${s.source} → ${s.engine_id}（${s.device}）  ${s.steps.length} 步  ${s.started_at}`)
      }
      lines.push("", "详情：local_infer_source_build(action=\"status\", build_id=\"…\")；日志：action=\"log\"")
      return { output: lines.join("\n"), data: { count: list.length, builds: list } }
    }

    if (action === "cancel") {
      const id = args.build_id != null ? String(args.build_id) : ""
      const st = id ? readBuildState(home, id) : null
      if (!st) return { output: `未找到编译任务 ${id || "(未给 build_id)"}` }
      if (st.state !== "running") return { output: `任务 ${st.build_id} 已是 ${st.state} 状态，无需取消。` }
      mkdirSync(buildDirOf(home), { recursive: true })
      writeFileSync(cancelPath(home, id), new Date().toISOString(), "utf-8")
      if (st.owner_pid !== process.pid) {
        st.state = "cancelled"
        st.phase = "cancelled"
        st.error = "执行进程已不在（服务重启后取消）"
        st.finished_at = new Date().toISOString()
        st.updated_at = st.finished_at
        writeBuildState(home, st)
        return { output: `任务 ${st.build_id} 的执行进程已退出，已标记为 cancelled。` }
      }
      return {
        output: [
          `已写入取消标记：${st.build_id}`,
          "编译会在**阶段之间**检查并停止（正在跑的 cmake 不会被强杀——那会留下半成品构建目录）；随后用 action=\"status\" 确认。",
        ].join("\n"),
        data: { build_id: st.build_id, cancelled: "marked" },
      }
    }

    return { output: `未知 action "${action}"——可用：build / status / log / list / cancel` }
  },
}

export const tools: Record<string, Tool> = { sources, source_build }
export const requiresApproval: Record<string, boolean> = { source_build: true }
