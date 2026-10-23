/**
 * 拟人模式的飞书授权横幅：**需要授权时出现在页面顶部**，点一下就完成。
 *
 * ## 为什么是"横幅"而不是"弹窗"
 *
 * 弹窗（模态）适合「必须立刻处理、否则继续不了」的事。授权不是这种：
 *
 * - 服务可以在**没有凭据**的状态下正常运行（只是人格不会回消息）
 * - 用户可能正在看别的会话、正在写代码，突然弹个模态打断他
 * - 授权往往发生在「另一台设备/手机飞书上点同意」，用户回来时已经完成了
 *
 * 横幅常驻但不打扰：看得到、不挡路、处理完自动消失。
 *
 * ## 状态来源
 *
 * `GET /api/v1/persona/status` → `{ready, state, message, authorizeUrl, ...}`
 *
 * 三处触发刷新：
 * 1. 页面加载（覆盖「启动时就需要授权」）
 * 2. 收到 `event.persona.credentials`（授权完成/失败后自动更新）
 * 3. 定时轮询（覆盖「另一端完成了授权」——用户可能不在这个页面点的链接）
 *
 * 轮询间隔刻意取长（30s）：授权是低频事件，秒级实时没有价值，白耗请求。
 */

/** 服务端状态（与 `PersonaCredentialStatus` 对齐；前端不依赖后端包类型）。 */
export interface PersonaStatus {
  ready: boolean
  state: "ok" | "missing" | "expired" | "error"
  message: string
  authorizedUser?: string
  expireAt?: number
  remainingMs?: number
  scopes?: string[]
  needsAuth: boolean
  authorizeUrl?: string
}

const POLL_MS = 30_000
const BANNER_ID = "persona-auth-banner"

let pollTimer: ReturnType<typeof setInterval> | null = null
/** 当前是否已挂载横幅（避免重复创建 DOM）。 */
let mounted = false
/** 用户主动收起后本次会话不再打扰（刷新页面会重现——这是刻意的：问题还在就该看到）。 */
let dismissedForSession = false

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch] ?? ch)
}

/** 把横幅插到 header 后面（页面最上方、内容之上）。 */
function ensureMounted(): HTMLElement | null {
  const existing = document.getElementById(BANNER_ID)
  if (existing) {
    mounted = true
    return existing
  }
  const header = document.querySelector("#app > header") ?? document.querySelector("header")
  if (!header || !header.parentElement) return null
  const bar = document.createElement("div")
  bar.id = BANNER_ID
  bar.className = "persona-auth-banner"
  bar.setAttribute("role", "alert")
  // 插在 header 之后：顶部导航仍是最上，横幅紧贴其下
  header.after(bar)
  mounted = true
  return bar
}

function unmount(): void {
  document.getElementById(BANNER_ID)?.remove()
  mounted = false
}

/** 渲染横幅内容。 */
function render(status: PersonaStatus): void {
  if (status.ready) {
    // 已就绪：撤掉横幅（授权完成后用户不该再看到提示）
    unmount()
    dismissedForSession = false
    return
  }
  if (dismissedForSession) return

  const bar = ensureMounted()
  if (!bar) return

  const hasUrl = !!status.authorizeUrl
  const label = status.state === "expired" ? "飞书授权已过期" : "飞书授权未完成"
  bar.innerHTML = `
    <span class="persona-auth-icon" aria-hidden="true">🔑</span>
    <span class="persona-auth-text">
      <strong>${esc(label)}</strong>
      <span class="persona-auth-msg">${esc(status.message || "人格暂时无法对外发消息")}</span>
    </span>
    <span class="persona-auth-actions">
      ${hasUrl ? `<a class="persona-auth-btn primary" href="${esc(status.authorizeUrl!)}" target="_blank" rel="noopener noreferrer">去授权</a>` : `<span class="persona-auth-hint">需先配置飞书应用凭据</span>`}
      <button type="button" class="persona-auth-btn" data-act="manual">已授权，粘贴回调地址</button>
      <button type="button" class="persona-auth-btn ghost" data-act="dismiss" aria-label="本次不再提示">忽略</button>
    </span>
  `

  bar.querySelector<HTMLButtonElement>('[data-act="dismiss"]')?.addEventListener("click", () => {
    dismissedForSession = true
    unmount()
  })

  // 手动粘贴：覆盖「回调地址指向别处，跳转回不来」的真实情况
  bar.querySelector<HTMLButtonElement>('[data-act="manual"]')?.addEventListener("click", () => {
    const input = prompt("把浏览器地址栏里带 code= 的完整回调地址粘贴到这里（或只粘贴 code）：")
    if (!input?.trim()) return
    void submitCode(input.trim(), bar)
  })
}

/** 提交授权码（手动兜底路径）。 */
async function submitCode(raw: string, bar: HTMLElement): Promise<void> {
  const msg = bar.querySelector<HTMLElement>(".persona-auth-msg")
  if (msg) msg.textContent = "正在兑换…"
  try {
    const res = await fetch("/api/v1/persona/authorize/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ code: raw }),
    })
    const json = (await res.json()) as { ok?: boolean; error?: string; status?: PersonaStatus }
    if (!res.ok || !json.ok) {
      if (msg) msg.textContent = json.error ?? `兑换失败（HTTP ${res.status}）`
      return
    }
    if (json.status) render(json.status)
  } catch (err) {
    if (msg) msg.textContent = `兑换失败：${String((err as Error)?.message || err)}`
  }
}

/** 拉取状态并渲染（失败静默：横幅只是提示，不该因网络抖动报错打扰）。 */
export async function refreshPersonaStatus(): Promise<void> {
  try {
    const res = await fetch("/api/v1/persona/status")
    if (res.status === 404) {
      // 拟人模式未启用：撤掉可能残留的横幅（例如刚关掉开关重启）
      unmount()
      return
    }
    if (!res.ok) return
    const status = (await res.json()) as PersonaStatus
    render(status)
  } catch {
    /* 静默：网络抖动不打扰用户 */
  }
}

/**
 * 启动横幅（幂等）。
 *
 * 只在**拟人模式启用**的部署里有意义——未启用时 `/status` 返回 404，
 * 本函数什么都不做（连 DOM 都不会碰）。
 */
export function startPersonaAuthBanner(): void {
  if (pollTimer !== null) return
  void refreshPersonaStatus()
  pollTimer = setInterval(() => void refreshPersonaStatus(), POLL_MS)
}

/** 停止轮询（测试/卸载用）。 */
export function stopPersonaAuthBanner(): void {
  if (pollTimer !== null) {
    clearInterval(pollTimer)
    pollTimer = null
  }
  unmount()
}

/** 供事件钩子调用：收到 `event.persona.credentials` 时立即刷新（不等轮询）。 */
export function onPersonaCredentialsEvent(): void {
  void refreshPersonaStatus()
}

/** 诊断用：横幅当前是否挂着。 */
export function isPersonaBannerMounted(): boolean {
  return mounted && !!document.getElementById(BANNER_ID)
}
