/**
 * 编辑器小地图（minimap）的**用户级偏好**：跨刷新与两个入口（独立 `/files` 页、分屏 iframe）一致。
 *
 * 与自动换行（`wrap.ts`）是**同一套机制的镜像**，只有默认值相反：小地图默认**开启**——
 * 它是长文件的结构概览（改动落在哪一片一眼看得出），关掉是少数派诉求（窄屏让位给正文、
 * 或纯粹不爱看那块缩略图），所以存储口径也反过来：**关闭才写键**（`"0"`），键不存在 = 开启。
 * 这样默认态不留任何 localStorage 残留，与 `wrap.ts`（开启才写 `"1"`）同一取向。
 */
const MINIMAP_KEY = "gebai.ui.minimap"

/** 小地图是否开启（缺省**开启**；只有显式存过 `"0"` 才算关闭，脏值一律当默认的开启）。 */
export function readMinimap(): boolean {
  try {
    return localStorage.getItem(MINIMAP_KEY) !== "0"
  } catch {
    return true
  }
}

/** 记住开关（开启时清键而不是写 `"1"`：默认态不留残留；隐私模式/配额满时静默失效）。 */
export function saveMinimap(on: boolean): void {
  try {
    if (on) localStorage.removeItem(MINIMAP_KEY)
    else localStorage.setItem(MINIMAP_KEY, "0")
  } catch {
    /* 偏好失效不影响本次使用 */
  }
}

/** 轮盘按钮的提示文案：按当前态给动作，与标签栏其他入口同调。 */
export function minimapTitle(on: boolean): string {
  return on ? "关闭小地图" : "开启小地图"
}
