/**
 * 爆金币特效：全屏 canvas 粒子引擎（金币 + 各面额纸币约各半），人民币主题招财猫专属。
 * - 金币：3D 翻转（椭圆面压缩 + 近侧视时的边缘厚度）、鎏金径向渐变、内环 + ¥ 浮雕、落地反弹两次后静止消隐。
 * - 纸币：第五套人民币六面额配色（100 红 / 50 绿 / 20 棕 / 10 蓝黑 / 5 紫 / 1 橄榄绿），飘落摇摆
 *   （终端速度 + 正弦横摆 + 绕横轴翻面的纵向压缩），版面细节（内框双线/水印圆/人像剪影/面额与角标数字）。
 * - 拖动轨迹（popTrailMoney）：小尺寸金币/纸币**漂浮**消散——负净加速度缓升 + 强空气阻力 + 轻摆慢转，
 *   不与地面交互（区别于爆发的坠落反弹物理）。
 * - 性能：金币翻转帧与各面额纸币均**预渲染精灵**（离屏 canvas 2x 超采样），逐帧只做 transform + drawImage；
 *   canvas 懒创建、定位样式内联（不依赖主题 CSS 加载时序），粒子耗尽自动移除；rAF 仅在有粒子时运行。
 * - 降级：低性能模式 / prefers-reduced-motion 不发射（装饰性动画，遵循全局低功耗约定）。
 */
import { CNY_SCHEMES } from "./theme"
import { isLowPower } from "./low-power"

interface Coin {
  kind: "coin"
  x: number
  y: number
  vx: number
  vy: number
  r: number
  phase: number
  phaseV: number
  tilt: number
  tiltV: number
  bounces: number
  age: number
  life: number
  /** 垂直净加速度覆盖（px/s²，负值=漂浮缓升；缺省用标准重力）。 */
  ay?: number
  /** 空气阻力系数（每秒速度衰减比例，漂浮粒子用；缺省无额外阻力）。 */
  drag?: number
  /** 漂浮粒子：不与地面交互（无反弹/无贴地，升空渐隐消散）。 */
  noFloor?: boolean
}

interface Note {
  kind: "note"
  x: number
  y: number
  vx: number
  vy: number
  w: number
  rot: number
  rotV: number
  flip: number
  flipV: number
  sway: number
  swayV: number
  scheme: number
  age: number
  life: number
  /** 垂直净加速度覆盖（px/s²，负值=漂浮缓升；缺省用标准重力+终端速度）。 */
  ay?: number
  /** 空气阻力系数（每秒速度衰减比例，漂浮粒子用；缺省无额外阻力）。 */
  drag?: number
  /** 漂浮粒子：不与地面交互（无贴地减速，升空渐隐消散）。 */
  noFloor?: boolean
}

type Particle = Coin | Note

/** 粒子上限（超出丢弃最旧，防长爆发堆积拖慢帧率）。 */
const MAX_PARTICLES = 420
/** 金币/纸币重力（px/s²；纸币轻、空气阻力大，重力小且有终端速度）。 */
const G_COIN = 2600
const G_NOTE = 420
/** 纸币垂直终端速度（px/s）。 */
const NOTE_TERMINAL_VY = 300
/** 金币翻转预渲染帧数（一整圈 2π 按 32 相位采样，近侧视帧含边缘厚度）。 */
const COIN_FRAMES = 32

/* ---------------- 精灵预渲染（离屏 canvas，2x 超采样） ---------------- */

/** hex 颜色明暗调整（f>0 向白靠拢，f<0 向黑靠拢）。 */
function shade(hex: string, f: number): string {
  const m = hex.replace("#", "")
  const ch = (i: number): number => {
    const v = parseInt(m.slice(i * 2, i * 2 + 2), 16)
    return Math.round(f > 0 ? v + (255 - v) * f : v * (1 + f))
  }
  return `rgb(${ch(0)},${ch(1)},${ch(2)})`
}

/** 圆角矩形路径（兼容无 ctx.roundRect 环境）。 */
function rrPath(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number): void {
  ctx.beginPath()
  ctx.moveTo(x + r, y)
  ctx.arcTo(x + w, y, x + w, y + h, r)
  ctx.arcTo(x + w, y + h, x, y + h, r)
  ctx.arcTo(x, y + h, x, y, r)
  ctx.arcTo(x, y, x + w, y, r)
  ctx.closePath()
}

/** 单帧金币：相位 φ（0=正脸），近侧视（|cos φ| 小）画边缘厚度。绘制在 2x 尺寸离屏画布，尺寸 r。 */
function renderCoinFrame(r: number, phi: number): HTMLCanvasElement {
  const S = 2 // 超采样
  const cv = document.createElement("canvas")
  const size = Math.ceil(r * 2) + 4
  cv.width = size * S
  cv.height = size * S
  const ctx = cv.getContext("2d")!
  ctx.scale(S, S)
  ctx.translate(size / 2, size / 2)
  const face = Math.abs(Math.cos(phi))
  if (face < 0.1) {
    // 侧视边缘：窄圆角竖条 + 纵向渐变（两侧暗中间亮，模拟圆柱高光）
    const t = Math.max(1.6, r * 0.22)
    const g = ctx.createLinearGradient(-t, 0, t, 0)
    g.addColorStop(0, "#8a5c10")
    g.addColorStop(0.5, "#e9c25e")
    g.addColorStop(1, "#8a5c10")
    ctx.fillStyle = g
    rrPath(ctx, -t, -r, t * 2, r * 2, t)
    ctx.fill()
  } else {
    const rx = r * face
    // 主体：鎏金径向渐变（左上高光）
    const g = ctx.createRadialGradient(-rx * 0.35, -r * 0.35, r * 0.1, 0, 0, r * 1.05)
    g.addColorStop(0, "#fff3bd")
    g.addColorStop(0.45, "#f6ce55")
    g.addColorStop(1, "#cf9420")
    ctx.fillStyle = g
    ctx.beginPath()
    ctx.ellipse(0, 0, rx, r, 0, 0, Math.PI * 2)
    ctx.fill()
    ctx.lineWidth = 1.1
    ctx.strokeStyle = "#9a6a12"
    ctx.stroke()
    // 内环（浮雕币缘）
    ctx.beginPath()
    ctx.ellipse(0, 0, rx * 0.74, r * 0.74, 0, 0, Math.PI * 2)
    ctx.lineWidth = 1
    ctx.strokeStyle = "rgba(150,98,15,0.55)"
    ctx.stroke()
    // ¥ 面额（正脸可见时）
    if (face > 0.5 && r >= 8) {
      ctx.fillStyle = "rgba(146,94,12,0.9)"
      ctx.font = `700 ${Math.round(r * 0.8)}px "Segoe UI", system-ui, sans-serif`
      ctx.textAlign = "center"
      ctx.textBaseline = "middle"
      ctx.fillText("¥", 0, r * 0.06)
    }
    // 左上高光弧
    ctx.beginPath()
    ctx.ellipse(-rx * 0.34, -r * 0.44, rx * 0.26, r * 0.16, -0.5, 0, Math.PI * 2)
    ctx.fillStyle = "rgba(255,255,255,0.45)"
    ctx.fill()
  }
  return cv
}

/** 金币翻转帧精灵：半径（整数 px）→ 32 帧画布。 */
const coinSprites = new Map<number, HTMLCanvasElement[]>()

function coinFrames(r: number): HTMLCanvasElement[] {
  const key = Math.round(r)
  let frames = coinSprites.get(key)
  if (!frames) {
    frames = []
    for (let i = 0; i < COIN_FRAMES; i++) frames.push(renderCoinFrame(key, (i / COIN_FRAMES) * Math.PI * 2))
    coinSprites.set(key, frames)
  }
  return frames
}

/** 纸币面额版面配色（由 CNY_SCHEMES 面额主色派生亮/暗色）。 */
function noteStyles(): Array<{ denom: string; base: string; dark: string; light: string }> {
  return CNY_SCHEMES.map((s) => ({ denom: s.id, base: s.base, dark: shade(s.base, -0.35), light: shade(s.base, 0.42) }))
}

/** 单张纸币精灵：面额版面（内框双线/水印圆/人像剪影/面额 + 角标数字），2x 超采样。 */
function renderNoteSprite(w: number, scheme: { denom: string; base: string; dark: string; light: string }): HTMLCanvasElement {
  const S = 2
  const h = Math.round(w * 0.45)
  const cv = document.createElement("canvas")
  cv.width = Math.ceil((w + 4) * S)
  cv.height = Math.ceil((h + 4) * S)
  const ctx = cv.getContext("2d")!
  ctx.scale(S, S)
  const ox = 2
  const oy = 2
  ctx.translate(ox, oy)
  // 主体：上亮下主色的纵向渐变 + 深色外框
  const g = ctx.createLinearGradient(0, 0, 0, h)
  g.addColorStop(0, scheme.light)
  g.addColorStop(0.5, scheme.base)
  g.addColorStop(1, scheme.dark)
  ctx.fillStyle = g
  rrPath(ctx, 0, 0, w, h, 4)
  ctx.fill()
  ctx.lineWidth = 1
  ctx.strokeStyle = scheme.dark
  ctx.stroke()
  // 内框双线（底纹暗线 + 亮线）
  ctx.strokeStyle = "rgba(255,252,240,0.55)"
  rrPath(ctx, 3, 3, w - 6, h - 6, 3)
  ctx.stroke()
  ctx.strokeStyle = "rgba(0,0,0,0.18)"
  rrPath(ctx, 4.5, 4.5, w - 9, h - 9, 2.5)
  ctx.stroke()
  // 左水印圆 + 人像剪影
  ctx.beginPath()
  ctx.arc(w * 0.24, h / 2, h * 0.31, 0, Math.PI * 2)
  ctx.fillStyle = "rgba(255,252,240,0.4)"
  ctx.fill()
  ctx.strokeStyle = "rgba(255,252,240,0.6)"
  ctx.stroke()
  ctx.beginPath()
  ctx.arc(w * 0.24, h * 0.46, h * 0.13, 0, Math.PI * 2)
  ctx.fillStyle = "rgba(0,0,0,0.35)"
  ctx.fill()
  // 面额主数字（右侧）
  ctx.fillStyle = "rgba(255,252,240,0.95)"
  ctx.font = `700 ${Math.round(h * 0.34)}px "Segoe UI", system-ui, sans-serif`
  ctx.textAlign = "center"
  ctx.textBaseline = "middle"
  ctx.fillText(`¥${scheme.denom}`, w * 0.62, h * 0.5)
  // 角标数字
  ctx.font = `700 ${Math.round(h * 0.2)}px "Segoe UI", system-ui, sans-serif`
  ctx.textAlign = "left"
  ctx.fillText(scheme.denom, 6, h * 0.16)
  ctx.textAlign = "right"
  ctx.fillText(scheme.denom, w - 6, h * 0.84)
  return cv
}

/** 纸币精灵：宽度 → 各面额画布。 */
const noteSprites = new Map<number, HTMLCanvasElement[]>()
let noteStyleList: Array<{ denom: string; base: string; dark: string; light: string }> | null = null

function noteFrame(w: number, scheme: number): HTMLCanvasElement {
  const key = Math.round(w)
  let frames = noteSprites.get(key)
  if (!frames) {
    frames = (noteStyleList ??= noteStyles()).map((s) => renderNoteSprite(key, s))
    noteSprites.set(key, frames)
  }
  return frames[scheme]
}

/* ---------------- 画布与主循环 ---------------- */

let canvas: HTMLCanvasElement | null = null
let ctx: CanvasRenderingContext2D | null = null
let rafId = 0
let lastTs = 0
let particles: Particle[] = []
let resizeBound = false

/** 特效是否可用（低性能模式 / 减少动画偏好下不发射）。 */
export function coinFxDisabled(): boolean {
  return isLowPower() || (typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches)
}

/** 懒创建全屏画布（定位样式内联：不依赖主题 CSS 是否已加载）。 */
function ensureCanvas(): void {
  if (canvas?.isConnected) return
  canvas = document.createElement("canvas")
  canvas.id = "gb-coin-fx"
  // 内联定位（主题 CSS 卸载时序不影响）：固定全屏、不挡交互、位于浮层之上
  canvas.style.cssText = "position:fixed;inset:0;z-index:450;pointer-events:none;"
  document.body.appendChild(canvas)
  ctx = canvas.getContext("2d")
  if (!resizeBound) {
    resizeBound = true
    window.addEventListener("resize", resizeCanvas, { passive: true })
  }
  resizeCanvas()
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

/** 启动主循环（已有粒子时空转调用）。 */
function startLoop(): void {
  if (rafId) return
  lastTs = 0
  rafId = requestAnimationFrame(tick)
}

function tick(ts: number): void {
  if (!ctx || !canvas) return
  const dt = lastTs ? Math.min(0.04, Math.max(0.001, (ts - lastTs) / 1000)) : 0.016
  lastTs = ts
  const W = window.innerWidth
  const H = window.innerHeight
  ctx.clearRect(0, 0, W, H)
  const alive: Particle[] = []
  for (const p of particles) {
    p.age += dt
    updateParticle(p, dt, H)
    const fadeStart = p.life * 0.75
    const alpha = p.age >= p.life ? 0 : p.age > fadeStart ? 1 - (p.age - fadeStart) / (p.life - fadeStart) : 1
    if (p.age < p.life && p.y < H + 120 && p.x > -120 && p.x < W + 120) {
      drawParticle(ctx, p, alpha)
      alive.push(p)
    }
  }
  particles = alive
  if (particles.length) rafId = requestAnimationFrame(tick)
  else {
    rafId = 0
    // 粒子耗尽：移除画布与 resize 监听（下次爆发重建）
    canvas.remove()
    canvas = null
    ctx = null
    window.removeEventListener("resize", resizeCanvas)
    resizeBound = false
  }
}

function updateParticle(p: Particle, dt: number, H: number): void {
  if (p.kind === "coin") {
    p.vy += (p.ay ?? G_COIN) * dt
    if (p.drag) {
      p.vx *= 1 - p.drag * dt
      p.vy *= 1 - p.drag * 0.6 * dt
    }
    p.x += p.vx * dt
    p.y += p.vy * dt
    p.phase += p.phaseV * dt
    p.tilt += p.tiltV * dt
    if (!p.noFloor) {
      const floor = H - p.r - 2
      if (p.y > floor && p.vy > 0) {
        if (p.bounces < 2 && p.vy > 150) {
          p.vy = -p.vy * 0.45
          p.vx *= 0.72
          p.tiltV *= 0.7
          p.phaseV *= 0.75
          p.bounces++
        } else {
          p.vy = 0
          p.vx *= 0.85
          p.tiltV *= 0.85
        }
        p.y = floor
      }
    }
  } else {
    if (p.ay !== undefined) {
      p.vy += p.ay * dt
    } else {
      p.vy = Math.min(p.vy + G_NOTE * dt, NOTE_TERMINAL_VY)
    }
    if (p.drag) {
      p.vx *= 1 - p.drag * dt
      p.vy *= 1 - p.drag * 0.6 * dt
    }
    p.sway += p.swayV * dt
    p.x += (p.vx + Math.sin(p.sway) * 72) * dt
    p.y += p.vy * dt
    p.rot += p.rotV * dt
    p.flip += p.flipV * dt
    if (!p.noFloor) {
      p.vx *= 1 - 0.6 * dt
      p.rotV *= 1 - 0.35 * dt
      if (p.y > H - 14) {
        p.y = H - 14
        p.vy = Math.min(p.vy, 40)
      }
    }
  }
}

function drawParticle(c: CanvasRenderingContext2D, p: Particle, alpha: number): void {
  c.globalAlpha = alpha
  if (p.kind === "coin") {
    const frames = coinFrames(p.r)
    const idx = Math.round(((p.phase / (Math.PI * 2)) % 1 + 1) % 1 * COIN_FRAMES) % COIN_FRAMES
    const spr = frames[idx]
    const size = spr.width / 2 // 精灵为 2x，逻辑尺寸减半
    c.save()
    c.translate(p.x, p.y)
    c.rotate(p.tilt)
    c.drawImage(spr, -size / 2, -size / 2, size, size)
    c.restore()
  } else {
    const spr = noteFrame(p.w, p.scheme)
    const w = spr.width / 2
    const h = spr.height / 2
    c.save()
    c.translate(p.x, p.y)
    c.rotate(p.rot)
    // 绕横轴翻面：纵向压缩
    c.scale(1, 0.3 + 0.7 * Math.abs(Math.cos(p.flip)))
    c.drawImage(spr, -w / 2, -h / 2, w, h)
    c.restore()
  }
  c.globalAlpha = 1
}

/* ---------------- 发射入口 ---------------- */

/** 生成一枚粒子（金币/纸币按 kind），加入池并裁剪上限。 */
function spawn(kind: "coin" | "note", x: number, y: number, angleDeg: number, speed: number): void {
  const rad = (angleDeg * Math.PI) / 180
  if (kind === "coin") {
    const r = 9 + Math.random() * 7
    particles.push({
      kind: "coin",
      x: x + (Math.random() * 28 - 14),
      y: y + (Math.random() * 20 - 10),
      vx: Math.cos(rad) * speed,
      vy: Math.sin(rad) * speed,
      r,
      phase: Math.random() * Math.PI * 2,
      phaseV: (7 + Math.random() * 8) * (Math.random() < 0.5 ? -1 : 1),
      tilt: (Math.random() - 0.5) * 0.8,
      tiltV: (Math.random() - 0.5) * 5,
      bounces: 0,
      age: 0,
      life: 2.4 + Math.random() * 1,
    })
  } else {
    const w = 56 + Math.random() * 16
    particles.push({
      kind: "note",
      x: x + (Math.random() * 30 - 15),
      y: y + (Math.random() * 20 - 10),
      vx: Math.cos(rad) * speed * 0.7,
      vy: Math.sin(rad) * speed,
      w,
      rot: (Math.random() - 0.5) * 1.2,
      rotV: (Math.random() - 0.5) * 4,
      flip: Math.random() * Math.PI * 2,
      flipV: (3 + Math.random() * 5) * (Math.random() < 0.5 ? -1 : 1),
      sway: Math.random() * Math.PI * 2,
      swayV: 2.2 + Math.random() * 2.5,
      scheme: Math.floor(Math.random() * CNY_SCHEMES.length),
      age: 0,
      life: 3.2 + Math.random() * 1.2,
    })
  }
  if (particles.length > MAX_PARTICLES) particles.splice(0, particles.length - MAX_PARTICLES)
}

/**
 * 爆发：从 (x,y) 向上扇形喷出金币与纸币（约各半）。
 * @param count 粒子总数
 * @param opts.speed 初速倍率（连击加成/大爆发用）
 * @param opts.waves 分波发射（大量粒子分 2-4 波更有节奏感，间隔 110ms）
 */
export function burstMoney(x: number, y: number, count: number, opts: { speed?: number; waves?: number } = {}): void {
  if (coinFxDisabled() || count <= 0) return
  const speed = opts.speed ?? 1
  const waves = Math.max(1, Math.min(5, Math.round(opts.waves ?? 1)))
  const per = Math.ceil(count / waves)
  let emitted = 0
  for (let w = 0; w < waves && emitted < count; w++) {
    const n = Math.min(per, count - emitted)
    emitted += n
    const delay = w * 110
    if (delay === 0) spawnWave(x, y, n, speed)
    else window.setTimeout(() => spawnWave(x, y, n, speed), delay)
  }
}

function spawnWave(x: number, y: number, n: number, speed: number): void {
  if (coinFxDisabled()) return
  ensureCanvas()
  for (let i = 0; i < n; i++) {
    // 上方扇形（±78°），纸币初速低（轻、飘）
    const angle = -90 + (Math.random() * 156 - 78)
    const isNote = i % 2 === 1
    const v = (isNote ? 240 + Math.random() * 280 : 460 + Math.random() * 520) * speed
    spawn(isNote ? "note" : "coin", x, y, angle, v)
  }
  startLoop()
}

/** 拖动轨迹粒子：小尺寸金币 + 半数概率小纸币，**漂浮**消散——负净加速度缓升、强空气阻力衰减初速、
 *  轻摆慢转、不与地面交互，升空过程渐隐（区别于爆发的坠落反弹物理）。 */
export function popTrailMoney(x: number, y: number): void {
  if (coinFxDisabled()) return
  ensureCanvas()
  particles.push({
    kind: "coin",
    x,
    y,
    vx: (Math.random() - 0.5) * 90,
    vy: -30 - Math.random() * 80,
    r: 4 + Math.random() * 2.5,
    phase: Math.random() * Math.PI * 2,
    phaseV: (3 + Math.random() * 3) * (Math.random() < 0.5 ? -1 : 1),
    tilt: (Math.random() - 0.5) * 0.6,
    tiltV: (Math.random() - 0.5) * 2,
    bounces: 2,
    age: 0,
    life: 1.4 + Math.random() * 0.6,
    ay: -50 - Math.random() * 60,
    drag: 2.2,
    noFloor: true,
  })
  if (Math.random() < 0.5) {
    particles.push({
      kind: "note",
      x: x + (Math.random() * 16 - 8),
      y: y + (Math.random() * 12 - 6),
      vx: (Math.random() - 0.5) * 110,
      vy: -20 - Math.random() * 70,
      w: 30 + Math.random() * 10,
      rot: (Math.random() - 0.5) * 1,
      rotV: (Math.random() - 0.5) * 2.5,
      flip: Math.random() * Math.PI * 2,
      flipV: (2 + Math.random() * 2.5) * (Math.random() < 0.5 ? -1 : 1),
      sway: Math.random() * Math.PI * 2,
      swayV: 2 + Math.random() * 2,
      scheme: Math.floor(Math.random() * CNY_SCHEMES.length),
      age: 0,
      life: 1.6 + Math.random() * 0.6,
      ay: -40 - Math.random() * 50,
      drag: 1.8,
      noFloor: true,
    })
  }
  if (particles.length > MAX_PARTICLES) particles.splice(0, particles.length - MAX_PARTICLES)
  startLoop()
}
