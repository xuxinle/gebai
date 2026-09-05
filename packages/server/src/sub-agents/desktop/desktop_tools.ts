import type { Tool, ToolContext, ToolResult } from "../../core/base/types"
import { artifactBlocks } from "../../core/tools"
import type { ToolSchema } from "@gebai/sdk"

/**
 * 桌面控制工具集（desktop 专用）：截图、窗口控制、输入。
 * 跨平台实现：Windows 走内置 PowerShell（无外部依赖）；macOS 走 screencapture + osascript；
 * Linux 依赖 xdotool/wmctrl/scrot/import（缺失时明确报错）。
 * 服务端部署（sandboxed）一律拒绝——桌面控制是对宿主机桌面的真实操作，仅限本地/桌面模式。
 */

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

/**
 * PowerShell DPI 感知声明（脚本头片段）：每个 PowerShell 进程独立声明，
 * 使屏幕/窗口/鼠标坐标统一为物理像素——否则高 DPI 缩放（如 150%）下
 * Screen.Bounds/CopyFromScreen 落入逻辑像素空间，与 OCR/点击坐标错位。
 * desktop_cv_tools 复用（截图脚本同需求）。
 */
export const PS_DPI_AWARE = `Add-Type @"
using System.Runtime.InteropServices;
public class GebaiDpi {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[GebaiDpi]::SetProcessDPIAware() | Out-Null`

/** 任意文本以 base64 注入 PowerShell 脚本（规避引号/特殊字符）。 */
function psLiteral(text: string): string {
  return `[System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String("${Buffer.from(text, "utf8").toString("base64")}"))`
}

/** 敏感值模式：sk- 前缀密钥、KEY/SECRET/TOKEN 赋值、Bearer 令牌。
 *  泛型长串分支独立为 longTokenHit 启发式（40+ 且字母数字混合、非纯十六进制——
 *  纯英文词/纯 hex（git commit sha）等正常长串不误判）。 */
const SENSITIVE_VALUE_RE =
  /(sk-[A-Za-z0-9_\-]{10,}|(?:secret|token|password|passwd|api[_-]?key|app[_-]?secret|access[_-]?key|private[_-]?key)\s*[=:：]\s*[^\s,;，。]{8,}|Bearer\s+[A-Za-z0-9._~+/=-]{20,})/i

/** 长令牌启发式：40+ 连续字母数字下划线连字符，含字母且含数字，非纯十六进制。 */
function longTokenHit(text: string): string | null {
  for (const m of text.matchAll(/[A-Za-z0-9_\-]{40,}/g)) {
    const t = m[0]
    if (/^[0-9a-fA-F]+$/.test(t)) continue
    if (/[0-9]/.test(t) && /[A-Za-z]/.test(t)) return t
  }
  return null
}

/** 敏感模式检测：命中返回脱敏后的命中片段（首 4 + **** + 尾 4），否则 null。 */
export function detectSensitive(text: string): string | null {
  const m = SENSITIVE_VALUE_RE.exec(text)
  const hit = m?.[0] ?? longTokenHit(text)
  if (!hit) return null
  return hit.length > 8 ? `${hit.slice(0, 4)}****${hit.slice(-4)}` : hit
}

/** SendKeys 转义：特殊字符（+ ^ % ~ ( ) [ ] { }）逐字符包 {}，防被解释为组合键/修饰键。 */
function sendKeysEscape(s: string): string {
  return s.replace(/[+^%~()\[\]{}]/g, (c) => `{${c}}`)
}

function num(v: unknown, dflt: number): number {
  const n = Number(v)
  return Number.isFinite(n) ? n : dflt
}

/** region 参数校验：'x,y,w,h'，x/y 可为负（副屏在主屏左侧/上方时坐标为负），w/h 非负。desktop_cv_tools 复用。 */
export function parseRegion(region: string): { x: number; y: number; w: number; h: number } | null {
  const m = /^(-?\d+),(-?\d+),(\d+),(\d+)$/.exec(region.trim())
  if (!m) return null
  const [, x, y, w, h] = m
  return { x: Number(x), y: Number(y), w: Number(w), h: Number(h) }
}

async function run(ctx: ToolContext, cmd: string, timeoutMs = 20000): Promise<ToolResult> {
  const { stdout, stderr, code } = await ctx.runCommand(cmd, { timeoutMs })
  if (code !== 0) return { output: `执行失败 [exit ${code}]:\n${stderr || stdout}` }
  return { output: stdout.trim() || "(无输出)" }
}

/** shell 单引号字符串转义（POSIX: ' → '\''），用于 Linux/macOS 命令拼接。 */
function sq(s: string): string {
  return s.replace(/'/g, `'\\''`)
}

/** 进程查找：pid 优先，其次按标题匹配。title 经 base64 注入脚本内解码后 .Contains 匹配（无 PowerShell 插值面）。 */
function procFilter(pid?: number, title?: string): string {
  if (pid) return `$p = Get-Process -Id ${pid} -ErrorAction SilentlyContinue`
  const b64 = Buffer.from(String(title ?? ""), "utf8").toString("base64")
  return `$t = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String("${b64}"))
$p = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle.Contains($t) }`
}

/* ---------- 截图 ---------- */

/** 黑帧/纯色告警：平均亮度极低提示显示器休眠/锁屏；暗色单色提示非真实画面。 */
function blackFrameWarn(mean: number | null, colors: number | null): string | null {
  if (mean === null) return null
  if (mean < 8) return "⚠️ 截图平均亮度极低（疑似黑屏：显示器休眠/关闭/锁屏），像素分析可能无意义。请确认屏幕状态后重试。"
  if (mean < 40 && colors !== null && colors < 16) return "⚠️ 截图整体为单一暗色（平均亮度低、色数少），可能非真实画面（黑屏/纯色屏）。"
  return null
}

export const screenshotTool: Tool = {
  name: "screenshot",
  description:
    "截取屏幕（全屏或指定区域），保存 PNG 到会话 tmp/ 并返回图片供 UI 展示。全屏覆盖所有显示器的虚拟屏幕（多显示器含负坐标副屏）；region 参数指定截取区域（相对主屏左上角，x/y 可为负以覆盖副屏，省略则全屏）。返回尺寸/原点元数据并自动检测黑屏/纯色帧（显示器休眠/锁屏时主动提示）。平台：Windows/macOS 内置支持；Linux 需 scrot 或 ImageMagick import。",
  card: { titleParams: ["region"], args: "none" },
  parameters: schema({
    region: { type: "string", description: "可选：截取区域 'x,y,w,h'（像素，相对主屏左上角，x/y 可为负；省略则全屏=虚拟屏幕）" },
    name: { type: "string", description: "可选：文件名（不含扩展名，默认 screenshot_<时间戳>）" },
  }),
  async execute(args, ctx) {
    desktopGate(ctx)
    // name 参数落地（此前声明未实现，模型按描述传名无效）：消毒为安全文件名基名
    const nameBase = String(args.name ?? "").trim().replace(/[\\/:*?"<>|\s]+/g, "_").slice(0, 60)
    const rel = `${nameBase || `screenshot_${Date.now()}`}.png`
    const path = ctx.resolvePath(rel)
    const regionRaw = String(args.region ?? "").trim()
    const region = parseRegion(regionRaw)
    if (regionRaw && !region) {
      return { output: `region 格式错误: ${regionRaw}（应为 x,y,w,h）` }
    }
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      // 全屏 = 虚拟屏幕（覆盖所有显示器，副屏可为负坐标）；区域截图 Rectangle 原点即 region 原点
      let bounds = "[System.Windows.Forms.SystemInformation]::VirtualScreen"
      if (region) {
        bounds = `New-Object System.Drawing.Rectangle(${region.x}, ${region.y}, ${region.w}, ${region.h})`
      }
      // 截图后抽样统计：平均亮度 + 采样色数（A1 黑帧检测），输出 STAT 行供解析
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$b = ${bounds}
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save('${path.replace(/'/g, "''")}', [System.Drawing.Imaging.ImageFormat]::Png)
$sum = 0.0; $n = 0; $colors = New-Object 'System.Collections.Generic.HashSet[int]'
$step = [Math]::Max(1, [int][Math]::Sqrt($bmp.Width * $bmp.Height / 500))
for ($x = 0; $x -lt $bmp.Width; $x += $step) {
  for ($y = 0; $y -lt $bmp.Height; $y += $step) {
    $p = $bmp.GetPixel($x, $y)
    $sum += 0.299 * $p.R + 0.587 * $p.G + 0.114 * $p.B
    $n++
    [void]$colors.Add((($p.R -shl 16) -bor ($p.G -shl 8)) -bor $p.B)
  }
}
"STAT $($b.X),$($b.Y) $($bmp.Width)x$($bmp.Height) mean=$([Math]::Round($sum / $n, 1)) colors=$($colors.Count)"
$g.Dispose(); $bmp.Dispose()
`)
    } else if (plat === "darwin") {
      cmd = regionRaw
        ? `screencapture -x -R ${regionRaw} '${path}'`
        : `screencapture -x '${path}'`
    } else {
      const probe = await ctx.runCommand("command -v scrot || command -v import || true", { timeoutMs: 5000 })
      const toolName = probe.stdout.split("\n").find(Boolean)?.split("/").pop() ?? ""
      if (!toolName) return { output: "Linux 桌面控制需要安装 scrot 或 ImageMagick(import)" }
      cmd = toolName === "scrot" ? `scrot '${path}'` : `import -window root '${path}'`
    }
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { timeoutMs: 30000 })
    if (code !== 0) return { output: `截图失败 [exit ${code}]:\n${stderr || stdout}` }
    // 质量统计解析：win32 内置 STAT 行；macOS/Linux 用平台工具补取尺寸/亮度
    const m = stdout.match(/STAT (-?\d+),(-?\d+) (\d+)x(\d+) mean=([\d.]+) colors=(\d+)/)
    let size = m ? `${m[3]}x${m[4]}` : ""
    let origin = m ? `${m[1]},${m[2]}` : ""
    let mean: number | null = m ? Number(m[5]) : null
    let colors: number | null = m ? Number(m[6]) : null
    if (region && regionRaw) origin = `${region.x},${region.y}`
    if (!m) {
      if (plat === "darwin") {
        const s = await ctx.runCommand(`sips -g pixelWidth -g pixelHeight '${path}'`, { timeoutMs: 10000 })
        const w = s.stdout.match(/pixelWidth:\s*(\d+)/)?.[1]
        const h = s.stdout.match(/pixelHeight:\s*(\d+)/)?.[1]
        if (w && h) size = `${w}x${h}`
      } else {
        const probe = await ctx.runCommand("command -v identify convert || true", { timeoutMs: 5000 })
        const tools = probe.stdout.split("\n").map((s) => s.split("/").pop()).filter(Boolean)
        if (tools.includes("identify")) {
          const id = await ctx.runCommand(`identify -format "%wx%h" '${path}'`, { timeoutMs: 10000 })
          if (id.code === 0) size = id.stdout.trim()
        }
        if (tools.includes("convert")) {
          const l = await ctx.runCommand(`convert '${path}' -format "%[fx:mean]" info:`, { timeoutMs: 10000 })
          if (l.code === 0 && l.stdout.trim()) mean = Number(Number(l.stdout.trim()).toFixed(1))
        }
      }
    }
    const warn = blackFrameWarn(mean, colors)
    const meta =
      (size ? `尺寸 ${size}` : "") +
      (origin ? `，原点 (${origin})——图片坐标加原点即屏幕坐标` : "") +
      (mean !== null ? `，平均亮度 ${mean}/255` : "")
    const scope = !regionRaw && plat === "win32" ? "（全屏=虚拟屏幕，覆盖所有显示器）" : ""
    return { output: `已截图: ${rel}${meta ? `（${meta}）` : ""}${scope}${warn ? `\n${warn}` : ""}`, blocks: artifactBlocks(rel) }
  },
}

/* ---------- 屏幕信息 ---------- */

export const screenInfoTool: Tool = {
  name: "screen_info",
  description: "列出所有显示器（分辨率、位置、是否主屏），供截图 region / 鼠标坐标参考。截图 region 坐标以主屏左上角为原点。",
  card: { args: "none" },
  parameters: schema({}),
  async execute(_args, ctx) {
    desktopGate(ctx)
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.Screen]::AllScreens | ForEach-Object { @($_.DeviceName, $_.Bounds.X, $_.Bounds.Y, $_.Bounds.Width, $_.Bounds.Height, $_.Primary) -join [char]9 }
`)
    } else if (plat === "darwin") {
      cmd = `osascript -e 'tell application "Finder" to get bounds of window of desktop'`
    } else {
      const probe = await ctx.runCommand("command -v xrandr || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 屏幕信息需要安装 xrandr" }
      cmd = `xrandr --query | grep " connected"`
    }
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { timeoutMs: 15000 })
    if (code !== 0) return { output: `屏幕信息获取失败 [exit ${code}]:\n${stderr || stdout}` }
    if (plat === "win32" && stdout.trim()) {
      const rows = stdout.trim().split("\n").map((l) => l.split("\t"))
      const lines = rows.map((r) => `${r[0]} 位置(${r[1]},${r[2]}) ${r[3]}x${r[4]}${r[5] === "True" ? "（主屏）" : ""}`)
      return { output: `共 ${rows.length} 个显示器：\n${lines.join("\n")}` }
    }
    if (plat === "darwin") return { output: `主屏 bounds: ${stdout.trim()}` }
    return { output: stdout.trim() || "（无输出）" }
  },
}

/* ---------- 窗口控制 ---------- */

export const windowListTool: Tool = {
  name: "window_list",
  description:
    "列出当前可见窗口（PID、进程名、前台标记*、窗口位置 x,y,w,h、标题），供定位目标窗口与确认前台窗口。只读操作。",
  card: { args: "none" },
  parameters: schema({}),
  async execute(_args, ctx) {
    desktopGate(ctx)
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      // 前台标记（GetForegroundWindow 比对）+ 窗口 bounds（GetWindowRect，物理像素）
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct GebaiRect3 { public int Left, Top, Right, Bottom; }
public class GebaiWinList {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out GebaiRect3 r);
}
"@
$fg = [GebaiWinList]::GetForegroundWindow()
Get-Process | Where-Object { $_.MainWindowHandle -ne 0 } | Sort-Object ProcessName | ForEach-Object {
  $r = New-Object GebaiRect3
  [GebaiWinList]::GetWindowRect($_.MainWindowHandle, [ref]$r) | Out-Null
  $mark = if ($_.MainWindowHandle -eq $fg) { "*" } else { "" }
  @($_.Id, $_.ProcessName, $mark, "$($r.Left),$($r.Top),$($r.Right - $r.Left),$($r.Bottom - $r.Top)", $_.MainWindowTitle) -join [char]9
}
`)
    } else if (plat === "darwin") {
      cmd = `osascript -e 'tell application "System Events" to get name of every process whose background only is false'`
    } else {
      const probe = await ctx.runCommand("command -v wmctrl || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 窗口控制需要安装 wmctrl" }
      cmd = `wmctrl -l`
    }
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { timeoutMs: 15000 })
    if (code !== 0) return { output: `窗口列表获取失败 [exit ${code}]:\n${stderr || stdout}` }
    // Windows TSV → 对齐文本（PID/进程名/前台标记/窗口位置/标题）
    if (plat === "win32" && stdout.trim()) {
      const rows = stdout.trim().split("\n").map((l) => l.split("\t"))
      const pidW = Math.max(...rows.map((r) => r[0]?.length ?? 0), 3)
      const nameW = Math.max(...rows.map((r) => r[1]?.length ?? 0), 4)
      const lines = rows.map((r) => `${(r[2] === "*" ? "*" : " ") + (r[0] ?? "").padEnd(pidW)}  ${(r[1] ?? "").padEnd(nameW)}  ${r[3] ?? ""}  ${r[4] ?? ""}`)
      const fg = rows.filter((r) => r[2] === "*").map((r) => r[1]).join(", ")
      return {
        output:
          `共 ${rows.length} 个窗口（* = 当前前台；第 4 列为窗口位置 x,y,w,h 屏幕像素）：\n${lines.join("\n")}` +
          (fg ? `\n当前前台: ${fg}` : "\n（无前台窗口标记——可能焦点在无主窗口的进程）"),
      }
    }
    return { output: stdout.trim() || "（无可见窗口）" }
  },
}

export const windowFocusTool: Tool = {
  name: "window_focus",
  description: "激活指定窗口到前台（按 PID 或标题匹配），激活后复核前台窗口确认生效；Windows 前台锁定拦截时经 Alt 键缓解重试，仍失败明确报错（请手动点击目标窗口后重试）。必要时先 window_list 定位。",
  card: { titleParams: ["pid", "title"], args: "none" },
  parameters: schema({
    pid: { type: "number", description: "可选：目标窗口的 PID（window_list 结果第一列）" },
    title: { type: "string", description: "可选：按标题模糊匹配窗口（pid 未提供时）" },
  }),
  async execute(args, ctx) {
    desktopGate(ctx)
    const pid = num(args.pid, 0)
    const title = String(args.title ?? "")
    if (!pid && !title) return { output: "请提供 pid 或 title" }
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      cmd = ps(`
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GebaiWin {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
}
"@
${procFilter(pid, title)} | Select-Object -First 1
if ($p -and $p.MainWindowHandle -ne 0) {
  $h = $p.MainWindowHandle
  [GebaiWin]::ShowWindow($h, 9) | Out-Null
  $ok = [GebaiWin]::SetForegroundWindow($h)
  if (-not $ok -or [GebaiWin]::GetForegroundWindow() -ne $h) {
    # Windows 前台锁定缓解：模拟 Alt 键击键使本进程获得前台权限后重试
    [GebaiWin]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
    [GebaiWin]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    $ok = [GebaiWin]::SetForegroundWindow($h)
  }
  if ($ok -and [GebaiWin]::GetForegroundWindow() -eq $h) {
    "已激活: $($p.ProcessName) (PID $($p.Id)) - $($p.MainWindowTitle)"
  } else {
    "激活失败: Windows 前台锁定拦截（后台进程禁止夺取焦点）。请手动点击一次目标窗口后重试，或改用其他验证通道确认窗口状态"
  }
} else { "未找到匹配窗口" }
`)
    } else if (plat === "darwin") {
      // PID 优先（unix id），否则按标题
      const target = pid ? `whose unix id is ${pid}` : `whose name contains ${sq(JSON.stringify(title))}`
      cmd = `osascript -e 'tell application "System Events" to set frontmost of (first process ${target}) to true'`
    } else {
      const probe = await ctx.runCommand("command -v wmctrl || command -v xdotool || true", { timeoutMs: 5000 })
      const t = probe.stdout.split("\n").find(Boolean)?.split("/").pop() ?? ""
      if (!t) return { output: "Linux 窗口控制需要安装 wmctrl 或 xdotool" }
      cmd = t === "wmctrl" ? `wmctrl -a '${sq(title)}'` : `xdotool search --name '${sq(title)}' windowactivate`
    }
    return run(ctx, cmd)
  },
}

export const windowMoveTool: Tool = {
  name: "window_move",
  description: "移动并可选调整窗口大小（x/y 为屏幕坐标，width/height 省略则保持原尺寸）。",
  parameters: schema(
    {
      x: { type: "number", description: "目标左上角 X" },
      y: { type: "number", description: "目标左上角 Y" },
      width: { type: "number", description: "可选：目标宽度" },
      height: { type: "number", description: "可选：目标高度" },
      pid: { type: "number", description: "目标窗口 PID" },
      title: { type: "string", description: "或按标题匹配" },
    },
    ["x", "y"],
  ),
  async execute(args, ctx) {
    desktopGate(ctx)
    const x = num(args.x, 0)
    const y = num(args.y, 0)
    const pid = num(args.pid, 0)
    const title = String(args.title ?? "")
    if (!pid && !title) return { output: "请提供 pid 或 title" }
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      const w = args.width != null ? String(num(args.width, 0)) : ""
      const h = args.height != null ? String(num(args.height, 0)) : ""
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct GebaiRect { public int Left, Top, Right, Bottom; }
public class GebaiWin2 {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out GebaiRect r);
}
"@
${procFilter(pid, title)} | Select-Object -First 1
if ($p -and $p.MainWindowHandle -ne 0) {
  $r = New-Object GebaiRect
  [GebaiWin2]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
  $w = ${w || `($r.Right - $r.Left)`}
  $h = ${h || `($r.Bottom - $r.Top)`}
  [GebaiWin2]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, ${x}, ${y}, $w, $h, 0x0040) | Out-Null
  "已移动: $($p.ProcessName) → (${x}, ${y}) ${w}x${h}"
} else { "未找到匹配窗口" }
`)
    } else if (plat === "darwin") {
      const target = pid ? `whose unix id is ${pid}` : `whose name contains ${sq(JSON.stringify(title))}`
      cmd = `osascript -e 'tell application "System Events" to set position of front window of (first process ${target}) to {${x}, ${y}}'`
    } else {
      const probe = await ctx.runCommand("command -v wmctrl || command -v xdotool || true", { timeoutMs: 5000 })
      const t = probe.stdout.split("\n").find(Boolean)?.split("/").pop() ?? ""
      if (!t) return { output: "Linux 窗口控制需要安装 wmctrl 或 xdotool" }
      if (t === "wmctrl") {
        cmd = `wmctrl -r '${sq(title)}' -e 0,${x},${y},${num(args.width, -1)},${num(args.height, -1)}`
      } else {
        const size = args.width != null ? ` ${num(args.width, 0)} ${num(args.height, 0)}` : ""
        cmd = `xdotool search --name '${sq(title)}' windowmove ${x} ${y}${size ? ` windowsize ${num(args.width, 0)} ${num(args.height, 0)}` : ""}`
      }
    }
    return run(ctx, cmd)
  },
}

/* ---------- 输入 ---------- */

export const typeTextTool: Tool = {
  name: "type_text",
  description:
    "向当前聚焦窗口输入文本。默认 clipboard 模式（剪贴板粘贴法，中文/符号可靠：写入回验重试、粘贴后延时恢复剪贴板，写入未生效时明确报错不粘贴；输入前自动做敏感信息检测并预览内容），keys 模式为纯按键逐字符输入（绕开剪贴板，仅 ASCII；中文 IME 激活时部分标点可能丢失）。先确保目标窗口已聚焦（window_focus）。",
  parameters: schema(
    {
      text: { type: "string", description: "要输入的文本" },
      mode: { enum: ["clipboard", "keys"], description: "可选：clipboard=剪贴板粘贴法（默认，写入回验）；keys=纯按键逐字符输入（仅 ASCII，IME 激活时标点可能丢失）" },
    },
    ["text"],
  ),
  async execute(args, ctx) {
    desktopGate(ctx)
    const text = String(args.text ?? "")
    if (!text) return { output: "text 不能为空" }
    const mode = String(args.mode ?? "clipboard")
    // 敏感信息主动告警（D2）：密钥/token 值模式检测到即中止，防经剪贴板泄漏
    const sensitive = detectSensitive(text)
    if (sensitive) {
      return {
        output:
          `⚠️ 检测到疑似敏感信息（密钥/令牌模式：${sensitive}），已中止输入，防止经剪贴板泄漏。` +
          `如确需输入：可改用 mode="keys" 纯按键模式（仅 ASCII 支持，绕开剪贴板），或在确认该文本非敏感后重试。`,
      }
    }
    const preview = text.length > 40 ? `${text.slice(0, 40)}…（共 ${text.length} 字符）` : text
    const plat = process.platform
    let cmd: string
    if (mode === "keys") {
      // 纯按键模式：绕剪贴板逐字符输入，仅 ASCII 可打印字符可靠（SendKeys/osascript keystroke 对非 ASCII 支持不稳定）
      if (/[^\x20-\x7E]/.test(text)) {
        return { output: `mode="keys" 仅支持 ASCII 可打印字符（当前含非 ASCII，如：${text.slice(0, 20)}…）。中文/符号请用默认 clipboard 模式。` }
      }
      if (plat === "win32") {
        cmd = ps(`
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.SendKeys]::SendWait(${psLiteral(sendKeysEscape(text))})
"已输入 ${text.length} 字符（keys 模式）"
`)
      } else if (plat === "darwin") {
        cmd = `osascript -e 'tell application "System Events" to keystroke ${sq(JSON.stringify(text))}'`
      } else {
        const probe = await ctx.runCommand("command -v xdotool || true", { timeoutMs: 5000 })
        if (!probe.stdout.trim()) return { output: "Linux 输入需要安装 xdotool" }
        cmd = `xdotool type --delay 15 '${sq(text)}'`
      }
      const r = await run(ctx, cmd)
      return { output: `${r.output}（内容预览：${preview}；keys 模式）` }
    }
    // clipboard 模式：剪贴板粘贴法。写入回验重试（剪贴板管理软件可能拦截/覆盖写入，未生效即报错不粘贴），
    // 粘贴后延时恢复（目标应用异步消费剪贴板，立即恢复会粘出旧内容）
    if (plat === "win32") {
      cmd = ps(`
Add-Type -AssemblyName System.Windows.Forms
$old = $null; $oldOk = $false
try { $old = Get-Clipboard -Raw; $oldOk = $true } catch {}
$cr = [string][char]13; $lf = [string][char]10
$want = ${psLiteral(text)}
$wantN = $want.Replace($cr + $lf, $lf).Replace($cr, $lf)
$setOk = $false
foreach ($i in 1..3) {
  try { Set-Clipboard -Value $want } catch {}
  Start-Sleep -Milliseconds 80
  try { if (((Get-Clipboard -Raw).Replace($cr + $lf, $lf).Replace($cr, $lf)) -ceq $wantN) { $setOk = $true; break } } catch {}
}
if (-not $setOk) {
  "输入失败: 剪贴板写入未生效（可能被剪贴板管理软件拦截），未执行粘贴，原剪贴板未被修改"
} else {
  try {
    [System.Windows.Forms.SendKeys]::SendWait("^v")
    Start-Sleep -Milliseconds 1000
  } finally {
    if ($oldOk) { try { Set-Clipboard -Value $old } catch { "警告: 原剪贴板恢复失败" } } else { "警告: 原剪贴板备份失败，未恢复" }
    Start-Sleep -Milliseconds 150
  }
  "已输入 ${text.length} 字符（剪贴板已恢复原内容）"
}
`)
    } else if (plat === "darwin") {
      cmd =
        `osascript -e 'try' -e 'set oldClip to the clipboard as text' -e 'on error' -e 'set oldClip to missing value' -e 'end try' ` +
        `-e 'set the clipboard to ${sq(JSON.stringify(text))}' -e 'tell application "System Events" to keystroke "v" using command down' ` +
        `-e 'delay 1' -e 'if oldClip is not missing value then set the clipboard to oldClip'`
    } else {
      const probe = await ctx.runCommand("command -v xdotool || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 输入需要安装 xdotool" }
      cmd = `xdotool type --delay 15 '${sq(text)}'`
    }
    const r = await run(ctx, cmd)
    if (r.output.startsWith("输入失败")) return { output: r.output }
    return { output: `${r.output}（内容预览：${preview}；剪贴板模式）` }
  },
}

/** 剪贴板读取（只读）：截断展示 + 敏感信息扫描主动告警（D2）。 */
export const clipboardReadTool: Tool = {
  name: "clipboard_read",
  description: "读取当前剪贴板内容（只读，不修改剪贴板），截断展示并自动做敏感信息扫描告警。",
  card: { args: "none" },
  parameters: schema({}),
  async execute(_args, ctx) {
    desktopGate(ctx)
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      cmd = ps(`$c = Get-Clipboard -Raw -ErrorAction SilentlyContinue; if ($null -eq $c) { "（剪贴板为空）" } else { $c }`)
    } else if (plat === "darwin") {
      cmd =
        `osascript -e 'try' -e 'set c to the clipboard as text' -e 'return c' -e 'on error' ` +
        `-e 'return "（剪贴板为空或非文本）"' -e 'end try'`
    } else {
      const probe = await ctx.runCommand("command -v xclip || command -v xsel || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 读取剪贴板需要安装 xclip 或 xsel" }
      cmd = probe.stdout.includes("xclip")
        ? "xclip -o -selection clipboard 2>/dev/null || echo （剪贴板为空）"
        : "xsel -b 2>/dev/null || echo （剪贴板为空）"
    }
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { timeoutMs: 15000 })
    if (code !== 0) return { output: `剪贴板读取失败 [exit ${code}]:\n${stderr || stdout}` }
    const content = stdout.replace(/\r?\n$/, "")
    const preview = content.length > 500 ? `${content.slice(0, 500)}…（共 ${content.length} 字符，已截断）` : content
    const sensitive = detectSensitive(content)
    const warn = sensitive
      ? `\n\n⚠️ 检测到疑似敏感信息（密钥/令牌模式：${sensitive}）。请勿将其粘贴到非信任应用；必要时使用 type_text 的 mode="keys"。`
      : ""
    return { output: `剪贴板内容：\n${preview}${warn}` }
  },
}

export const keyPressTool: Tool = {
  name: "key_press",
  description:
    "发送按键/组合键到当前聚焦窗口。keys 使用 SendKeys 语法：{ENTER} {TAB} {ESC} {F5}，^c=Ctrl+C，%{F4}=Alt+F4，+{TAB}=Shift+Tab。macOS 用 osascript 语法（如 \"c\" using command down）。",
  card: { titleParams: ["keys"], args: "none" },
  parameters: schema({ keys: { type: "string", description: "按键/组合键" } }, ["keys"]),
  async execute(args, ctx) {
    desktopGate(ctx)
    const keys = String(args.keys ?? "")
    if (!keys) return { output: "keys 不能为空" }
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      cmd = ps(`
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.SendKeys]::SendWait(${psLiteral(keys)})
"已发送: ${keys}"
`)
    } else if (plat === "darwin") {
      cmd = `osascript -e 'tell application "System Events" to keystroke ${sq(JSON.stringify(keys))}'`
    } else {
      const probe = await ctx.runCommand("command -v xdotool || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 输入需要安装 xdotool" }
      // xdotool 键名仅限字母/数字/下划线/加号（如 ctrl+a），白名单防 shell 注入
      if (!/^[A-Za-z0-9_+]+( [A-Za-z0-9_+]+)*$/.test(keys)) {
        return { output: `keys 含非法字符（仅支持 xdotool 键名组合，如 ctrl+a）：${keys}` }
      }
      cmd = `xdotool key ${keys}`
    }
    return run(ctx, cmd)
  },
}

export const mouseMoveTool: Tool = {
  name: "mouse_move",
  description: "移动鼠标指针到指定屏幕坐标（像素，主屏左上角为原点）。",
  parameters: schema({ x: { type: "number" }, y: { type: "number" } }, ["x", "y"]),
  async execute(args, ctx) {
    desktopGate(ctx)
    const x = num(args.x, 0)
    const y = num(args.y, 0)
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GebaiMouse {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@
[GebaiMouse]::SetCursorPos(${x}, ${y}) | Out-Null
"已移动至 (${x}, ${y})"
`)
    } else if (plat === "darwin") {
      return { output: "macOS 鼠标控制需要安装 cliclick（brew install cliclick），当前未集成" }
    } else {
      const probe = await ctx.runCommand("command -v xdotool || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 鼠标控制需要安装 xdotool" }
      cmd = `xdotool mousemove ${x} ${y}`
    }
    return run(ctx, cmd)
  },
}

export const mouseClickTool: Tool = {
  name: "mouse_click",
  description: "移动鼠标到指定坐标并点击（button: left/right/double，默认 left）。",
  parameters: schema({
    x: { type: "number" },
    y: { type: "number" },
    button: { enum: ["left", "right", "double"] },
  }, ["x", "y"]),
  async execute(args, ctx) {
    desktopGate(ctx)
    const x = num(args.x, 0)
    const y = num(args.y, 0)
    const btn = String(args.button ?? "left")
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      const press = btn === "right" ? "0x0008" : "0x0002"
      const release = btn === "right" ? "0x0010" : "0x0004"
      const double = btn === "double"
        ? "\n  [GebaiMouse2]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero); [GebaiMouse2]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)"
        : ""
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GebaiMouse2 {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
"@
[GebaiMouse2]::SetCursorPos(${x}, ${y}) | Out-Null
[GebaiMouse2]::mouse_event(${press}, 0, 0, 0, [UIntPtr]::Zero)
[GebaiMouse2]::mouse_event(${release}, 0, 0, 0, [UIntPtr]::Zero)${double}
"已${btn}点击 (${x}, ${y})"
`)
    } else if (plat === "darwin") {
      return { output: "macOS 鼠标控制需要安装 cliclick（brew install cliclick），当前未集成" }
    } else {
      const probe = await ctx.runCommand("command -v xdotool || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 鼠标控制需要安装 xdotool" }
      const btnArg = btn === "right" ? "3" : btn === "double" ? "--repeat 2 1" : "1"
      cmd = `xdotool mousemove ${x} ${y} click ${btnArg}`
    }
    return run(ctx, cmd)
  },
}

/* ---------- 滚动 / 拖拽 ---------- */

export const mouseScrollTool: Tool = {
  name: "mouse_scroll",
  description:
    "移动鼠标到指定坐标并滚动滚轮（direction: down/up=垂直（默认 down），left/right=水平；amount 滚动格数默认 3，每格约 3 行）。用于滚动列表/页面/画布。",
  parameters: schema(
    {
      x: { type: "number" },
      y: { type: "number" },
      direction: { enum: ["down", "up", "left", "right"], description: "可选：滚动方向（默认 down）" },
      amount: { type: "number", description: "可选：滚动格数（默认 3，范围 1-30）" },
    },
    ["x", "y"],
  ),
  async execute(args, ctx) {
    desktopGate(ctx)
    const x = num(args.x, 0)
    const y = num(args.y, 0)
    const dir = String(args.direction ?? "down")
    const amount = Math.max(1, Math.min(30, Math.round(num(args.amount, 3))))
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      // wheel 正值向上、负值向下；hwheel 正值向右、负值向左；每格 120 单位（dwData 以带符号解释）
      const units = amount * 120
      const flag = dir === "left" || dir === "right" ? "0x1000" : "0x0800"
      const data = dir === "down" || dir === "left" ? -units : units
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GebaiMouse3 {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, int d, UIntPtr e);
}
"@
[GebaiMouse3]::SetCursorPos(${x}, ${y}) | Out-Null
[GebaiMouse3]::mouse_event(${flag}, 0, 0, ${data}, [UIntPtr]::Zero)
"已向${dir}滚动 ${amount} 格 (${x}, ${y})"
`)
    } else if (plat === "darwin") {
      return { output: "mouse_scroll 当前仅实现 Windows（macOS/Linux 暂未支持）" }
    } else {
      const probe = await ctx.runCommand("command -v xdotool || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 鼠标控制需要安装 xdotool" }
      // xdotool 按键号：4=上 5=下 6=左 7=右
      const btn = { up: "4", down: "5", left: "6", right: "7" }[dir] ?? "5"
      cmd = `xdotool mousemove ${x} ${y} click --repeat ${amount} ${btn}`
    }
    return run(ctx, cmd)
  },
}

export const mouseDragTool: Tool = {
  name: "mouse_drag",
  description:
    "按住左键从 (from_x,from_y) 拖拽到 (to_x,to_y)（插值移动模拟真实轨迹，适配依赖鼠标移动事件的目标）。用于文件拖放、滑块调整、选区。",
  parameters: schema(
    {
      from_x: { type: "number" },
      from_y: { type: "number" },
      to_x: { type: "number" },
      to_y: { type: "number" },
    },
    ["from_x", "from_y", "to_x", "to_y"],
  ),
  async execute(args, ctx) {
    desktopGate(ctx)
    const fx = num(args.from_x, 0)
    const fy = num(args.from_y, 0)
    const tx = num(args.to_x, 0)
    const ty = num(args.to_y, 0)
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GebaiMouse4 {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
"@
[GebaiMouse4]::SetCursorPos(${fx}, ${fy}) | Out-Null
Start-Sleep -Milliseconds 60
[GebaiMouse4]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
$steps = 12
for ($i = 1; $i -le $steps; $i++) {
  $nx = ${fx} + [int]((${tx} - ${fx}) * $i / $steps)
  $ny = ${fy} + [int]((${ty} - ${fy}) * $i / $steps)
  [GebaiMouse4]::SetCursorPos($nx, $ny) | Out-Null
  Start-Sleep -Milliseconds 12
}
[GebaiMouse4]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
"已拖拽 (${fx}, ${fy}) → (${tx}, ${ty})"
`)
    } else if (plat === "darwin") {
      return { output: "mouse_drag 当前仅实现 Windows（macOS/Linux 暂未支持）" }
    } else {
      const probe = await ctx.runCommand("command -v xdotool || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 鼠标控制需要安装 xdotool" }
      cmd = `xdotool mousemove ${fx} ${fy} mousedown 1 mousemove ${tx} ${ty} mouseup 1`
    }
    return run(ctx, cmd)
  },
}

/* ---------- 窗口状态 ---------- */

export const windowStateTool: Tool = {
  name: "window_state",
  description:
    "调整窗口状态（action: minimize=最小化 / maximize=最大化 / restore=还原 / close=关闭）。按 pid 或 title 定位窗口（同 window_focus）。close 经 WM_CLOSE 优雅关闭（应用可弹保存确认）。",
  card: { titleParams: ["action", "pid", "title"], args: "none" },
  parameters: schema(
    {
      action: { enum: ["minimize", "maximize", "restore", "close"], description: "目标状态" },
      pid: { type: "number", description: "可选：目标窗口的 PID（window_list 结果第一列）" },
      title: { type: "string", description: "可选：按标题模糊匹配窗口（pid 未提供时）" },
    },
    ["action"],
  ),
  async execute(args, ctx) {
    desktopGate(ctx)
    const action = String(args.action ?? "")
    const pid = num(args.pid, 0)
    const title = String(args.title ?? "")
    if (!pid && !title) return { output: "请提供 pid 或 title" }
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      // ShowWindow：SW_CLOSE=0（发 WM_CLOSE 优雅关闭）/ SW_MAXIMIZE=3 / SW_MINIMIZE=6 / SW_RESTORE=9
      const sw = { close: "0", maximize: "3", minimize: "6", restore: "9" }[action]
      const verb = { close: "已发送关闭指令", maximize: "已最大化", minimize: "已最小化", restore: "已还原" }[action]
      if (sw === undefined) return { output: `未知 action: ${action}` }
      cmd = ps(`
${PS_DPI_AWARE}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GebaiWinState {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
"@
${procFilter(pid, title)} | Select-Object -First 1
if ($p -and $p.MainWindowHandle -ne 0) {
  [GebaiWinState]::ShowWindow($p.MainWindowHandle, ${sw}) | Out-Null
  "${verb}: $($p.ProcessName) (PID $($p.Id)) - $($p.MainWindowTitle)"
} else { "未找到匹配窗口" }
`)
    } else if (plat === "darwin") {
      return { output: "window_state 当前仅实现 Windows（macOS/Linux 暂未支持）" }
    } else {
      const probe = await ctx.runCommand("command -v wmctrl || command -v xdotool || true", { timeoutMs: 5000 })
      const t = probe.stdout.split("\n").find(Boolean)?.split("/").pop() ?? ""
      if (!t) return { output: "Linux 窗口控制需要安装 wmctrl 或 xdotool" }
      const target = title ? `'${sq(title)}'` : ""
      if (t === "wmctrl") {
        const prop = { minimize: "add,hidden", maximize: "add,maximized_vert,maximized_horz", restore: "remove,maximized_vert,maximized_horz" }[action]
        if (action === "close") return { output: "Linux 下关闭窗口请用 wmctrl -c（或 xdotool key alt+F4），本工具暂未封装" }
        cmd = `wmctrl -r ${target} -b ${prop}`
      } else {
        const op = { minimize: "windowminimize", maximize: "windowsize 100% 100%", restore: "windowsize 50% 50%", close: "key alt+F4" }[action]
        cmd = `xdotool search --name ${target} ${op}`
      }
    }
    return run(ctx, cmd)
  },
}

/* ---------- 剪贴板写入 ---------- */

export const clipboardWriteTool: Tool = {
  name: "clipboard_write",
  description:
    "写入文本到系统剪贴板（覆盖原内容，写入回验重试——剪贴板管理软件拦截时明确报错）。用于把内容交给用户手动粘贴。检测到疑似敏感值时告警但不中止（复制密钥供本人粘贴是常见需求）。",
  parameters: schema({ text: { type: "string", description: "要写入的文本" } }, ["text"]),
  async execute(args, ctx) {
    desktopGate(ctx)
    const text = String(args.text ?? "")
    if (!text) return { output: "text 不能为空" }
    const plat = process.platform
    let cmd: string
    if (plat === "win32") {
      cmd = ps(`
Add-Type -AssemblyName System.Windows.Forms
$want = ${psLiteral(text)}
$setOk = $false
foreach ($i in 1..3) {
  try { Set-Clipboard -Value $want } catch {}
  Start-Sleep -Milliseconds 80
  try { if ((Get-Clipboard -Raw) -ceq $want) { $setOk = $true; break } } catch {}
}
if ($setOk) { "已写入" } else { "写入失败: 剪贴板写入未生效（可能被剪贴板管理软件拦截），原剪贴板可能已被部分修改" }
`)
    } else if (plat === "darwin") {
      cmd = `osascript -e 'set the clipboard to ${sq(JSON.stringify(text))}'`
    } else {
      const probe = await ctx.runCommand("command -v xclip || command -v xsel || true", { timeoutMs: 5000 })
      if (!probe.stdout.trim()) return { output: "Linux 剪贴板需要安装 xclip 或 xsel" }
      cmd = probe.stdout.includes("xclip")
        ? `printf '%s' ${sq(text)} | xclip -selection clipboard`
        : `printf '%s' ${sq(text)} | xsel -b -i`
    }
    const r = await run(ctx, cmd)
    if (r.output.startsWith("写入失败")) return { output: r.output }
    const preview = text.length > 40 ? `${text.slice(0, 40)}…（共 ${text.length} 字符）` : text
    const sensitive = detectSensitive(text)
    const warn = sensitive ? `\n\n⚠️ 内容含疑似敏感信息（${sensitive}），已按原样写入——请勿粘贴到非信任位置。` : ""
    return { output: `${r.output} ${text.length} 字符（内容预览：${preview}）${warn}` }
  },
}
