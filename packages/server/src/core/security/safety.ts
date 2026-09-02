/**
 * 安全模式（DESIGN「安全模式」）：GEBAI_SAFE_MODE=true 启动时加载的**降级运行形态**——风险能力降级而非一刀切禁用：
 * - `sh`：只读命令白名单 + 输出重定向限安全写范围（validateShCommandSafeMode，工具 execute 前调用）
 * - `js`：静态扫描（scanJsReadOnly 拦 import/require/eval/Function 等运行时无法拦截的通道）+
 *   子进程运行时 shim（Bun 写/进程/网络 API 屏蔽，仅保留文件读取）
 * - `py`：子进程内 sys.addaudithook 审计钩子（PY_SAFE_BOOTSTRAP）：写模式 open、进程/网络/文件变更系统调用全部拒绝
 * - `write`/`edit`/`patch`/`file`：限定安全写范围内（safeModeWriteCheck）
 * - 定时任务调度（cron_add/update/remove/trigger）：维持硬阻断（定时/立即触发任意执行，无法降级）
 * - 子Agent 工具：自主声明 `Tool.safeMode`（true=作者判定可提供 / false=判定不提供），未声明按短名风险规则默认
 *
 * 引擎主/子循环、flow step 层与 js 脚本工具 RPC 分发层共用 isToolBlockedInSafeMode（三者直接/间接执行工具，拦截规则须一致）。
 * 「安全写范围」：沙箱模式=用户数据根（users/{user}，会话 tmp 已在其中）；本地模式=OS 用户主目录 + GEBAI_HOME + 会话工作目录。
 */
import { homedir } from "node:os"
import { join, resolve, sep } from "node:path"

/** 安全模式下硬阻断的工具（无法降级）：定时任务调度/手动触发可立即或延迟触发任意 shell/js 执行。 */
export const SAFE_MODE_BLOCKED_TOOLS = new Set(["cron_add", "cron_update", "cron_remove", "cron_trigger"])

/** 工具是否被安全模式硬阻断（精确名或子Agent 命名空间前缀命中，如 my_cron_add）：引擎/flow step 层/js RPC 分发层共用。 */
export function isToolBlockedInSafeMode(name: string): boolean {
  for (const b of SAFE_MODE_BLOCKED_TOOLS) {
    if (name === b || name.endsWith(`_${b}`)) return true
  }
  return false
}

/** 短名风险规则（安全模式下子Agent 工具的**默认**注册判定；`Tool.safeMode` 声明可覆盖）：
 *  命令执行（sh/py/js）/写删文件/定时任务调度的短名视为默认不提供。 */
export const SAFE_MODE_RISKY_TOOLS = new Set([
  "sh", "py", "js", "write", "edit", "patch", "file", "delete",
  "cron_add", "cron_update", "cron_remove", "cron_trigger",
])

/** 子Agent 工具短名是否命中默认风险规则（如 code_sh → sh、widgets_delete → delete、my_cron_add → cron_add）。
 *  按完整短名后缀匹配（`_${risk}` endsWith），多段风险名（cron_add）同样命中。 */
export function isRiskyToolName(name: string): boolean {
  for (const b of SAFE_MODE_RISKY_TOOLS) {
    if (name === b || name.endsWith(`_${b}`)) return true
  }
  return false
}

/** 安全模式硬阻断的限制信息（引擎与 flow 工具共用，措辞一致）。 */
export function safeModeRestrictionMsg(name: string): string {
  return `安全模式：工具 ${name} 已限制（定时任务调度类无法降级为只读，安全模式下不提供）。请改用只读方式（如 read/grep/fetch_url），或直接给出分析与建议。`
}

// ---------------------------------------------------------------------------
// 安全写范围（write/edit/patch/file 与 sh 重定向目标共用）
// ---------------------------------------------------------------------------

/** 安全模式下允许写入的根目录列表：沙箱模式=用户数据根；本地模式=OS 用户主目录 + GEBAI_HOME + 会话工作目录。 */
export function safeModeWriteRoots(ctx: { sandboxed?: boolean; home: string; user: string; workdir?: string }): string[] {
  if (ctx.sandboxed) return [join(ctx.home, "users", ctx.user)]
  const roots = [homedir(), ctx.home, ctx.workdir ?? ctx.home]
  return [...new Set(roots.map((r) => resolve(r)))]
}

function pathWithinRoots(p: string, roots: string[]): boolean {
  const abs = resolve(p)
  const cmp = process.platform === "win32" ? (s: string) => s.toLowerCase() : (s: string) => s
  return roots.some((r) => cmp(abs) === cmp(r) || cmp(abs).startsWith(`${cmp(r)}${sep}`) || cmp(abs).startsWith(`${cmp(r)}/`))
}

/** 安全模式写范围校验：全部路径在范围内返回 null，否则返回拒绝消息（作为工具结果返回，模型可调整路径重试）。 */
export function safeModeWriteCheck(paths: string[], ctx: { sandboxed?: boolean; home: string; user: string; workdir?: string }): string | null {
  const roots = safeModeWriteRoots(ctx)
  for (const p of paths) {
    if (!pathWithinRoots(p, roots)) {
      const shown = roots.map((r) => r.replace(/[\\/]+$/, "")).join("、")
      return `安全模式：文件写入限定在用户目录内（${shown}），${p} 越界。请改用用户目录内路径。`
    }
  }
  return null
}

// ---------------------------------------------------------------------------
// sh 只读命令白名单（安全模式）
// ---------------------------------------------------------------------------

/** 只读命令白名单（POSIX 与 Windows 并集；交互式分页器 less/more 有 shell 逃逸（! 前缀）不入列，
 *  sed/awk 有脚本内写/执行通道（sed w 命令、awk system()/重定向）不入列——流式变换用降级后的 py/js）。
 *  git 仅读子命令可用（GIT_READ_SUBCOMMANDS）；find 禁 -exec/-delete 等执行/写动作。 */
export const SH_SAFE_MODE_ALLOW = new Set([
  "cat", "head", "tail", "grep", "egrep", "fgrep", "find", "findstr", "rg", "ls", "dir", "tree", "pwd",
  "echo", "printf", "wc", "file", "stat", "du", "df", "sort", "uniq", "cut", "tr", "comm", "join", "paste",
  "rev", "fold", "expand", "unexpand", "nl", "column", "base32", "base64", "xxd", "od", "hexdump",
  "md5sum", "sha1sum", "sha256sum", "sha512sum", "cksum", "diff", "diff3", "cmp", "basename", "dirname", "realpath",
  "whoami", "id", "groups", "uname", "date", "uptime", "env", "printenv", "locale", "which", "where", "whereis", "hostname",
  "command", "type", "true", "false", "ps", "tasklist", "systeminfo", "ver", "vol", "free", "vmstat", "iostat", "lsof",
  "git",
])

/** git 读子命令（首个非 flag 参数须在此集合内；branch -d/remote add/config --set 等变更动作不通过）。 */
const GIT_READ_SUBCOMMANDS = new Set([
  "status", "log", "diff", "show", "blame", "rev-parse", "ls-files", "describe", "shortlog", "reflog", "grep", "cat-file",
])

/** approval:false 免审放行判定用的子命令白名单（见 shApprovalFreeAllowed）。 */
const APPROVAL_FREE_ANY_ARGS = new Set([
  "pytest", "py.test", "vitest", "jest", "mocha", "karma",
  "tsc", "eslint", "biome", "stylelint", "ruff", "mypy", "flake8", "prettier",
])
/** 需要子命令联判的命令：git 仅读子命令；包管理器 test / run <脚本名>；语言工具链 test 子命令。 */
const APPROVAL_FREE_GIT_READ = GIT_READ_SUBCOMMANDS
const APPROVAL_FREE_PKG_MGR = new Set(["npm", "pnpm", "yarn", "bun"])
const APPROVAL_FREE_TEST_SUBCMD = new Set(["go", "cargo", "mvn", "gradle"])

/** find 的执行/写动作参数（出现即拒绝）。 */
const FIND_WRITE_FLAGS = /^-(exec|execdir|delete|fprint|fprintf|fls)/

/** sort 的输出文件参数（-o/--output 写文件）。 */
const SORT_OUTPUT_FLAG = /^(--output(=|$)|-o($|.))/

/** 安全模式下允许的重定向目标特殊值（空设备，非数据文件）。 */
const REDIRECT_SINK_OK = new Set(["/dev/null", "nul"])

/** 命令归一化：取基础名（路径形式调用如 /bin/cat）、剥离 Windows .exe 后缀、小写。 */
function normalizeCmdName(token: string): string {
  const base = token.replace(/[\\/]+/g, "/").split("/").pop() ?? token
  return base.replace(/\.exe$/i, "").toLowerCase()
}

interface ShSegment {
  name: string
  args: string[]
  redirectTargets: string[]
}

const SH_DENY_PREFIX = "安全模式：sh 仅允许只读命令（白名单：cat/head/tail/grep/find/ls/git 等查看与文本处理类，完整清单见 DESIGN「安全模式」）；"

/**
 * shell 子集解析与只读校验（fail-closed：引号外无法识别的结构一律拒绝），返回 null=放行、非空=拒绝消息。
 * 支持：`&&` `||` `;` `|` 换行分隔的多命令、引号（单引号全字面、双引号内 `$(...)` 与反引号按替换语义递归校验/拒绝）、
 * `\` 转义、`#` 行注释、`$(...)` 命令替换（递归校验）、裸括号子 shell（递归校验）、输入重定向 `<`（读不限制）、
 * 输出重定向 `>` `>>` `2>` `&>`（目标须在安全写范围或空设备）、fd 复制 `2>&1`/`1>&2`（放行）、fd 数字前缀、
 * 环境变量赋值前缀与 `time` 前缀关键字。
 * 拒绝：反引号、单独 `&`、`^`、`<(` `>(` 进程替换按普通重定向/递归处理。
 */
export function validateShCommandSafeMode(cmd: string, ctx: { sandboxed?: boolean; home: string; user: string; workdir?: string }): string | null {
  /** 递归校验（$()/括号块/双引号替换）产生的拒绝消息：parse 内部递归点写入，validateSegmentList 出口统一取用
   *  （不能作为 parse 返回值——其返回类型是段列表，混淆会让拒绝消息被当作段迭代而失效）。 */
  let pendingDeny: string | null = null

  /** 校验一段命令文本（递归入口：$(...)、括号块内容复用）。 */
  const validateSegmentList = (text: string): string | null => {
    pendingDeny = null
    const segs = parse(text)
    if (pendingDeny) return pendingDeny
    if (segs === null) return `${SH_DENY_PREFIX}命令包含无法解析的结构（反引号/后台执行/未闭合引号等）`
    for (const seg of segs) {
      if (!seg.name) continue
      // 赋值前缀（FOO=bar cmd）与 time 前缀关键字：跳过，取下一个 token 作命令名
      if (seg.name === "time" || /^[A-Za-z_][A-Za-z0-9_]*=/.test(seg.name)) {
        const shifted = { name: seg.args[0] ?? "", args: seg.args.slice(1), redirectTargets: seg.redirectTargets }
        if (!shifted.name) continue
        const bad = validateOne(shifted.name, shifted.args, shifted.redirectTargets)
        if (bad) return bad
        continue
      }
      const bad = validateOne(seg.name, seg.args, seg.redirectTargets)
      if (bad) return bad
    }
    return null
  }

  const validateOne = (rawName: string, args: string[], redirectTargets: string[]): string | null => {
    const name = normalizeCmdName(rawName)
    if (!SH_SAFE_MODE_ALLOW.has(name)) return `${SH_DENY_PREFIX}命令 ${rawName} 不在白名单`
    if (name === "git") {
      const sub = args.find((a) => !a.startsWith("-"))
      if (!sub || !GIT_READ_SUBCOMMANDS.has(sub)) return `${SH_DENY_PREFIX}git 仅允许读子命令（status/log/diff/show/blame/rev-parse/ls-files/describe 等）`
    }
    if (name === "find" && args.some((a) => FIND_WRITE_FLAGS.test(a))) {
      return `${SH_DENY_PREFIX}find 不允许 -exec/-execdir/-delete/-fprint 等执行/写动作`
    }
    if (name === "sort" && args.some((a) => SORT_OUTPUT_FLAG.test(a))) {
      return `${SH_DENY_PREFIX}sort 不允许 -o/--output 输出文件`
    }
    if (name === "env" && args.length) return `${SH_DENY_PREFIX}env 携带参数即执行命令，仅允许单独 env`
    if (name === "hostname" && args.some((a) => !a.startsWith("-"))) return `${SH_DENY_PREFIX}hostname 携带非 flag 参数会设置主机名`
    if (name === "date" && args.some((a) => /^(-s|--set)/.test(a))) return `${SH_DENY_PREFIX}date 不允许 -s/--set 设置时间`
    for (const t of redirectTargets) {
      if (REDIRECT_SINK_OK.has(t.replace(/[\\/]+/g, "/").toLowerCase())) continue
      const abs = resolve(ctx.workdir ?? ctx.home, t)
      if (safeModeWriteCheck([abs], ctx)) return `${SH_DENY_PREFIX}重定向目标越界（${t} 须在用户目录内或 /dev/null）`
    }
    return null
  }

  /** 定位 $( 的匹配右括号（引号与嵌套感知）：返回其下标，未闭合返回 -1。 */
  const matchParen = (text: string, open: number): number => {
    let depth = 1
    let j = open + 2
    while (j < text.length) {
      const c = text[j]
      if (c === "'" || c === '"') {
        const q = c
        j++
        while (j < text.length && text[j] !== q) {
          if (q === '"' && text[j] === "\\" && (text[j + 1] === '"' || text[j + 1] === "\\" || text[j + 1] === "$")) j++
          j++
        }
        if (j >= text.length) return -1
        j++
        continue
      }
      if (c === "(") depth++
      else if (c === ")") {
        depth--
        if (depth === 0) return j
      }
      j++
    }
    return -1
  }

  /** 解析命令文本为段列表；返回 null=无法解析（fail-closed）。 */
  const parse = (text: string): ShSegment[] | null => {
    const segs: ShSegment[] = []
    let cur: ShSegment | null = null
    let token = ""
    let collectingRedirectTarget = false
    const flushToken = () => {
      if (!token) return
      if (collectingRedirectTarget) {
        cur?.redirectTargets.push(token)
        collectingRedirectTarget = false
      } else if (cur) {
        if (!cur.name) cur.name = token
        else cur.args.push(token)
      }
      token = ""
    }
    const flushSeg = () => {
      flushToken()
      collectingRedirectTarget = false
      if (cur && (cur.name || cur.redirectTargets.length)) segs.push(cur)
      cur = null
    }
    let i = 0
    const n = text.length
    while (i < n) {
      const ch = text[i]
      // 引号：单引号全字面；双引号内保留 \ 转义，$(...) 按替换语义递归校验、反引号拒绝（POSIX 均会执行）
      if (ch === "'" || ch === '"') {
        const quote = ch
        if (!cur && !token) cur = { name: "", args: [], redirectTargets: [] }
        i++
        while (i < n && text[i] !== quote) {
          const c = text[i]
          if (quote === '"' && c === "\\" && (text[i + 1] === '"' || text[i + 1] === "\\" || text[i + 1] === "$")) {
            token += text[i + 1]
            i += 2
            continue
          }
          if (quote === '"' && c === "`") return null // 双引号内反引号替换：拒绝
          if (quote === '"' && c === "$" && text[i + 1] === "(") {
            const close = matchParen(text, i)
            if (close < 0) return null
            const bad = validateSegmentList(text.slice(i + 2, close))
            if (bad) {
              pendingDeny = bad
              return null
            }
            token += "$()"
            i = close + 1
            continue
          }
          token += c
          i++
        }
        if (i >= n) return null // 未闭合引号
        i++
        continue
      }
      if (ch === "\\") {
        if (i + 1 >= n) return null // 行尾续行不支持
        if (!cur) cur = { name: "", args: [], redirectTargets: [] }
        token += text[i + 1]
        i += 2
        continue
      }
      if (ch === "#" && !token) {
        while (i < n && text[i] !== "\n") i++
        continue
      }
      if (ch === "`") return null // 反引号命令替换：拒绝
      if (ch === "^") return null // cmd 转义符：拒绝
      if (ch === "$" && text[i + 1] === "(") {
        const close = matchParen(text, i)
        if (close < 0) return null
        if (!cur) cur = { name: "", args: [], redirectTargets: [] }
        const bad = validateSegmentList(text.slice(i + 2, close))
        if (bad) {
          pendingDeny = bad
          return null
        }
        token += "$()"
        i = close + 1
        continue
      }
      if (ch === "(") {
        // 子 shell：括号块内容作为独立命令列表递归校验（<( 进程替换在 < 分支先行拒绝）
        const close = matchParen(text, i - 1) // matchParen 从 "$("/"(" 起：open=i-1 使扫描从 i+1 起
        if (close < 0) return null
        flushSeg()
        const bad = validateSegmentList(text.slice(i + 1, close))
        if (bad) {
          pendingDeny = bad
          return null
        }
        i = close + 1
        continue
      }
      if (ch === ")") return null // 顶层裸右括号
      if (ch === "<") {
        if (text[i + 1] === "(") return null // 进程替换
        if (!cur) cur = { name: "", args: [], redirectTargets: [] }
        flushToken() // 输入重定向：读不限制，目标词按普通 token 收集（不参与命令名校验）
        i++
        continue
      }
      if (ch === ">") {
        if (!cur) cur = { name: "", args: [], redirectTargets: [] }
        // fd 数字前缀（2> / 12>>）：整个 token 为纯数字时视为 fd 号丢弃
        if (/^\d+$/.test(token)) token = ""
        flushToken()
        let j = i + 1
        if (text[j] === ">") j++
        if (text[j] === "&") {
          const digit = text[j + 1]
          if (digit >= "0" && digit <= "9") {
            i = j + 2 // fd 复制（2>&1/1>&2）：放行
            continue
          }
          j++ // &> file：等价输出重定向
        }
        i = j
        collectingRedirectTarget = true
        continue
      }
      if (ch === "|" || ch === "&") {
        if (ch === "|" && text[i + 1] === "|") {
          flushSeg()
          i += 2
          continue
        }
        if (ch === "&" && text[i + 1] === "&") {
          flushSeg()
          i += 2
          continue
        }
        if (ch === "|") {
          flushSeg()
          i++
          continue
        }
        return null // 单独 &（后台执行）：拒绝
      }
      if (ch === ";") {
        flushSeg()
        i++
        continue
      }
      if (ch === "\n" || ch === "\r") {
        flushSeg()
        i++
        continue
      }
      if (ch === " " || ch === "\t") {
        flushToken()
        i++
        continue
      }
      if (!cur) cur = { name: "", args: [], redirectTargets: [] }
      token += ch
      i++
    }
    flushSeg()
    return segs
  }

  return validateSegmentList(cmd)
}

// ---------------------------------------------------------------------------
// 审批免审标记剥离（js RPC 分发层与引擎无交互硬门槛共用）
// ---------------------------------------------------------------------------

/** 递归剥离参数中的 approval 免审标记（仅删键，其余原样深拷贝）：`approval:false` 只放宽交互审批，
 *  不得绕过「未经审阅的执行体免审调用需审批工具」的判定——引擎服务模式无交互硬门槛、js 免审运行的
 *  内部工具拦截均按剥离后的审批姿态解析（防脚本内再传 approval:false 自我免审）。 */
export function stripApprovalFlags(v: unknown): unknown {
  if (Array.isArray(v)) return v.map(stripApprovalFlags)
  if (v && typeof v === "object") {
    const out: Record<string, unknown> = {}
    for (const [k, val] of Object.entries(v as Record<string, unknown>)) if (k !== "approval") out[k] = stripApprovalFlags(val)
    return out
  }
  return v
}

/**
 * `approval:false` 免审放行判定（审批门免审例外的强制口径，防提示词注入借免审标记执行任意命令）：
 * 模型可自行传参声明免审，但仅对**只读/测试验证类**命令生效——
 * 1) validateShCommandSafeMode 全量解析通过（只读白名单）→ 放行；
 * 2) 测试/静态检查/包管理器 test 子命令 → 放行（`run` 仅脚本名形态，禁文件路径直跑）；
 * 3) 其余（含命令替换/反引号/输出重定向/无法识别结构）一律不放行（fail-closed，仍走审批）。
 */
export function shApprovalFreeAllowed(cmd: string, ctx: { sandboxed?: boolean; home: string; user: string; workdir?: string }): boolean {
  if (!cmd.trim()) return false
  if (validateShCommandSafeMode(cmd, ctx) === null) return true
  if (/`|\$\(/.test(cmd)) return false
  // 轻量分段（&& || ; | 换行）：引号内分隔符会被多切一段——多切出的段同样要过白名单，方向安全（从严）
  for (const segRaw of cmd.split(/(?:&&|\|\||;|\||\n)/)) {
    const seg = segRaw.trim()
    if (!seg) continue
    if (seg.includes(">")) return false // 输出重定向：写动作，免审不放行
    if (validateShCommandSafeMode(seg, ctx) === null) continue // 该段本身只读（如管道尾的 head/grep）
    const tokens = seg.split(/\s+/).filter(Boolean)
    let i = 0
    while (i < tokens.length && (tokens[i] === "time" || /^[A-Za-z_][A-Za-z0-9_]*=/.test(tokens[i]))) i++
    const name = normalizeCmdName(tokens[i] ?? "")
    const sub = tokens[i + 1] ?? ""
    if (APPROVAL_FREE_ANY_ARGS.has(name)) continue
    if (name === "git") {
      if (APPROVAL_FREE_GIT_READ.has(sub)) continue
      return false
    }
    if (APPROVAL_FREE_PKG_MGR.has(name)) {
      if (sub === "test") continue
      if (sub === "run" && tokens[i + 2] && /^[A-Za-z0-9:_-]+$/.test(tokens[i + 2])) continue // run <脚本名>（package.json scripts；带路径/扩展名的直跑不放行）
      if (name !== "bun" && sub === "ci") continue
      return false
    }
    if (APPROVAL_FREE_TEST_SUBCMD.has(name)) {
      if (sub === "test") continue
      return false
    }
    return false
  }
  return true
}

// ---------------------------------------------------------------------------
// js 只读静态扫描（安全模式）
// ---------------------------------------------------------------------------

/** 运行时 shim 无法可靠拦截的通道（动态模块加载拿到全新全局环境、字符串代码执行）：
 *  按**词元级**拒绝而非调用形态——别名绕过（`const rq = require; rq(...)`）、括号访问（`Bun["fetch"]`）、
 *  静态 import（提升先于 shim 执行）均只认 token 才能封死：
 *  - `import`/`require` 任意出现即拒绝（安全模式无任何合法模块加载形态）；
 *  - `Bun` 仅放行 `Bun.file` 只读通道（fetch/sqlite 为不可覆写 getter、Bun 全局本身 non-configurable，
 *    运行时无法代理，扫描是唯一防线）；
 *  - `getBuiltinModule` 词元拒绝（调用形态外的别名同样拿到未受控模块环境）。 */
const JS_READONLY_DENY: Array<[RegExp, string]> = [
  [/\bimport\b/, "import（静态/动态均屏蔽）"],
  [/\brequire\b/, "require（含别名引用）"],
  [/\bBun\b(?!\s*\.\s*file\b)/, "Bun.*（安全模式仅允许 Bun.file 只读访问；fetch/sqlite 等为不可覆写入口）"],
  [/\beval\s*\(/, "eval()"],
  [/\bFunction\s*\(/, "Function 构造器"],
  [/\bgetBuiltinModule\b/, "process.getBuiltinModule"],
  [/process\s*\.\s*binding/, "process.binding"],
]

/** js 脚本安全模式只读预检：命中不可运行时拦截的通道返回拒绝消息，否则 null。
 *  运行时另有 shim 兜底（Bun 写/进程/网络 API 屏蔽 + require 模块级中和）——静态扫描拦截的是绕过 shim 的源头。 */
export function scanJsReadOnly(code: string): string | null {
  for (const [re, what] of JS_READONLY_DENY) {
    if (re.test(code)) {
      return `安全模式：js 脚本仅保留文件读取，${what} 已屏蔽（动态加载模块可获得未受控的全局环境）。文件写入用 write 工具、命令执行用 sh 白名单、网络用 fetch_url 工具。`
    }
  }
  return null
}

// ---------------------------------------------------------------------------
// py 只读运行时（安全模式）：sys.addaudithook 审计钩子引导段
// ---------------------------------------------------------------------------

/** 追加在用户 py 代码前的安全模式引导：审计钩子拦截写模式 open 与进程/网络/文件变更系统调用。
 *  钩子堆叠不可移除（再注册也只增不减）；ctypes 在 dlopen 处拦截，阻止绕过钩子的裸系统调用。 */
export const PY_SAFE_BOOTSTRAP = `import sys as _g_sys, os as _g_os
_G_O_WRITE = (_g_os.O_WRONLY | _g_os.O_RDWR | _g_os.O_CREAT | _g_os.O_TRUNC | _g_os.O_APPEND)
_G_BLOCK_EVENTS = frozenset([
    "os.system", "os.exec", "os.posix_spawn", "os.spawn", "os.fork", "os.forkpty",
    "subprocess", "subprocess.Popen",
    "socket.connect", "socket.bind", "socket.sendto", "socket.sendmsg",
    "os.remove", "os.rename", "os.rmdir", "os.mkdir", "os.symlink", "os.link",
    "os.truncate", "os.chmod", "os.chown", "os.utime", "os.chflags",
    "shutil.rmtree", "shutil.copyfile", "shutil.move", "shutil.copy",
    "ctypes.dlopen", "ctypes.dlsym", "ctypes.dlsym/handle", "ctypes.dlopen/handle",
    "sqlite3.connect", "sqlite3.enable_load_extension",
])
def _g_safe_hook(_g_event, _g_args):
    if _g_event in _G_BLOCK_EVENTS or _g_event.startswith("subprocess."):
        raise PermissionError("安全模式：%s 已屏蔽（仅保留文件读取）" % _g_event)
    if _g_event == "open":
        _g_write = False
        if len(_g_args) >= 2 and isinstance(_g_args[1], str):
            _g_write = any(_g_c in _g_args[1] for _g_c in "wax+")
        if not _g_write and len(_g_args) >= 3 and isinstance(_g_args[2], int):
            _g_write = bool(_g_args[2] & _G_O_WRITE)
        if not _g_write and len(_g_args) >= 2 and isinstance(_g_args[1], int):
            _g_write = bool(_g_args[1] & _G_O_WRITE)
        if _g_write:
            raise PermissionError("安全模式：文件写入已屏蔽（仅保留文件读取）：%s" % (_g_args[0] if _g_args else "?"))
_g_sys.addaudithook(_g_safe_hook)
`
