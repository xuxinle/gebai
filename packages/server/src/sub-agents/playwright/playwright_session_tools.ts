import type { ToolSet } from "../../core/base/types"
import { artifactBlocks, truncate, assertPublicHttpUrl } from "../../core/tools"
import type { ToolSchema } from "@gebai/sdk"
import { copyFile, mkdir } from "node:fs/promises"
import { dirname } from "node:path"
import { createLazyBridge, withSessionLock, type BridgeLike } from "../../core/browser/bridge"

/**
 * playwright 子Agent 会话类工具集：存储/仿真/下载/对话框/导出。
 *
 * 与 playwright_tools.ts 共享同一惰性桥接单例与会话锁（同会话操作同一浏览器上下文）。
 * 安全口径：真实凭证（cookie 值/localStorage/storageState）只在审批门控工具中暴露，
 * 与 evaluate 同级敏感；文件落盘一律经 ctx.resolvePath（沙箱内）。
 */

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

function num(v: unknown, dflt: number): number {
  const n = Number(v)
  return Number.isFinite(n) && n > 0 ? n : dflt
}

function fail(err: unknown): string {
  return err instanceof Error ? err.message : String(err)
}

export function createPlaywrightSessionTools(deps: { bridge?: BridgeLike } = {}): ToolSet {
  const bridge: BridgeLike = deps.bridge ?? createLazyBridge()
  const request = (sessionId: string, op: string, args: Record<string, unknown>): Promise<unknown> =>
    withSessionLock(sessionId, () => bridge.request(op, { sessionId, ...args }))

  return {
    pdf: {
      name: "pdf",
      description: "把当前页面导出为 PDF（打印版渲染），保存到会话 tmp/ 并返回文件。format 纸张型号（默认 A4），landscape 横向。",
      card: { args: "none" },
      parameters: schema({
        file: { type: "string", description: "可选：保存的相对路径（默认 tmp/ 下自动命名 .pdf）" },
        format: { type: "string", description: "纸张型号：A1/A2/A3/A4/A5/Legal/Letter/Tabloid（默认 A4）" },
        landscape: { type: "boolean", description: "是否横向（默认 false）" },
      }),
      async execute(args, ctx) {
        const rel = String(args.file ?? "").trim() || `tmp/playwright_pdf_${Date.now()}.pdf`
        const abs = ctx.resolvePath(rel)
        try {
          await request(ctx.sessionId, "pdf", { path: abs, format: String(args.format ?? "A4"), landscape: args.landscape === true })
          return { output: `已导出 PDF: ${rel}\n绝对路径: ${abs}`, blocks: artifactBlocks(rel) }
        } catch (err) {
          return { output: `PDF 导出失败: ${fail(err)}` }
        }
      },
    },

    downloads: {
      name: "downloads",
      description: "查看/保存页面触发的文件下载（页面点击下载链接后自动捕获到临时目录，几秒后可见）。action=list 列出已完成下载；action=save 把指定下载复制进会话目录。",
      parameters: schema({
        action: { type: "string", description: "list（默认，列出下载）/ save（保存指定下载到会话目录）" },
        index: { type: "number", description: "save 时的下载序号（list 返回的 [n]）" },
        dest: { type: "string", description: "save 时的目标相对路径（默认按原文件名存到 tmp/）" },
      }),
      async execute(args, ctx) {
        const action = String(args.action ?? "list")
        let r: { downloads: Array<{ filename: string; path: string; url: string; time: number }> }
        try {
          r = (await request(ctx.sessionId, "downloads_list", {})) as typeof r
        } catch (err) {
          return { output: `读取下载记录失败: ${fail(err)}` }
        }
        if (action === "save") {
          const idx = Number(args.index)
          const dl = r.downloads[idx]
          if (!dl) return { output: `下载序号无效（现有 ${r.downloads.length} 条，见 action=list）` }
          const dest = String(args.dest ?? "").trim() || `tmp/${dl.filename}`
          const abs = ctx.resolvePath(dest)
          try {
            await mkdir(dirname(abs), { recursive: true })
            await copyFile(dl.path, abs)
            return { output: `已保存下载到: ${dest}`, blocks: artifactBlocks(dest) }
          } catch (err) {
            return { output: `保存失败: ${fail(err)}` }
          }
        }
        if (r.downloads.length === 0) return { output: "暂无下载记录。页面点击下载链接后会自动捕获（保存需几秒），稍后再查。" }
        const lines = r.downloads.map((d, i) => `[${i}] ${d.filename}\n    来源: ${d.url}`)
        return { output: `共 ${r.downloads.length} 个下载（save + index 保存到会话目录）：\n${lines.join("\n")}` }
      },
    },

    dialogs: {
      name: "dialogs",
      description: "管理页面对话框（alert/confirm/prompt/beforeunload）：action=list 查看已弹出的对话框记录；action=auto 配置自动应答（mode=accept/dismiss，prompt 可配 prompt_text；省略 mode = 取消自动应答）；action=clear 清空记录。未配置自动应答时对话框一律 dismiss（页面不会卡住），需接受时在触发交互前配置 auto。",
      parameters: schema({
        action: { type: "string", description: "list（默认）/ auto / clear" },
        mode: { type: "string", description: "auto 时的应答方式：accept（接受，如确认删除）/ dismiss（拒绝）；省略表示取消自动应答恢复默认 dismiss" },
        prompt_text: { type: "string", description: "auto + accept 时 prompt 对话框填入的文本" },
      }),
      async execute(args, ctx) {
        const action = String(args.action ?? "list")
        try {
          if (action === "auto") {
            const mode = args.mode === undefined ? "" : String(args.mode)
            const r = (await request(ctx.sessionId, "dialog_auto", { mode, promptText: args.prompt_text === undefined ? "" : String(args.prompt_text) })) as { auto: { mode: string; promptText: string } | null }
            return { output: r.auto ? `已配置自动应答: ${r.auto.mode}${r.auto.promptText ? `（prompt 填入 "${r.auto.promptText}"）` : ""}` : "已取消自动应答（恢复默认 dismiss）" }
          }
          if (action === "clear") {
            const r = (await request(ctx.sessionId, "dialog_clear", {})) as { cleared: number }
            return { output: `已清空 ${r.cleared} 条对话框记录` }
          }
          if (action !== "list") return { output: `action 必须是 list/auto/clear: ${action}` }
          const r = (await request(ctx.sessionId, "dialog_list", {})) as {
            dialogs: Array<{ type: string; message: string; defaultText?: string; handled?: string; time: number }>
            auto: { mode: string; promptText: string } | null
          }
          const head = `自动应答: ${r.auto ? r.auto.mode : "未配置（默认 dismiss）"}`
          if (r.dialogs.length === 0) return { output: `${head}\n暂无对话框记录` }
          const lines = r.dialogs.map((d) => `[${d.type}] ${d.message}${d.defaultText ? `（默认输入: ${d.defaultText}）` : ""} → ${d.handled ?? "-"}`)
          return { output: `${head}\n已弹出 ${r.dialogs.length} 个对话框：\n${lines.join("\n")}` }
        } catch (err) {
          return { output: `对话框操作失败: ${fail(err)}` }
        }
      },
    },

    emulate: {
      name: "emulate",
      description:
        "浏览器环境仿真：视口尺寸（width+height）/用户代理（user_agent）/语言（locale，如 zh-CN）/时区（timezone，如 Asia/Shanghai）/移动端模式（mobile=true 含触摸）。注意：立即生效会重建浏览器上下文——已打开页面、Cookie 与登录态清空（需保留先 storage_state save）；action=reset 恢复默认环境。",
      parameters: schema({
        user_agent: { type: "string", description: "用户代理字符串（UA 伪装/移动端 UA 测试）" },
        locale: { type: "string", description: "浏览器语言（如 zh-CN / en-US）" },
        timezone: { type: "string", description: "时区标识（如 Asia/Shanghai）" },
        width: { type: "number", description: "视口宽（与 height 同时提供）" },
        height: { type: "number", description: "视口高（与 width 同时提供）" },
        mobile: { type: "boolean", description: "移动端模式（视口触摸 + isMobile，配合移动 UA 使用）" },
        action: { type: "string", description: "reset = 恢复默认环境（省略 = 应用仿真参数）" },
      }),
      async execute(args, ctx) {
        try {
          if (String(args.action ?? "") === "reset") {
            await request(ctx.sessionId, "emulate", { reset: true })
            return { output: "已恢复默认浏览器环境（上下文已重建）" }
          }
          const payload: Record<string, unknown> = {}
          if (args.user_agent !== undefined && String(args.user_agent).trim()) payload.userAgent = String(args.user_agent).trim()
          if (args.locale !== undefined && String(args.locale).trim()) payload.locale = String(args.locale).trim()
          if (args.timezone !== undefined && String(args.timezone).trim()) payload.timezoneId = String(args.timezone).trim()
          if (args.width !== undefined || args.height !== undefined) {
            const w = num(args.width, 0)
            const h = num(args.height, 0)
            if (!w || !h) return { output: "width 与 height 需同时为正整数" }
            payload.width = w
            payload.height = h
          }
          if (args.mobile !== undefined) payload.mobile = args.mobile === true
          if (Object.keys(payload).length === 0) return { output: "未提供仿真参数（user_agent/locale/timezone/width/height/mobile）" }
          const r = (await request(ctx.sessionId, "emulate", payload)) as { emulated: Record<string, unknown> | null }
          return { output: `仿真已应用（浏览器上下文已重建，页面/Cookie 清空）: ${JSON.stringify(r.emulated)}` }
        } catch (err) {
          return { output: `仿真设置失败: ${fail(err)}` }
        }
      },
    },

    cookies: {
      name: "cookies",
      description:
        "读写浏览器 Cookie（当前会话上下文）：action=list（默认，列出全部 cookie，可按 urls 过滤）/ set（注入 cookie 对象数组）/ clear（清空全部）。输出含真实凭证值，用于登录态分析与注入。",
      parameters: schema({
        action: { type: "string", description: "list（默认）/ set / clear" },
        cookies: { type: "string", description: 'set 时的 cookie 对象数组 JSON，如 [{"name":"sid","value":"abc","url":"https://a.com"}]（每项需 name/value + url 或 domain+path，可选 expires/httpOnly/secure/sameSite）' },
        urls: { type: "string", description: "list 时的过滤 URL（单个字符串或 JSON 字符串数组，只列匹配域的 cookie）" },
      }),
      async execute(args, ctx) {
        const action = String(args.action ?? "list")
        try {
          if (action === "set") {
            const raw = args.cookies === undefined ? "" : String(args.cookies).trim()
            if (!raw) return { output: "缺少 cookies 参数（cookie 对象数组 JSON）" }
            let parsed: unknown
            try {
              parsed = JSON.parse(raw)
            } catch {
              return { output: `cookies 不是合法 JSON: ${raw.slice(0, 100)}` }
            }
            if (!Array.isArray(parsed) || parsed.length === 0 || parsed.some((c) => !c || typeof c !== "object")) {
              return { output: "cookies 必须是非空 cookie 对象数组" }
            }
            const r = (await request(ctx.sessionId, "cookies_set", { cookies: parsed })) as { set: number }
            return { output: `已注入 ${r.set} 个 cookie` }
          }
          if (action === "clear") {
            await request(ctx.sessionId, "cookies_clear", {})
            return { output: "已清空全部 cookie" }
          }
          if (action !== "list") return { output: `action 必须是 list/set/clear: ${action}` }
          let urls: string[] | undefined
          if (args.urls !== undefined && String(args.urls).trim()) {
            const raw = String(args.urls).trim()
            try {
              const p = JSON.parse(raw)
              urls = Array.isArray(p) ? p.map(String) : [String(p)]
            } catch {
              urls = [raw] // 非 JSON 按单个 URL
            }
          }
          const r = (await request(ctx.sessionId, "cookies_get", urls ? { urls } : {})) as {
            cookies: Array<{ name: string; value: string; domain: string; path: string; expires?: number; httpOnly?: boolean; secure?: boolean }>
          }
          if (r.cookies.length === 0) return { output: "当前上下文没有 cookie（未打开页面或未登录）" }
          const lines = r.cookies.map((c) => `${c.domain}${c.path}  ${c.name}=${c.value}${c.httpOnly ? "  [httpOnly]" : ""}${c.secure ? "  [secure]" : ""}`)
          return truncate(`共 ${r.cookies.length} 个 cookie：\n${lines.join("\n")}`, "playwright_cookies", ctx)
        } catch (err) {
          return { output: `cookie 操作失败: ${fail(err)}` }
        }
      },
    },

    local_storage: {
      name: "local_storage",
      description: "读写当前页面的 localStorage（作用域 = 当前页面 origin，操作前先 open 到目标站点）：action=list（默认，列出全部键值）/ get（读 key）/ set（写 key=value）/ remove（删 key）/ clear（清空）。",
      parameters: schema({
        action: { type: "string", description: "list（默认）/ get / set / remove / clear" },
        key: { type: "string", description: "get/set/remove 时的键名" },
        value: { type: "string", description: "set 时的值" },
      }),
      async execute(args, ctx) {
        const action = String(args.action ?? "list")
        const key = args.key === undefined ? "" : String(args.key)
        try {
          if (action === "set") {
            if (!key) return { output: "缺少 key 参数" }
            if (args.value === undefined) return { output: "缺少 value 参数" }
            await request(ctx.sessionId, "local_storage", { action, key, value: String(args.value) })
            return { output: `已写入 localStorage[${key}]` }
          }
          if (action === "get" || action === "remove") {
            if (!key) return { output: "缺少 key 参数" }
            const r = (await request(ctx.sessionId, "local_storage", { action, key })) as { value?: string | null }
            if (action === "remove") return { output: `已删除 localStorage[${key}]` }
            return { output: r.value === null || r.value === undefined ? `(键不存在: ${key})` : `${key} = ${r.value}` }
          }
          if (action === "clear") {
            await request(ctx.sessionId, "local_storage", { action })
            return { output: "已清空当前页面 localStorage" }
          }
          if (action !== "list") return { output: `action 必须是 list/get/set/remove/clear: ${action}` }
          const r = (await request(ctx.sessionId, "local_storage", { action })) as { items: Record<string, string> }
          const entries = Object.entries(r.items ?? {})
          if (entries.length === 0) return { output: "当前页面没有 localStorage 数据" }
          return truncate(entries.map(([k, v]) => `${k} = ${v}`).join("\n"), "playwright_local_storage", ctx)
        } catch (err) {
          return { output: `localStorage 操作失败: ${fail(err)}` }
        }
      },
    },

    storage_state: {
      name: "storage_state",
      description:
        "登录态整体保存/恢复（playwright storageState 格式 = Cookie + 各站点 localStorage）：action=save 把当前登录态写入文件（默认 tmp/browser_state.json）；action=restore 从文件恢复到当前浏览器（恢复后无需重新登录，restore 会访问各 origin 写入 localStorage）。文件含敏感凭证，只存会话目录、不外传不入版本库。",
      parameters: schema({
        action: { type: "string", description: "save（默认）/ restore" },
        file: { type: "string", description: "状态文件相对路径（默认 tmp/browser_state.json；多套登录态用不同文件名区分）" },
      }),
      async execute(args, ctx) {
        const action = String(args.action ?? "save")
        const rel = String(args.file ?? "").trim() || "tmp/browser_state.json"
        const abs = ctx.resolvePath(rel)
        try {
          if (action === "restore") {
            let state: { cookies?: unknown[]; origins?: Array<{ origin: string; localStorage?: unknown[] }> }
            try {
              state = JSON.parse(await ctx.readFile(abs))
            } catch {
              return { output: `状态文件不存在或不是合法 JSON: ${rel}（先 storage_state save 生成）` }
            }
            if (!state || !Array.isArray(state.cookies)) return { output: `状态文件格式无效（非 storageState JSON）: ${rel}` }
            if (ctx.sandboxed) {
              for (const o of state.origins ?? []) {
                try {
                  await assertPublicHttpUrl(o.origin)
                } catch (err) {
                  return { output: `storage_state restore 失败: origin 非公网地址（${o.origin}）: ${fail(err)}` }
                }
              }
            }
            const r = (await request(ctx.sessionId, "storage_apply", { state })) as { cookies: number; origins: number; warnings: string[] }
            const warn = r.warnings?.length ? `\n告警: ${r.warnings.join("; ")}` : ""
            return { output: `登录态已恢复: cookie ${r.cookies} 个 / origin ${r.origins} 个${warn}` }
          }
          if (action !== "save") return { output: `action 必须是 save/restore: ${action}` }
          const r = (await request(ctx.sessionId, "storage_save", {})) as {
            state: unknown
            cookies: number
            origins: number
          }
          await ctx.writeFile(abs, JSON.stringify(r.state, null, 2))
          return { output: `登录态已保存: ${rel}（cookie ${r.cookies} 个 / origin ${r.origins} 个）\n恢复用 storage_state action=restore file=${rel}` }
        } catch (err) {
          return { output: `登录态操作失败: ${fail(err)}` }
        }
      },
    },
  }
}
