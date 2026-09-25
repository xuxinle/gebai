/**
 * 输出速率（tok/s）显示：任务运行期间在标题栏（单轮计时器右侧）显示模型输出速度，便于观测模型服务性能。
 * - 速率的唯一数据源是服务端 `event.session.tps`：一次模型调用内按节流周期推送生成中的估算帧，
 *   调用结束时推送收尾帧（接口 usage 真值时为实测口径）；前端只展示，不自算速率
 * - 常驻只显示数值本身（单位、口径与路况级名在 hover 提示里给出）；估算帧整体降不透明度
 * - **仪表常驻**：两帧之间（工具执行 / 审批等待 / 思考停顿）保持上一帧的值与配色，不闪没；
 *   速率按会话记账，随会话视图切换显示对应会话的值，无该会话速率帧时隐藏
 * - 默认开启；设置面板「外观」tab 可关闭（根元素 data-token-rate="off"，CSS 隐藏全部速率元素）
 * - localStorage `gebai.ui.tokenRate` 仅存 "off"（关闭）；不存/其它值 = 开启（默认）
 * - 跨标签页 storage 事件同步
 */

import { getCurrentSession, tokenRateEl } from "./state"
import { formatTps, rateLevel, type RateBand, type TokenRatePayload } from "./token-rate-core"

export type TokenRateSetting = "on" | "off"

const KEY = "gebai.ui.tokenRate"

/** 用户设置（默认 on）。 */
export function getTokenRateSetting(): TokenRateSetting {
  try {
    if (localStorage.getItem(KEY) === "off") return "off"
  } catch {
    /* 隐私模式等场景忽略 */
  }
  return "on"
}

export function isTokenRateEnabled(): boolean {
  return getTokenRateSetting() === "on"
}

/** 应用设置：off 时在根元素标记 data-token-rate（CSS 据此隐藏速率元素）；开启时立即补渲染一帧。 */
export function applyTokenRate(): void {
  const el = document.documentElement
  if (getTokenRateSetting() === "off") {
    el.dataset.tokenRate = "off"
    tokenRateEl.hidden = true
  } else {
    delete el.dataset.tokenRate
    renderTokenRate()
  }
}

/** 手动设置（设置面板「外观」）：on=开启（默认，清存储不留冗余）；off=关闭。持久化 + 立即生效。 */
export function setTokenRateSetting(v: TokenRateSetting): void {
  try {
    if (v === "off") localStorage.setItem(KEY, v)
    else localStorage.removeItem(KEY)
  } catch {
    /* ignore */
  }
  applyTokenRate()
}

/** 各会话最近一次速率帧（即展示值本身：帧之间不刷新，仪表保持它）。 */
const frames = new Map<string, TokenRatePayload>()

/** 服务端速率帧（`event.session.tps`）：生成中周期推送 + 调用结束收尾帧。 */
export function noteTpsFrame(sessionId: string, payload: TokenRatePayload): void {
  if (!isTokenRateEnabled()) return
  frames.set(sessionId, payload)
  renderTokenRate()
}

/** 提示文案（两行固定结构：速率行 = 值 + 单位 + 路况级名 + 口径；依据行 = 输出 tokens 与生成窗口）。 */
function tipText(p: TokenRatePayload, band: RateBand): string {
  const caliber = p.est ? "估算" : "实测"
  return `${formatTps(p.tps)} tok/s · ${band.label}（${caliber}）\n输出 ${p.outTokens} tokens / ${(p.genMs / 1000).toFixed(1)}s`
}

/** 刷新速率显示（唯一渲染入口）：只渲染当前会话的速率帧，无该会话帧时隐藏。 */
export function renderTokenRate(): void {
  if (!isTokenRateEnabled()) return // CSS data-token-rate="off" 隐藏全部速率元素，无需渲染
  const cur = getCurrentSession()
  const p = cur ? frames.get(cur.id) : undefined
  if (!p) {
    tokenRateEl.hidden = true
    return
  }
  tokenRateEl.hidden = false
  const band = rateLevel(p.tps) // 交通拥挤配色分级：绿（畅通）→黄（缓行）→红（严重拥堵）
  tokenRateEl.dataset.rate = String(band.level)
  if (p.est) tokenRateEl.dataset.est = "1"
  else delete tokenRateEl.dataset.est
  const text = tokenRateEl.querySelector<HTMLElement>(".tr-text")
  // 常驻只显示数值本身（单位与口径标记都不上屏，避免标题栏噪声）：hover 提示里给出
  if (text) text.textContent = formatTps(p.tps)
  tokenRateEl.dataset.tip = tipText(p, band)
}

/** 初始化：应用当前设置，会话视图切换随动，并跨标签页同步（其他标签修改设置后本标签即时生效）。 */
export function initTokenRate(): void {
  applyTokenRate()
  document.addEventListener("gebai:session-view", renderTokenRate)
  window.addEventListener("storage", (e) => {
    if (e.key !== KEY) return
    applyTokenRate()
  })
}
