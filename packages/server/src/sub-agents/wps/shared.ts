import type { ToolSchema } from "@gebai/sdk"
import { artifactBlocks, previewLogicalPath } from "../../core/tools"
import type { ToolContext } from "../../core/types"

export function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

/** 颜色统一为 6 位大写 RRGGBB（无 # 前缀）：接受 "FF0000"/"#FF0000"/"F00"（3 位展开）；非法返回 null。 */
export function normColor(v: unknown): string | null {
  if (typeof v !== "string") return null
  let s = v.trim().replace(/^#/, "")
  if (/^[0-9a-fA-F]{3}$/.test(s)) s = s.split("").map((c) => c + c).join("")
  if (!/^[0-9a-fA-F]{6}$/.test(s)) return null
  return s.toUpperCase()
}

/** 图片扩展名 → docx ImageRun / OOXML content type 用类型名。 */
export function imageTypeOf(path: string): "png" | "jpg" | "gif" | "bmp" | null {
  const m = /\.([a-z0-9]+)$/i.exec(path)
  const ext = m ? m[1].toLowerCase() : ""
  if (ext === "png") return "png"
  if (ext === "jpg" || ext === "jpeg") return "jpg"
  if (ext === "gif") return "gif"
  if (ext === "bmp") return "bmp"
  return null
}

/** 探测图片像素尺寸（PNG/JPEG/GIF/BMP，头部字节直读）；未知格式返回 null（调用方给默认尺寸）。 */
export function probeImageSize(bytes: Uint8Array, type: string): { width: number; height: number } | null {
  try {
    if (type === "png" && bytes.length > 24) {
      const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
      return { width: dv.getUint32(16), height: dv.getUint32(20) }
    }
    if (type === "gif" && bytes.length > 10) return { width: bytes[6] | (bytes[7] << 8), height: bytes[8] | (bytes[9] << 8) }
    if (type === "bmp" && bytes.length > 26) {
      const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
      return { width: dv.getInt32(18, true), height: Math.abs(dv.getInt32(22, true)) }
    }
    if (type === "jpg") {
      // 逐段扫描 SOS 前的 SOFn 标记（C0-CF 除 C4/C8/CC）
      let i = 2
      while (i + 9 < bytes.length) {
        if (bytes[i] !== 0xff) { i++; continue }
        const marker = bytes[i + 1]
        if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) {
          const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
          return { height: dv.getUint16(i + 5), width: dv.getUint16(i + 7) }
        }
        const len = (bytes[i + 2] << 8) | bytes[i + 3]
        if (len <= 0) break
        i += 2 + len
      }
    }
  } catch {
    /* 头部异常按未知处理 */
  }
  return null
}

/** 读取嵌入图片（会话/项目内路径）：返回字节 + 归一化类型；非图片扩展名抛错。 */
export async function readImage(relPath: string, ctx: ToolContext): Promise<{ bytes: Uint8Array; type: "png" | "jpg" | "gif" | "bmp"; path: string }> {
  const abs = ctx.resolvePath(relPath)
  const type = imageTypeOf(abs)
  if (!type) throw new Error(`图片路径 ${relPath} 扩展名不受支持（支持 png/jpg/jpeg/gif/bmp）`)
  const bytes = await ctx.readBinaryFile(abs)
  return { bytes, type, path: abs }
}

/** 图片默认展示尺寸：探测自然尺寸（px，96dpi 口径），超宽按 480px 等比缩小；探测失败给 480×320。 */
export function fitImage(bytes: Uint8Array, type: string, width?: number, height?: number): { width: number; height: number } {
  if (width && height) return { width: Math.round(width), height: Math.round(height) }
  const nat = probeImageSize(bytes, type)
  const w = nat?.width || 480
  const h = nat?.height || 320
  if (width) return { width: Math.round(width), height: Math.round((width * h) / w) }
  if (height) return { width: Math.round((height * w) / h), height: Math.round(height) }
  if (w <= 480) return { width: w, height: h }
  return { width: 480, height: Math.round((480 * h) / w) }
}

/** 写入文件的前置守卫（wps 各写工具共用）：二进制写通道可用性 + 写范围守卫（self_optimize 等装载期政策）。 */
export async function writeGuards(absPath: string, ctx: ToolContext): Promise<string | null> {
  if (!ctx.writeBinaryFile) {
    return "当前环境未启用二进制写入通道（writeBinaryFile），无法生成 Office 文档。"
  }
  const guardMsg = await ctx.writeGuard?.([absPath])
  if (guardMsg) return guardMsg
  return null
}

/** 防盲覆盖守卫（全局 write 同语义）：文件已存在且本会话未读取过 → 拒绝（先 word_read/excel_read/read 后再覆盖）。 */
export async function blindOverwriteGuard(absPath: string, displayPath: string, ctx: ToolContext): Promise<string | null> {
  if (!ctx.fileGuard) return null
  try {
    await ctx.readBinaryFile(absPath)
  } catch {
    return null // 不存在 → 新建，不受限
  }
  if (!ctx.fileGuard.hasRead(absPath)) {
    return `目标文件 ${displayPath} 已存在，但本会话尚未读取过其内容（防盲覆盖）。请先读取（word_read/excel_read/ppt_read 或全局 read）确认现有内容后再整体覆盖；只在末尾追加内容用 word_append / 局部修改用 excel_edit。`
  }
  return null
}

/** 产物文件块（前端文件面板展示/下载，路径转可预览逻辑路径）。 */
export function fileBlocks(absPath: string, ctx: ToolContext) {
  return artifactBlocks(previewLogicalPath(absPath, ctx))
}

/** 数字/整数参数宽松取值（undefined 用默认值；非法 NaN 回退默认值）。 */
export function asNum(v: unknown, def: number): number {
  const n = typeof v === "number" ? v : typeof v === "string" ? Number(v) : NaN
  return Number.isFinite(n) ? n : def
}
