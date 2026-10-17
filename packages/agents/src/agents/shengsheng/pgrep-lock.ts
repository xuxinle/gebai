/**
 * `pgrep -f` **自匹配活锁**的识别：`review` 的处置建议里报「疑似 `pgrep` 自匹配活锁」。
 *
 * ## 为什么需要它（两次实测，代价可量化）
 *
 * `pgrep -f <pattern>` 匹配的是**整条命令行**。若 pattern 原样出现在发起它的那条命令行里
 * （最常见的形态就是 `sh -c "for i in …; do pgrep -f 'x.py' … done"`），`pgrep` 每次都会命中
 * **这条 shell 自己** ⇒ 条件恒为真 ⇒ 「等它结束」变成**永远等下去**。
 *
 * 现场一（会话 `a8319257`，别名 `ctx-units-1`）：`pgrep -f "reverse_verify.py"` 的等待循环，
 * 而它自己的 `-c` 命令行里就写着 `reverse_verify.py`——**11:34:51 → 11:46:19 空转 12 分钟**、
 * 159 条消息、106K ctx、**一行实现都没写**。它当时以为自己在「守规矩地等别人释放文件」。
 *
 * 现场二（同一窗口，生生自己的命令行）：`pgrep -c -f 'python3.*reverse_verify'` 同样命中自己；
 * 改成 `'python3.*reverse_verify[.]py'` 后正确返回 0。**破字串（字符类断开）就能让判据自救**。
 *
 * ## 判据（只读，不跑任何命令）
 *
 * 从该 worker 会话最近的消息里筛出「等待类命令」（含 `pgrep` / `until`·`while`·`for i in` 的轮询壳），
 * 取出它们**探测的 pattern**，再：
 *
 * 1. **自匹配**：pattern 出现在该命令它自己的命令行里（把 pattern 当字面量，看它是否为命令文本的子串）
 *    ——这是恒真的根因，单条命中即可报。
 * 2. **活锁惯性**：同一条 pattern 在最近 `REPEAT_THRESHOLD` 次等待里反复被探测，**而进程表里现在
 *    没有任何进程匹配它**（用字符类断开自身，避免本判据自己复刻同一个坑）⇒ 报「连续 N 次等同一个
 *    gate、而该 gate 现已无进程」。
 *
 * ## 为什么是「疑似」而不是「确诊」
 *
 * · 判据是**文本**：`grep reverse_verify.py` 这类命令行会被当成为等待（安全侧：多报一条）。
 * · 会话消息只留最近一部分时，更早的等待看不到（`REPEAT_THRESHOLD` 之内看不到就不报——宁缺勿滥）。
 * · pattern 可以匹配到**无关的同名进程**：那时「现在还有进程」为真，于是不报活锁（安全侧）。
 * · 拿不到进程表（非 Linux / 无 `/proc`）时**如实说「本项未核」**，不当作「没有活锁」。
 *
 * ## 与 `verify_guard` 的一条实测教训同源
 *
 * `verify_guard.py` 里已记着这一型的**另一面**：`pgrep -af reverse_verify` 会命中它自己那条命令，
 * 不排除就「永远有人在跑」。那边用「排除自身/祖先/后代」解决；本模块用**字符类断开 + 排除自身
 * 与祖先链**解决。两处都必须做——只做一处，另一个入口就是新的假信号源。
 */
import type { ProcRow } from "./proc-tree"

/** 同一 gate 被反复等待的次数阈值：达到即视为「活锁惯性」（实测现场都是 ≥5 次）。 */
export const REPEAT_THRESHOLD = 3

/** 从会话消息里最多回看的条数（会话越来越长，全量扫描既慢又没必要）。 */
export const SCAN_MESSAGE_LIMIT = 60

/** 一条「等待类命令」的取证结果。 */
export interface WaitProbe {
  /** 该命令探测的 pattern 原文（多条时取第一个可判的）。 */
  pattern: string
  /** 命令原文（截断到 {@link PROBE_SNIPPET} 字，回显用）。 */
  command: string
  /** 命令出现在消息流里的序号（越靠后越新）。 */
  at: number
  /** 自匹配：pattern 原样出现在该命令自己的命令行里 ⇒ `pgrep` 必定命中自己。 */
  selfMatch: boolean
}

/** 回显里每条命令保留的字数。 */
export const PROBE_SNIPPET = 160

/** 一条检测结论。 */
export interface PgrepLockVerdict {
  /** 是否报「疑似 `pgrep` 自匹配活锁」。 */
  locked: boolean
  /** 本项是否真的核过（false ⇒ 拿不到进程表/没有可判的命令，**不等于「没有活锁」**）。 */
  checked: boolean
  /** 给 `review` 处置建议用的一行（没命中为空串）。 */
  advice: string
  /** 结构化字段（控制面/前端按字段用，不靠抠文本）。 */
  block: {
    /** 命中自匹配的命令（最新在前，最多 {@link PROBE_SNIPPET} 条）。 */
    selfMatches: Array<{ pattern: string; command: string; at: number }>
    /** 反复被等而现已无进程的 gate（pattern + 次数）。 */
    staleGates: Array<{ pattern: string; waits: number }>
    /** 扫到的等待类命令条数（0 ⇒ 本项无从判断）。 */
    probes: number
    /** 进程表判据用到的「这些 gate 现在还有几个进程」。 */
    alive: Record<string, number>
  }
}

/**
 * 从一条命令行里抽出 `pgrep` 探测的 pattern。
 *
 * 支持三种形态（都是现场真出现过的）：
 *   `pgrep -f "x.py"` / `pgrep -c -f 'x.py'` / `pgrep -af x.py`
 * 只取 `-f`（匹配整条命令行）的 pattern——`pgrep -x name` 按进程名，不受自匹配影响，不当证据。
 */
export function extractPatterns(command: string): string[] {
  const out: string[] = []
  // 逐个 `pgrep` 片段看：`pgrep` 之后到下一个命令分隔符（`;` `|` `&&` `)` 换行）之间的 flags+pattern
  const re = /\bpgrep\b([^;|&\n)]*)/g
  for (const m of command.matchAll(re)) {
    const args = m[1] ?? ""
    if (!/(?:^|\s)-[a-zA-Z]*f[a-zA-Z]*(?:\s|$)/.test(args)) continue // 没有 -f ⇒ 不扫命令行，跳过
    // pattern 是**第一个非选项**参数（选项可聚合：`-c -f` / `-af`）
    const tokens = args.trim().split(/\s+/).filter(Boolean)
    let pat = ""
    for (const t of tokens) {
      if (t.startsWith("-")) continue
      pat = t
      break
    }
    pat = stripQuotes(pat)
    if (pat) out.push(pat)
  }
  return out
}

function stripQuotes(s: string): string {
  const t = s.trim()
  if (t.length >= 2 && ((t.startsWith('"') && t.endsWith('"')) || (t.startsWith("'") && t.endsWith("'")))) return t.slice(1, -1)
  return t
}

/**
 * 自匹配判据：pattern（**按 pgrep 的语义，即正则**）能匹配到**发起它的那条命令行**。
 *
 * 必须是正则语义而不是字面子串：pgrep 对 `-f` 的 pattern 做的就是正则匹配，于是
 * · `pgrep -f 'reverse_verify.py'` 的 cmdline 里正是 `reverse_verify.py` ⇒ 正则匹配上自己 ⇒ 恒真；
 * · `pgrep -f 'reverse_verify[.]py'` 的 cmdline 里是字面 `reverse_verify[.]py`，而正则
 *   `reverse_verify[.]py` 要求的是 `reverse_verify.py` ⇒ **匹配不到自己** ⇒ 正确写法能自救。
 * 于是本判据与「破字串为何有效」是同一个判据的两面，不会对着正确写法报假警。
 *
 * pattern 不是合法正则时退回字面子串比对（安全侧：可能多报一条，不会漏报恒真条件）。
 *
 * ⚠ **不能把「正则语法在命令行里找不到字面原文」当成免罪**：`sgs_gate_ok_2718[.]py` 这条
 * pattern 的正则确实匹配到了**它自己的那段命令行**（命令行里就是那句话）——因为 shell 把
 * `[.]` 当作普通字符传给了 `pgrep`，而 `pgrep` 把 pattern 当正则。换句话说：
 * **凡是以 `-f` 把 pattern 写在自己命令行里的等待，它都已经自匹配**（不论 pattern 长什么形状）。
 * 真正能自救的不是「把 pattern 写成什么样」，而是 **pattern 不以字面形式出现在自己的命令行里**：
 * 拼接（`p="x.py"; pgrep -f "$p"`）、字符类断开且命令行上只留字面 `[.]` 的那种写法（`x[.]py`
 * 写在命令行上时，命令行里是字面 `[.]`，正则要的却是 `.` ⇒ 匹配不上）例外。
 *
 * 于是判据只问一件事：**这条命令行里是否有一处文本，使得「把它当正则去跑」时能匹配到这条命令行自己**。
 * 对 `x[.]py`，命令行里的文本是字面 `x[.]py`，字面 `[.]` ≠ 正则里的 `.` ⇒ 不匹配 ⇒ 正确。
 * 对 `x.py`，命令行里的文本是 `x.py`，正则 `x.py` 匹配它自己 ⇒ 恒真。
 */
export function isSelfMatch(command: string, pattern: string): boolean {
  if (!pattern) return false
  try {
    // 正则能编译 ⇒ 它的结论就是终论（不得再拿字面比对“兜一下”：那会把正确的破字串写法
    // `x[.]py` 当成“字面 `x[.]py` 出现在命令行里”而误报——本单实测踩到，两个用例当场变红）
    return new RegExp(pattern).test(command)
  } catch {
    // 非法正则：退回字面子串比对（安全侧：可能多报一条，不会漏报恒真条件）
    return command.includes(pattern)
  }
}

/**
 * 进程表里现在有几个进程**真的**匹配该 pattern（用字符类断开自身，避免本判据自己复刻同一个坑）。
 *
 * 为什么必须断开：模式串出现在本进程自己的命令行里时（比如会话里 `grep` 过它），
 * 朴素的做法会让「现在还有进程」恒真 ⇒ 活锁检测**永远不会报**。断开的做法是把 pattern 里
 * 最后一个字面字符包成字符类（`x.py` → `x.p[y]`），正则匹配同一个字符串，但**命令行的字面文本
 * 不再精确包含 pattern** ⇒ 自己的那条命令行匹配不上，真进程照常匹配得上。
 *
 * @param exclude 要排除的 pid（自身与祖先链）：它们即便匹配也不算「gate 还活着」。
 */
export function aliveMatching(rows: readonly ProcRow[], pattern: string, exclude: Iterable<number> = []): ProcRow[] {
  if (!pattern) return []
  const skip = new Set(exclude)
  let re: RegExp
  try {
    re = new RegExp(disjoinTail(pattern))
  } catch {
    return []
  }
  return rows.filter((r) => !skip.has(r.pid) && r.cmd && re.test(r.cmd))
}

/** 把 pattern 的最后一个**字面字符**包成字符类：`reverse_verify.py` → `reverse_verify.p[y]`。
 *
 *  正则语义不变（`[y]` ≡ `y`），但它**不再以字面形式**出现在命令行里——于是「发起它的那条命令行」
 *  与自己写下的 pattern 不再精确重合。只对末尾是字母/数字/下划线时做：末尾是正则元字符时
 *  `x.*` 包成 `x.[*]` 会**改语义**，宁可退回原文（安全侧：只是可能多报一条假活）。 */
export function disjoinTail(pattern: string): string {
  if (pattern.length < 2) return pattern
  const tail = pattern.slice(-1)
  if (!/[A-Za-z0-9_]/.test(tail)) return pattern
  return `${pattern.slice(0, -1)}[${tail}]`
}

/** 从会话消息里筛出「等待类命令」（工具调用参数里的 shell 命令，或消息正文里的代码块）。 */
export function waitProbes(messages: readonly unknown[], limit = SCAN_MESSAGE_LIMIT): WaitProbe[] {
  const texts: Array<{ at: number; command: string }> = []
  const tail = messages.slice(-limit)
  tail.forEach((m, i) => {
    const at = messages.length - tail.length + i
    for (const command of commandsOf(m)) {
      if (!isWaitCommand(command)) continue
      texts.push({ at, command })
    }
  })
  const out: WaitProbe[] = []
  for (const { at, command } of texts) {
    const patterns = extractPatterns(command)
    if (!patterns.length) continue
    // 一条命令里多个 pattern：任一自匹配即整条算自匹配（恒真条件是 **或** 关系）
    const selfMatch = patterns.some((p) => isSelfMatch(command, p))
    out.push({ pattern: patterns[0], command: command.slice(0, PROBE_SNIPPET), at, selfMatch })
  }
  return out
}

/** **真的会把命令交给 shell 去跑**的工具名。
 *
 *  只扫这些工具的调用参数，是因为判据要的是「它真跑过这条命令」：
 *  · 助手正文里**引用**一条命令（`它当时跑的是 pgrep -f "x.py"`）不构成活锁——把它算进来会让
 *    每一个在报告里复盘这件事的 worker 都被判成活锁（噪音会淹没真信号）；
 *  · `write`/`edit` 的正文写一个等待**脚本文件**反而是**正确**做法（`bash script.sh` 的命令行里
 *    只有脚本路径，pattern 不出现 ⇒ `pgrep` 匹配不到自己），所以更不能算。 */
export const SHELL_RUNNER_TOOLS: readonly string[] = ["sh", "py", "js", "bash", "shell"]

/** 消息里所有「真的被执行过的命令」文本：只取 shell 类工具调用的参数（`command`/`code`/`input`）。 */
function commandsOf(m: unknown): string[] {
  const out: string[] = []
  const msg = (m ?? {}) as Record<string, unknown>
  for (const call of (msg.toolCalls ?? []) as Array<Record<string, unknown>>) {
    const name = String(call?.name ?? "")
    if (!SHELL_RUNNER_TOOLS.includes(name)) continue
    const args = (call?.arguments ?? {}) as Record<string, unknown>
    for (const key of ["command", "code", "input"]) {
      const v = args[key]
      if (typeof v === "string" && v) out.push(v)
    }
  }
  return out
}

/** 是不是一条「等待/轮询」命令：有 `pgrep`，且形态像循环（`for i in` / `while` / `until`）。
 *
 *  只认循环形态：单次 `pgrep -f x && echo ok` 即便自匹配也不构成活锁（它立刻返回），
 *  报它只会稀释警报。 */
export function isWaitCommand(command: string): boolean {
  if (!/\bpgrep\b/.test(command)) return false
  return /\b(?:for\s+\w+\s+in|while|until)\b/.test(command)
}

/**
 * 主判据：从消息流 + 进程表得出「疑似 `pgrep` 自匹配活锁」的结论。
 *
 * @param messages 该 worker 会话的消息（只看最后 {@link SCAN_MESSAGE_LIMIT} 条）
 * @param rows 进程表（`readProcTable().rows`）；`tableError` 非空 ⇒ 本项未核
 * @param exclude 进程判据要排除的 pid（自身 + 祖先链；由调用方用 `protectLineage()` 取）
 */
export function detectPgrepLock(
  messages: readonly unknown[],
  rows: readonly ProcRow[],
  opts: { exclude?: Iterable<number>; tableError?: string; threshold?: number } = {},
): PgrepLockVerdict {
  const threshold = opts.threshold ?? REPEAT_THRESHOLD
  const probes = waitProbes(messages)
  const block: PgrepLockVerdict["block"] = { selfMatches: [], staleGates: [], probes: probes.length, alive: {} }
  if (!probes.length) {
    return { locked: false, checked: false, advice: "", block }
  }

  // ① 自匹配（单条即报）：恒真条件的根因，无需等第二次。**纯文本判据**——与进程表无关，
  //    所以进程表读不到时它依然成立（这正是「自匹配」比「活锁惯性」更硬的地方）。
  for (const p of probes) {
    if (!p.selfMatch) continue
    if (block.selfMatches.length < 3) block.selfMatches.push({ pattern: p.pattern, command: p.command, at: p.at })
  }
  if (opts.tableError) {
    // 拿不到进程表：「活锁惯性」那一路不能判——如实说未核，不得当作“已核无异常”。
    const locked = block.selfMatches.length > 0
    const advice = locked
      ? `${renderAdvice(block)}\n  另外：「同一 gate 反复被等、而它现已无进程」这一路**本项未核**（${opts.tableError}）。`
      : ""
    return { locked, checked: false, advice, block }
  }

  // ② 活锁惯性：同 pattern 反复被等、而它现在**没有任何进程**
  const counts = new Map<string, number>()
  for (const p of probes) counts.set(p.pattern, (counts.get(p.pattern) ?? 0) + 1)
  for (const [pattern, waits] of counts) {
    if (waits < threshold) continue
    const alive = aliveMatching(rows, pattern, opts.exclude ?? [])
    block.alive[pattern] = alive.length
    if (alive.length === 0) block.staleGates.push({ pattern, waits })
  }

  const locked = block.selfMatches.length > 0 || block.staleGates.length > 0
  return { locked, checked: true, advice: locked ? renderAdvice(block) : "", block }
}

/**
 * 等待外部进程时的**硬纪律**（派工模板逐单注入）。
 *
 * 为何由代码生成而不写死在模板里：这条纪律与 `review` 的自动检测（{@link detectPgrepLock}）
 * 必须同源——两处各写一份就会漂成「消息教正确写法、检测却按另一套判」。
 */
export function waitProtocolLine(): string {
  return [
    "等待纪律（`pgrep -f` 会匹配到发起它的那条 shell 自己——本仓实测两次，一次空转 12 分钟零产出）：",
    "❌ 禁止 `pgrep -f '<串>'` 去等一个**写在你这条命令里**的串（条件恒真 ⇒ 你会一直等一个早已结束的东西，而你以为自己在守规矩）；",
    "✅ 三条正确写法：① 有 pid 就别用 pgrep：`while kill -0 <pid> 2>/dev/null; do sleep 1; done`；② 只按进程名：`pgrep -x <进程名>`（不扫命令行）；③ 仍要 -f 时破字串并排除自身与父：`pgrep -f 'x[.]py' | grep -vx -e $$ -e $PPID`。",
    "等待循环必须有上限（`for i in $(seq 1 60)`）——超时就如实报「gate 未等到」并继续干能干的，不要静默空转。",
  ].join(" ")
}

/** 处置建议的一行（含**正确写法**——只说「你错了」而不给替代，下一息还会撞）。 */
export function renderAdvice(block: PgrepLockVerdict["block"]): string {
  const lines: string[] = []
  const self = block.selfMatches[0]
  const stale = block.staleGates[0]
  if (self) {
    lines.push(
      `⚠ **疑似 \`pgrep\` 自匹配活锁**：等待命令里 \`pgrep -f "${self.pattern}"\` 的 pattern（\`pgrep\` 按**正则**解释）能匹配到**这条命令自己的命令行** ⇒ \`pgrep\` 每次命中发起它的那条 shell 自己 ⇒ 条件恒真 ⇒ 你会一直等一个早已结束的东西，**而且你以为自己在守规矩**（实测代价：一个 worker 12 分钟零产出、159 条消息）。`,
    )
    lines.push(`  现场命令：\`${self.command}\``)
  } else if (stale) {
    lines.push(
      `⚠ **疑似 \`pgrep\` 自匹配活锁**：同一 gate \`${stale.pattern}\` 在最近被连续等待 ${stale.waits} 次，而**此刻进程表里没有任何进程匹配它** ⇒ 那个「还在跑」的判断很可能是它自己（或它的父 shell）冒充的。`,
    )
  }
  lines.push(
    `  正确写法（三种，按优先级）：① **有 pid 就别用 pgrep**：\`while kill -0 <pid> 2>/dev/null; do sleep 1; done\`；② 只按进程名：\`pgrep -x <进程名>\`（\`-x\` 不扫命令行，不会自匹配）；③ 仍要 \`-f\` 时**破字串**：\`pgrep -f 'x[.]py'\`（字符类断开最后一个字面字符，正则语义不变而命令行文本不再精确重合），并排除自身与父：\`pgrep -f 'x[.]py' | grep -vx -e $$ -e $PPID\`。`,
  )
  lines.push(
    `  另外：等待循环必须**有上限**（\`for i in $(seq 1 60)\` + 超时后如实报「gate 未等到」），否则判据错一次就是无限期空转。`,
  )
  return lines.join("\n")
}
