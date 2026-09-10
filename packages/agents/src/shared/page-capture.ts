/** page_capture 工具（自 server core/tools/extras.ts 迁入；waitForCapture 已入 sdk 契约——
 *  引擎按任务注入，测试桩未注入时返回不可用说明）。 */
import { dirname } from "node:path"
import type { ContentBlock, Tool } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"

/** 前端捕获 html 截断长度（WS 传输与落盘上限；完整 DOM 通常远超此，超出截取首部）。 */
export const PAGE_CAPTURE_HTML_LIMIT = 300 * 1024

export const pageCaptureTool: Tool = {
  name: "page_capture",
  // 仅实时前端可用（请求当前页面捕获并由前端回传），多轮交互/无交互模式禁用
  interaction: "realtime",
  description:
    "请求前端（当前浏览器页面）捕获实际渲染结果：读取渲染后的 DOM html 与页面截图，产物落盘会话 tmp/capture/。适合验证 Web UI 修改后的真实效果——页面即当前打开的 歌白界面（dev 模式修改后自动热更新，捕获前可提示用户刷新页面）；html 用 read 读取完整内容，截图用 read 直接查看（主模型多模态时图片内联进上下文）或 vision 子代理（vision_analyze）分析视觉效果。前端离线或 30 秒未响应时返回失败。",
  parameters: schema({
    full_page: { type: "boolean", description: "是否截整页（默认 false 截视口首屏；整页含全部滚动内容，大页面截图较慢）" },
    delay: { type: "number", description: "捕获前等待毫秒数（默认 0；UI 操作/动画/异步渲染完成后截图，上限 10000）" },
  }),
  async execute(args, ctx) {
    const delayMs = Math.max(0, Math.min(10000, Number(args.delay) || 0))
    if (!ctx.waitForCapture) return { output: "当前环境不支持页面捕获（waitForCapture 服务未注入）。" }
    // full_page 入参 → 前端捕获契约载荷键 fullPage（WS 协议字段，两端契约不动）
    const cap = await ctx.waitForCapture({ fullPage: args.full_page === true, delayMs })
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
      output: `已捕获当前页面: ${htmlRel}（${cap.html.length} 字符，可用 read 读取完整内容）${imgRel ? `；截图 ${imgRel}（${ctx.multimodal ? "可用 read 直接查看（多模态内联）" : "可用 vision_analyze 分析图片内容（vision 子代理）"}）` : "；前端未返回截图"}`,
      blocks,
    }
  },
}

// save_tool/delete_tool（HTML 小工具库）已下沉 widgets 子Agent：core/mini-tools.ts 提供存储实现，
// widgets_save/widgets_list/widgets_get/widgets_delete 以子Agent 命名空间暴露（与模型工具语义区分）
// draw/render_html/show_file 三工具已合并为 show（内容统一展示入口）：图表/HTML/文件三分支
