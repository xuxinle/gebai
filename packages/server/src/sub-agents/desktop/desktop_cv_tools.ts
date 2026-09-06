/**
 * 桌面本地识别工具集（desktop 专用）：ocr/locate/locate_image 三工具复用共享工厂
 * core/tools/cv-analysis（core/cv 域本地小模型推理的消费层——onnxruntime-web wasm 进程内
 * 推理，离线运行不耗模型配额），本文件注入 desktop 的缺省图像源（现截宿主机屏幕）与本地
 * 模式闸门；desktop 独有的 detect（自备 YOLO 检测）与 wait_for（屏幕条件轮询）仍在此处。
 * 全部只读操作（不点击/不输入），坐标返回映射回主屏原点像素系的像素值，供 mouse_click
 * 直接使用；识别在裁剪/缩放后的图像上进行，坐标一律映射回原始像素系。
 */
import type { Tool, ToolContext, ToolResult } from "../../core/base/types"
import { getCvRunner } from "../../core/cv/cv"
import { pairObjectsWithText } from "../../core/cv/detect"
import { decodePng, type RgbaImage } from "../../core/cv/image"
import { createCvAnalysisTools, loadAnalysisSource, type CvSource, type CvSourceLoader } from "../../core/tools/cv-analysis"
import { parseRegion, schema } from "../../core/tools/shared"
import { PS_DPI_AWARE } from "./desktop_tools"

function desktopGate(ctx: ToolContext): void {
  if (ctx.sandboxed) throw new Error("桌面控制仅在本地/桌面模式可用（服务端部署已禁用）")
}

/** PowerShell 脚本 → 命令串：UTF-16LE base64 避免引号转义（cmd 兼容）。 */
function ps(script: string): string {
  const b64 = Buffer.from(script, "utf16le").toString("base64")
  return `powershell -NoProfile -NonInteractive -EncodedCommand ${b64}`
}

/* ---------- 缺省图像源：现截宿主机屏幕 ---------- */

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

/** desktop 缺省识别源：固定文件名 cv_capture.png 覆盖复用（不随调用累积），有 region 时
 *  captureTo 直接按区域截取（不再二次裁剪），无 region 截全屏虚拟屏幕。坐标偏移=捕获原点。 */
async function captureScreen(ctx: ToolContext, region: string): Promise<CvSource | { error: string }> {
  const path = ctx.resolvePath("cv_capture.png")
  const cap = await captureTo(ctx, path, region)
  if ("error" in cap) return cap
  let img: RgbaImage
  try {
    img = decodePng(await ctx.readBinaryFile(path))
  } catch (e) {
    return { error: `截图解码失败: ${e instanceof Error ? e.message : e}` }
  }
  return {
    img,
    offX: cap.offX,
    offY: cap.offY,
    sourceDesc: region
      ? `当前屏幕（区域 ${region}，原点即区域原点）`
      : `当前屏幕（全屏=虚拟屏幕，原点 (${cap.offX},${cap.offY})）`,
  }
}

/** detect 复用的图像装载约定（与三工具同一缺省源）。 */
const screenLoader: CvSourceLoader = { captureDefault: captureScreen, pngOnlyTail: "，或直接省略 image 现截屏幕" }

/* ---------- desktop_ocr / desktop_locate / desktop_locate_image（共享工厂，desktop 文案） ---------- */

const analysis = createCvAnalysisTools({
  gate: desktopGate,
  ...screenLoader,
  wording: {
    descriptions: {
      ocr: "本地 OCR 识别屏幕/图片文字（PP-OCR 中英文小模型，离线运行、快、带精确像素坐标）。image 省略则现截全屏；region 限定区域（'x,y,w,h'）；find 关键词过滤。返回坐标相对（截图区域/图像）原点，可直接用于 mouse_click。读屏文字优先用本工具，语义理解/非文字内容才用 vision。",
      locate: "在屏幕/图片中定位目标文字的精确像素坐标（本地 OCR，比视觉模型估坐标可靠）。target 为要找的文字（如按钮/菜单/链接文字）；返回最佳匹配的中心坐标（可直接 mouse_click）与全部候选。image 省略则现截全屏；region 限定搜索区域。",
      locateImage: "在屏幕/图片中按模板定位图标/图形元素（本地模板匹配，零训练——补「文字走 desktop_locate、检测需自训 YOLO」之间的空白）。template 为模板 PNG 路径，或 template_region 从搜索图坐标内取模板区域；返回最佳匹配中心坐标（可直接 mouse_click）与候选。同尺寸匹配（模板需与目标显示尺寸一致——同一显示环境截图裁剪），threshold 相似度阈值默认 0.8。image 省略则现截全屏；region 限定搜索区域。",
    },
    imageParam: {
      ocr: "可选：PNG 图片路径（相对会话工作目录，省略则现截全屏）",
      locate: "可选：PNG 图片路径（省略则现截全屏）",
      locateImage: "可选：PNG 搜索图路径（省略则现截全屏）",
    },
    regionParam: {
      ocr: "可选：识别区域 'x,y,w,h'（像素；现截时为屏幕坐标，image 时为图片内坐标）",
      locate: "可选：搜索区域 'x,y,w,h'（像素）",
      locateImage: "可选：搜索区域 'x,y,w,h'（像素）",
    },
    templateRegionParam: "可选：'x,y,w,h' 在搜索图坐标系内取模板区域（与 template 二选一；坐标系同 desktop_ocr 对同一 image/region 的输出）",
    clickHint: (cx, cy) => `可直接 mouse_click(${cx}, ${cy})`,
    locateNotFound:
      "1) 用 desktop_ocr 读取全部文本确认实际措辞；2) 文字可能是图标/图形（无文字），改用 desktop_detect（需自备模型）或 vision 工具语义分析；3) 目标可能不在当前屏幕/区域内，检查窗口是否在前台。",
    locateImageNotFound:
      "2) 确认模板与目标为同一显示环境同尺寸截图（本工具不做缩放匹配）；3) 目标可能是文字——改用 desktop_locate；4) 用 vision 工具对截图做语义分析。",
  },
})

export const ocrTool = analysis.ocr
export const locateTool = analysis.locate
export const locateImageTool = analysis.locate_image

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
          lines = (await getCvRunner().ocr(img, { env: ctx.env })).lines
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
    "本地目标检测（自备 YOLO ONNX 模型：GEBAI_CV_DETECT_MODEL 指定，或放入 {GEBAI_HOME}/models/detect/ 唯一 .onnx 自动生效；ultralytics 导出的 ONNX 自动读取内嵌输入尺寸与类别，免标签配置）。返回检测对象类别与像素坐标，并默认与 OCR 配对输出每框文本（pair_text 可关）——组件类别不含语义，找特定文字按钮仍以 desktop_locate 为主，本工具适合结构感知与无文字元素定位。推理分层：GPU sidecar（GEBAI_CV_DETECT_BACKEND，Windows DirectML/任意 DX12 显卡、CUDA、macOS CoreML）不可用时自动回落 wasm CPU。image 省略则现截全屏；conf 置信度阈值（默认 0.25）；iou NMS 阈值（默认 0.45，密集小控件场景建议 0.1）。",
  card: { titleParams: ["region"], args: "none" },
  parameters: schema(
    {
      image: { type: "string", description: "可选：PNG 图片路径（省略则现截全屏）" },
      region: { type: "string", description: "可选：检测区域 'x,y,w,h'（像素）" },
      conf: { type: "number", description: "可选：置信度阈值（默认 0.25）" },
      iou: { type: "number", description: "可选：NMS IoU 阈值（默认 0.45；密集小控件/重叠元素场景调低至 0.1）" },
      pair_text: { type: "boolean", description: "可选：检测框与 OCR 文本配对输出「类别+文本」（默认 true；纯检测提速可关）" },
    },
    [],
  ),
  async execute(args, ctx): Promise<ToolResult> {
    desktopGate(ctx)
    const loaded = await loadAnalysisSource(ctx, screenLoader, String(args.image ?? "").trim(), String(args.region ?? "").trim())
    if ("error" in loaded) return { output: loaded.error }
    const conf = Number(args.conf)
    const iou = Number(args.iou)
    let outcome: { objects: Array<{ label: string; score: number; x: number; y: number; w: number; h: number }>; backend: string }
    try {
      outcome = await getCvRunner().detect(loaded.img, {
        env: ctx.env,
        conf: Number.isFinite(conf) && conf > 0 && conf < 1 ? conf : 0.25,
        iou: Number.isFinite(iou) && iou > 0 && iou < 1 ? iou : undefined,
      })
    } catch (e) {
      return { output: `目标检测失败: ${e instanceof Error ? e.message : e}` }
    }
    // 检测框 × OCR 行配对（配对在图像像素系进行，再统一加偏移）：检测只给组件类别，
    // 配对文本补齐「哪一个按钮/输入框」的语义（完整屏幕解析的本地拼装）
    let pairedTexts: Array<string | undefined> = outcome.objects.map(() => undefined)
    if (args.pair_text !== false && outcome.objects.length) {
      try {
        const lines = (await getCvRunner().ocr(loaded.img, { env: ctx.env })).lines
        pairedTexts = pairObjectsWithText(outcome.objects, lines.map((l) => ({ text: l.text, ...l.box }))).map((o) => o.text)
      } catch { /* OCR 模型未配置等——跳过配对，仅输出检测框 */ }
    }
    const objects = outcome.objects.map((o, i) => ({
      label: o.label,
      score: o.score,
      x: Math.round(o.x + loaded.offX),
      y: Math.round(o.y + loaded.offY),
      w: Math.round(o.w),
      h: Math.round(o.h),
      text: pairedTexts[i],
    }))
    if (!objects.length) {
      return {
        output: `未检测到目标对象（${loaded.sourceDesc}）。可尝试降低 conf 阈值，或确认模型/标签与场景匹配。`,
        data: { objects, backend: outcome.backend },
      }
    }
    const body = objects
      .map((o) => {
        const cx = Math.round(o.x + o.w / 2)
        const cy = Math.round(o.y + o.h / 2)
        return `${o.label}  [${o.x},${o.y},${o.w},${o.h}] → 中心 (${cx},${cy})  置信度 ${o.score.toFixed(2)}${o.text ? `  文本: ${o.text}` : ""}`
      })
      .join("\n")
    return {
      output: `检测到 ${objects.length} 个对象（${loaded.sourceDesc}，坐标相对其原点；后端 ${outcome.backend}）：\n${body}`,
      data: { source: loaded.sourceDesc, backend: outcome.backend, objects },
    }
  },
}
