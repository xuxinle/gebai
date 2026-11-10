/**
 * `shuangtian_doc`：按**路径**读霜天开发文档树。
 *
 * ## 为什么是一个"读文件"的工具，而不是一个"文档树"工具
 *
 * 文档本身是**多目录的 Markdown 文件**（`docs/agent-docs/…`）：层级由目录与页内链接表达，
 * 不需要工具再塑一遍树。模型要做的动作永远只有两个：
 *
 * 1. **读一页**——内容里带着更深一层的路径；
 * 2. **顺着那个路径再读**。
 *
 * 所以工具只提供"给路径 → 回内容"，把导航能力放进**内容本身**（每页末尾的「更细」）。
 * 多一个 `tree` 工具只会造成两份真相：树的形状与文档的实际组织迟早不一致。
 *
 * ## 为什么必须限长（这是本工具存在的第二个理由）
 *
 * `DESIGN.md` 单份 31 万字、`PITFALLS.md` 21 万字——直接整篇返回会被**输出护栏**截断
 * （头尾各 4K，中间挖空），模型看到的是"头 + 尾"，中间那一段不存在却看不出丢了。
 * 所以本工具按 `max_chars`（缺省 6000）**按行切块**，并明确告出"第 N/M 块 + 续读方式"：
 * 截断这件事被显式建模，而不是被静默发生。
 *
 * ## 路径解析（三处根，按序尝试）
 *
 * | 形态 | 例子 | 说明 |
 * |---|---|---|
 * | 文档树内相对路径 | `components/input/Slider.md` | 最常见（页内链接就是这种形态） |
 * | 树内绝对形态 | `docs/agent-docs/components/…` | 从仓库根抄来的路径也能用 |
 * | 旧文档 | `docs/PITFALLS.md`、`DESIGN.md` | 本树大量"更细"指向旧文档，必须能直达 |
 *
 * 越界（`..` 爬出根、绝对路径落在根外）**直接拒绝**并列出允许的根——
 * 宁可报错，也不要让"读文档"变成任意文件读取。
 */
import { existsSync, readdirSync, statSync } from "node:fs"
import { isAbsolute, join, relative, resolve } from "node:path"
import type { Tool, ToolContext, ToolResult } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"

/** 文档树在框架根下的位置。 */
const DOCS_SUBDIR = "docs/agent-docs"
/** 文档树入口（省略 path 时读它）。 */
const ENTRY = "index.md"
/** 单次返回的字符预算：远低于 12000 的护栏阈值，留出页头页脚与代码块余量。 */
const DEFAULT_BUDGET = 6000
/** 预算上限（调用方可以要更多，但不能逼近护栏）。 */
const MAX_BUDGET = 11000

interface Resolved {
  /** 绝对路径。 */
  file: string
  /** 相对所属根的展示路径（模型用来回填 path）。 */
  display: string
  /** 所属根（`agent-docs` 或框架根）。 */
  root: string
  /** 在文档树内（决定能否给出"树内相对路径"提示）。 */
  inTree: boolean
}

/**
 * 框架根：`framework` 参数 > `SHUANGTIAN_PROJECT` > 会话下的 `shuangtian/` >
 * `{GEBAI_HOME}/shuangtian`（单仓布局里框架就在主仓根下）> `{GEBAI_HOME}/vendor/shuangtian`（随包分发）。
 *
 * 判据是“这里有框架/分发根的特征”（`st.pkg` / `docs/agent-docs` / `docs`），
 * 不是“目录存在”——后者会让一个空目录抢掉真正的位置，报错时指向错的地方。
 */
function frameworkRoot(ctx: ToolContext, args: Record<string, unknown>): { root: string; tried: string[] } {
  const explicit = typeof args.framework === "string" ? args.framework.trim() : ""
  const fromEnv = String(ctx.env.SHUANGTIAN_PROJECT ?? "").trim()
  const looksLikeRoot = (dir: string): boolean =>
    existsSync(join(dir, "st.pkg")) || existsSync(join(dir, DOCS_SUBDIR)) || existsSync(join(dir, "docs"))
  const tried: string[] = []
  const candidates = [explicit, fromEnv, "shuangtian", join(ctx.home, "shuangtian"), join(ctx.home, "vendor", "shuangtian")]
  for (const candidate of candidates) {
    if (candidate === "") continue
    const resolved = ctx.resolvePath(candidate)
    if (tried.includes(resolved)) continue
    tried.push(resolved)
    if (looksLikeRoot(resolved)) return { root: resolved, tried }
  }
  // 都没命中：返回最可能想用的那一个（错误信息里会把找过的地方全列出来）
  return { root: tried[0] ?? ctx.resolvePath("shuangtian"), tried }
}

/** 路径是否落在根内（防 `..` 爬出）。 */
function inside(root: string, file: string): boolean {
  const rel = relative(root, file)
  return rel !== "" && !rel.startsWith("..") && !isAbsolute(rel)
}

/** YAML front matter（本树每页都有：`title` / `summary`）。 */
interface FrontMatter {
  title: string
  summary: string
  /** 正文起始偏移（跳过 front matter）。 */
  bodyOffset: number
}

/**
 * 拆 front matter。
 *
 * 它存在是为了让**入口页之外的页**也能被"只看标题就判断要不要读"——所以它不该占据正文预算，
 * 也不该被当正文返回（一堆 `---`/`title:` 对模型是噪声）。拆出来之后放进页头一行。
 */
function splitFrontMatter(text: string): FrontMatter {
  if (!text.startsWith("---")) return { title: "", summary: "", bodyOffset: 0 }
  const end = text.indexOf("\n---", 3)
  if (end < 0) return { title: "", summary: "", bodyOffset: 0 }
  const raw = text.slice(3, end)
  const title = (/^title:\s*(.+)$/m.exec(raw)?.[1] ?? "").trim()
  const summary = (/^summary:\s*(.+)$/m.exec(raw)?.[1] ?? "").trim()
  const after = text.indexOf("\n", end + 1)
  return { title, summary, bodyOffset: after < 0 ? text.length : after + 1 }
}

/**
 * 解析路径。返回 `{ ok: false, ... }` 时带上"试过哪些位置"，让调用方能自己纠偏。
 */
function resolveDoc(
  ctx: ToolContext,
  args: Record<string, unknown>,
): { ok: true; hit: Resolved } | { ok: false; message: string; tried: string[]; suggestions: string[] } {
  const raw = typeof args.path === "string" ? args.path.trim() : ""
  const wanted = raw === "" ? ENTRY : raw.replace(/\\/g, "/").replace(/^\.\//, "")
  const { root: framework, tried: frameworkTried } = frameworkRoot(ctx, args)
  const tree = join(framework, DOCS_SUBDIR)
  const tried: string[] = []
  const roots: Array<{ root: string; inTree: boolean }> = [
    { root: tree, inTree: true },
    // `docs/agent-docs/…` 这种从仓库根抄来的形态（以及旧文档 `DESIGN.md` / `docs/PITFALLS.md`）
    { root: framework, inTree: false },
  ]

  for (const entry of roots) {
    const candidate = resolve(entry.root, wanted)
    tried.push(candidate)
    if (!inside(entry.root, candidate)) continue // 越界：不试、不读
    if (!existsSync(candidate)) continue
    let file = candidate
    if (statSync(candidate).isDirectory()) {
      const index = join(candidate, "index.md")
      if (!existsSync(index)) continue
      file = index
      tried.push(file)
    }
    const rel = relative(entry.root, file).replace(/\\/g, "/")
    // 树内页给树内相对路径（页内链接就是这种形态）；旧文档给相对**框架根**的路径
    // （`DESIGN.md` / `docs/PITFALLS.md`）——两者都是调用方可以直接回填的 path。
    return { ok: true, hit: { file, display: rel, root: entry.root, inTree: entry.inTree } }
  }

  // 越界要**说清是越界**，不能报成"文档不存在"——后者会让人去猜拼写，而真正的问题是路径形状
  const escaped = roots.some((entry) => !inside(entry.root, resolve(entry.root, wanted)))

  // 找不到：按末段名字做一次近似匹配，把"你可能想读的"列出来（比只说"不存在"有用得多）
  const base = wanted.split("/").pop() ?? wanted
  const stem = base.replace(/\.md$/i, "").toLowerCase()
  const suggestions: string[] = []
  const walk = (dir: string, depth: number): void => {
    if (depth > 4 || suggestions.length >= 12) return
    let entries: string[]
    try {
      entries = readdirSync(dir)
    } catch {
      return
    }
    for (const name of entries) {
      const full = join(dir, name)
      let isDir = false
      try {
        isDir = statSync(full).isDirectory()
      } catch {
        continue
      }
      if (isDir) {
        walk(full, depth + 1)
        continue
      }
      if (!name.endsWith(".md")) continue
      const nameStem = name.replace(/\.md$/i, "").toLowerCase()
      const close = nameStem === stem || editDistance(nameStem, stem) <= 2
      if (close) {
        const rel = relative(tree, full).replace(/\\/g, "/")
        suggestions.push(rel.startsWith("..") ? relative(framework, full).replace(/\\/g, "/") : rel)
      }
    }
  }
  walk(tree, 0)
  return {
    ok: false,
    message: escaped
      ? `路径越界（拒绝读取）：${wanted} —— 文档路径只能是文档树内相对路径或旧文档路径（如 \`docs/PITFALLS.md\`）`
      : `文档不存在：${wanted}`,
    tried: [...tried, ...frameworkTried],
    suggestions,
  }
}

interface Heading {
  level: number
  title: string
  /** 在全文中的字符偏移（用于 `offset` 跳转）。 */
  offset: number
}

/** 抽标题（只认 ATX `#`，跳过代码块内）。 */
function headingsOf(text: string): Heading[] {
  const out: Heading[] = []
  let offset = 0
  let inFence = false
  for (const line of text.split("\n")) {
    if (/^\s*```/.test(line)) inFence = !inFence
    else if (!inFence) {
      const m = /^(#{1,4})\s+(.+?)\s*$/.exec(line)
      if (m) out.push({ level: m[1].length, title: m[2], offset })
    }
    offset += line.length + 1
  }
  return out
}

/** 按行切块：从 `offset` 起尽量凑满预算，且不切断行。 */
function sliceByBudget(text: string, offset: number, budget: number): { body: string; next: number; endsMidLine: boolean } {
  if (offset >= text.length) return { body: "", next: text.length, endsMidLine: false }
  let cursor = offset
  let used = 0
  let endsMidLine = false
  while (cursor < text.length) {
    const nl = text.indexOf("\n", cursor)
    const lineEnd = nl < 0 ? text.length : nl + 1
    const line = text.slice(cursor, lineEnd)
    if (used > 0 && used + line.length > budget) break
    if (used === 0 && line.length > budget) {
      // 单行超预算（压缩过的长表格行等）：按字符硬切并如实标注
      const cut = cursor + budget
      return { body: text.slice(cursor, cut), next: cut, endsMidLine: true }
    }
    used += line.length
    cursor = lineEnd
    endsMidLine = nl < 0
  }
  return { body: text.slice(offset, cursor), next: cursor, endsMidLine }
}

/** 编辑距离（上限截断；用于“你想读的是不是这个”的近似建议）。 */
function editDistance(a: string, b: string): number {
  if (Math.abs(a.length - b.length) > 3) return 99
  const prev = new Array<number>(b.length + 1)
  const cur = new Array<number>(b.length + 1)
  for (let j = 0; j <= b.length; j++) prev[j] = j
  for (let i = 1; i <= a.length; i++) {
    cur[0] = i
    for (let j = 1; j <= b.length; j++) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
    }
    for (let j = 0; j <= b.length; j++) prev[j] = cur[j]
  }
  return prev[b.length]
}

/**
 * 抽「更细」式链接：行内代码里以 `.md` 结尾、**且真能解析到**的路径。
 *
 * 只回能解析的——页里写死但已失效的路径列出来只会让模型多撞一次墙
 * （每一跳都被本函数的解析器验证过，所以列出的都能直接读）。
 *
 * 输出形态统一成"调用方可以直接回填的 path"：树内页给树内相对路径，
 * 旧文档给相对框架根的路径（`docs/…` / `DESIGN.md`）。
 */
function nextHops(text: string, ctx: ToolContext, args: Record<string, unknown>): string[] {
  const { root: framework } = frameworkRoot(ctx, args)
  const tree = join(framework, DOCS_SUBDIR)
  const found = new Set<string>()
  for (const m of text.matchAll(/`([^`\n]+\.md)`/g)) {
    const candidate = m[1].trim()
    if (found.size >= 12) break
    if (candidate.startsWith("http") || candidate.includes("*")) continue
    const attempt = resolveDoc(ctx, { ...args, path: candidate })
    if (!attempt.ok) continue
    const relTree = relative(tree, attempt.hit.file).replace(/\\/g, "/")
    found.add(relTree.startsWith("..") ? relative(framework, attempt.hit.file).replace(/\\/g, "/") : relTree)
  }
  return [...found]
}

function renderOutline(headings: Heading[], charBudget = 2000): string {
  const picked = headings.filter((h) => h.level <= 3).slice(0, 24)
  if (picked.length === 0) return ""
  const lines: string[] = []
  let used = 0
  for (const h of picked) {
    const short = h.title.length > 60 ? `${h.title.slice(0, 60)}…` : h.title
    const line = `${"  ".repeat(Math.max(0, h.level - 1))}- ${short}  @${h.offset}`
    if (used + line.length > charBudget) break
    lines.push(line)
    used += line.length + 1
  }
  const total = headings.filter((h) => h.level <= 3).length
  if (lines.length < total) {
    lines.push(`- …（另有 ${total - lines.length} 个小节，完整列用 outline=true）`)
  }
  return ["本页小节（`offset` 可直接传给下一次调用跳转）：", ...lines].join("\n")
}

/**
 * 单次输出的**硬上限**：正文 + 页头页脚一起算。
 *
 * 为何要有它：正文按预算切了，但页脚（小节列表 + 下一跳）是额外开销——
 * 实测"预算 11000 + 页脚"能顶到 13.3K，逼近 12000 的输出护栏。护栏一触发
 * 就是**静默挖空中间**，本工具为防这件事做的切块就白做了。
 */
const HARD_CAP = 11400

/** 位置描述：第几块 / 共几块（按预算粗算，给调用方一个尺度感）。 */
function partInfo(text: string, offset: number, budget: number): string {
  const total = Math.max(1, Math.ceil(text.length / budget))
  const current = Math.min(total, Math.floor(offset / budget) + 1)
  return `第 ${current}/${total} 块（offset=${offset}/${text.length} 字符）`
}

export const docTool: Tool = {
  name: "doc",
  description:
    "按路径读霜天开发文档（多目录 Markdown 树）。省略 path 读入口 index.md；页面内容自带更深一层的路径（页尾「更细」），顺着读即可下钻。" +
    `单次返回受 max_chars 限制（缺省 ${DEFAULT_BUDGET}），超出按行切块并告出"第 N/M 块 + 续读方式"（用 offset 续读）——不会被输出护栏静默截断。` +
    "大纲模式（outline=true）只回标题 + 偏移，用来先看一页值不值得读。可读的路径形态：" +
    "`components/input/Slider.md`（树内）、`docs/PITFALLS.md` / `DESIGN.md`（旧文档，本树大量指向它们）。越界路径直接拒绝。",
  parameters: schema(
    {
      path: {
        type: "string",
        description: `文档路径（树内相对路径，如 components/index.md；或 docs/PITFALLS.md）。缺省 ${ENTRY}`,
      },
      offset: { type: "number", description: "起始字符偏移（续读用；页内小节列表会给可直接用的 offset）" },
      max_chars: { type: "number", description: `单次字符预算（缺省 ${DEFAULT_BUDGET}，上限 ${MAX_BUDGET}）` },
      outline: { type: "boolean", description: "只返回标题大纲（含 offset），不返回正文" },
      framework: { type: "string", description: "框架根（缺省 SHUANGTIAN_PROJECT 或会话下 shuangtian/）" },
    },
    [],
  ),
  requiresApproval: false,
  execute: async (args, ctx): Promise<ToolResult> => {
    const resolved = resolveDoc(ctx, args)
    if (!resolved.ok) {
      const lines = [resolved.message]
      if (resolved.suggestions.length > 0) {
        lines.push("", "可能想读的（把 path 换成下面任一项）：", ...resolved.suggestions.map((s) => `- ${s}`))
      } else {
        lines.push("", "先读入口拿路径：`shuangtian_doc()`（省略 path）")
      }
      lines.push("", "找过的位置：", ...resolved.tried.map((t) => `- ${t}`))
      return { output: lines.join("\n"), data: { ok: false, path: String(args.path ?? ENTRY), suggestions: resolved.suggestions } }
    }

    const hit = resolved.hit
    const raw = await Bun.file(hit.file).text()
    const front = splitFrontMatter(raw)
    // 正文从 front matter 之后开始：标题与摘要已在页头一行给出，不再占预算
    const text = front.bodyOffset > 0 ? raw.slice(front.bodyOffset) : raw
    const headings = headingsOf(text)
    const budget = Math.min(MAX_BUDGET, Math.max(600, Number(args.max_chars) || DEFAULT_BUDGET))

    /** 页头：标题 + 路径 + （若为旧文档）来源标注。 */
    const heading = (): string =>
      [`# ${front.title || hit.display}`, `路径：${hit.display}${hit.inTree ? "" : "（旧文档·非本树）"}`].join("\n")

    if (args.outline === true) {
      const outline = renderOutline(headings)
      const head = [heading(), text.length > budget ? partInfo(text, 0, budget) : ""]
        .filter((s) => s !== "")
        .join("\n")
      return {
        output: [head, "", outline || `（本页无小节标题；正文 ${text.length} 字符，直接读正文即可）`].join("\n"),
        data: {
          ok: true,
          path: hit.display,
          title: front.title,
          chars: text.length,
          headings: headings.map((h) => ({ level: h.level, title: h.title, offset: h.offset })),
        },
      }
    }

    const offset = Math.max(0, Math.min(text.length, Math.floor(Number(args.offset) || 0)))

    // 先凑齐“固定开销”（页头 + 页脚），再倒推正文能占多少。
    //
    // 为何这个顺序：页脚会随页面变大（小节列表 + 下一跳），而总额超过 12000 就会被
    // 输出护栏**静默挖空中间**。先定常量、再填正文，总长度在构造上就成立；
    // 反之（先切正文再拼页脚再截）会把“还有 N 字符没读”这条指令自己截掉。
    const hops = nextHops(text, ctx, args)
    const hopBlock = hops.length > 0 ? ["更细（可直接作为下一次 path）：", ...hops.map((h) => `- ${h}`)].join("\n") : ""
    const outline = headings.length > 0 ? renderOutline(headings) : ""

    /** 页头：标题 + 路径 + 摘要 + 块位置。 */
    const headOf = (bodyBudget: number, next: number): string => {
      const lines: string[] = [heading()]
      if (front.summary) lines.push(`摘要：${front.summary}`)
      if (text.length <= bodyBudget) lines.push(`（正文 ${text.length} 字符，已完整返回）`)
      else lines.push(partInfo(text, offset, bodyBudget), next < text.length ? `续读位置：offset=${next}` : "")
      return lines.filter((s) => s !== "").join("\n")
    }

    /** 拼一次完整输出（页头 + 正文 + 页脚）；同时给出正文实际长度，供反算开销。 */
    const compose = (bodyBudget: number, withOutline: boolean): { text: string; next: number; bodyLen: number } => {
      const body = sliceByBudget(text, offset, bodyBudget)
      const head = headOf(bodyBudget, body.next)
      const tail: string[] = []
      if (body.next < text.length) {
        tail.push(
          `**还有 ${text.length - body.next} 字符没读**：再调一次带 \`offset=${body.next}\`` +
            (body.endsMidLine ? "（上一块在一行中间断开，续读会接着这行）" : ""),
        )
      }
      if (withOutline && outline) tail.push(outline)
      if (hopBlock) tail.push(hopBlock)
      const sections = [head, body.body.trimEnd()].filter((s) => s !== "")
      const text_ = tail.length > 0 ? `${sections.join("\n\n")}\n\n---\n\n${tail.join("\n\n")}` : sections.join("\n\n")
      return { text: text_, next: body.next, bodyLen: body.body.trimEnd().length }
    }

    // 预算按**实测开销**反算：页头/页脚的长度取决于本页（小节数、下一跳数），
    // 不能拍一个固定值——拍小了白浪费一半预算，拍大了就顶到护标。
    // 做法：先按请求预算试一次，量出总长与正文长之差（= 开销），再二次凑到硬上限内。
    let composed = compose(budget, true)
    let chosenBudget = budget
    if (composed.text.length > HARD_CAP) {
      const overhead = Math.max(0, composed.text.length - composed.bodyLen)
      const fitted = Math.max(600, HARD_CAP - overhead - 80)
      chosenBudget = Math.min(budget, fitted)
      composed = compose(chosenBudget, true)
      if (composed.text.length > HARD_CAP) {
        // 仍超额（开销本身很大，如小节列表极长）：丢掉小节列表再凑一次
        const overhead2 = Math.max(0, composed.text.length - composed.bodyLen)
        chosenBudget = Math.max(600, Math.min(chosenBudget, HARD_CAP - overhead2 - 80))
        composed = compose(chosenBudget, false)
      }
    }

    const output = composed.text.length <= HARD_CAP ? composed.text : composed.text.slice(0, HARD_CAP)
    return {
      output,
      data: {
        ok: true,
        path: hit.display,
        title: front.title,
        chars: text.length,
        offset,
        next: composed.next,
        complete: composed.next >= text.length,
        budget: chosenBudget,
        headings: headings.map((h) => ({ level: h.level, title: h.title, offset: h.offset })),
        hops,
      },
    }
  },
}
