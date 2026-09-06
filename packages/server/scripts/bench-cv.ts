/**
 * CV 分层视觉栈综合实测（性能/可靠性/准确性）——诊断工具，不进构建链。
 * 运行：cd packages/server && bun run scripts/bench-cv.ts
 * 前提：models 资源仓库就位（零配置），sidecar 可用（GPU/native）。
 */
import { getCvRunner } from "../src/core/cv/cv"
import { decodePng, cropImage } from "../src/core/cv/image"
import { matchTemplate } from "../src/core/cv/template"

const TMP = "C:/Users/Administrator/AppData/Local/Temp/"
const SHOT = TMP + "bench-full.png"
const GROUND = TMP + "bench-ground.png"
const sidecarEnv: Record<string, string> = {}
const wasmEnv: Record<string, string> = { GEBAI_CV_BACKEND: "wasm" }

const ps = (script: string) =>
  Bun.spawnSync(["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand", Buffer.from(script, "utf16le").toString("base64")])

function capture(path: string, region?: string): void {
  const bounds = region
    ? `New-Object System.Drawing.Rectangle(${region.split(",").join(", ")})`
    : "[System.Windows.Forms.SystemInformation]::VirtualScreen"
  ps(`
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
}

function groundTruth(): { text: string; x: number; y: number } {
  // 已知文字绘制在白底图 (60, 90)，48px Arial —— OCR/locate 应读出且坐标应落在该区域
  ps(`
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 900, 320
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font('Arial', 48)
$g.DrawString('GEBAI TEST 2026', $font, [System.Drawing.Brushes]::Black, 60, 90)
$g.DrawString('识别准确率', $font, [System.Drawing.Brushes]::Black, 60, 200)
$bmp.Save('${GROUND}', [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
`)
  return { text: "GEBAI TEST 2026", x: 60, y: 90 }
}

const median = (xs: number[]) => xs.slice().sort((a, b) => a - b)[Math.floor(xs.length / 2)]
const p95 = (xs: number[]) => xs.slice().sort((a, b) => a - b)[Math.min(xs.length - 1, Math.floor(xs.length * 0.95))]
const fmt = (xs: number[]) => `中位 ${median(xs)}ms / p95 ${p95(xs)}ms / 最小 ${Math.min(...xs)}ms`

/** 事件循环阻塞探测：在 during() 执行期间以 10ms 心跳计时，报告最大间隙。 */
async function loopGap(during: () => Promise<unknown>): Promise<{ total: number; maxGap: number }> {
  let maxGap = 0
  let last = Date.now()
  let stop = false
  const timer = setInterval(() => {
    const now = Date.now()
    maxGap = Math.max(maxGap, now - last)
    last = now
  }, 10)
  const t0 = Date.now()
  await during()
  // 等待阻塞期间排队的 tick 执行（若循环被阻塞，恢复后首拍 gap 即阻塞时长）再停表
  await new Promise((r) => setTimeout(r, 60))
  stop = true
  clearInterval(timer)
  void stop
  return { total: Date.now() - t0 - 60, maxGap }
}

/** 找 sidecar node 进程（cv-driver.mjs）。按进程名 node.exe 过滤——否则查询自身命令行
 *  含 cv-driver 关键字会自匹配（powershell/bash 链全被算进去，每次查询 PID 都不同）。 */
function sidecarProcs(): Array<{ pid: number; ws: number }> {
  const r = Bun.spawnSync(["powershell", "-NoProfile", "-NonInteractive", "-Command",
    "Get-CimInstance Win32_Process -Filter \"Name='node.exe'\" | Where-Object { $_.CommandLine -like '*cv-driver.mjs*' } | ForEach-Object { $p = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue; \"{0},{1}\" -f $_.ProcessId, ($p.WorkingSet64/1MB) }"])
  return r.stdout.toString().split(/\r?\n/).map((l) => l.trim()).filter(Boolean)
    .map((l) => ({ pid: Number(l.split(",")[0]), ws: Number(l.split(",")[1]) }))
}

async function main(): Promise<void> {
  console.log("========== 准备 ==========")
  capture(SHOT)
  const full = decodePng(new Uint8Array(await Bun.file(SHOT).arrayBuffer()))
  const probe = await getCvRunner().ocr(full, { env: sidecarEnv })
  const anchor = probe.lines[Math.floor(probe.lines.length / 2)] ?? probe.lines[0]
  const ax = anchor ? Math.max(0, Math.min(full.width - 800, Math.floor(anchor.box.x + anchor.box.w / 2 - 400))) : Math.floor(full.width / 2 - 400)
  const ay = anchor ? Math.max(0, Math.min(full.height - 600, Math.floor(anchor.box.y + anchor.box.h / 2 - 300))) : Math.floor(full.height / 2 - 300)
  const region = cropImage(full, { x: ax, y: ay, w: 800, h: 600 })
  console.log(`全屏 ${full.width}x${full.height}（OCR ${probe.lines.length} 行）｜测试区域 800x600@(${ax},${ay}) 围绕文字行「${anchor?.text.trim().slice(0, 10) ?? "?"}」裁取｜sidecar: ${sidecarProcs().length ? "已启动" : "未启动（首调拉起）"}`)

  /* ---------------- 性能 ---------------- */
  console.log("\n========== 性能 ==========")
  let t0 = Date.now()
  await getCvRunner().ocr(region, { env: sidecarEnv })
  console.log(`[OCR 区域] sidecar 冷启动（含会话建立/驱动拉起）: ${Date.now() - t0}ms`)
  t0 = Date.now()
  await getCvRunner().detect(region, { env: sidecarEnv, conf: 0.1, iou: 0.1 })
  console.log(`[detect] sidecar 冷启动（含模型加载 ${""}）: ${Date.now() - t0}ms`)

  const ocrWarm: number[] = []
  for (let i = 0; i < 5; i++) {
    t0 = Date.now()
    await getCvRunner().ocr(region, { env: sidecarEnv })
    ocrWarm.push(Date.now() - t0)
  }
  console.log(`[OCR 区域 800x600] sidecar 热 x5: ${fmt(ocrWarm)}`)
  const ocrFull: number[] = []
  for (let i = 0; i < 3; i++) {
    t0 = Date.now()
    await getCvRunner().ocr(full, { env: sidecarEnv })
    ocrFull.push(Date.now() - t0)
  }
  console.log(`[OCR 全屏 ${full.width}x${full.height}] sidecar 热 x3: ${fmt(ocrFull)}`)
  t0 = Date.now()
  const ocrWasm = await getCvRunner().ocr(region, { env: wasmEnv })
  console.log(`[OCR 区域] wasm 强制单次: ${Date.now() - t0}ms（行数 ${ocrWasm.lines.length}）`)

  const detWarm: number[] = []
  for (let i = 0; i < 5; i++) {
    t0 = Date.now()
    await getCvRunner().detect(region, { env: sidecarEnv, conf: 0.1, iou: 0.1 })
    detWarm.push(Date.now() - t0)
  }
  console.log(`[detect 区域@1280] sidecar 热 x5: ${fmt(detWarm)}`)
  t0 = Date.now()
  const detWasm = await getCvRunner().detect(region, { env: wasmEnv, conf: 0.1, iou: 0.1 })
  console.log(`[detect 区域@1280] wasm 强制单次: ${Date.now() - t0}ms（对象 ${detWasm.objects.length}，后端 ${detWasm.backend}）`)

  const tpl = cropImage(region, { x: 300, y: 250, w: 48, h: 48 })
  const tplTimes: number[] = []
  for (let i = 0; i < 3; i++) {
    t0 = Date.now()
    matchTemplate(region, tpl)
    tplTimes.push(Date.now() - t0)
  }
  console.log(`[locate_image 模板匹配 48px@800x600]（纯 JS 进程内）x3: ${fmt(tplTimes)}`)

  const g1 = await loopGap(() => getCvRunner().detect(region, { env: sidecarEnv, conf: 0.1, iou: 0.1 }))
  console.log(`[事件循环] sidecar detect 期间: 总耗时 ${g1.total}ms，心跳最大间隙 ${g1.maxGap}ms（间隙=进程内前后处理的同步段；推理在子进程）`)
  const g2 = await loopGap(() => getCvRunner().ocr(region, { env: wasmEnv }))
  console.log(`[事件循环] wasm OCR 期间: 总耗时 ${g2.total}ms，心跳最大间隙 ${g2.maxGap}ms（推理同步阻塞）`)

  /* ---------------- 可靠性 ---------------- */
  console.log("\n========== 可靠性 ==========")
  let fails = 0
  const counts: number[] = []
  const rss0 = process.memoryUsage.rss() / 1e6
  t0 = Date.now()
  for (let i = 0; i < 20; i++) {
    try {
      const r = await getCvRunner().ocr(region, { env: sidecarEnv })
      counts.push(r.lines.length)
    } catch {
      fails++
    }
  }
  const rss1 = process.memoryUsage.rss() / 1e6
  const sc0 = sidecarProcs()
  console.log(`[OCR 连续 x20] 失败 ${fails}/20｜行数 min=${Math.min(...counts)} max=${Math.max(...counts)}（稳定=${Math.min(...counts) === Math.max(...counts)}）｜总 ${Date.now() - t0}ms｜主进程 RSS ${rss0.toFixed(0)}MB → ${rss1.toFixed(0)}MB｜sidecar ${sc0.length} 个进程 RSS ${sc0.map((p) => p.ws.toFixed(0) + "MB").join("/")}`)

  // 进程被杀自愈
  const victims = sidecarProcs()
  if (victims.length) {
    Bun.spawnSync(["powershell", "-NoProfile", "-NonInteractive", "-Command", `Stop-Process -Id ${victims[0].pid} -Force`])
    await new Promise((r) => setTimeout(r, 300))
    try {
      t0 = Date.now()
      const r = await getCvRunner().detect(region, { env: sidecarEnv, conf: 0.1, iou: 0.1 })
      const healed = sidecarProcs()
      console.log(`[进程被杀自愈] kill PID ${victims[0].pid} → 下次调用自动重启: ${Date.now() - t0}ms 恢复，检出 ${r.objects.length} 个（后端 ${r.backend}，新 PID ${healed[0]?.pid ?? "?"}）`)
    } catch (e) {
      console.log(`[进程被杀自愈] 失败: ${e instanceof Error ? e.message : e}`)
    }
  }

  // 双后端并发（sidecar 不阻塞主进程，OCR 与 detect 应可并行）
  t0 = Date.now()
  const [c1, c2] = await Promise.all([
    getCvRunner().ocr(region, { env: sidecarEnv }),
    getCvRunner().detect(region, { env: sidecarEnv, conf: 0.1, iou: 0.1 }),
  ])
  console.log(`[并发] OCR(${c1.lines.length} 行) + detect(${c2.objects.length} 个) 并行总耗时 ${Date.now() - t0}ms`)

  /* ---------------- 准确性 ---------------- */
  console.log("\n========== 准确性 ==========")
  const a = await getCvRunner().ocr(region, { env: sidecarEnv })
  const b = await getCvRunner().ocr(region, { env: wasmEnv })
  const sameCount = a.lines.length === b.lines.length
  let textEq = 0
  let maxDelta = 0
  const n = Math.min(a.lines.length, b.lines.length)
  for (let i = 0; i < n; i++) {
    if (a.lines[i].text.replace(/\s/g, "") === b.lines[i].text.replace(/\s/g, "")) textEq++
    const da = a.lines[i].box
    const db = b.lines[i].box
    maxDelta = Math.max(maxDelta, Math.abs(da.x + da.w / 2 - (db.x + db.w / 2)), Math.abs(da.y + da.h / 2 - (db.y + db.h / 2)))
  }
  console.log(`[OCR 双后端等价] sidecar ${a.lines.length} 行 vs wasm ${b.lines.length} 行｜行数一致 ${sameCount}｜文本一致 ${textEq}/${n}｜中心坐标最大偏差 ${maxDelta.toFixed(1)}px`)

  const d1 = await getCvRunner().detect(region, { env: sidecarEnv, conf: 0.1, iou: 0.1 })
  const d2 = await getCvRunner().detect(region, { env: wasmEnv, conf: 0.1, iou: 0.1 })
  // 贪心 IoU 配对
  const iou = (p: { x: number; y: number; w: number; h: number }, q: { x: number; y: number; w: number; h: number }) => {
    const x1 = Math.max(p.x, q.x), y1 = Math.max(p.y, q.y)
    const x2 = Math.min(p.x + p.w, q.x + q.w), y2 = Math.min(p.y + p.h, q.y + q.h)
    const inter = Math.max(0, x2 - x1) * Math.max(0, y2 - y1)
    return inter / (p.w * p.h + q.w * q.h - inter)
  }
  let matched = 0
  let labelAgree = 0
  let centerMax = 0
  const used = new Set<number>()
  for (const o of d1.objects) {
    let best = -1
    let bestIoU = 0
    d2.objects.forEach((p, j) => {
      if (used.has(j)) return
      const v = iou(o, p)
      if (v > bestIoU) { bestIoU = v; best = j }
    })
    if (best >= 0 && bestIoU > 0.5) {
      used.add(best)
      matched++
      if (d2.objects[best].label === o.label) labelAgree++
      const p = d2.objects[best]
      centerMax = Math.max(centerMax, Math.abs(o.x + o.w / 2 - (p.x + p.w / 2)), Math.abs(o.y + o.h / 2 - (p.y + p.h / 2)))
    }
  }
  console.log(`[detect 双后端等价] sidecar ${d1.objects.length} vs wasm ${d2.objects.length}｜IoU>0.5 配对 ${matched}｜标签一致 ${labelAgree}/${matched}｜中心最大偏差 ${centerMax.toFixed(1)}px`)

  // ground truth：已知绘制文字
  const gt = groundTruth()
  const gImg = decodePng(new Uint8Array(await Bun.file(GROUND).arrayBuffer()))
  const gOcr = await getCvRunner().ocr(gImg, { env: sidecarEnv })
  const hit = gOcr.lines.find((l) => l.text.toUpperCase().includes("GEBAI"))
  const zh = gOcr.lines.find((l) => l.text.includes("识别") || l.text.includes("准确"))
  if (hit) {
    const cx = hit.box.x + hit.box.w / 2
    const cy = hit.box.y + hit.box.h / 2
    console.log(`[ground-truth OCR] 读出「${hit.text.trim()}」｜中心 (${cx.toFixed(0)},${cy.toFixed(0)}) vs 绘制区 (60~860, 90~160)｜在区内 ${cx >= 60 && cx <= 860 && cy >= 90 && cy <= 160}`)
  } else {
    console.log(`[ground-truth OCR] 未读出 GEBAI（读到: ${gOcr.lines.map((l) => l.text).join(" | ").slice(0, 80)}）`)
  }
  console.log(`[ground-truth 中文] ${zh ? `读出「${zh.text.trim()}」` : "未读出（中文字典受限可接受）"}`)

  // 跨通道一致性：locate 的文字中心应落在某个 detect 框内（配对几何约定）
  const located = a.lines[0]
  if (located) {
    const inBox = d1.objects.some((o) => {
      const cx = located.box.x + located.box.w / 2
      const cy = located.box.y + located.box.h / 2
      return cx >= o.x && cx <= o.x + o.w && cy >= o.y && cy <= o.y + o.h
    })
    console.log(`[跨通道一致] OCR 首行「${located.text.trim().slice(0, 12)}」中心 ${inBox ? "落在" : "不在"}任一 detect 框内（Text 框覆盖关系的样本观察，非硬性要求）`)
  }

  console.log("\n========== 完成 ==========")
  process.exit(0)
}

await main()
