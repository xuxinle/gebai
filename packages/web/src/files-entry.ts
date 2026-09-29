/**
 * 文件工作台入口的 **URL 拼装与跳转**：`filesUrl`（拼参数）、`openFiles`（在当前/新标签打开），
 * 以及主界面那条 `Ctrl+\` 键位。
 *
 * 标题栏入口的**两个按钮不在这里**：它们都是“同窗”动作（主按钮 = 分屏并列，副按钮 = 全屏打开文件工作台），
 * 实现在 `files-split.ts`（本模块只给它提供拼 iframe src 的 `filesUrl`）——文件工作台那一侧的
 * 「关闭文件工作台 / 进入分屏」在 `files/main.ts` 的活动栏最下方。
 * 这里剩下的 `openFiles` 服务于**另一类入口**：消息流里的文件产物等需要“另开一个页面看”的场景。
 *
 * 传参：
 *   · `session`  = 会话 id（令 `sess:` 根指向该会话工作区，Agent 产物就地可查）；
 *                 缺省取当前会话（历史消息里的产物链接需显式传它所属会话）；
 *   · `root`     = 根 id（`proj:gebai` / `abs:/path` …），显式指定则直接打开该根；
 *   · `project`  = 预置项目名（等价于 `root=proj:<name>`）；
 *   · `path`     = 直达文件（会话相对或绝对路径均可，工作台自行定位所属根）；
 *   · `line`     = 直达行号（1 起始，与 `path` 同用）；
 *   · `from`/`to`= 直接开比较视图的两端；
 *   · 主题**不进 URL**：工作台与主界面共享同一份用户级偏好（localStorage `gebai.ui.style`，
 *     theme-core 的 initTheme 会以 urlPrefs:false 忽略 URL 上的主题参数），
 *     否则一个旧链接就能把两页拆成两套配色。
 */
import { getCurrentSession } from "./state"
import type { KeyBinding } from "./keymap"
import { FOCUS_WITH_INPUT } from "./keymap"
import { wbUrl } from "./files/url-base"

/** 打开工作台的参数（各字段可选，缺省按当前会话/主题补齐）。 */
export interface FilesOpenOpts {
  path?: string
  root?: string
  project?: string
  from?: string
  to?: string
  /** 1 起始行号（与 path 同用，打开后跳到该行）。 */
  line?: number
  /** 显式会话 id（历史消息的产物链接须传它渲染时的会话，否则落到当前会话的工作区）。 */
  session?: string
}

/** 文件工作台 URL（相对路径 + 会话上下文；主题不走 URL）。 */
export function filesUrl(opts: FilesOpenOpts = {}): string {
  const params = new URLSearchParams()
  const session = opts.session ?? getCurrentSession()?.id
  if (session) params.set("session", session)
  if (opts.root) params.set("root", opts.root)
  if (opts.project) params.set("project", opts.project)
  if (opts.path) params.set("path", opts.path)
  if (opts.line && opts.line > 0) params.set("line", String(opts.line))
  if (opts.from) params.set("from", opts.from)
  if (opts.to) params.set("to", opts.to)
  const qs = params.toString()
  return `${wbUrl("/files")}${qs ? `?${qs}` : ""}`
}

/** 在当前标签（或新标签）打开文件工作台。 */
export function openFiles(opts: FilesOpenOpts = {}, newTab = true): void {
  const url = filesUrl(opts)
  if (newTab) window.open(url, "_blank", "noopener")
  else location.href = url
}

/**
 * 主界面快捷键：`Ctrl+\` 开关文件工作台（同一个键管三态，见 files-split.ts 的 toggleSplit）：
 * 会话桌面 → 并列；并列 → 回会话桌面；窗口容不下分屏时它是全屏开关，全屏态下它关掉文件工作台。
 * 连按两次总能回到会话桌面，而不是攒出两个新标签页。**全屏的进入另有副按钮（浮空弹出）**——
 * 一个键管两种“开”只会让人猜；也因此副按钮不标这个键（标了就会让人以为它能开全屏）。
 *
 * `focus` 含 `input`：主界面的默认焦点就在聊天输入框（进草稿页/切会话/回答结束都会 `focusInput()`），
 * 不含它这条快捷键就基本没机会命中（Ctrl+\ 在输入框里没有输入语义，
 * 中文候选态另由分发器的 `isComposing` 守卫兜住）。
 */
export const splitBindings: KeyBinding[] = [
  {
    id: "main.split.toggle",
    keys: "Ctrl+\\",
    label: "开关文件分屏（全屏态下是关闭文件工作台）",
    group: "main.session",
    browser: "override",
    focus: FOCUS_WITH_INPUT,
    run: () => void import("./files-split").then((m) => m.toggleSplit({ path: undefined })),
  },
]
