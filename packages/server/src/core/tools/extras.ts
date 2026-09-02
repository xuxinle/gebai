/** 子Agent 命名空间专属工具（git/page_capture/read_feedback/system_info/env_detect/preview_server）——
 *  不进全局表（DESIGN：编码工作流归 code/explore 命名空间、反馈通道归 self_optimize），由各子 Agent def
 *  以 `{agent}_{tool}` 命名空间引用。自 core/tools.ts 按域拆分。 */
import { connect, createServer, type AddressInfo } from "node:net"
import { homedir, tmpdir } from "node:os"
import { dirname, join } from "node:path"
import type { ContentBlock } from "@gebai/sdk"
import type { Tool, ToolResult } from "../base/types"
import { isBinaryMode } from "../base/config"
import { truncate } from "../support/truncate"
import { schema } from "./shared"

/** 前端捕获 html 截断长度（WS 传输与落盘上限；完整 DOM 通常远超此，超出截取首部）。 */
export const PAGE_CAPTURE_HTML_LIMIT = 300 * 1024

export const pageCaptureTool: Tool = {
  name: "page_capture",
  // 仅实时前端可用（请求当前页面捕获并由前端回传），多轮交互/无交互模式禁用
  interaction: "realtime",
  description:
    "请求前端（当前浏览器页面）捕获实际渲染结果：读取渲染后的 DOM html 与页面截图，产物落盘会话 tmp/capture/。适合验证 Web UI 修改后的真实效果——页面即当前打开的 歌白界面（dev 模式修改后自动热更新，捕获前可提示用户刷新页面）；html 用 read 读取完整内容，截图用 read 直接查看（主模型多模态时图片内联进上下文）或 vision 工具分析视觉效果。前端离线或 30 秒未响应时返回失败。",
  parameters: schema({
    fullPage: { type: "boolean", description: "是否截整页（默认 false 截视口首屏；整页含全部滚动内容，大页面截图较慢）" },
    delay: { type: "number", description: "捕获前等待毫秒数（默认 0；UI 操作/动画/异步渲染完成后截图，上限 10000）" },
  }),
  async execute(args, ctx) {
    const delayMs = Math.max(0, Math.min(10000, Number(args.delay) || 0))
    const cap = await ctx.waitForCapture({ fullPage: args.fullPage === true, delayMs })
    if (!cap) return { output: "页面捕获失败：前端未能在限定时间内完成捕获（前端离线或捕获超时）。请确认浏览器页面已打开后重试。" }
    if (cap.error) return { output: `页面捕获失败: ${cap.error}` }
    const ts = Date.now()
    const htmlRel = `tmp/capture/page-${ts}.html`
    await ctx.writeFile(ctx.resolvePath(htmlRel), cap.html)
    const blocks: ContentBlock[] = [{ type: "file", path: htmlRel, name: `page-${ts}.html`, mime: "text/html" }]
    let imgRel = ""
    if (cap.imageBase64) {
      // data URL 与裸 base64 均接受（png/jpeg）；非法字符集/解码为空按无截图处理
      const m = cap.imageBase64.match(/^data:(image\/(?:png|jpeg));base64,/)
      const isJpeg = m?.[1] === "image/jpeg"
      const b64 = (m ? cap.imageBase64.slice(m[0].length) : cap.imageBase64).replace(/\s/g, "")
      const buf = /^[A-Za-z0-9+/=]+$/.test(b64) ? Buffer.from(b64, "base64") : Buffer.alloc(0)
      if (buf.byteLength > 0) {
        imgRel = `tmp/capture/page-${ts}.${isJpeg ? "jpg" : "png"}`
        const abs = ctx.resolvePath(imgRel)
        const { mkdir, writeFile } = await import("node:fs/promises")
        await mkdir(dirname(abs), { recursive: true })
        await writeFile(abs, buf)
        blocks.push({ type: "image", path: imgRel, name: imgRel.split("/").pop()!, mime: isJpeg ? "image/jpeg" : "image/png" })
      }
    }
    return {
      output: `已捕获当前页面: ${htmlRel}（${cap.html.length} 字符，可用 read 读取完整内容）${imgRel ? `；截图 ${imgRel}（${ctx.multimodal ? "可用 read 直接查看（多模态内联）" : "可用 vision 工具分析图片内容"}）` : "；前端未返回截图"}`,
      blocks,
    }
  },
}

// save_tool/delete_tool（HTML 小工具库）已下沉 widgets 子Agent：core/mini-tools.ts 提供存储实现，
// widgets_save/widgets_list/widgets_get/widgets_delete 以子Agent 命名空间暴露（与模型工具语义区分）
// draw/render_html/show_file 三工具已合并为 show（内容统一展示入口）：图表/HTML/文件三分支
/** git 只读工具：log 条数默认与上限。 */
const GIT_DEFAULT_LOG = 10
const GIT_MAX_LOG = 50

/** git 命令内嵌参数（ref/path）安全字符校验：拒绝 shell/cmd 元字符（引号拼接注入、& | 管道、% 变量展开、^ 转义等）。 */
function safeGitArg(v: string): boolean {
  return v.trim() !== "" && !/["&|<>^%`\r\n;]/.test(v)
}

/** git grep pattern 安全校验：双引号内 cmd/shell 活动元字符黑名单（`"` 断引号、`%` 变量展开、`$`/反引号 sh 展开）；
 *  `&`/`|`/`<`/`>` 在双引号内为字面量**放行**——正则交替 `a|b` 等语法需要。 */
function safeGitPattern(v: string): boolean {
  return v.trim() !== "" && !/["%$`\r\n]/.test(v)
}

export const gitTool: Tool = {
  name: "git",
  description:
    "只读 Git 检查（status/diff/log/show/branch/ls-files/grep 七操作，不修改仓库；各操作说明见 action 参数）。写操作（add/commit 等）请用 sh。",
  card: { args: "none" },
  parameters: schema(
    {
      action: { type: "string", enum: ["status", "diff", "log", "show", "branch", "ls-files", "grep"], description: "status 工作区状态 / diff 变更内容 / log 提交历史 / show 查看某提交或文件的完整内容（ref 默认 HEAD）/ branch 本地与远程分支列表 / ls-files 已跟踪文件清单（自动尊重 .gitignore，摸底项目结构快于 glob）/ grep 在**已跟踪文件**中内容搜索（自动尊重 .gitignore——grep 工具不读 .gitignore 的补口；扩展正则 ERE；未 add 的新文件不在结果）" },
      dir: { type: "string", description: "Git 仓库目录（默认会话工作目录）" },
      staged: { type: "boolean", description: "diff 是否查看暂存区（--staged），默认否" },
      maxEntries: { type: "integer", description: "log 条数（默认 10，上限 50）" },
      ref: { type: "string", description: "diff/log/show 的 Git 引用：提交哈希/分支/tag/HEAD~n/范围（main..dev）等（show 默认 HEAD；diff/log 不传则工作区/当前分支）" },
      path: { type: "string", description: "路径过滤（可带目录/文件前缀）：diff/log/show 限定该路径的变更（-- <path>）；ls-files 的路径过滤（前缀或 glob，如 src/、*.test.ts）；grep 的搜索范围限定（可选）" },
      pattern: { type: "string", description: "grep 的搜索模式（扩展正则 ERE，如 error|warn、foo\\.bar）" },
    },
    ["action"],
  ),
  outputSchema: schema({
    action: { type: "string", enum: ["status", "diff", "log", "show", "branch", "ls-files", "grep"] },
    branch: { type: "string", description: "当前分支（仅 status）" },
    ahead: { type: "integer", description: "领先远端提交数（仅 status，无则省略）" },
    behind: { type: "integer", description: "落后远端提交数（仅 status，无则省略）" },
    changes: { type: "array", description: "变更文件（仅 status）", items: schema({ status: { type: "string", description: "git 状态码（如 M/A/??）" }, path: { type: "string" } }, ["status", "path"]) },
    commits: { type: "array", description: "提交历史（仅 log）", items: schema({ hash: { type: "string" }, subject: { type: "string" } }, ["hash", "subject"]) },
    files: { type: "array", description: "文件清单（仅 ls-files）", items: { type: "string" } },
  }, ["action"]),
  async execute(args, ctx) {
    const action = String(args.action)
    const dir = ctx.resolvePath(args.dir ? String(args.dir) : ".")
    // ref/path 内嵌参数（引号拼接）：safeGitArg 元字符校验后拼入命令（path 在 diff/log/show/ls-files/grep 通用）
    const ref = args.ref ? String(args.ref) : ""
    const path = args.path ? String(args.path) : ""
    if (ref && !safeGitArg(ref)) return { output: `git: 非法 ref（含命令元字符）: ${ref}` }
    if (path && !safeGitArg(path)) return { output: `git: 非法 path（含命令元字符）: ${path}` }
    const pathSuffix = path ? ` -- "${path}"` : ""
    const refPart = ref ? ` "${ref}"` : ""
    let cmd = ""
    if (action === "status") cmd = "git status --short --branch"
    else if (action === "diff") cmd = `git diff${args.staged === true ? " --staged" : ""} --no-color${refPart}${pathSuffix}`
    else if (action === "log") {
      const n = Math.min(Math.max(Number(args.maxEntries ?? GIT_DEFAULT_LOG) || GIT_DEFAULT_LOG, 1), GIT_MAX_LOG)
      cmd = `git log --oneline -n ${n}${refPart}${pathSuffix}`
    } else if (action === "show") {
      cmd = `git show --no-color "${ref || "HEAD"}"${pathSuffix}`
    } else if (action === "branch") {
      cmd = "git branch -a -v --no-color"
    } else if (action === "ls-files") {
      cmd = path ? `git ls-files -- "${path}"` : "git ls-files"
    } else if (action === "grep") {
      const pattern = args.pattern ? String(args.pattern) : ""
      if (!pattern.trim()) return { output: "git: grep 需要 pattern（扩展正则 ERE 搜索模式）。" }
      if (!safeGitPattern(pattern)) return { output: `git: 非法 pattern（含引号内活动元字符 " % $ 反引号——请改写模式或用 grep 工具）: ${pattern.slice(0, 60)}` }
      cmd = `git grep -n -I --no-color -E -e "${pattern}"${pathSuffix}`
    } else return { output: `git: 未知操作: ${action}（status/diff/log/show/branch/ls-files/grep）` }
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { workdir: dir })
    if (code !== 0) return { output: `git ${action} 失败（exit ${code}，目录 ${args.dir || "."} 可能不是 Git 仓库）:\n${stderr || stdout}` }
    if (!stdout.trim()) {
      const empty: Record<string, string> = { diff: "（工作区无变更）", status: "（工作区干净）", log: "（无提交记录）", show: "（无内容）", branch: "（无分支）", "ls-files": "（无跟踪文件）", grep: "（无匹配）" }
      const emptyData: Record<string, Record<string, unknown>> = { status: { changes: [] }, log: { commits: [] }, "ls-files": { files: [] } }
      return { output: empty[action] ?? "（无输出）", data: { action, ...(emptyData[action] ?? {}) } }
    }
    if (action === "status") {
      const lines = stdout.split("\n").filter(Boolean)
      const branchLine = lines[0]?.startsWith("##") ? lines[0].slice(2).trim() : ""
      const m = branchLine.match(/^(\S+?)(?:\.\.\.)?(?:\s+\[ahead (\d+)(?:, behind (\d+))?\])?/)
      const rest = lines.filter((l) => !l.startsWith("##"))
      return {
        ...(await truncate(stdout, "git", ctx)),
        data: {
          action,
          branch: m?.[1] ?? branchLine,
          ...(m?.[2] ? { ahead: Number(m[2]) } : {}),
          ...(m?.[3] ? { behind: Number(m[3]) } : {}),
          changes: rest.map((l) => ({ status: l.slice(0, 2).trim(), path: l.slice(3).trim() })),
        },
      }
    }
    if (action === "log") {
      const commits = stdout.split("\n").filter(Boolean).map((l) => {
        const i = l.indexOf(" ")
        return i < 0 ? { hash: l, subject: "" } : { hash: l.slice(0, i), subject: l.slice(i + 1) }
      })
      return { ...(await truncate(stdout, "git", ctx)), data: { action, commits } }
    }
    if (action === "ls-files") {
      const files = stdout.split("\n").map((l) => l.trim()).filter(Boolean)
      return { ...(await truncate(files.join("\n"), "git", ctx)), data: { action, files: files.slice(0, 2000) } }
    }
    return { ...(await truncate(stdout, "git", ctx)), data: { action } }
  },
}
/** 用户反馈查询工具（反馈闭环）：self_optimize 等子Agent 经本工具读取用户反馈，
 *  反馈数据才真正进入 Agent 上下文（此前仅有 REST/WS 通道，Agent 无法消费）。 */
export const readFeedbackTool: Tool = {
  name: "read_feedback",
  description:
    "读取用户提交的反馈（本用户，按时间倒序）。用于自我优化等场景了解用户对既往输出的评价（点赞/点踩/文字反馈/建议）与改进点。",
  parameters: schema(
    {
      limit: { type: "integer", description: "返回条数（默认 10，上限 50）" },
      sessionId: { type: "string", description: "可选：仅返回该会话的反馈" },
    },
  ),
  outputSchema: schema({
    items: {
      type: "array",
      description: "反馈列表（按时间倒序）",
      items: schema({
        type: { type: "string", description: "thumbs_up/thumbs_down/suggestion/text" },
        createdAt: { type: "integer", description: "毫秒时间戳" },
        sessionId: { type: "string" },
        messageId: { type: "string" },
        label: { type: "string" },
        subAgent: { type: "string" },
        text: { type: "string" },
      }, ["type", "createdAt"]),
    },
  }, ["items"]),
  async execute(args, ctx) {
    const { readFeedback } = await import("../../feedback")
    const list = await readFeedback(ctx.home, ctx.user)
    const filtered = args.sessionId ? list.filter((f) => f.sessionId === String(args.sessionId)) : list
    const n = Math.min(Math.max(Number(args.limit ?? 10) || 10, 1), 50)
    const items = filtered.slice(0, n)
    if (!items.length) return { output: args.sessionId ? `该会话暂无反馈记录。` : "暂无反馈记录。", data: { items: [] } }
    const label = (t: string) => (t === "thumbs_up" ? "👍" : t === "thumbs_down" ? "👎" : t === "suggestion" ? "建议" : "文字")
    return {
      output:
        `用户反馈（最近 ${items.length} 条${args.sessionId ? `，会话 ${String(args.sessionId)}` : ""}）：\n` +
        items
          .map((f) => {
            const parts = [
              `- [${new Date(f.createdAt).toISOString().slice(0, 19).replace("T", " ")}] ${label(f.type)}`,
              f.sessionId ? `会话 ${f.sessionId}` : "",
              f.messageId ? `消息 ${f.messageId}` : "",
              f.label ? `标签 ${f.label}` : "",
              f.subAgent ? `子Agent ${f.subAgent}` : "",
            ].filter(Boolean)
            const body = f.text ? `\n  ${f.text.slice(0, 500)}` : ""
            return `${parts.join("，")}${body}`
          })
          .join("\n"),
      data: { items },
    }
  },
}
export const systemInfoTool: Tool = {
  name: "system_info",
  description: "获取系统信息（平台、架构、Node 版本、当前工作目录）。",
  card: { args: "none" },
  parameters: schema({}),
  outputSchema: schema({
    platform: { type: "string" }, arch: { type: "string" }, runtime: { type: "string" }, cwd: { type: "string" }, pid: { type: "integer" },
  }, ["platform", "arch", "runtime", "cwd", "pid"]),
  async execute() {
    const info = {
      platform: process.platform,
      arch: process.arch,
      runtime: `bun ${Bun.version}`,
      cwd: process.cwd(),
      pid: process.pid,
    }
    return { output: JSON.stringify(info, null, 2), data: info }
  },
}
/** 环境探测：一次性输出平台/PATH（去重）与关键工具链版本；Windows 下附 VS Build Tools 与 WebView2 状态。只读，无需审批。 */
export const envDetectTool: Tool = {
  name: "env_detect",
  description:
    "探测当前运行环境：平台/架构、PATH（去重）、关键工具链版本（node/bun/python/git/cargo/rustc/go/docker 等，缺失标记不可用）、Windows 下 VS Build Tools（MSVC，cargo/rustc 依赖）与 WebView2 运行时状态。用于一次判断工具链可用性、指导安装缺失组件，避免逐命令探测。",
  card: { args: "none" },
  parameters: schema({}),
  async execute(_args, ctx) {
    const probe = async (label: string, cmd: string): Promise<string> => {
      try {
        const r = await ctx.runCommand(cmd, { timeoutMs: 8000 })
        if (r.code !== 0) return `${label}: 不可用（exit ${r.code}）`
        const v = r.stdout.trim().split(/\r?\n/)[0].trim()
        return `${label}: ${v || "（成功但无版本输出）"}`
      } catch {
        return `${label}: 探测失败`
      }
    }
    const lines = [`环境探测（${process.platform} ${process.arch}）`]
    lines.push(
      ...(await Promise.all([
        probe("node", "node --version"),
        probe("bun", "bun --version"),
        probe("python", "python --version"),
        probe("git", "git --version"),
        probe("cargo", "cargo --version"),
        probe("rustc", "rustc --version"),
        probe("go", "go version"),
        probe("docker", "docker --version"),
      ])),
    )
    if (process.platform === "win32") {
      const vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe"
      try {
        const { existsSync } = await import("node:fs")
        if (existsSync(vswhere)) {
          const r = await ctx.runCommand(
            `"${vswhere}" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`,
            { timeoutMs: 8000 },
          )
          lines.push(`VS Build Tools: ${r.stdout.trim() || "未找到 MSVC 工具链（cargo/rustc 编译需要，建议安装 Visual Studio Build Tools 并勾选 C++ 工作负载）"}`)
        } else {
          lines.push("VS Build Tools: vswhere 不存在（未安装 Visual Studio / Build Tools，cargo/rustc 编译需要 MSVC）")
        }
      } catch {
        lines.push("VS Build Tools: 探测失败")
      }
      try {
        const r = await ctx.runCommand(
          'reg query "HKLM\\SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" /v pv',
          { timeoutMs: 8000 },
        )
        lines.push(`WebView2 运行时: ${(r.stdout.trim().split(/\s+/).pop() ?? "").trim() || "已安装（版本未知）"}`)
      } catch {
        lines.push("WebView2 运行时: 未检测到")
      }
    }
    // PATH 去重（Windows 风格路径大小写不敏感——按路径形态判定而非宿主平台，Linux 下模拟/探测 Windows PATH 同样生效；空项剔除）
    const pathVar = (ctx.env.PATH || process.env.PATH || "").split(";")
    const looksWindows = (p: string) => p.includes("\\") || /^[A-Za-z]:/.test(p)
    const seen = new Set<string>()
    const uniq = pathVar.filter((p) => {
      if (!p) return false
      const k = looksWindows(p) ? p.toLowerCase() : p
      if (seen.has(k)) return false
      seen.add(k)
      return true
    })
    lines.push(`PATH（去重 ${uniq.length}/${pathVar.filter(Boolean).length} 项）：`)
    lines.push(...uniq.map((p) => `  - ${p}`))
    return truncate(lines.join("\n"), "env_detect", ctx)
  },
}
const PREVIEW_START_TIMEOUT_MS = 15000
const PREVIEW_POLL_INTERVAL_MS = 300
const PREVIEW_STATE_FILE = "gebai-preview.json"

/** 预览服务状态记录（写入 os.tmpdir()/gebai-preview.json，跨进程可见）。 */
export interface PreviewServerEntry {
  port: number
  pid: number
  url: string
  log: string
  startedAt: number
}

interface PreviewServerDeps {
  host: string
  /** 脚本模式服务端入口（默认 packages/server/src/index.ts）。 */
  entry: string
  /** 二进制模式：spawn 自身可执行文件（环境变量覆盖 GEBAI_PORT）。 */
  binary: boolean
  /** 状态文件与日志目录（默认系统临时目录）。 */
  tmpDir: string
  timeoutMs: number
  intervalMs: number
}

function previewServerDeps(overrides: Partial<PreviewServerDeps> = {}): PreviewServerDeps {
  return {
    host: process.env.GEBAI_HOST || "127.0.0.1",
    entry: join(import.meta.dirname, "..", "..", "index.ts"),
    binary: isBinaryMode(),
    tmpDir: tmpdir(),
    timeoutMs: PREVIEW_START_TIMEOUT_MS,
    intervalMs: PREVIEW_POLL_INTERVAL_MS,
    ...overrides,
  }
}

function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

/** TCP 探测端口是否可连接（就绪探测与占用检查统一走 127.0.0.1，不受绑定地址影响）。 */
function isPortOpen(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const sock = connect({ host: "127.0.0.1", port })
    sock.setTimeout(1000)
    sock.once("connect", () => {
      sock.destroy()
      resolve(true)
    })
    sock.once("error", () => {
      sock.destroy()
      resolve(false)
    })
    sock.once("timeout", () => {
      sock.destroy()
      resolve(false)
    })
  })
}

function findFreePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const srv = createServer()
    srv.unref()
    srv.once("error", reject)
    srv.listen(0, "127.0.0.1", () => {
      const { port } = srv.address() as AddressInfo
      srv.close(() => resolve(port))
    })
  })
}

async function waitPortOpen(port: number, timeoutMs: number, intervalMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (await isPortOpen(port)) return true
    await new Promise((r) => setTimeout(r, intervalMs))
  }
  return false
}

/** 读取日志文件尾部（失败/未就绪时的诊断信息）。 */
async function logTail(log: string, lines = 20): Promise<string> {
  try {
    const content = await Bun.file(log).text()
    return content.split("\n").slice(-lines).join("\n")
  } catch {
    return ""
  }
}

async function loadPreviewState(tmpDir: string): Promise<PreviewServerEntry[]> {
  try {
    const raw = await Bun.file(join(tmpDir, PREVIEW_STATE_FILE)).json()
    if (!Array.isArray(raw)) return []
    return (raw as PreviewServerEntry[]).filter((e) => typeof e.pid === "number" && pidAlive(e.pid))
  } catch {
    return []
  }
}

async function savePreviewState(tmpDir: string, entries: PreviewServerEntry[]): Promise<void> {
  const { writeFile } = await import("node:fs/promises")
  try {
    await writeFile(join(tmpDir, PREVIEW_STATE_FILE), JSON.stringify(entries, null, 2))
  } catch {
    /* 状态文件写失败不影响服务本身 */
  }
}

async function killPid(pid: number): Promise<boolean> {
  try {
    process.kill(pid, "SIGTERM")
  } catch {
    return !pidAlive(pid)
  }
  for (let i = 0; i < 25; i++) {
    if (!pidAlive(pid)) return true
    await new Promise((r) => setTimeout(r, 200))
  }
  try {
    process.kill(pid, "SIGKILL")
  } catch {
    /* 已退出 */
  }
  return !pidAlive(pid)
}

function displayHostFor(host: string): string {
  return host === "0.0.0.0" || host === "::" || host === "::0" ? "127.0.0.1" : host
}

async function startPreview(deps: PreviewServerDeps, rawPort?: unknown): Promise<ToolResult> {
  const port = rawPort === undefined ? await findFreePort() : Number(rawPort)
  if (!Number.isInteger(port) || port < 1 || port > 65535) return { output: `无效端口: ${rawPort}` }
  if (await isPortOpen(port)) return { output: `端口 ${port} 已被占用，请换一个端口或省略 port 自动选取。` }
  const log = join(deps.tmpDir, `gebai-preview-${port}.log`)
  const cmd = deps.binary ? [process.execPath] : [process.execPath, deps.entry]
  const proc = Bun.spawn({
    cmd,
    env: { ...process.env, GEBAI_PORT: String(port) },
    cwd: deps.binary ? homedir() : process.cwd(),
    stdout: Bun.file(log),
    stderr: Bun.file(log),
    detached: true,
  })
  const up = await waitPortOpen(port, deps.timeoutMs, deps.intervalMs)
  if (!up) {
    await killPid(proc.pid)
    const tail = await logTail(log)
    return { output: `验证服务启动失败（端口 ${port}）${tail ? `，日志尾部：\n${tail}` : ""}` }
  }
  const host = displayHostFor(deps.host)
  const url = `http://${host}:${port}`
  const state = await loadPreviewState(deps.tmpDir)
  const entry: PreviewServerEntry = { port, pid: proc.pid, url, log, startedAt: Date.now() }
  await savePreviewState(deps.tmpDir, [...state.filter((e) => e.port !== port), entry])
  return {
    output: `验证服务已启动（独立进程，不中断当前会话）：${url}\nPID: ${proc.pid}\n日志: ${log}\n验证完成后用 preview_server action=stop 停止（pid=${proc.pid}）。`,
  }
}

async function stopPreview(deps: PreviewServerDeps, rawPid?: unknown, rawPort?: unknown): Promise<ToolResult> {
  const state = await loadPreviewState(deps.tmpDir)
  const pid = rawPid !== undefined ? Number(rawPid) : undefined
  const port = rawPort !== undefined ? Number(rawPort) : undefined
  const target = pid ? state.find((e) => e.pid === pid) : port ? state.find((e) => e.port === port) : undefined
  if (!target) {
    const hint = pid ? `PID ${pid}` : port ? `端口 ${port}` : ""
    return { output: hint ? `未找到 ${hint} 对应的预览服务（可能已停止）。` : "停止预览服务需要提供 pid 或 port。" }
  }
  const dead = await killPid(target.pid)
  await savePreviewState(deps.tmpDir, state.filter((e) => e.pid !== target.pid))
  return { output: dead ? `预览服务已停止：${target.url}（PID ${target.pid}）` : `预览服务停止失败（PID ${target.pid}），请手动 kill。` }
}

/** 预览/验证服务：在临时新端口启动一份独立进程（不中断当前会话），供用户验证改动；依赖注入便于测试。 */
export function makePreviewServerTool(overrides: Partial<PreviewServerDeps> = {}): Tool {
  const deps = previewServerDeps(overrides)
  return {
    name: "preview_server",
    description: "在临时新端口启动/停止一份 歌白验证服务（独立进程，不中断当前会话与主服务），供用户验证代码改动。action=start（默认）启动并返回访问 URL/PID/日志路径；action=stop 停止（pid 或 port 指定）。验证完必须停止，避免残留进程。",
    parameters: schema({
      action: { type: "string", enum: ["start", "stop"], description: "操作（默认 start）" },
      port: { type: "number", description: "启动/停止目标端口（启动时默认自动选取空闲端口）" },
      pid: { type: "number", description: "停止时指定进程 PID（与 port 二选一）" },
    }),
    async execute(args) {
      if (args.action === "stop") return stopPreview(deps, args.pid, args.port)
      return startPreview(deps, args.port)
    },
  }
}
