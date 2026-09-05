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
import { matchTemplate, type TemplateMatch } from "../../core/cv/template"
import { VISION_MAX_IMAGE_BYTES } from "../../core/tools/vision"
import { PS_DPI_AWARE, parseRegion } from "./desktop_tools"

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

/** 载入待识别图像：image 参数优先（PNG），省略则现截一张（region 可限定区域，坐标可为负覆盖副屏）。
 *  返回图像与坐标偏移——region 截图偏移为 region 原点，全屏截图偏移为捕获原点
 *  （Windows 全屏=虚拟屏幕，原点可为负）；坐标输出一律映射回主屏原点像素系。 */
async function loadOrCapture(
  ctx: ToolContext,
  image: string,
  region: string,
): Promise<{ img: RgbaImage; offX: number; offY: number; sourceDesc: string } | { error: string }> {
  const parsed = region ? parseRegion(region) : null
  if (region && !parsed) {
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
    if (parsed) {
      // image 是完整图：region 在图内裁剪（坐标为图片内坐标）
      img = cropImage(img, parsed)
      offX = parsed.x
      offY = parsed.y
      sourceDesc = `图像 ${image}（区域 ${region}）`
    }
  } else {
    // 固定文件名复用：每次现截覆盖同一文件，避免会话内 cv_capture_*.png 只增不减。
    // 有 region 时 captureTo 直接按区域截取（不再二次裁剪），无 region 截全屏虚拟屏幕。
    const path = ctx.resolvePath("cv_capture.png")
    const cap = await captureTo(ctx, path, region)
    if ("error" in cap) return cap
    offX = cap.offX
    offY = cap.offY
    try {
      img = decodePng(await ctx.readBinaryFile(path))
    } catch (e) {
      return { error: `截图解码失败: ${e instanceof Error ? e.message : e}` }
    }
    sourceDesc = region
      ? `当前屏幕（区域 ${region}，原点即区域原点）`
      : `当前屏幕（全屏=虚拟屏幕，原点 (${offX},${offY})）`
  }
  return { img, offX, offY, sourceDesc }
}

/** 平台截图命令（无统计——cv 场景不需要黑帧检测）；返回捕获原点（虚拟屏幕语义）。 */
async function captureTo(
  ctx: ToolContext,
  path: string,
  region: string,
): Promise<{ ok: true; offX: number; offY: number } | { error: string }> {
  const plat = process.platform
  let cmd: string
  if (plat === "win32") {
    // 全屏 = 虚拟屏幕（覆盖所有显示器）；脚本输出 CAP x,y 供解析原点（可能为负）
    const bounds = region
      ? `New-Object System.Drawing.Rectangle(${region.split(",").join(", ")})`
      : "[System.Windows.Forms.SystemInformation]::VirtualScreen"
    cmd = ps(`
${PS_DPI_AWARE}
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$b = ${bounds}
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save('${path.replace(/'/g, "''")}', [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"CAP $($b.X),$($b.Y)"
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
  const cap = stdout.match(/CAP (-?\d+),(-?\d+)/)
  const reg = region ? parseRegion(region) : null
  return { ok: true, offX: reg ? reg.x : cap ? Number(cap[1]) : 0, offY: reg ? reg.y : cap ? Number(cap[2]) : 0 }
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

/* ---------- desktop_locate_image（模板匹配） ---------- */

export const locateImageTool: Tool = {
  name: "locate_image",
  description:
    "在屏幕/图片中按模板定位图标/图形元素（本地模板匹配，零训练——补「文字走 desktop_locate、检测需自训 YOLO」之间的空白）。template 为模板 PNG 路径，或 template_region 从搜索图坐标内取模板区域；返回最佳匹配中心坐标（可直接 mouse_click）与候选。同尺寸匹配（模板需与目标显示尺寸一致——同一显示环境截图裁剪），threshold 相似度阈值默认 0.8。image 省略则现截全屏；region 限定搜索区域。",
  card: { titleParams: ["template", "template_region", "region"], args: "none" },
  parameters: schema(
    {
      template: { type: "string", description: "可选：模板 PNG 路径（相对会话工作目录）" },
      template_region: { type: "string", description: "可选：'x,y,w,h' 在搜索图坐标系内取模板区域（与 template 二选一；坐标系同 desktop_ocr 对同一 image/region 的输出）" },
      image: { type: "string", description: "可选：PNG 搜索图路径（省略则现截全屏）" },
      region: { type: "string", description: "可选：搜索区域 'x,y,w,h'（像素）" },
      threshold: { type: "number", description: "可选：相似度阈值 0-1（默认 0.8，降低可放宽匹配）" },
    },
    [],
  ),
  async execute(args, ctx): Promise<ToolResult> {
    desktopGate(ctx)
    const templatePath = String(args.template ?? "").trim()
    const templateRegionRaw = String(args.template_region ?? "").trim()
    if (!templatePath && !templateRegionRaw) {
      return { output: "请提供 template（模板 PNG 路径）或 template_region（搜索图内模板区域）二者之一" }
    }
    const loaded = await loadOrCapture(ctx, String(args.image ?? "").trim(), String(args.region ?? "").trim())
    if ("error" in loaded) return { output: loaded.error }
    let tpl: RgbaImage
    if (templatePath) {
      const path = ctx.resolvePath(templatePath)
      if (!path.toLowerCase().endsWith(".png")) {
        return { output: `模板仅支持 PNG（当前 ${templatePath}）` }
      }
      const bytes = await ctx.readBinaryFile(path)
      if (bytes.byteLength > VISION_MAX_IMAGE_BYTES) {
        return { output: `模板过大（${(bytes.byteLength / 1024 / 1024).toFixed(1)}MB，上限 8MB）` }
      }
      try {
        tpl = decodePng(bytes)
      } catch (e) {
        return { output: `模板 PNG 解码失败: ${e instanceof Error ? e.message : e}` }
      }
    } else {
      const box = parseRegion(templateRegionRaw)
      if (!box) return { output: `template_region 格式错误: ${templateRegionRaw}（应为 x,y,w,h）` }
      if (box.x < 0 || box.y < 0 || box.x + box.w > loaded.img.width || box.y + box.h > loaded.img.height) {
        return { output: `template_region 超出搜索图范围（图 ${loaded.img.width}x${loaded.img.height}，区域 ${templateRegionRaw}）` }
      }
      tpl = cropImage(loaded.img, box)
    }
    const threshold = Number(args.threshold)
    let matches: TemplateMatch[]
    try {
      matches = matchTemplate(loaded.img, tpl, {
        threshold: Number.isFinite(threshold) && threshold > 0 && threshold < 1 ? threshold : 0.8,
      })
    } catch (e) {
      return { output: `模板匹配失败: ${e instanceof Error ? e.message : e}` }
    }
    if (!matches.length) {
      return {
        output:
          `未找到匹配的模板图形（${loaded.sourceDesc}）。建议：1) 降低 threshold（如 0.7）放宽匹配；` +
          "2) 确认模板与目标为同一显示环境同尺寸截图（本工具不做缩放匹配）；3) 目标可能是文字——改用 desktop_locate；4) 用 vision 工具对截图做语义分析。",
        data: { found: false },
      }
    }
    const withOffset = (m: TemplateMatch) => ({
      ...m,
      x: m.x + loaded.offX,
      y: m.y + loaded.offY,
      center: [Math.round(m.x + loaded.offX + m.w / 2), Math.round(m.y + loaded.offY + m.h / 2)] as [number, number],
    })
    const best = withOffset(matches[0])
    const rest = matches.slice(1, 6).map((m) => {
      const o = withOffset(m)
      return `相似度 ${o.score.toFixed(2)}  [${o.x},${o.y},${o.w},${o.h}] → 中心 (${o.center[0]},${o.center[1]})`
    })
    return {
      output:
        `找到模板匹配（${loaded.sourceDesc}）：中心 (${best.center[0]},${best.center[1]})，框 [${best.x},${best.y},${best.w},${best.h}]，相似度 ${best.score.toFixed(2)}\n` +
        `可直接 mouse_click(${best.center[0]}, ${best.center[1]})。${rest.length ? `\n其余候选：\n${rest.join("\n")}` : ""}`,
      data: { found: true, best, candidates: matches.slice(1, 6).map(withOffset) },
    }
  },
}

/* ---------- desktop_wait_for（等待界面条件） ---------- */

/** change 模式判定：灰度采样平均绝对差（0-255 尺度）超过该值视为画面变化。 */
const CHANGE_DIFF_THRESHOLD = 2

/** 灰度隔点采样（~2000 点），供 change 模式做帧间差异比较。 */
function graySample(img: RgbaImage): Float32Array {
  const step = Math.max(1, Math.floor(Math.sqrt((img.width * img.height) / 2000)))
  const out: number[] = []
  for (let y = 0; y < img.height; y += step) {
    for (let x = 0; x < img.width; x += step) {
      const o = (y * img.width + x) * 4
      out.push(0.299 * img.data[o] + 0.587 * img.data[o + 1] + 0.114 * img.data[o + 2])
    }
  }
  return new Float32Array(out)
}

function meanAbsDiff(a: Float32Array, b: Float32Array): number {
  const n = Math.min(a.length, b.length)
  let sum = 0
  for (let i = 0; i < n; i++) sum += Math.abs(a[i] - b[i])
  return n ? sum / n : 0
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

export const waitForTool: Tool = {
  name: "wait_for",
  description:
    "等待屏幕满足条件后返回（只读，轮询截图判断，省去反复手动截图轮询）：mode=text 等待指定文字出现（本地 OCR）；text_gone 等待文字消失；change 等待区域内画面发生变化（相对首次截图）。timeout_s 默认 20（上限 120），interval_s 轮询间隔默认 2。超时不视为错误，返回当前最后观察状态。",
  card: { titleParams: ["text", "mode", "region"], args: "none" },
  parameters: schema(
    {
      mode: { enum: ["text", "text_gone", "change"], description: "可选：等待条件（默认 text）" },
      text: { type: "string", description: "text/text_gone 模式的目标文字（必填）" },
      timeout_s: { type: "number", description: "可选：超时秒数（默认 20，上限 120）" },
      interval_s: { type: "number", description: "可选：轮询间隔秒（默认 2，范围 0.5-10）" },
      region: { type: "string", description: "可选：限定区域 'x,y,w,h'（小区域轮询更快）" },
    },
    [],
  ),
  async execute(args, ctx): Promise<ToolResult> {
    desktopGate(ctx)
    const mode = String(args.mode ?? "text")
    const text = String(args.text ?? "").trim()
    const timeoutS = Math.max(1, Math.min(120, num2(args.timeout_s, 20)))
    const intervalS = Math.max(0.5, Math.min(10, num2(args.interval_s, 2)))
    const region = String(args.region ?? "").trim()
    if (region && !parseRegion(region)) return { output: `region 格式错误: ${region}（应为 x,y,w,h）` }
    if (mode !== "change" && !text) return { output: `mode=${mode} 需要提供 text 参数` }
    const path = ctx.resolvePath("cv_capture.png")
    const start = Date.now()
    let baseline: Float32Array | null = null
    let lastDesc = "尚未观察"
    let polls = 0
    for (;;) {
      polls++
      const cap = await captureTo(ctx, path, region)
      if ("error" in cap) return { output: cap.error }
      let img: RgbaImage
      try {
        img = decodePng(await ctx.readBinaryFile(path))
      } catch (e) {
        return { output: `截图解码失败: ${e instanceof Error ? e.message : e}` }
      }
      if (mode === "change") {
        const cur = graySample(img)
        if (baseline === null) {
          baseline = cur
          lastDesc = "已记录基线画面"
        } else {
          const diff = meanAbsDiff(baseline, cur)
          lastDesc = `画面平均差异 ${diff.toFixed(1)}/255`
          if (diff > CHANGE_DIFF_THRESHOLD) {
            return { output: `已满足：画面已变化（平均差异 ${diff.toFixed(1)}/255 > ${CHANGE_DIFF_THRESHOLD}；等待 ${((Date.now() - start) / 1000).toFixed(1)}s，共 ${polls} 次轮询）` }
          }
        }
      } else {
        let lines: Array<{ text: string }>
        try {
          lines = await getCvRunner().ocr(img, { env: ctx.env })
        } catch (e) {
          return { output: `等待失败（本地 OCR 不可用）: ${e instanceof Error ? e.message : e}` }
        }
        const hit = lines.some((l) => l.text.toLowerCase().includes(text.toLowerCase()))
        lastDesc = `文字「${text}」${hit ? "在当前画面中" : "不在当前画面中"}`
        if (mode === "text" && hit) {
          return { output: `已满足：${lastDesc}（等待 ${((Date.now() - start) / 1000).toFixed(1)}s，共 ${polls} 次轮询）` }
        }
        if (mode === "text_gone" && !hit) {
          return { output: `已满足：文字「${text}」已消失（等待 ${((Date.now() - start) / 1000).toFixed(1)}s，共 ${polls} 次轮询）` }
        }
      }
      const elapsed = Date.now() - start
      if (elapsed + intervalS * 1000 > timeoutS * 1000) {
        return { output: `等待超时（${timeoutS}s，共 ${polls} 次轮询）：${lastDesc}。可增大 timeout_s 或改用其他验证通道（desktop_ocr/screenshot）。` }
      }
      await sleep(intervalS * 1000)
    }
  },
}

function num2(v: unknown, dflt: number): number {
  const n = Number(v)
  return Number.isFinite(n) ? n : dflt
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
