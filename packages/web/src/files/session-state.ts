/**
 * 文件工作台的**状态记忆**（刷新页面后回到原处）：打开的标签、活动标签、当前根、左栏视图。
 *
 * 为什么放 `sessionStorage` 而不是 `localStorage`：这里记的是**本标签页的会话状态**，不是用户级偏好——
 * 独立打开的 `/files` 标签页与主界面分屏里的工作台（同源 iframe，与宿主共享同一份 sessionStorage）
 * 各记各的，互不覆盖；刷新（含 dev-reload）后保留，关掉标签页即随会话一起消失。用户级偏好
 * （自动换行 `gebai.ui.wordWrap`、小地图 `gebai.ui.minimap`、行尾 blame、主题、面板宽度）仍走 `localStorage`。
 *
 * 为什么只记**普通文件标签**：差异 / 合并 / 暂存 / 比较标签各自需要打开时的上下文（端点对、冲突文件、
 * 比较两端、来源），一个路径恢复不出来；它们本就是由文件派生的临时视图，刷新后重新打开即可。
 * 未保存的修改同样不在记忆范围内（内容不进浏览器存储）——恢复时按磁盘内容以**查看态**打开，
 * 并把「有修改未保留」如实告诉用户一次。
 *
 * 为什么单独一个文件：解析必须对脏值免疫（旧版本写入的其它形状、手改过的存储、别的页面同键），
 * 且这层逻辑零 DOM——抽出来就能直接测（见 `session-state.test.ts`）。
 */

export type LeftView = "explorer" | "changes" | "search"

export interface FwTabState {
  root: string
  path: string
  /** 查看 / 编辑（有未保存修改的标签按查看打开：内容留不下来，不该让人以为改动还在） */
  mode: "view" | "edit"
  /** 光标行（1 起，>1 才记；回到刷新前看的那一行） */
  line?: number
  /** 记下时有未保存的修改（内容跨不了刷新，恢复时据此如实提示一次） */
  dirty?: boolean
}

export interface FwSessionState {
  /** 当前根（左栏树所在的根）；不在根清单里时不恢复 */
  root?: string
  tabs: FwTabState[]
  /** 活动标签键（`root|path`，见 tabKey） */
  active?: string
  leftView?: LeftView
  leftVisible?: boolean
}

/** sessionStorage 键（per-tab）。 */
export const FW_SESSION_KEY = "gebai.ui.fwSession"

/** 标签上限：记太多会让下一次刷新变成一串请求，超出部分按打开顺序保留前 N 个。 */
export const FW_TAB_LIMIT = 24

/** 标签键（`root|path`）：根 id 与路径都不含 `|`，分隔无歧义。 */
export function tabKey(root: string, path: string): string {
  return `${root}|${path}`
}

function str(v: unknown): string | undefined {
  return typeof v === "string" && v ? v : undefined
}

/**
 * 归一化一份状态：形状不对的条目直接丢弃（不猜、不修补）。
 * 返回 null = 这份数据根本不像状态（调用方当作「没有记忆」）。
 */
export function normalizeSession(raw: unknown): FwSessionState | null {
  if (!raw || typeof raw !== "object") return null
  const o = raw as Record<string, unknown>
  const tabs: FwTabState[] = []
  if (Array.isArray(o.tabs)) {
    for (const item of o.tabs) {
      if (!item || typeof item !== "object") continue
      const t = item as Record<string, unknown>
      const root = str(t.root)
      const path = str(t.path)
      if (!root || !path) continue
      // line 只认数字（字符串是脏值）；0 与 1 都当「第一行」丢掉（只有 >1 才值得记）
      const line = typeof t.line === "number" ? t.line : 0
      tabs.push({
        root,
        path,
        mode: t.mode === "edit" ? "edit" : "view",
        line: Number.isFinite(line) && line > 1 ? Math.floor(line) : undefined,
        dirty: t.dirty === true ? true : undefined,
      })
      if (tabs.length >= FW_TAB_LIMIT) break
    }
  }
  const lv = o.leftView
  return {
    root: str(o.root),
    tabs,
    active: str(o.active),
    leftView: lv === "explorer" || lv === "changes" || lv === "search" ? lv : undefined,
    leftVisible: typeof o.leftVisible === "boolean" ? o.leftVisible : undefined,
  }
}

/** JSON 文本 → 状态（坏 JSON 当没有记忆）。 */
export function parseSession(json: string | null): FwSessionState | null {
  if (!json) return null
  try {
    return normalizeSession(JSON.parse(json))
  } catch {
    return null
  }
}

/** 读本标签页的记忆；没有可恢复的内容（无标签也无根）时返回 null。 */
export function loadSession(): FwSessionState | null {
  try {
    const s = parseSession(sessionStorage.getItem(FW_SESSION_KEY))
    if (!s) return null
    return s.tabs.length || s.root ? s : null
  } catch {
    return null
  }
}

/** 写回记忆：空状态清键（不留残留）。 */
export function saveSession(s: FwSessionState): void {
  try {
    if (!s.tabs.length && !s.root) {
      sessionStorage.removeItem(FW_SESSION_KEY)
      return
    }
    sessionStorage.setItem(FW_SESSION_KEY, JSON.stringify(s))
  } catch {
    /* 隐私模式/配额满：记忆失效不影响本次使用 */
  }
}

/** 清掉记忆（工作台被明确要求「忘掉上次」时用）。 */
export function clearSession(): void {
  try {
    sessionStorage.removeItem(FW_SESSION_KEY)
  } catch {
    /* 忽略 */
  }
}
