/**
 * 界面字号档位（学生向可读性）：标准（默认 16px）/ 大（18px）/ 特大（20px）。
 * - 手动设置：localStorage `gebai.ui.fontSize` = "lg" | "xl"；不存/其它值 = 标准
 * - 生效方式：根元素 `data-fontsize` 属性 → base.css 覆写 `--font-size`（正文/输入/Markdown 等随之缩放）
 * - 主题不覆写 `--font-size`，档位在任何主题下均生效；跨标签页 storage 事件同步
 */

export type FontSizeSetting = "std" | "lg" | "xl"

const KEY = "gebai.ui.fontSize"

/** 用户设置（默认 std；非法存储值按标准处理）。 */
export function getFontSizeSetting(): FontSizeSetting {
  try {
    const v = localStorage.getItem(KEY)
    if (v === "lg" || v === "xl") return v
  } catch {
    /* 隐私模式等场景忽略 */
  }
  return "std"
}

/** 应用字号档位：设置根元素 data-fontsize 标记（std 不留属性，回落 base.css 默认 16px）。 */
export function applyFontSize(): void {
  const v = getFontSizeSetting()
  const el = document.documentElement
  if (v === "std") delete el.dataset.fontsize
  else el.dataset.fontsize = v
}

/** 手动设置（设置面板「外观」）：持久化 + 立即生效。 */
export function setFontSizeSetting(v: FontSizeSetting): void {
  try {
    if (v === "std") localStorage.removeItem(KEY) // 标准 = 默认，不留冗余存储
    else localStorage.setItem(KEY, v)
  } catch {
    /* ignore */
  }
  applyFontSize()
}

/** 初始化：应用当前设置，并跨标签页同步。 */
export function initFontSize(): void {
  applyFontSize()
  window.addEventListener("storage", (e) => {
    if (e.key !== KEY) return
    applyFontSize()
  })
}
