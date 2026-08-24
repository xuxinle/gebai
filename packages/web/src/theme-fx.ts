/**
 * 主题高级特效引擎：为各主题提供画布环境粒子特效。
 *
 * - 画布 #gb-theme-fx 固定全屏、z-index:-1、pointer-events:none——位于背景装饰之上、
 *   全部内容之下（主题 CSS 将 main 背景透明化以透出特效，同 aurora/synthwave 既有模式）；
 *   气泡/卡片自身背景保证正文可读
 * - 挂载模式同 cny-cat：监听 gebai:theme-change 按 data-theme 启停；
 *   低性能模式整体停用（gebai:low-power-change 跟随启停）
 * - 粒子量随视口面积缩放；rAF 循环 dt 钳制，标签页隐藏由浏览器自动暂停
 * - 各主题特效均为「环境层」：低透明度、不遮挡、不交互；默认主题（acrylic）不配环境特效
 *   （保持毛玻璃原味），cny 的招财猫/爆金币为独立交互层
 */
import { isLowPower } from "./low-power"
import type { ThemeId } from "./theme"

type Cleanup = () => void
type Effect = (ctx: CanvasRenderingContext2D) => Cleanup

const rand = (min: number, max: number) => min + Math.random() * (max - min)
const pick = <T>(arr: readonly T[]): T => arr[(Math.random() * arr.length) | 0]

/** "#rrggbb" + 透明度 → rgba() 字符串。 */
function hexA(hex: string, a: number): string {
  const n = parseInt(hex.slice(1), 16)
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a.toFixed(3)})`
}

/** rAF 主循环：fn(dt 秒, t 秒)，dt 钳制防隐藏后恢复跳帧；返回取消函数。 */
function runLoop(fn: (dt: number, t: number) => void): Cleanup {
  let raf = 0
  let last = 0
  const tick = (ts: number) => {
    const dt = last ? Math.min(0.05, Math.max(0.001, (ts - last) / 1000)) : 0.016
    last = ts
    fn(dt, ts / 1000)
    raf = requestAnimationFrame(tick)
  }
  raf = requestAnimationFrame(tick)
  return () => cancelAnimationFrame(raf)
}

/** 粒子数量按视口面积缩放（1440×900 基准，0.55~1.6 钳制）。 */
function density(base: number): number {
  const k = Math.sqrt((window.innerWidth * window.innerHeight) / (1440 * 900))
  return Math.max(4, Math.round(base * Math.min(1.6, Math.max(0.55, k))))
}

/** 柔光圆：径向渐变光晕，core=true 时加中心亮核（萤火/光尘用）。 */
function glowDot(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  r: number,
  color: string,
  a: number,
  core = false,
): void {
  const g = ctx.createRadialGradient(x, y, 0, x, y, r)
  g.addColorStop(0, hexA(color, a))
  g.addColorStop(1, hexA(color, 0))
  ctx.fillStyle = g
  ctx.beginPath()
  ctx.arc(x, y, r, 0, Math.PI * 2)
  ctx.fill()
  if (core) {
    ctx.fillStyle = hexA(color, Math.min(1, a * 2))
    ctx.beginPath()
    ctx.arc(x, y, Math.max(0.7, r * 0.1), 0, Math.PI * 2)
    ctx.fill()
  }
}

/* ---------------- 各主题特效 ---------------- */

/** 数字雨（matrix）：逐列下落字符流，destination-out 逐帧渐隐形成拖尾（帧率无关）；整体压暗防刺眼。 */
function matrixFx(ctx: CanvasRenderingContext2D): Cleanup {
  const GLYPHS = "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホ0123456789ZXYWUSRPMKGB:#$+-=<>"
  const FONT = 15
  const HEAD = "#b8e8c2"
  const BODY = "#00c437"
  type Col = { y: number; speed: number; dim: number }
  let cols: Col[] = []
  const spawnCol = (): Col => ({ y: rand(-40, 0), speed: rand(4, 10.5), dim: rand(0.3, 0.75) })
  const spawn = () => {
    cols = Array.from({ length: Math.ceil(window.innerWidth / FONT) }, spawnCol)
    ctx.font = `${FONT}px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`
    ctx.textAlign = "center"
    ctx.textBaseline = "top"
  }
  spawn()
  const onResize = () => spawn()
  window.addEventListener("resize", onResize, { passive: true })
  const draw = (col: number, row: number, color: string, a: number, rows: number) => {
    if (row < 0 || row > rows) return
    ctx.fillStyle = color
    ctx.globalAlpha = a
    ctx.fillText(GLYPHS[(Math.random() * GLYPHS.length) | 0], col * FONT + FONT / 2, row * FONT)
  }
  const stop = runLoop((dt) => {
    const w = window.innerWidth
    const h = window.innerHeight
    const rows = Math.ceil(h / FONT)
    ctx.globalCompositeOperation = "destination-out"
    ctx.globalAlpha = 1
    ctx.fillStyle = `rgba(0,0,0,${(1 - Math.exp(-dt * 3.4)).toFixed(4)})`
    ctx.fillRect(0, 0, w, h)
    ctx.globalCompositeOperation = "source-over"
    for (let i = 0; i < cols.length; i++) {
      const c = cols[i]
      const prev = Math.floor(c.y)
      c.y += c.speed * dt
      const cur = Math.floor(c.y)
      for (let r = prev; r < cur; r++) draw(i, r, BODY, 0.5 * c.dim, rows) // 拖尾字：画一次后随帧渐隐
      draw(i, cur, HEAD, 0.55 * Math.max(0.45, c.dim), rows) // 头部亮字
      if (c.y > rows + 8) {
        c.y = rand(-46, -2)
        c.speed = rand(4, 10.5)
        c.dim = rand(0.3, 0.75)
      }
    }
    ctx.globalAlpha = 1
  })
  return () => {
    window.removeEventListener("resize", onResize)
    stop()
  }
}

/** 流星子系统（tokyo-night / synthwave 共用）：隔随机时长划过一颗，带渐隐尾迹。 */
type Meteor = { x: number; y: number; vx: number; vy: number; life: number; age: number; tail: number; color: string }
function meteors(colors: readonly string[], minGap: number, maxGap: number) {
  const m: Meteor[] = []
  let next = rand(minGap, maxGap)
  return (dt: number, ctx: CanvasRenderingContext2D, w: number, h: number) => {
    next -= dt
    if (next <= 0) {
      next = rand(minGap, maxGap)
      const dir = Math.random() < 0.5 ? -1 : 1
      const speed = rand(480, 820)
      const ang = rand(Math.PI * 0.12, Math.PI * 0.2)
      m.push({
        x: rand(w * 0.15, w * 0.85),
        y: rand(h * 0.05, h * 0.3),
        vx: Math.cos(ang) * speed * dir,
        vy: Math.sin(ang) * speed,
        life: rand(0.7, 1.1),
        age: 0,
        tail: rand(80, 150),
        color: pick(colors),
      })
    }
    ctx.lineWidth = 1.6
    ctx.lineCap = "round"
    for (let i = m.length - 1; i >= 0; i--) {
      const p = m[i]
      p.age += dt
      p.x += p.vx * dt
      p.y += p.vy * dt
      if (p.age >= p.life || p.y > h * 0.75) {
        m.splice(i, 1)
        continue
      }
      const a = (1 - p.age / p.life) * 0.8
      const vlen = Math.hypot(p.vx, p.vy)
      const k = (p.tail * (1 - (p.age / p.life) * 0.4)) / vlen
      const tx = p.x - p.vx * k
      const ty = p.y - p.vy * k
      const g = ctx.createLinearGradient(p.x, p.y, tx, ty)
      g.addColorStop(0, hexA(p.color, a))
      g.addColorStop(1, hexA(p.color, 0))
      ctx.strokeStyle = g
      ctx.beginPath()
      ctx.moveTo(p.x, p.y)
      ctx.lineTo(tx, ty)
      ctx.stroke()
      glowDot(ctx, p.x, p.y, 7, p.color, a * 0.5)
    }
  }
}

/** 霓虹车流（tokyo-night）：夜城高窗视角的横向流光——远中近三层车速的車灯拖尾穿行，偶有流星。 */
function tokyoNightFx(ctx: CanvasRenderingContext2D): Cleanup {
  const COLORS = ["#7aa2f7", "#bb9af7", "#7dcfff", "#ff9e64", "#c0caf5"]
  type Car = { x: number; y: number; dir: number; speed: number; len: number; color: string; alpha: number; lw: number }
  const cars: Car[] = []
  const spawnCar = (): Car => {
    // 三层纵深：远（上部，慢而细淡）→ 近（下部，快而长亮），方向随层固定成对向车流
    const layer = Math.random()
    const near = layer < 0.34 ? 0 : layer < 0.67 ? 0.5 : 1
    const w = window.innerWidth
    const h = window.innerHeight
    return {
      x: rand(-w * 0.2, w * 1.2),
      y: rand(h * (0.12 + near * 0.28), h * (0.24 + near * 0.62)),
      dir: Math.random() < 0.5 ? 1 : -1,
      speed: rand(50, 110) + near * rand(130, 230),
      len: rand(26, 60) + near * rand(40, 90),
      color: pick(COLORS),
      alpha: 0.1 + near * rand(0.14, 0.34),
      lw: 1.2 + near * 1.4,
    }
  }
  for (let i = 0; i < density(24); i++) cars.push(spawnCar())
  const meteorStep = meteors(["#c0caf5", "#bb9af7"], 7, 15)
  const stop = runLoop((dt) => {
    const w = window.innerWidth
    const h = window.innerHeight
    ctx.clearRect(0, 0, w, h)
    ctx.lineCap = "round"
    for (const c of cars) {
      c.x += c.speed * c.dir * dt
      if (c.dir > 0 && c.x - c.len > w + 40) c.x = -rand(20, 240)
      if (c.dir < 0 && c.x + c.len < -40) c.x = w + rand(20, 240)
      const tail = c.x - c.dir * c.len
      const g = ctx.createLinearGradient(c.x, c.y, tail, c.y)
      g.addColorStop(0, hexA(c.color, c.alpha))
      g.addColorStop(1, hexA(c.color, 0))
      ctx.strokeStyle = g
      ctx.lineWidth = c.lw
      ctx.beginPath()
      ctx.moveTo(tail, c.y)
      ctx.lineTo(c.x, c.y)
      ctx.stroke()
      glowDot(ctx, c.x, c.y, 5 + c.lw * 2, c.color, c.alpha * 0.6) // 车灯头
    }
    meteorStep(dt, ctx, w, h)
  })
  return stop
}

/** 霓虹雨（cyberpunk）：青粉雨丝缓落微斜（低密度慢速不频闪），偶发故障闪线横贯全屏。 */
function cyberpunkFx(ctx: CanvasRenderingContext2D): Cleanup {
  const COLORS = ["#22d3ee", "#ff2d78", "#ff2d78", "#fbbf24"]
  type Drop = { x: number; y: number; len: number; speed: number; color: string; alpha: number }
  const spawnDrop = (): Drop => ({
    x: rand(0, window.innerWidth + 80),
    y: rand(-window.innerHeight, window.innerHeight),
    len: rand(12, 26),
    speed: rand(130, 340),
    color: pick(COLORS),
    alpha: rand(0.07, 0.2),
  })
  const drops = Array.from({ length: density(26) }, spawnDrop)
  type Glitch = { y: number; h: number; color: string; alpha: number; life: number; age: number }
  const glitches: Glitch[] = []
  let nextGlitch = rand(7, 16)
  const stop = runLoop((dt) => {
    const w = window.innerWidth
    const h = window.innerHeight
    ctx.clearRect(0, 0, w, h)
    ctx.lineWidth = 1.2
    ctx.lineCap = "round"
    for (const d of drops as Drop[]) {
      d.y += d.speed * dt
      d.x -= d.speed * 0.12 * dt
      if (d.y - d.len > h) Object.assign(d, spawnDrop(), { y: rand(-60, -10) })
      ctx.strokeStyle = hexA(d.color, d.alpha)
      ctx.beginPath()
      ctx.moveTo(d.x, d.y)
      ctx.lineTo(d.x + d.len * 0.12, d.y - d.len)
      ctx.stroke()
    }
    nextGlitch -= dt
    if (nextGlitch <= 0) {
      nextGlitch = rand(7, 16)
      glitches.push({
        y: rand(0, h),
        h: pick([1, 2, 3]),
        color: pick(["#22d3ee", "#ff2d78"]),
        alpha: rand(0.05, 0.14),
        life: rand(0.08, 0.2),
        age: 0,
      })
    }
    for (let i = glitches.length - 1; i >= 0; i--) {
      const g = glitches[i]
      g.age += dt
      if (g.age >= g.life) {
        glitches.splice(i, 1)
        continue
      }
      ctx.fillStyle = hexA(g.color, g.alpha * (1 - g.age / g.life))
      ctx.fillRect(0, g.y, w, g.h)
    }
  })
  return stop
}

/** 地平线光柱（synthwave）：自底部网格升起的律动光柱群（合成器等律动感），配流星划过上半天幕。 */
function synthwaveFx(ctx: CanvasRenderingContext2D): Cleanup {
  const COLORS = ["#ff6ec7", "#c084fc", "#22d3ee", "#ffd6f3", "#ff9e64"]
  const beams = Array.from({ length: density(26) }, () => ({
    x: rand(0, window.innerWidth),
    w: rand(2, 4.5),
    maxH: rand(110, 320),
    phase: rand(0, Math.PI * 2),
    speed: rand(0.5, 1.3),
    color: pick(COLORS),
  }))
  const meteorStep = meteors(["#ff6ec7", "#ffd6f3", "#c7b8ff"], 4, 10)
  const stop = runLoop((dt) => {
    const w = window.innerWidth
    const h = window.innerHeight
    ctx.clearRect(0, 0, w, h)
    ctx.globalCompositeOperation = "lighter"
    for (const b of beams) {
      b.phase += b.speed * dt
      const k = Math.max(0, Math.sin(b.phase)) ** 2 // 等律动脉冲：柱高与亮度同拍
      if (k <= 0.01) continue
      const bh = b.maxH * k
      const a = 0.08 + 0.3 * k
      const y0 = h - bh
      // 光柱主体（底亮顶隐）+ 两侧柔光晕
      let g = ctx.createLinearGradient(0, h, 0, y0)
      g.addColorStop(0, hexA(b.color, a))
      g.addColorStop(1, hexA(b.color, 0))
      ctx.fillStyle = g
      ctx.fillRect(b.x - b.w / 2, y0, b.w, bh)
      g = ctx.createLinearGradient(0, h, 0, y0)
      g.addColorStop(0, hexA(b.color, a * 0.3))
      g.addColorStop(1, hexA(b.color, 0))
      ctx.fillStyle = g
      ctx.fillRect(b.x - b.w * 2, y0, b.w * 4, bh)
    }
    meteorStep(dt, ctx, w, h)
    ctx.globalCompositeOperation = "source-over"
  })
  return stop
}

/** 棱镜流光（aether）：光透过玻璃的色散光域——大尺寸彩色光斑旋转流动变形，加法混光叠出彩光。 */
function aetherFx(ctx: CanvasRenderingContext2D): Cleanup {
  const COLORS = ["#22d3ee", "#a78bfa", "#f472b6", "#60a5fa", "#5eead4"]
  const shards = Array.from({ length: 9 }, () => ({
    x: rand(0, window.innerWidth),
    y: rand(0, window.innerHeight),
    rx: rand(130, 300),
    ry: 0, // rx * rate 下方计算
    rate: rand(0.45, 0.85),
    rot: rand(0, Math.PI * 2),
    vr: rand(0.04, 0.16) * (Math.random() < 0.5 ? -1 : 1),
    vx: rand(-9, 9),
    vy: rand(-6, 6),
    color: pick(COLORS),
    alpha: rand(0.028, 0.06),
    phase: rand(0, Math.PI * 2),
    pv: rand(0.15, 0.4),
  }))
  const stop = runLoop((dt) => {
    const w = window.innerWidth
    const h = window.innerHeight
    ctx.clearRect(0, 0, w, h)
    ctx.globalCompositeOperation = "lighter"
    for (const s of shards) {
      s.x += s.vx * dt
      s.y += s.vy * dt
      s.rot += s.vr * dt
      s.phase += s.pv * dt
      const rx = s.rx
      const ry = s.rx * s.rate
      if (s.x < -rx) s.x = w + rx
      if (s.x > w + rx) s.x = -rx
      if (s.y < -ry) s.y = h + ry
      if (s.y > h + ry) s.y = -ry
      const a = s.alpha * (0.7 + 0.3 * Math.sin(s.phase))
      const g = ctx.createRadialGradient(0, 0, 0, 0, 0, 1)
      g.addColorStop(0, hexA(s.color, a))
      g.addColorStop(1, hexA(s.color, 0))
      ctx.save()
      ctx.translate(s.x, s.y)
      ctx.rotate(s.rot)
      ctx.scale(rx, ry)
      ctx.fillStyle = g
      ctx.beginPath()
      ctx.arc(0, 0, 1, 0, Math.PI * 2)
      ctx.fill()
      ctx.restore()
    }
    ctx.globalCompositeOperation = "source-over"
  })
  return stop
}

/** 极光帷幕（aurora）：数道垂落光幕——幕沿纵向波动漂移，亮度波沿幕面行进（青绿紫）。 */
function auroraFx(ctx: CanvasRenderingContext2D): Cleanup {
  const COLORS = ["#2dd4bf", "#8b5cf6", "#34d399", "#c084fc"]
  const curtains = Array.from({ length: 3 }, (_, i) => ({
    baseX: window.innerWidth * (0.2 + i * 0.3) + rand(-60, 60),
    vx: rand(6, 16) * (Math.random() < 0.5 ? -1 : 1),
    amp: rand(60, 140),
    kx: rand(0.003, 0.006),
    waveLen: rand(180, 320), // 亮度波纵向波长
    waveSpeed: rand(0.4, 0.9),
    color: COLORS[i % COLORS.length],
    alphaBase: rand(0.1, 0.18),
    phase: rand(0, Math.PI * 2),
  }))
  const stop = runLoop((dt, t) => {
    const w = window.innerWidth
    const h = window.innerHeight
    ctx.clearRect(0, 0, w, h)
    ctx.globalCompositeOperation = "lighter"
    ctx.lineCap = "round"
    for (const c of curtains) {
      c.baseX += c.vx * dt
      if (c.baseX > w + c.amp + 30) c.baseX = -c.amp - 30
      if (c.baseX < -c.amp - 30) c.baseX = w + c.amp + 30
      for (let y = -16; y < h + 16; y += 14) {
        const x = c.baseX + Math.sin(y * c.kx + t * 0.25 + c.phase) * c.amp
        const wave = 0.5 + 0.5 * Math.sin((y / c.waveLen) * Math.PI * 2 - t * c.waveSpeed + c.phase * 3)
        const edge = Math.min(1, Math.min((y + 16) / 90, (h + 16 - y) / 90)) // 上下渐隐
        const a = c.alphaBase * (0.25 + 0.75 * wave) * edge
        if (a <= 0.008) continue
        // 宽柔晕 + 细亮芯两笔构成一节光幕
        ctx.strokeStyle = hexA(c.color, a * 0.35)
        ctx.lineWidth = 8
        ctx.beginPath()
        ctx.moveTo(x, y - 8)
        ctx.lineTo(x, y + 8)
        ctx.stroke()
        ctx.strokeStyle = hexA(c.color, a)
        ctx.lineWidth = 2.5
        ctx.beginPath()
        ctx.moveTo(x, y - 8)
        ctx.lineTo(x, y + 8)
        ctx.stroke()
      }
    }
    ctx.globalCompositeOperation = "source-over"
  })
  return stop
}

/** 墨韵（ink）：宣纸上墨雾缓漂 + 墨滴晕开（小点渐显后涟漪状扩散消散），亮色画布用暗墨低透明度。 */
function inkFx(ctx: CanvasRenderingContext2D): Cleanup {
  const INK = "#1f1c18"
  const mists = Array.from({ length: 7 }, () => ({
    x: rand(0, window.innerWidth),
    y: rand(0, window.innerHeight),
    r: rand(90, 220),
    vx: rand(-6, 6),
    vy: rand(-4, 4),
    alpha: rand(0.018, 0.042),
    phase: rand(0, Math.PI * 2),
    pv: rand(0.15, 0.4),
  }))
  type Drop = { x: number; y: number; t: number; dropDur: number; bloomVr: number; alpha0: number }
  const drops: Drop[] = []
  let nextDrop = rand(1.5, 3.5)
  const stop = runLoop((dt) => {
    const w = window.innerWidth
    const h = window.innerHeight
    ctx.clearRect(0, 0, w, h)
    for (const m of mists) {
      m.x += m.vx * dt
      m.y += m.vy * dt
      m.phase += m.pv * dt
      if (m.x < -m.r) m.x = w + m.r
      if (m.x > w + m.r) m.x = -m.r
      if (m.y < -m.r) m.y = h + m.r
      if (m.y > h + m.r) m.y = -m.r
      glowDot(ctx, m.x, m.y, m.r, INK, m.alpha * (0.7 + 0.3 * Math.sin(m.phase)))
    }
    nextDrop -= dt
    if (nextDrop <= 0) {
      nextDrop = rand(2.2, 5.5)
      drops.push({
        x: rand(w * 0.08, w * 0.92),
        y: rand(h * 0.08, h * 0.92),
        t: 0,
        dropDur: rand(0.4, 0.7),
        bloomVr: rand(26, 42),
        alpha0: rand(0.16, 0.26),
      })
    }
    ctx.lineWidth = 1.4
    for (let i = drops.length - 1; i >= 0; i--) {
      const d = drops[i]
      d.t += dt
      if (d.t < d.dropDur) {
        // 落墨：小点渐显
        ctx.fillStyle = hexA(INK, (d.t / d.dropDur) * 0.5 * d.alpha0 * 2)
        ctx.beginPath()
        ctx.arc(d.x, d.y, 2.2, 0, Math.PI * 2)
        ctx.fill()
        continue
      }
      const k = d.t - d.dropDur
      const a = d.alpha0 * Math.max(0, 1 - k / 2.2)
      if (a <= 0.004) {
        drops.splice(i, 1)
        continue
      }
      // 晕开：扩散墨环 + 内晕
      const r = 3 + k * d.bloomVr
      glowDot(ctx, d.x, d.y, r * 1.15, INK, a * 0.45)
      ctx.strokeStyle = hexA(INK, a)
      ctx.beginPath()
      ctx.arc(d.x, d.y, r, 0, Math.PI * 2)
      ctx.stroke()
    }
  })
  return stop
}

/* ---------------- 画布与启停引擎 ---------------- */

const EFFECTS: Partial<Record<ThemeId, Effect>> = {
  matrix: matrixFx,
  "tokyo-night": tokyoNightFx,
  cyberpunk: cyberpunkFx,
  synthwave: synthwaveFx,
  aether: aetherFx,
  aurora: auroraFx,
  ink: inkFx,
}

let canvas: HTMLCanvasElement | null = null
let ctx: CanvasRenderingContext2D | null = null
let activeTheme: string | null = null
let cleanup: Cleanup | null = null
let resizeBound = false
let inited = false

/** 特效是否停用（低性能模式）。 */
function fxDisabled(): boolean {
  return isLowPower()
}

function resizeCanvas(): void {
  if (!canvas) return
  const dpr = Math.min(window.devicePixelRatio || 1, 2)
  canvas.width = Math.round(window.innerWidth * dpr)
  canvas.height = Math.round(window.innerHeight * dpr)
  canvas.style.width = `${window.innerWidth}px`
  canvas.style.height = `${window.innerHeight}px`
  ctx?.setTransform(dpr, 0, 0, dpr, 0, 0)
}

/** 懒创建全屏画布（定位样式内联：不依赖主题 CSS 是否已加载）。 */
function ensureCanvas(): void {
  if (canvas?.isConnected) return
  canvas = document.createElement("canvas")
  canvas.id = "gb-theme-fx"
  canvas.style.cssText = "position:fixed;inset:0;z-index:-1;pointer-events:none;"
  document.body.appendChild(canvas)
  ctx = canvas.getContext("2d")
  if (!resizeBound) {
    resizeBound = true
    window.addEventListener("resize", resizeCanvas, { passive: true })
  }
  resizeCanvas()
}

function teardown(): void {
  cleanup?.()
  cleanup = null
  activeTheme = null
  if (canvas) {
    canvas.remove()
    canvas = null
    ctx = null
  }
  if (resizeBound) {
    window.removeEventListener("resize", resizeCanvas)
    resizeBound = false
  }
}

function sync(): void {
  const id = document.documentElement.dataset.theme ?? ""
  const effect = EFFECTS[id as ThemeId]
  if (!effect || fxDisabled()) {
    teardown()
    return
  }
  if (id === activeTheme) return
  teardown()
  ensureCanvas()
  if (!ctx) return
  activeTheme = id
  cleanup = effect(ctx)
}

/** 初始化（main.ts 一次调用）：随主题切换与低功耗开关启停。 */
export function initThemeFx(): void {
  if (inited) return
  inited = true
  document.addEventListener("gebai:theme-change", sync)
  document.addEventListener("gebai:low-power-change", sync)
  sync()
}
