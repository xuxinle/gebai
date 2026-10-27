/**
 * 内置渠道 `local`：**把「自己家的事」也收进同一套动作面**。
 *
 * 这个渠道存在的意义不只是「有个能测的样例」——它是抽象是否成立的关键证据：
 *
 * | 场景 | 传统做法 | 本渠道 |
 * |---|---|---|
 * | 写长期记忆 | 单独造 `memory_write` 工具 | `act(to:"local:memory", verb:"append")` |
 * | 落一份产物 | 直接用 `write`（无审计语义） | `act(to:"local:outbox", verb:"put")` |
 * | 读记忆 | 单独造 `memory_read` | `observe(to:"local:memory", what:"all")` |
 * | 看产物 | 自己拼路径 | `observe(to:"local:outbox", what:"list")` |
 *
 * 如果「发飞书消息」和「写自己的记忆」能用同一个 `act` 面表达，那这个抽象就是对的；
 * 如果哪天为了某个渠道必须给 `act` 加特例字段，说明抽象错了。
 *
 * ## 地址语义
 *
 * - `local:memory`   → `{GEBAI_HOME}/persona/memory.md`（按 section 分段的追加式记忆）
 * - `local:outbox`   → `{GEBAI_HOME}/persona/outbox/`（落盘产物，带时间戳文件名）
 *
 * ## 为何 memory 用「分段追加」而不是整体重写
 *
 * 多会话并发写时，整体重写会让「另一个会话刚写的内容」消失。追加 + section 定位
 * 是唯一无冲突的形态（文件写入本身由内存队列串行化，见 `writeChain`）。
 */

import { appendFile, mkdir, readFile, readdir, rm, stat, writeFile } from "node:fs/promises"
import { join } from "node:path"
import type { ChannelContext, ChannelDef } from "../channel"

/**
 * 每个订阅实例的清理器集合。
 *
 * 键是 `home + 渠道定义对象` 的组合，两个维度缺一不可：
 *
 * - **按 def 分键**：热加载后新旧 def 对象共存，各自持自己的清理器
 *   （只按 home 分键时，新的会覆盖旧的，旧 timer 永远停不下来）
 * - **按 home 分键**：同一份 def 被多个注册表实例复用（并行测试、多租户、多用户）时，
 *   各实例的订阅互不覆盖（只按 def 分键时，A 的 stop 会把 B 的订阅断掉）
 */
const cleanups = new Map<string, () => void>()
/** def 对象 → 稳定序号（WeakMap 避免给渠道定义挂属性）。 */
const defIds = new WeakMap<object, number>()
let defIdSeq = 0
function cleanupKey(home: string, def: object): string {
  let id = defIds.get(def)
  if (id === undefined) {
    id = ++defIdSeq
    defIds.set(def, id)
  }
  return `${home}\u0000${id}`
}

/** 记忆文件里 section 的标记样式（`## 待跟进`）。 */
const SECTION_PREFIX = "## "

/** 写串行化：同一进程内对同一文件的追加必须排队（跨会话并发写会交错）。 */
const writeChains = new Map<string, Promise<unknown>>()
function serialize<T>(key: string, fn: () => Promise<T>): Promise<T> {
  const prev = writeChains.get(key) ?? Promise.resolve()
  const next = prev.then(fn, fn)
  // 链上只保留「已完成」状态，避免无限增长与未处理拒绝
  writeChains.set(
    key,
    next.then(
      () => undefined,
      () => undefined,
    ),
  )
  return next
}

function memoryPath(home: string): string {
  return join(home, "persona", "memory.md")
}
function outboxDir(home: string): string {
  return join(home, "persona", "outbox")
}

/** 现有 section 名（供 append 校验与 observe 展示）。 */
function sectionsOf(text: string): string[] {
  return text
    .split("\n")
    .filter((l) => l.startsWith(SECTION_PREFIX))
    .map((l) => l.slice(SECTION_PREFIX.length).trim())
    .filter(Boolean)
}

/**
 * 删除 UTF-8 BOM。
 *
 * 为何必须处理：Windows 工具（PowerShell `Out-File -Encoding utf8`、记事本、部分编辑器）
 * 默认写 BOM，而 `JSON.parse` 遇到 BOM 会抛 `Unrecognized token '\uFEFF'`。
 * 投递目录是人手操作与脚本齐飞的地方，不能假设文件一定无 BOM。
 */
function stripBom(s: string): string {
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s
}

/** 时间戳文件名（可读 + 单调）。保留中文（\w 不包含 CJK，直接用白名单剔除路径危险字符）。 */
function stampName(base: string): string {
  const d = new Date()
  const p = (n: number) => String(n).padStart(2, "0")
  const ts = `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`
  // 只剔除文件系统危险字符：保留中文/字母/数字/常见连接符（否则「巡检报告.md」会变成「_.md」）
  const safe = base.replace(/[\\/:*?"<>|\x00-\x1f]+/g, "_").trim().slice(0, 80) || "artifact"
  return `${ts}-${safe}`
}

export const channel: ChannelDef = {
  name: "local",
  description: "本机渠道：长期记忆（memory）与产物落盘（outbox）——同一套 act/observe 面表达「自己家的事」",
  envVars: [
    { name: "GEBAI_CHANNELS_LOCAL_OUTBOX_MAX", description: "outbox 单目录条目上限（默认 500，超出不自动删除只提示）" },
    { name: "GEBAI_CHANNELS_LOCAL_DROP_DIR", description: "文件投递目录（缺省 {GEBAI_HOME}/channel-drop/——与事件源 file_drop 的 persona-drop/ 刻意分离，避免双消费）" },
    { name: "GEBAI_CHANNELS_LOCAL_INBOX_DIR", description: "收件目录（缺省 {GEBAI_HOME}/persona-inbox/）" },
  ],

  verbs: {
    append: {
      summary: "向 memory.md 的指定 section 追加一段（section 不存在则创建）",
      writes: true,
      args: {
        section: { type: "string", description: "段落名（如 人 / 事 / 待跟进）", required: true },
        text: { type: "string", description: "要追加的内容（markdown）", required: true },
      },
    },
    replace: {
      summary: "替换 memory.md 全文（慎用：并发时可能覆盖别的会话刚写的内容；优先用 append）",
      writes: true,
      args: { text: { type: "string", description: "新的全文内容", required: true } },
    },
    put: {
      summary: "把一段文本落盘到 outbox/（文件名自动带时间戳），适合产物交付",
      writes: true,
      args: {
        name: { type: "string", description: "文件名提示（会被规整并加时间戳前缀）", required: true },
        content: { type: "string", description: "文件内容", required: true },
      },
    },
  },

  observes: {
    all: {
      summary: "读取 memory.md 全文",
    },
    sections: {
      summary: "列出 memory.md 中已有的段落名",
    },
    section: {
      summary: "读取 memory.md 中指定段落的内容",
      args: { section: { type: "string", description: "段落名", required: true } },
    },
    list: {
      summary: "列出 outbox/ 下的产物文件（名字 + 大小 + 修改时间）",
      args: { limit: { type: "number", description: "最多列出多少条（默认 50）" } },
    },
    get: {
      summary: "读取 outbox/ 下指定文件的内容",
      args: { name: { type: "string", description: "文件名（list 可见）", required: true } },
    },
  },

  /**
   * 输入面：**文件投递**（把 JSON 放进 directory 即产生一条事件）。
   *
   * 为何把投递放在 `local` 而不是单独一个渠道：投递是「本机的事」——与 memory/outbox
   * 同一个世界（本机文件系统）。放在一起更符合「渠道 = 一个外部世界」的语义，
   * 也让 `local` 成为「同一渠道同时声明收发」的完整范例。
   */
  listens: {
    drop: {
      summary: "监听投递目录：把 JSON 放进 {GEBAI_HOME}/channel-drop/ 即产生一条事件（联调与测试入口）",
    },
    inbox: {
      summary: "监听收件目录：把文本/Markdown 放进 {GEBAI_HOME}/persona-inbox/ 即产生一条消息事件（人工投喂）",
    },
  },

  /**
   * 地址级白名单：只认自己家里的两个地址。
   *
   * 这里刻意**给显式清单而非 null**：即便 `local` 是「自己家」，地址写错
   * （如 `local:memory2`）也该直接拒绝并列出可用值，而不是落到一个不存在的路径上
   * 静默建个新文件。
   */
  targets() {
    return ["memory", "outbox"]
  },

  async start(ctx: ChannelContext) {
    await mkdir(outboxDir(ctx.home), { recursive: true })
    ctx.log("info", `local 渠道就绪：memory=${memoryPath(ctx.home)} outbox=${outboxDir(ctx.home)}`)
  },

  /**
   * 订阅本机目录（轮询）。
   *
   * 关键写法（踩过两次的坑）：**状态全放本次调用的闭包**。
   * `startListen` 可能被同一进程的多个注册表实例调用（热加载新旧并存、测试并行、
   * 多租户），模块级变量会互相清空 `handled`/`timer`，表现为「事件偶发丢失」。
   */
  async startListen(sink, ctx) {
    const dropDir = ctx.env("GEBAI_CHANNELS_LOCAL_DROP_DIR") || join(ctx.home, "channel-drop")
    const inboxDir = ctx.env("GEBAI_CHANNELS_LOCAL_INBOX_DIR") || join(ctx.home, "persona-inbox")
    let stopped = false
    let timer: ReturnType<typeof setInterval> | null = null
    const handled = new Set<string>()

    const pollDrop = async (): Promise<void> => {
      if (stopped) return
      let names: string[]
      try {
        names = (await readdir(dropDir)).filter((n) => n.endsWith(".json"))
      } catch {
        return // 目录不存在：静默（首次投递时自然可见）
      }
      for (const name of names) {
        const full = join(dropDir, name)
        if (handled.has(full)) continue
        handled.add(full) // 先登记：并发 poll 不重复处理同一文件
        try {
          const info = await stat(full)
          const parsed = JSON.parse(stripBom(await readFile(full, "utf8"))) as Record<string, unknown>
          if (!parsed.body || !parsed.summary) {
            ctx.log("warn", `${name} 缺少 body/summary，已跳过`)
          } else {
            const r = await sink.emit({
              summary: String(parsed.summary),
              body: String(parsed.body),
              source: typeof parsed.source === "string" ? parsed.source : "本机投递",
              ...(typeof parsed.kind === "string" ? { kind: parsed.kind as never } : {}),
              ...(parsed.refs && typeof parsed.refs === "object" ? { refs: parsed.refs as never } : {}),
              // 幂等键：文件名 + mtime（同一文件被重写视为新事件）
              id: typeof parsed.id === "string" && parsed.id ? parsed.id : `filedrop:${name}@${info.mtimeMs}`,
              at: info.mtimeMs,
            })
            if (r.status !== "accepted") {
              ctx.log("warn", `${name} 未被接管（${r.status}${r.reason ? `：${r.reason}` : ""}）`)
            }
          }
        } catch (err) {
          ctx.log("warn", `处理 ${name} 失败: ${String((err as Error)?.message || err)}`)
        }
        // 处理完即删（保留会无限重扫）；失败也删，坏文件不无限重试
        await rm(full, { force: true }).catch(() => {})
      }
    }

    const pollInbox = async (): Promise<void> => {
      if (stopped) return
      let names: string[]
      try {
        names = (await readdir(inboxDir)).filter((n) => /\.(md|txt|markdown)$/i.test(n))
      } catch {
        return
      }
      for (const name of names) {
        const full = join(inboxDir, name)
        if (handled.has(full)) continue
        handled.add(full)
        try {
          const info = await stat(full)
          const text = stripBom(await readFile(full, "utf8"))
          const firstLine = text.split("\n").find((l) => l.trim()) ?? name
          const r = await sink.emit({
            summary: firstLine.replace(/^#+\s*/, "").slice(0, 120),
            body: text,
            source: `人工投喂 ${name}`,
            kind: "message",
            id: `inbox:${name}@${info.mtimeMs}`,
            at: info.mtimeMs,
          })
          if (r.status !== "accepted") ctx.log("warn", `${name} 未被接管（${r.status}）`)
        } catch (err) {
          ctx.log("warn", `处理 ${name} 失败: ${String((err as Error)?.message || err)}`)
        }
        await rm(full, { force: true }).catch(() => {})
      }
    }

    const tick = async (): Promise<void> => {
      await pollDrop()
      await pollInbox()
    }

    ctx.log("info", `本机输入面监听中：drop=${dropDir} inbox=${inboxDir}`)
    void tick() // 首次立即扫（目录里可能已有存量）
    timer = setInterval(() => void tick(), 1500)
    timer.unref?.()

    // 本次订阅的清理器（闭包持 timer/handled/stopped）
    cleanups.set(cleanupKey(ctx.home, this as object), () => {
      stopped = true
      if (timer !== null) clearInterval(timer)
      timer = null
      handled.clear()
    })
  },

  async stopListen(ctx) {
    const key = cleanupKey(ctx.home, this as object)
    const fn = cleanups.get(key)
    if (fn) {
      cleanups.delete(key)
      fn()
    }
  },

  async act(input, ctx) {
    if (input.verb === "append") {
      const section = String(input.args.section ?? "").trim()
      const text = String(input.args.text ?? "")
      if (!section) throw new Error("append 需要 section（段落名）")
      if (!text.trim()) throw new Error("append 需要非空 text")
      const file = memoryPath(ctx.home)
      return serialize(file, async () => {
        await mkdir(join(ctx.home, "persona"), { recursive: true })
        const existing = await readFile(file, "utf8").catch(() => "")
        const heading = `${SECTION_PREFIX}${section}`
        const lines = existing.split("\n")
        const idx = lines.findIndex((l) => l.trim() === heading)
        if (idx < 0) {
          // 新段落：追加到文件末尾（追加式，不影响已有段落）
          const prefix = existing && !existing.endsWith("\n") ? "\n" : ""
          const chunk = `${prefix}\n${heading}\n${text.trimEnd()}\n`
          await appendFile(file, chunk, "utf8")
          return {
            summary: `已在 memory.md 新建「${section}」段并写入（${text.length} 字符）`,
            ref: file,
            data: { path: file, section, created: true, bytes: Buffer.byteLength(chunk) },
          }
        }
        // 已存在的段落：插入到**该段末尾**（下一个标题之前），而不是文件末尾
        // ——直接 appendFile 会让内容跑到最后一段下面，表现为「写进了别的段」。
        let end = lines.length
        for (let i = idx + 1; i < lines.length; i++) {
          if (lines[i].startsWith(SECTION_PREFIX)) {
            end = i
            break
          }
        }
        // 段末可能带空行：插到它们之前，保持段间空行结构
        while (end > idx + 1 && lines[end - 1].trim() === "") end--
        lines.splice(end, 0, text.trimEnd())
        const next = lines.join("\n")
        await writeFile(file, next, "utf8")
        return {
          summary: `已追加到 memory.md 的「${section}」段（${text.length} 字符）`,
          ref: file,
          data: { path: file, section, created: false, bytes: Buffer.byteLength(next) },
        }
      })
    }

    if (input.verb === "replace") {
      const text = String(input.args.text ?? "")
      const file = memoryPath(ctx.home)
      return serialize(file, async () => {
        await mkdir(join(ctx.home, "persona"), { recursive: true })
        const before = await readFile(file, "utf8").catch(() => "")
        await writeFile(file, text, "utf8")
        return {
          summary: `已替换 memory.md 全文（原 ${before.length} 字符 → 现 ${text.length} 字符）`,
          ref: file,
          data: { path: file, beforeBytes: Buffer.byteLength(before), afterBytes: Buffer.byteLength(text) },
        }
      })
    }

    if (input.verb === "put") {
      const name = String(input.args.name ?? "").trim()
      const content = String(input.args.content ?? "")
      if (!name) throw new Error("put 需要 name")
      const dir = outboxDir(ctx.home)
      return serialize(dir, async () => {
        await mkdir(dir, { recursive: true })
        const fileName = stampName(name)
        const full = join(dir, fileName)
        await writeFile(full, content, "utf8")
        return {
          summary: `已落盘 outbox/${fileName}（${Buffer.byteLength(content)} 字节）`,
          ref: full,
          data: { path: full, name: fileName, bytes: Buffer.byteLength(content) },
        }
      })
    }

    throw new Error(`local 渠道不支持 verb「${input.verb}」`)
  },

  async observe(input, ctx) {
    if (input.what === "all") {
      const text = await readFile(memoryPath(ctx.home), "utf8").catch(() => "")
      return {
        summary: text.trim() ? text : "（memory.md 还是空的）",
        data: { text, path: memoryPath(ctx.home), sections: sectionsOf(text) },
      }
    }

    if (input.what === "sections") {
      const text = await readFile(memoryPath(ctx.home), "utf8").catch(() => "")
      const list = sectionsOf(text)
      return {
        summary: list.length ? `memory.md 共 ${list.length} 段：${list.join("、")}` : "（memory.md 还没有任何段落）",
        data: { sections: list },
      }
    }

    if (input.what === "section") {
      const section = String(input.args.section ?? "").trim()
      if (!section) throw new Error("section 需要段落名")
      const text = await readFile(memoryPath(ctx.home), "utf8").catch(() => "")
      const lines = text.split("\n")
      const idx = lines.findIndex((l) => l.trim() === `${SECTION_PREFIX}${section}`)
      if (idx < 0) {
        return { summary: `memory.md 没有「${section}」段（已有：${sectionsOf(text).join("、") || "无"}）`, data: { found: false, section } }
      }
      const body: string[] = []
      for (let i = idx + 1; i < lines.length; i++) {
        if (lines[i].startsWith(SECTION_PREFIX)) break
        body.push(lines[i])
      }
      const content = body.join("\n").trim()
      return { summary: content || `（「${section}」段是空的）`, data: { found: true, section, content } }
    }

    if (input.what === "list") {
      const limit = Math.max(1, Math.min(500, Number(input.args.limit ?? 50) || 50))
      const dir = outboxDir(ctx.home)
      let names: string[]
      try {
        names = (await readdir(dir)).filter((n) => !n.startsWith("."))
      } catch {
        return { summary: "（outbox/ 还不存在，尚无产物）", data: { files: [] } }
      }
      names.sort()
      const shown = names.slice(-limit)
      const rows: Array<{ name: string; size: number; mtime: number }> = []
      for (const n of shown) {
        const st = await stat(join(dir, n)).catch(() => null)
        if (st?.isFile()) rows.push({ name: n, size: st.size, mtime: st.mtimeMs })
      }
      const lines = rows.map((r) => `- ${r.name}（${r.size} 字节，${new Date(r.mtime).toLocaleString()}）`)
      return {
        summary: rows.length ? `outbox/ 共 ${names.length} 个产物（显示最近 ${rows.length} 个）：\n${lines.join("\n")}` : "（outbox/ 是空的）",
        data: { files: rows, total: names.length },
      }
    }

    if (input.what === "get") {
      const name = String(input.args.name ?? "").trim()
      if (!name) throw new Error("get 需要 name")
      // 路径穿越防护：name 只允许文件名，不允许分隔符
      if (name.includes("/") || name.includes("\\") || name.includes("..")) {
        throw new Error(`name 只能是 outbox/ 下的文件名（不接受路径分隔符）：${name}`)
      }
      const full = join(outboxDir(ctx.home), name)
      const text = await readFile(full, "utf8").catch(() => null)
      if (text === null) {
        const dir = outboxDir(ctx.home)
        const names = (await readdir(dir).catch(() => [] as string[])).slice(-20)
        throw new Error(`outbox/ 下没有「${name}」${names.length ? `（可用：${names.join("、")}）` : "（目录为空）"}`)
      }
      return { summary: text, data: { name, path: full, text } }
    }

    throw new Error(`local 渠道不支持 observe「${input.what}」`)
  },
}
