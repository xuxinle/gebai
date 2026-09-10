/**
 * 模板匹配纯函数（core/cv）：在图像中定位与模板同尺寸的子图位置（零训练，
 * 补「OCR 找文字、YOLO 需自训模型」之间的图标/控件定位空白）。
 * 归一化互相关（NCC）+ 粗扫（box 降采样）+ 全分辨率精化两阶段：
 * 窗口均值/方差经「和 + 平方和」两张积分图 O(1) 求取，分子对零均值模板直接累加。
 * 只做同尺寸匹配（同一显示环境截图裁剪的模板），不做缩放不变。
 */
import type { RgbaImage } from "./image"

export interface TemplateMatch {
  /** NCC 相似度 [-1, 1]。 */
  score: number
  /** 匹配位置左上角（输入图像像素系）。 */
  x: number
  y: number
  w: number
  h: number
}

interface Gray {
  data: Float32Array
  w: number
  h: number
}

/** 一张积分图（前导零行零列），sum=false 为灰度和、true 为平方和。 */
interface Integrals {
  s1: Float64Array
  s2: Float64Array
  w: number
}

function toGray(img: RgbaImage): Gray {
  const n = img.width * img.height
  const out = new Float32Array(n)
  for (let i = 0; i < n; i++) {
    const o = i * 4
    out[i] = 0.299 * img.data[o] + 0.587 * img.data[o + 1] + 0.114 * img.data[o + 2]
  }
  return { data: out, w: img.width, h: img.height }
}

/** box 降采样（s×s 块均值，边缘块按实际像素数平均）；(ox,oy) 为采样原点相位偏移。 */
function downsample(g: Gray, s: number, ox = 0, oy = 0): Gray {
  if (s <= 1) return g
  const w = Math.ceil((g.w - ox) / s)
  const h = Math.ceil((g.h - oy) / s)
  const out = new Float32Array(Math.max(0, w) * Math.max(0, h))
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      let sum = 0
      let n = 0
      for (let dy = 0; dy < s && oy + y * s + dy < g.h; dy++) {
        for (let dx = 0; dx < s && ox + x * s + dx < g.w; dx++) {
          sum += g.data[(oy + y * s + dy) * g.w + ox + x * s + dx]
          n++
        }
      }
      out[y * w + x] = sum / n
    }
  }
  return { data: out, w, h }
}

/** 零均值模板（NCC 分子只需 Σ g·t̂，模板均值在分子中消去）。 */
interface Tpl {
  data: Float32Array
  w: number
  h: number
  /** √(Σ t̂²)，NCC 分母的模板侧。 */
  norm: number
}

function zeroMeanTpl(g: Gray): Tpl {
  const n = g.w * g.h
  let mean = 0
  for (let i = 0; i < n; i++) mean += g.data[i]
  mean /= n
  const data = new Float32Array(n)
  let sq = 0
  for (let i = 0; i < n; i++) {
    const v = g.data[i] - mean
    data[i] = v
    sq += v * v
  }
  return { data, w: g.w, h: g.h, norm: Math.sqrt(sq) }
}

function integrals(g: Gray): Integrals {
  const w = g.w + 1
  const s1 = new Float64Array(w * (g.h + 1))
  const s2 = new Float64Array(w * (g.h + 1))
  for (let y = 0; y < g.h; y++) {
    let row1 = 0
    let row2 = 0
    for (let x = 0; x < g.w; x++) {
      const v = g.data[y * g.w + x]
      row1 += v
      row2 += v * v
      s1[(y + 1) * w + x + 1] = s1[y * w + x + 1] + row1
      s2[(y + 1) * w + x + 1] = s2[y * w + x + 1] + row2
    }
  }
  return { s1, s2, w }
}

/** (x,y) 处模板窗口的 NCC；窗口无方差（纯色区）/ 分母过小返回 0。 */
function ncc(search: Gray, it: Integrals, tpl: Tpl, x: number, y: number): number {
  const sw = search.w
  let dot = 0
  for (let ty = 0; ty < tpl.h; ty++) {
    const sRow = (y + ty) * sw + x
    const tRow = ty * tpl.w
    for (let tx = 0; tx < tpl.w; tx++) dot += search.data[sRow + tx] * tpl.data[tRow + tx]
  }
  const iw = it.w
  const n = tpl.w * tpl.h
  const x2 = x + tpl.w
  const y2 = y + tpl.h
  const w1 = it.s1[y2 * iw + x2] - it.s1[y * iw + x2] - it.s1[y2 * iw + x] + it.s1[y * iw + x]
  const q2 = it.s2[y2 * iw + x2] - it.s2[y * iw + x2] - it.s2[y2 * iw + x] + it.s2[y * iw + x]
  const mean = w1 / n
  const varSum = q2 - n * mean * mean
  const denom = Math.sqrt(Math.max(varSum, 0)) * tpl.norm
  return denom > 1e-6 ? dot / denom : 0
}

/**
 * 模板匹配：返回按相似度降序的匹配（非极大值抑制后至多 5 个，score ≥ threshold）。
 * 模板大于搜索图 / 模板纯色（无结构可匹配）时抛中文错误。
 */
export function matchTemplate(
  search: RgbaImage,
  template: RgbaImage,
  opts?: { threshold?: number },
): TemplateMatch[] {
  const threshold = opts?.threshold ?? 0.8
  if (template.width > search.width || template.height > search.height) {
    throw new Error(`模板（${template.width}x${template.height}）大于搜索图（${search.width}x${search.height}），无法匹配`)
  }
  const gSearch = toGray(search)
  const gTpl = toGray(template)
  const tplFull = zeroMeanTpl(gTpl)
  if (tplFull.norm < 1e-6) throw new Error("模板图像为纯色（无结构），无法匹配——请换一个含内容的模板区域")
  // 粗扫降采样倍率：模板短边缩到 ~12px（最小 1——小模板直接全分辨率扫）。
  // 相位偏移粗扫：目标未必落在粗网格对齐点上（高频内容错位即去相关），
  // 按 origin {0, s/2}² 多相位分别降采样扫描，残余错位 ≤1 物理像素。
  const scale = Math.max(1, Math.floor(Math.min(template.width, template.height) / 12))
  const cTpl = zeroMeanTpl(downsample(gTpl, scale))
  const phaseStep = Math.max(1, scale >> 1)
  // 粗扫门限低于最终阈值（粗扫只负责把精化窗口引到目标附近，误报由全分辨率精化剔除）
  const cands: Array<{ score: number; x: number; y: number }> = []
  for (let oy = 0; oy < Math.max(1, scale); oy += phaseStep) {
    for (let ox = 0; ox < Math.max(1, scale); ox += phaseStep) {
      const cSearch = downsample(gSearch, scale, ox, oy)
      if (cSearch.w < cTpl.w || cSearch.h < cTpl.h) continue
      const cInteg = integrals(cSearch)
      for (let y = 0; y + cTpl.h <= cSearch.h; y++) {
        for (let x = 0; x + cTpl.w <= cSearch.w; x++) {
          const s = ncc(cSearch, cInteg, cTpl, x, y)
          if (s > 0.4) cands.push({ score: s, x: ox + x * scale, y: oy + y * scale })
        }
      }
    }
  }
  cands.sort((a, b) => b.score - a.score)
  // 全分辨率精化：在各粗候选邻域 ±(scale+2) 内逐点 NCC
  const fullInteg = integrals(gSearch)
  const radius = scale + 2
  const refined: TemplateMatch[] = []
  for (const c of cands.slice(0, 16)) {
    const x0 = Math.max(0, c.x - radius)
    const y0 = Math.max(0, c.y - radius)
    const x1 = Math.min(gSearch.w - tplFull.w, c.x + radius)
    const y1 = Math.min(gSearch.h - tplFull.h, c.y + radius)
    let best: TemplateMatch | null = null
    for (let y = y0; y <= y1; y++) {
      for (let x = x0; x <= x1; x++) {
        const s = ncc(gSearch, fullInteg, tplFull, x, y)
        if (!best || s > best.score) best = { score: s, x, y, w: tplFull.w, h: tplFull.h }
      }
    }
    if (best) refined.push(best)
  }
  refined.sort((a, b) => b.score - a.score)
  // 非极大值抑制：与已保留匹配中心距小于模板半宽/半高的丢弃
  const kept: TemplateMatch[] = []
  for (const m of refined) {
    if (m.score < threshold) break
    if (kept.some((k) => Math.abs(k.x - m.x) < m.w / 2 && Math.abs(k.y - m.y) < m.h / 2)) continue
    kept.push(m)
    if (kept.length >= 5) break
  }
  return kept
}
