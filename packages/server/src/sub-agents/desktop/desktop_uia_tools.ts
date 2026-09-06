/**
 * desktop UIA 语义树工具（desktop 专用，仅 Windows）：只读枚举目标窗口的 UI Automation
 * 控件树——角色/名称/AutomationId/bounds/状态/值，补「像素通道拿不到控件语义与状态」的缺口。
 * 设计边界（实测验证）：Chromium/Electron/WebView2 的无障碍树是惰性构建（检测到 UIA 客户端
 * 查询才构建），故固定「触发查询 → 400ms → 正式收集」双查询；Flutter/游戏/自绘框架不暴露
 * 语义（浅树/空树），此时明确提示回落像素通道（desktop_ocr/locate/locate_image/detect）。
 * 只读免审批；不点击不输入，坐标供 mouse_click 交叉校验。
 */
import type { Tool, ToolResult } from "../../core/base/types"
import { schema } from "../../core/tools/shared"

function desktopGate(ctx: { sandboxed?: boolean }): void {
  if ((ctx as { sandboxed?: boolean }).sandboxed) throw new Error("桌面控制仅在本地/桌面模式可用（服务端部署已禁用）")
}

function num(v: unknown, dflt: number): number {
  const n = Number(v)
  return Number.isFinite(n) ? n : dflt
}

/** 输出/数据节点行数上限（控件树规模失控保护；超出标记 truncated 建议收窄 depth/region）。 */
const UIA_NODE_LIMIT = 400

export const uiaInspectTool: Tool = {
  name: "uia_inspect",
  description:
    "枚举目标窗口的 UI Automation 语义控件树（仅 Windows，只读）：控件角色/名称/AutomationId/类名/bounds/状态（enabled/focused）/值（输入框文本等）。定位窗口按 pid 或 title（可组合）。与像素通道互补：控件状态（禁用/勾选/焦点）与值是 OCR 判不了的，控件中心坐标可直接 mouse_click 交叉校验；Chromium/Electron 系无障碍树惰性构建已内置双查询（首查触发构建，400ms 后正式收集）。空树/浅树（<5 节点）说明目标是 Flutter/游戏/自绘框架（不暴露语义），应回落 desktop_ocr/desktop_locate/desktop_locate_image 像素通道。find 关键词只输出名称/ID 匹配的控件（含路径），depth 控制深度（默认 6，上限 12），max 控制行数上限（默认 200，上限 400）。",
  card: { titleParams: ["pid", "title", "find"], args: "none" },
  parameters: schema(
    {
      pid: { type: "number", description: "可选：目标窗口 PID（window_list 结果第一列）" },
      title: { type: "string", description: "可选：按标题模糊匹配窗口（与 pid 可组合，同进程多窗口时区分用）" },
      find: { type: "string", description: "可选：关键词过滤，只输出名称/AutomationId 含关键词的控件（含其路径）" },
      depth: { type: "number", description: "可选：枚举深度（默认 6，上限 12）" },
      max: { type: "number", description: "可选：输出行数上限（默认 200，上限 400）" },
    },
    [],
  ),
  async execute(args, ctx): Promise<ToolResult> {
    desktopGate(ctx)
    if (process.platform !== "win32") {
      return { output: "uia_inspect 仅支持 Windows（UI Automation 语义树；macOS AX / Linux AT-SPI 暂未集成）" }
    }
    const pid = num(args.pid, 0)
    const title = String(args.title ?? "").trim()
    if (!pid && !title) return { output: "请提供 pid 或 title（定位目标窗口）" }
    const find = String(args.find ?? "").trim()
    const depthMax = Math.max(1, Math.min(12, num(args.depth, 6)))
    const maxRows = Math.max(10, Math.min(UIA_NODE_LIMIT, num(args.max, 200)))
    // PowerShell 全量注入 base64（find 关键词经 psLiteral 同路径，无插值面）——沿用 desktop_tools 约定
    const psScript = `
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
$trueCond = [System.Windows.Automation.Condition]::TrueCondition
$rootEl = [System.Windows.Automation.AutomationElement]::RootElement
$wins = $rootEl.FindAll([System.Windows.Automation.TreeScope]::Children, $trueCond)
$target = $null
foreach ($w in $wins) {
  $c = $w.Current
  if (${pid ? `$c.ProcessId -ne ${pid}` : "$false"}) { continue }
  $wt = [string]$c.Name
  ${title ? `if (-not $wt.Contains([System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String("${Buffer.from(title, "utf8").toString("base64")}")))) { continue }` : ""}
  $target = $w; break
}
if (-not $target) { "NOTFOUND"; exit }
# 双查询：首查触发 Chromium 系惰性无障碍树构建，400ms 后正式收集
[void]$target.FindAll([System.Windows.Automation.TreeScope]::Descendants, $trueCond)
Start-Sleep -Milliseconds 400
$rows = New-Object System.Collections.Generic.List[object]
$script:truncated = $false
$script:total = 0
$findB64 = "${Buffer.from(find, "utf8").toString("base64")}"
$findKw = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($findB64))
function Walk($el, [int]$depth, [string]$path) {
  $script:total++
  if ($rows.Count -ge ${maxRows} + ${find ? "0" : "0"}) { $script:truncated = $true; return }
  $c = $el.Current
  $ct = $c.ControlType.ProgrammaticName -replace "^ControlType\\.", ""
  if (-not $ct) { $ct = "Unknown" }
  $nm = [string]$c.Name
  $aid = [string]$c.AutomationId
  $r = $c.BoundingRectangle
  $states = @()
  if (-not $c.IsEnabled) { $states += "disabled" }
  if ($c.HasKeyboardFocus) { $states += "focused" }
  # 屏幕外/未布局元素 Rect 可为 Infinity/NaN——归零防 Int32 转换异常
  if ([double]::IsInfinity($r.X) -or [double]::IsNaN($r.X) -or [double]::IsInfinity($r.Y) -or [double]::IsNaN($r.Y) -or [double]::IsInfinity($r.Width) -or [double]::IsNaN($r.Width) -or [double]::IsInfinity($r.Height) -or [double]::IsNaN($r.Height)) { $r = New-Object System.Windows.Rect(0, 0, 0, 0) }
  $val = ""
  try {
    $vp = $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    if ($vp) { $val = [string]($vp).Current.Value }
  } catch {}
  $thisLabel = $ct + ":" + $nm
  $newPath = if ($path) { $path + " > " + $thisLabel } else { $thisLabel }
  $kwHit = ($findKw -eq "") -or ($nm -like "*$findKw*") -or ($aid -like "*$findKw*")
  if ($kwHit) {
    $rows.Add([pscustomobject]@{
      depth = $depth; role = $ct; name = $nm; automationId = $aid
      className = [string]$c.ClassName
      x = [int][Math]::Round($r.X); y = [int][Math]::Round($r.Y)
      w = [int][Math]::Round($r.Width); h = [int][Math]::Round($r.Height)
      states = ($states -join ","); value = $val; path = $newPath
    }) | Out-Null
  }
  if ($depth -ge ${depthMax}) { return }
  $kids = $el.FindAll([System.Windows.Automation.TreeScope]::Children, $trueCond)
  foreach ($k in $kids) { Walk $k ($depth + 1) $newPath }
}
Walk $target 0 ""
if ($rows.Count -eq 0 -and $script:total -le 1) {
  "EMPTY total=$($script:total) title=$([string]$target.Current.Name)"
} elseif ($rows.Count -eq 0 -and "${find}" -ne "") {
  "NOKW total=$($script:total)"
} else {
  $json = $rows | ConvertTo-Json -Depth 4 -Compress
  "OK total=$($script:total) shown=$($rows.Count) truncated=$($script:truncated) kw=${find}"
  $json
}
`
    const b64 = Buffer.from(psScript, "utf16le").toString("base64")
    const { stdout, stderr, code } = await ctx.runCommand(`powershell -NoProfile -NonInteractive -EncodedCommand ${b64}`, { timeoutMs: 60000 })
    if (code !== 0) return { output: `UIA 枚举失败 [exit ${code}]: ${(stderr || stdout).slice(0, 500)}` }
    const text = stdout.trim()
    if (text.startsWith("NOTFOUND")) return { output: `未找到匹配窗口（pid=${pid}${title ? ` title~${title}` : ""}）——用 window_list 确认目标存在` }
    if (text.startsWith("EMPTY")) {
      const total = Number(text.match(/total=(\d+)/)?.[1] ?? 0)
      return {
        output:
          `目标窗口无障碍树为空（${total} 节点）——Flutter/游戏/自绘框架不暴露 UIA 语义，请回落像素通道：desktop_ocr（读文字）/desktop_locate（定位文字）/desktop_locate_image（模板匹配）后 mouse_click。`,
      }
    }
    if (text.startsWith("NOKW")) {
      const total = Number(text.match(/total=(\d+)/)?.[1] ?? 0)
      return { output: `枚举完成（共 ${total} 节点），无名称/AutomationId 匹配「${find}」的控件——放宽关键词或去掉 find 看全树。`, data: { found: false, total } }
    }
    if (!text.startsWith("OK")) return { output: `UIA 输出异常: ${text.slice(0, 300)}` }
    const lines = text.split("\n")
    const metaLine = lines[0]
    const total = Number(metaLine.match(/total=(\d+)/)?.[1] ?? 0)
    const truncated = metaLine.includes("truncated=True")
    // JSON 可能跨多行（ConvertTo-Json 折行）——元数据行之后全部拼接
    let nodes: unknown[] = []
    try {
      nodes = JSON.parse(lines.slice(1).join("\n"))
      if (!Array.isArray(nodes)) nodes = [nodes] // 单元素时 ConvertTo-Json 输出对象而非数组
    } catch {
      // JSON 解析失败仍输出文本行（保底可用）
    }
    const shallow = total >= 1 && total < 5
    // 中心坐标 + 展开行（缩进按 depth）
    const rows = (nodes as Array<Record<string, unknown>>).slice(0, maxRows)
    const body = rows
      .map((n) => {
        const x = Number(n.x), y = Number(n.y), w = Number(n.w), h = Number(n.h)
        const cx = Math.round(x + w / 2), cy = Math.round(y + h / 2)
        const states = [n.states, n.value ? `值="${String(n.value).slice(0, 40)}"` : ""].filter(Boolean).join(" ")
        return `${"  ".repeat(Number(n.depth) ?? 0)}${n.role}  "${String(n.name ?? "").slice(0, 60)}"  [${x},${y},${w},${h}] → 中心 (${cx},${cy})${states ? `  ${states}` : ""}${n.automationId ? `  id=${n.automationId}` : ""}`
      })
      .join("\n")
    const hint = shallow
      ? `\n⚠️ 树极浅（${total} 节点）——疑似 Flutter/游戏/自绘框架（不暴露语义），像素通道（desktop_ocr/locate/locate_image）优先。`
      : ""
    return {
      output:
        `UIA 语义树（共 ${total} 节点，显示 ${rows.length}${truncated ? "，已达上限被截断——收窄 depth 或用 find 过滤" : ""}；坐标为屏幕像素，中心可直接 mouse_click）：\n${body}${hint}`,
      data: { found: true, total, truncated, nodes: rows },
    }
  },
}
