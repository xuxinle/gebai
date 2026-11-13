/**
 * 标题栏模型配置切换器：在当前会话上选 `GEBAI_LLM_MODELS` 里的哪一条配置。
 *
 * 选择结果由**服务端持久化在会话**（chat.json 的 `modelConfig`），此后该会话的每次运行
 * （对话 / 定时任务 / 飞书桥接 / 子会话继承）都按它解析 Provider——前端只负责把入口对齐到
 * 当前会话的选择，不把模型配置另存一份到 localStorage（两处真相必然漂移）。
 *
 * 入口是一行无边框的文字（当前配置名）：悬停即在下方展开全部配置（每条带模型与多模态信息），
 * 点条目直接切换——**清单本身就是悬浮提示**，不必先点开一个下拉再选。触屏没有悬停，
 * 改为点击展开同一份清单。
 *
 * 草稿页（新会话尚未落盘）没有会话可存选择：它在内存里暂存，首条消息建会话时写入
 * （`createSessionWithDraftModel`）——选择仍然只存会话一处。
 *
 * 未配置清单（没有可选条目）时整组隐藏：那时只有一份启动配置，没有可切换的东西。
 */
import type { ModelConfigSummary, SessionInfo } from "@gebai/sdk"
import { client, el, getCurrentSession, setSessionCtxWindow } from "./state"
import { popKeyScope, pushEscScope } from "./keymap"
import { toast } from "./ui"
import { configCtxWindow, configMenuDetail, draftConfigToWrite, resolveConfigName } from "./model-picker-core"

/** 悬停展开/收起的延迟（毫秒）：展开留一点余量避免划过标题栏就弹；收起留余量让光标移进清单。 */
const HOVER_OPEN_MS = 120
const HOVER_CLOSE_MS = 180

/** 宿主容器（标题栏右侧最左）。 */
let host: HTMLElement | null = null
/** 文字入口（显示当前配置名）。 */
let trigger: HTMLButtonElement | null = null
/** 展开的配置清单（懒创建，关闭即移除）。 */
let menu: HTMLElement | null = null
/** 清单占用的 Esc 作用域（收起时归还）。 */
let menuScope: string | null = null
/** 可用配置清单（启动时拉一次：清单来自进程环境，运行期不会变）。 */
let configs: ModelConfigSummary[] = []
/** 当前生效的配置名（入口文字与清单高亮共用一处）。 */
let current = ""
/** 草稿页（新会话尚未创建）暂存的选择：此时没有会话可存，选择只在内存里等着——
 *  首条消息经 `createSessionWithDraftModel()` 建会话时写进该会话。空串 = 未选（用默认配置）。 */
let draftChoice = ""
let openTimer: ReturnType<typeof setTimeout> | null = null
let closeTimer: ReturnType<typeof setTimeout> | null = null

/** 清单默认配置名（第一条）：会话未选择时实际生效的就是它。 */
function defaultName(): string {
  return configs[0]?.name ?? ""
}

/** 上下文窗口随配置切换：不同配置的 max_context 可能差数倍，圆环占比必须按实际窗口算。
 *  未声明的配置清除覆盖（回落服务端快照给的默认窗口）。 */
function applyCtxWindow(name: string): void {
  setSessionCtxWindow(configCtxWindow(configs, name))
}

/** 入口文字（当前配置名）：切换会话/切换配置后都要对齐，故收在一处。 */
function renderTrigger(): void {
  if (!trigger) return
  trigger.textContent = current
  trigger.setAttribute("aria-label", `模型配置：${current}（悬停或点击切换）`)
}

/** 清单选中态：当前生效的那条高亮（切换后无需重建清单）。 */
function markActive(): void {
  if (!menu) return
  for (const opt of menu.querySelectorAll<HTMLButtonElement>(".model-opt")) {
    const on = opt.dataset.value === current
    opt.classList.toggle("active", on)
    opt.setAttribute("aria-selected", String(on))
  }
}

/** 当前设备是否有悬停能力（匹配 CSS 的 `(hover: hover) and (pointer: fine)`）。 */
function hoverCapable(): boolean {
  return typeof window.matchMedia !== "function" || window.matchMedia("(hover: hover) and (pointer: fine)").matches
}

/** 这次指针事件是否走悬停路径：触屏/无悬停设备（指针粗）走点击路径。 */
function hoverPointer(e: PointerEvent): boolean {
  return e.pointerType === "mouse" && hoverCapable()
}

const cancelOpen = (): void => {
  if (openTimer) clearTimeout(openTimer)
  openTimer = null
}

const cancelClose = (): void => {
  if (closeTimer) clearTimeout(closeTimer)
  closeTimer = null
}

function closeMenu(): void {
  cancelOpen()
  cancelClose()
  menu?.remove()
  menu = null
  trigger?.setAttribute("aria-expanded", "false")
  document.removeEventListener("pointerdown", onOutside, true)
  document.removeEventListener("scroll", onScroll, true)
  if (menuScope) {
    popKeyScope(menuScope)
    menuScope = null
  }
}

function buildMenu(): HTMLElement {
  const box = el("div", "model-menu")
  box.setAttribute("role", "listbox")
  for (const cfg of configs) {
    const opt = el("button", "model-opt")
    opt.type = "button"
    opt.setAttribute("role", "option")
    opt.dataset.value = cfg.name
    opt.append(el("span", "mo-name", cfg.name), el("span", "mo-sub", configMenuDetail(cfg)))
    opt.onclick = () => {
      closeMenu()
      void applyChoice(cfg.name)
    }
    box.appendChild(opt)
  }
  return box
}

function openMenu(): void {
  cancelOpen()
  cancelClose()
  if (!trigger || menu || !configs.length) return
  const box = buildMenu()
  menu = box
  document.body.appendChild(box)
  // 浮层 fixed 定位（避开标题栏裁剪）：宽度由内容决定（max-content），量出实际宽高后再收边
  const r = trigger.getBoundingClientRect()
  const w = box.offsetWidth
  const h = box.offsetHeight
  const vw = document.documentElement.clientWidth
  const vh = window.innerHeight
  box.style.left = `${Math.max(8, Math.min(r.right - w, vw - w - 8))}px`
  const below = r.bottom + 6
  box.style.top = `${below + h > vh - 8 ? Math.max(8, r.top - 6 - h) : below}px`
  trigger.setAttribute("aria-expanded", "true")
  markActive()
  box.addEventListener("pointerenter", cancelClose)
  box.addEventListener("pointerleave", (e) => {
    if (hoverPointer(e)) closeTimer = setTimeout(closeMenu, HOVER_CLOSE_MS)
  })
  document.addEventListener("pointerdown", onOutside, true)
  document.addEventListener("scroll", onScroll, true)
  menuScope = pushEscScope("main.modelMenu", "收起模型配置", closeMenu)
}

const onScroll = (e: Event): void => {
  // 清单自身滚动（条目多时）不动它
  if (menu && e.target instanceof Node && menu.contains(e.target)) return
  // 只认「入口所在容器或页面级」滚动：标题栏钉在顶部、不随内容流动，消息流自动滚动等无关容器
  // 的滚动不该把已展开的清单冲掉（同 tooltip 的隐藏口径）
  const t = e.target as Node
  if (t === document || (t && typeof t.contains === "function" && !!trigger && t.contains(trigger))) closeMenu()
}

const onOutside = (e: Event): void => {
  const t = e.target as Node
  if (trigger?.contains(t) || menu?.contains(t)) return
  closeMenu()
}

/**
 * 新建会话并把草稿页选中的模型配置写进去。
 *
 * 建会话的入口（输入框首条消息、URL 携带提示词自动运行）都走它：写入**先于**任务启动，
 * 服务端才能按会话的 `modelConfig` 解析 Provider（WS 同连接按序，请求先后即生效先后）。
 * 写入失败只提示并放弃本次选择，不让配置写入拦住建会话与发送（会话随后按清单默认配置跑）。
 */
export async function createSessionWithDraftModel(): Promise<SessionInfo> {
  const name = draftConfigToWrite(configs, draftChoice)
  draftChoice = ""
  const s = await client.createSession()
  if (!name) return s
  try {
    await client.setSessionModelConfig(s.id, name)
    s.modelConfig = name
  } catch (err) {
    toast(`模型配置写入失败：${(err as Error).message}（本次按默认配置运行）`, "error")
  }
  return s
}

/** 下拉对齐当前会话（切换会话/新建会话后调用）。 */
export function syncModelPicker(): void {
  if (!host || !trigger) return
  const cur = getCurrentSession()
  // 草稿态（会话尚未创建）：选择还没有会话可存，但不该就此不让人选——标着「新会话」的页面
  // 恰恰是最该先定模型的地方。改为内存暂存（draftChoice），首条消息建出会话时写进去。
  if (!cur) {
    current = resolveConfigName(configs, draftChoice)
    closeMenu()
    renderTrigger()
    applyCtxWindow(current)
    markActive()
    // 能走到这里说明清单非空（空清单在 initModelPicker 就隐藏了整组、也不会建入口）
    host.hidden = false
    return
  }
  current = resolveConfigName(configs, cur.modelConfig)
  // 会话切换：入口文字与清单高亮跟着变，展开中的清单按新会话重建内容（不留在屏上误导）
  closeMenu()
  renderTrigger()
  applyCtxWindow(current)
  markActive()
  host.hidden = false
}

/** 用户切换：写回会话（服务端持久化），失败则把入口恢复成服务端的实际值。 */
async function applyChoice(name: string): Promise<void> {
  const cur = getCurrentSession()
  if (name === current) return
  // 草稿态：没有会话可写，先记在内存里（随首条消息落到新会话）
  if (!cur) {
    draftChoice = name
    current = name
    renderTrigger()
    applyCtxWindow(name)
    markActive()
    return
  }
  try {
    // 默认配置不写名字：清单顺序变了「默认」跟着走，会话不必因为写在里面的旧名字失配
    await client.setSessionModelConfig(cur.id, name === defaultName() ? null : name)
  } catch (err) {
    toast(`模型配置切换失败：${(err as Error).message}`)
    syncModelPicker()
    return
  }
  // 会话对象是 state 的活引用：就地更新让标题栏/后续发送立即看到新选择（其他字段不受影响）
  cur.modelConfig = name
  current = name
  renderTrigger()
  applyCtxWindow(name)
  markActive()
  toast(`模型配置已切换为「${name}」`, "ok")
}

/** 初始化（首屏）：拉清单 → 建入口 → 绑定会话切换。清单拉取失败静默隐藏入口（不阻断首屏）。 */
export async function initModelPicker(): Promise<void> {
  host = document.getElementById("model-picker")
  if (!host) return
  try {
    configs = await client.listModelConfigs()
  } catch {
    configs = []
  }
  if (!configs.length) {
    host.hidden = true
    return
  }
  trigger = el("button", "model-trigger")
  trigger.type = "button"
  trigger.setAttribute("aria-haspopup", "listbox")
  trigger.setAttribute("aria-expanded", "false")
  // 点击：触屏无悬停，这点是唯一开关；有悬停能力的设备上悬停已展开，点一下只负责把
  // 键盘/异步路径叫开（已开时不再关掉——否则「悬停展开后随手一点反而收起」很迷惑）
  trigger.addEventListener("click", () => {
    if (menu) {
      if (!hoverCapable()) closeMenu()
      return
    }
    openMenu()
  })
  // 悬停：仅指针精确且有 hover 能力的设备——划过标题栏即见清单，不必点击
  trigger.addEventListener("pointerenter", (e) => {
    if (!hoverPointer(e)) return
    cancelClose()
    openTimer = setTimeout(openMenu, HOVER_OPEN_MS)
  })
  trigger.addEventListener("pointerleave", (e) => {
    cancelOpen()
    if (hoverPointer(e)) closeTimer = setTimeout(closeMenu, HOVER_CLOSE_MS)
  })
  host.appendChild(trigger)
  // 会话视图切换（setCurrentSession 派发）：入口跟着会话走——每个会话记住自己的模型配置
  document.addEventListener("gebai:session-view", syncModelPicker)
  syncModelPicker()
}

/** 供测试重置模块状态（bun test 单进程共享模块缓存）。 */
export function __resetModelPickerForTest(): void {
  closeMenu()
  host = null
  trigger = null
  configs = []
  current = ""
  draftChoice = ""
}

/** 供测试注入清单（不触网）。 */
export function __setModelConfigsForTest(list: ModelConfigSummary[]): void {
  configs = list
}
