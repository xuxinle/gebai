/**
 * 桌面本地识别工具集（desktop 专用）：本地小模型图像识别——OCR 文本读取/定位与自备
 * YOLO 目标检测（core/cv 域，onnxruntime-web wasm 进程内推理，离线运行不耗模型配额）。
 * 全部只读操作（不点击/不输入），坐标返回相对（截图区域/图像）原点的像素值，
 * 供 mouse_click 直接使用；识别在裁剪/缩放后的图像上进行，坐标一律映射回原始像素系。
 */
import type { Tool, ToolContext, ToolResult } from "../../core/base/types"
import type { ToolSchema } from "@gebai/sdk"
import { getCvRunner } from "../../core/cv/cv"
import { cropImage, decodePng, type RgbaImage } from "../../core/cv/image"
import { VISION_MAX_IMAGE_BYTES } from "../../core/tools/vision"

function desktopGate(ctx: ToolContext): void {
  if (ctx.sandboxed) throw new Error("桌面控制仅在本地/桌面模式可用（服务端部署已禁用）")
}

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

/** PowerShell 脚本 → 命令串：UTF-16LE base64 避免引号转义（cmd 兼容）。 */
function ps(script: string): string {
  const b64 = Buffer.from(script, "utf16le").toString("base64")
  return `powershell -NoProfile -NonInteractive -EncodedCommand ${b64}`
}

/** OCR 结果行数上限（超出截断提示用 desktop_ocr 的 find 过滤收窄）。 */
const OCR_LINE_LIMIT = 200

interface LineOut {
  text: string
  score: number
  x: number
  y: number
  w: number
  h: number
}

/** 载入待识别图像：image 参数优先（PNG），省略则现截一张（region 可限定区域）。返回图像与坐标偏移。 */
async function loadOrCapture(
  ctx: ToolContext,
  image: string,
  region: string,
): Promise<{ img: RgbaImage; offX: number; offY: number; sourceDesc: string } | { error: string }> {
  if (region && !/^\d+,\d+,\d+,\d+$/.test(region)) {
    return { error: `region 格式错误: ${region}（应为 x,y,w,h）` }
  }
  let img: RgbaImage
  let offX = 0
  let offY = 0
  let sourceDesc: string
  if (image) {
    const path = ctx.resolvePath(image)
    if (!path.toLowerCase().endsWith(".png")) {
      return { error: `本地识别仅支持 PNG（当前 ${image}）——JPEG 等格式请先转换，或直接省略 image 现截屏幕` }
    }
    const bytes = await ctx.readBinaryFile(path)
    if (bytes.byteLength > VISION_MAX_IMAGE_BYTES) {
      return { error: `图片过大（${(bytes.byteLength / 1024 / 1024).toFixed(1)}MB，上限 8MB）` }
    }
    try {
      img = decodePng(bytes)
    } catch (e) {
      return { error: `PNG 解码失败: ${e instanceof Error ? e.message : e}` }
    }
    sourceDesc = `图像 ${image}`
  } else {
    const rel = `cv_capture_${Date.now()}.png`
    const path = ctx.resolvePath(rel)
    const cap = await captureTo(ctx, path, region)
    if ("error" in cap) return cap
    try {
      img = decodePng(await ctx.readBinaryFile(path))
    } catch (e) {
      return { error: `截图解码失败: ${e instanceof Error ? e.message : e}` }
    }
    sourceDesc = `当前屏幕${region ? `（区域 ${region}）` : "（全屏）"}`
  }
  if (region) {
    const [x, y, w, h] = region.split(",").map(Number)
    img = cropImage(img, { x, y, w, h })
    offX = x
    offY = y
  }
  return { img, offX, offY, sourceDesc }
}

/** 平台截图命令（无统计——cv 场景不需要黑帧检测）。 */
async function captureTo(ctx: ToolContext, path: string, region: string): Promise<{ ok: true } | { error: string }> {
  const plat = process.platform
  let cmd: string
  if (plat === "win32") {
    const bounds = region
      ? `New-Object System.Drawing.Rectangle(${region.split(",").join(", ")})`
      : "[System.Windows.Forms.Screen]::PrimaryScreen.Bounds"
    cmd = ps(`
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$b = ${bounds}
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save('${path.replace(/'/g, "''")}', [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"OK"
`)
  } else if (plat === "darwin") {
    cmd = region ? `screencapture -x -R ${region} '${path}'` : `screencapture -x '${path}'`
  } else {
    const probe = await ctx.runCommand("command -v scrot || command -v import || true", { timeoutMs: 5000 })
    const toolName = probe.stdout.split("\n").find(Boolean)?.split("/").pop() ?? ""
    if (!toolName) return { error: "Linux 桌面控制需要安装 scrot 或 ImageMagick(import)" }
    cmd = toolName === "scrot" ? `scrot '${path}'` : `import -window root '${path}'`
  }
  const { stdout, stderr, code } = await ctx.runCommand(cmd, { timeoutMs: 30000 })
  if (code !== 0) return { error: `截图失败 [exit ${code}]: ${stderr || stdout}` }
  return { ok: true }
}

function mapLines(lines: Array<{ text: string; score: number; box: { x: number; y: number; w: number; h: number } }>, offX: number, offY: number): LineOut[] {
  return lines.map((l) => ({
    text: l.text,
    score: l.score,
    x: Math.round(l.box.x + offX),
    y: Math.round(l.box.y + offY),
    w: Math.round(l.box.w),
    h: Math.round(l.box.h),
  }))
}

function formatLine(l: LineOut): string {
  const cx = Math.round(l.x + l.w / 2)
  const cy = Math.round(l.y + l.h / 2)
  return `${l.text}  [${l.x},${l.y},${l.w},${l.h}] → 中心 (${cx},${cy})  置信度 ${l.score.toFixed(2)}`
}

/* ---------- desktop_ocr ---------- */

export const ocrTool: Tool = {
  name: "ocr",
  description:
    "本地 OCR 识别屏幕/图片文字（PP-OCR 中英文小模型，离线运行、快、带精确像素坐标）。image 省略则现截全屏；region 限定区域（'x,y,w,h'）；find 关键词过滤。返回坐标相对（截图区域/图像）原点，可直接用于 mouse_click。读屏文字优先用本工具，语义理解/非文字内容才用 vision。",
  card: { titleParams: ["find", "region"], args: "none" },
  parameters: schema({
    image: { type: "string", description: "可选：PNG 图片路径（相对会话工作目录，省略则现截全屏）" },
    region: { type: "string", description: "可选：识别区域 'x,y,w,h'（像素；现截时为屏幕坐标，image 时为图片内坐标）" },
    find: { type: "string", description: "可选：关键词过滤，只返回包含该关键词的文本行" },
  }),
  async execute(args, ctx): Promise<ToolResult> {
    desktopGate(ctx)
    const loaded = await loadOrCapture(ctx, String(args.image ?? "").trim(), String(args.region ?? "").trim())
    if ("error" in loaded) return { output: loaded.error }
    const find = String(args.find ?? "").trim().toLowerCase()
    let lines: LineOut[]
    try {
      const raw = await getCvRunner().ocr(loaded.img, { env: ctx.env })
      lines = mapLines(raw, loaded.offX, loaded.offY)
    } catch (e) {
      return { output: `本地识别失败: ${e instanceof Error ? e.message : e}` }
    }
    if (find) lines = lines.filter((l) => l.text.toLowerCase().includes(find))
    const capped = lines.length > OCR_LINE_LIMIT
    const shown = lines.slice(0, OCR_LINE_LIMIT)
    const head = `识别到 ${lines.length} 行（${loaded.sourceDesc}，坐标相对其原点）${find ? `，过滤「${args.find}」` : ""}：`
    const body = shown.map(formatLine).join("\n")
    const tail = capped ? `\n…（共 ${lines.length} 行，仅显示前 ${OCR_LINE_LIMIT} 行；可用 find 参数过滤收窄）` : ""
    return {
      output: lines.length ? `${head}\n${body}${tail}` : `${head}\n（无${find ? "匹配" : "识别到"}文本——可能是图形界面无文字、分辨率过低，或需用 vision 工具做语义分析）`,
      data: { source: loaded.sourceDesc, find: args.find ?? null, lines },
    }
  },
}

/* ---------- desktop_locate ---------- */

function normText(s: string): string {
  return s.trim().toLowerCase()
}

/** 匹配分级：2=完全相等，1=行包含目标，0=目标包含行（≥2 字），-1=不匹配。 */
function matchRank(line: string, target: string): number {
  const l = normText(line)
  const t = normText(target)
  if (!l || !t) return -1
  if (l === t) return 2
  if (l.includes(t)) return 1
  if (t.includes(l) && l.length >= 2) return 0
  return -1
}

export const locateTool: Tool = {
  name: "locate",
  description:
    "在屏幕/图片中定位目标文字的精确像素坐标（本地 OCR，比视觉模型估坐标可靠）。target 为要找的文字（如按钮/菜单/链接文字）；返回最佳匹配的中心坐标（可直接 mouse_click）与全部候选。image 省略则现截全屏；region 限定搜索区域。",
  card: { titleParams: ["target", "region"], args: "none" },
  parameters: schema(
    {
      target: { type: "string", description: "要定位的目标文字（如「保存」「确定」）" },
      image: { type: "string", description: "可选：PNG 图片路径（省略则现截全屏）" },
      region: { type: "string", description: "可选：搜索区域 'x,y,w,h'（像素）" },
    },
    ["target"],
  ),
  async execute(args, ctx): Promise<ToolResult> {
    desktopGate(ctx)
    const target = String(args.target ?? "").trim()
    if (!target) return { output: "target 不能为空" }
    const loaded = await loadOrCapture(ctx, String(args.image ?? "").trim(), String(args.region ?? "").trim())
    if ("error" in loaded) return { output: loaded.error }
    let lines: LineOut[]
    try {
      lines = mapLines(await getCvRunner().ocr(loaded.img, { env: ctx.env }), loaded.offX, loaded.offY)
    } catch (e) {
      return { output: `本地识别失败: ${e instanceof Error ? e.message : e}` }
    }
    const candidates = lines
      .map((l) => ({ line: l, rank: matchRank(l.text, target) }))
      .filter((c) => c.rank >= 0)
      .sort((a, b) => b.rank - a.rank || b.line.score - a.line.score)
      .map((c) => c.line)
    if (!candidates.length) {
      return {
        output:
          `未找到目标文字「${target}」（${loaded.sourceDesc}）。建议：1) 用 desktop_ocr 读取全部文本确认实际措辞；` +
          "2) 文字可能是图标/图形（无文字），改用 desktop_detect（需自备模型）或 vision 工具语义分析；3) 目标可能不在当前屏幕/区域内，检查窗口是否在前台。",
        data: { target, found: false },
      }
    }
    const best = candidates[0]
    const cx = Math.round(best.x + best.w / 2)
    const cy = Math.round(best.y + best.h / 2)
    const rest = candidates.slice(1, 11).map(formatLine)
    const output =
      `找到「${best.text}」（${loaded.sourceDesc}）：中心 (${cx},${cy})，框 [${best.x},${best.y},${best.w},${best.h}]，置信度 ${best.score.toFixed(2)}\n` +
      `可直接 mouse_click(${cx}, ${cy})。${rest.length ? `\n其余候选：\n${rest.join("\n")}` : ""}`
    return { output, data: { target, found: true, best: { ...best, center: [cx, cy] }, candidates } }
  },
}

/* ---------- desktop_detect ---------- */

export const detectTool: Tool = {
  name: "detect",
  description:
    "本地目标检测（自备 YOLO ONNX 模型，GEBAI_CV_DETECT_MODEL / GEBAI_CV_DETECT_LABELS 环境变量指定；模型不随构建内嵌，COCO 预训练类别对 UI 无意义，需自训练图标/控件模型）。返回检测对象标签与像素坐标。image 省略则现截全屏；conf 置信度阈值（默认 0.25）。",
  card: { titleParams: ["region"], args: "none" },
  parameters: schema({
    image: { type: "string", description: "可选：PNG 图片路径（省略则现截全屏）" },
    region: { type: "string", description: "可选：检测区域 'x,y,w,h'（像素）" },
    conf: { type: "number", description: "可选：置信度阈值（默认 0.25）" },
  }),
  async execute(args, ctx): Promise<ToolResult> {
    desktopGate(ctx)
    const loaded = await loadOrCapture(ctx, String(args.image ?? "").trim(), String(args.region ?? "").trim())
    if ("error" in loaded) return { output: loaded.error }
    const conf = Number(args.conf)
    let objects: Array<{ label: string; score: number; x: number; y: number; w: number; h: number }>
    try {
      const raw = await getCvRunner().detect(loaded.img, {
        env: ctx.env,
        conf: Number.isFinite(conf) && conf > 0 && conf < 1 ? conf : 0.25,
      })
      objects = raw.map((o) => ({
        label: o.label,
        score: o.score,
        x: Math.round(o.x + loaded.offX),
        y: Math.round(o.y + loaded.offY),
        w: Math.round(o.w),
        h: Math.round(o.h),
      }))
    } catch (e) {
      return { output: `目标检测失败: ${e instanceof Error ? e.message : e}` }
    }
    if (!objects.length) {
      return { output: `未检测到目标对象（${loaded.sourceDesc}）。可尝试降低 conf 阈值，或确认模型/标签与场景匹配。`, data: { objects } }
    }
    const body = objects.map((o) => formatLine({ ...o, text: o.label })).join("\n")
    return {
      output: `检测到 ${objects.length} 个对象（${loaded.sourceDesc}，坐标相对其原点）：\n${body}`,
      data: { source: loaded.sourceDesc, objects },
    }
  },
}
