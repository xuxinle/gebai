/** 会话文件域路由：/sessions/:id/files 列表/内容/下载（含 zip 打包）/预览（含 Office 阅读视图）。
 *  依赖 sessions.ts 注册的会话 id 白名单中间件（app.ts 装配顺序保证先于本文件挂载）。 */
import type { RouteCtx } from "./context"
import type { FileEntry } from "@gebai/sdk"
import { existsSync, statSync } from "node:fs"
import { buildZip } from "../zip"

export function registerSessionFileRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  app.get("/api/v1/sessions/:id/files", async (c) => {
    const user = await userOf(c)
    const files: FileEntry[] = await d.store.listSessionFiles(c.req.param("id"), user.id)
    return c.json(files)
  })
  app.get("/api/v1/sessions/:id/files/content", async (c) => {
    const user = await userOf(c)
    const path = c.req.query("path") || ""
    // 文件接口以会话 tmp/ 为根（DESIGN：文件操作严格限定在会话 tmp/ 内），兼容 tmp/ 前缀路径
    const safe = d.store.resolveSessionTmpFile(c.req.param("id"), user.id, path, d.sandbox.enforcedFor(user.id))
    // 原始字节流式返回（Bun.file 自动按扩展名设置 Content-Type，如 image/png）：
    // 图片等二进制经 text() 会被 UTF-8 解码损坏，前端 <img> 将无法解码显示
    return new Response(Bun.file(safe))
  })
  app.get("/api/v1/sessions/:id/files/download", async (c) => {
    const user = await userOf(c)
    const path = c.req.query("path") || ""
    const safe = d.store.resolveSessionTmpFile(c.req.param("id"), user.id, path, d.sandbox.enforcedFor(user.id))
    const file = Bun.file(safe)
    return new Response(file.stream(), { headers: { "Content-Disposition": `attachment; filename="${encodeURIComponent(path)}"` } })
  })
  // 多选打包下载：POST body 指定 paths 列表，返回 zip（DESIGN REST 协议表）
  app.post("/api/v1/sessions/:id/files/download", async (c) => {
    const user = await userOf(c)
    const body = (await c.req.json().catch(() => ({}))) as { paths?: string[] }
    const files: Array<{ name: string; data: Uint8Array }> = []
    for (const p of body.paths ?? []) {
      const safe = d.store.resolveSessionTmpFile(c.req.param("id"), user.id, p, d.sandbox.enforcedFor(user.id))
      if (!existsSync(safe)) return c.json({ error: `file not found: ${p}` }, 404)
      const buf = await Bun.file(safe).arrayBuffer()
      files.push({ name: p, data: new Uint8Array(buf) })
    }
    const zip = buildZip(files)
    return new Response(zip, {
      headers: { "Content-Type": "application/zip", "Content-Disposition": 'attachment; filename="files.zip"' },
    })
  })

  // 文件预览（DESIGN「文件链接弹窗查看」）：read/write/edit/patch 等文件工具卡片链接的取数入口——
  // 会话相对路径以 tmp/ 为根；绝对路径（code 项目文件）按用户隔离边界放行（沙箱用户限本用户数据目录内）。
  // ?download=1 时以附件形式返回（文件卡工具栏下载对项目文件同样经此入口）。
  app.get("/api/v1/sessions/:id/files/preview", async (c) => {
    const user = await userOf(c)
    const path = c.req.query("path") || ""
    let safe: string
    try {
      safe = d.store.resolvePreviewFile(c.req.param("id"), user.id, path, d.sandbox.enforcedFor(user.id))
    } catch (err) {
      return c.json({ error: String((err as Error).message || err) }, 403)
    }
    let isFile = false
    try {
      isFile = statSync(safe).isFile()
    } catch {
      return c.json({ error: `file not found: ${path}` }, 404)
    }
    if (!isFile) return c.json({ error: `not a file: ${path}` }, 404)
    // Office 阅读视图（wps 文档预览，DESIGN「文件链接弹窗查看」）：docx/xlsx/xlsm/pptx 按本参数返回
    // 结构化 HTML（前端文件卡/弹窗 iframe 渲染）；渲染器惰性引入（exceljs/docx 解析较重，不拖启动），
    // 解析单一真相源在 wps 子Agent。非法/损坏文件 422（前端回退二进制占位与下载引导）。
    if (c.req.query("render") === "office") {
      const { renderOfficeReadingView } = await import("../sub-agents/wps/preview")
      try {
        const html = await renderOfficeReadingView(safe)
        return new Response(html, { headers: { "Content-Type": "text/html; charset=utf-8" } })
      } catch (err) {
        return c.json({ error: `office 阅读视图渲染失败: ${(err as Error).message}` }, 422)
      }
    }
    const headers: Record<string, string> = {}
    if (c.req.query("download") === "1") headers["Content-Disposition"] = `attachment; filename="${encodeURIComponent(path.replace(/\\/g, "/").split("/").pop() || "file")}"`
    return new Response(Bun.file(safe), { headers })
  })
}
