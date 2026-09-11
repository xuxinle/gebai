/**
 * 文件工作台 · 编辑器层：Monaco（VSCode 同款内核）只读/编辑双态 + 差异视图，附降级编辑器。
 *
 * 为什么用 AMD 版 Monaco（`/vendor/monaco/vs/loader.js`）而不是 `import * as monaco from "monaco-editor"`：
 * - vite 会把 ESM 版切成大量带 hash 的 chunk，dev-reload（vite build --watch）重建后旧页面引用旧 chunk → 404；
 *   与 mermaid/plantuml/d2 同一处置：**稳定文件名的静态 vendor** 由 build-vendor.ts 拷入 public/vendor/；
 * - 语言高亮 / 各语言 worker 按需从同目录加载，无需前端打包器介入。
 *
 * 主题：用 `editor.defineTheme` 把当前界面的 CSS 令牌（--bg-inset/--text/--accent…）映射成 Monaco 主题，
 * 主题切换时重新 defineTheme + setTheme（与主界面换肤联动，而不是硬编码一套配色）。
 * 降级：vendor 缺失（如裁剪构建）/加载超时 → 自动降级为 `highlight.js 静态高亮 + textarea 编辑`，
 * 功能不缺失（查看/编辑/保存仍可用），仅体验降级。
 */

type Monaco = typeof import("monaco-editor")

/** 光标/选择变化回调。 */
export type CursorListener = (info: { line: number; column: number; selected: number }) => void

export interface EditorOptions {
  value: string
  language: string
  readOnly: boolean
  /** 小地图（文件工作台默认开启，窄屏由 CSS 折衷） */
  minimap?: boolean
  wordWrap?: boolean
  /** 大文件降级阈值（字符数）：超过则关闭小地图/括号彩化/词法高亮，保流畅 */
  largeFileChars?: number
}

export interface EditorHandle {
  kind: "monaco" | "fallback"
  getValue(): string
  setValue(value: string): void
  setLanguage(language: string): void
  setReadOnly(readOnly: boolean): void
  isReadOnly(): boolean
  focus(): void
  layout(): void
  revealLine(line: number, column?: number): void
  getScrollTop(): number
  setScrollTop(top: number): void
  getCursor(): { line: number; column: number; selected: number }
  onCursor(cb: CursorListener): void
  onChange(cb: () => void): void
  /** 全文替换（撤销栈视为一次编辑；保存后重新对齐基线用） */
  markClean(): void
  /** blame 行装饰（只读模式下的 Git 归因） */
  setBlame(lines: Array<{ line: number; hash: string; author: string; time: number; summary: string; uncommitted: boolean }>): void
  dispose(): void
}

export interface DiffHandle {
  kind: "monaco" | "fallback"
  layout(): void
  dispose(): void
  /**
   * 差异块导航（Monaco 可用；降级模式为 null）。
   * `state().index` 从 1 起，0 = 位置未知（尚未定位）；`onChange` 让工具条上的计数实时跟随。
   */
  nav?: DiffNav
}

/** 差异块导航：在「上/下一处差异」间跳，位置跟随滚动实时变化。 */
export interface DiffNav {
  next(): void
  prev(): void
  state(): { index: number; total: number }
  /**
   * 订阅计数变化（滚动、跳转、差异重算都会触发）。
   * 返回退订函数——订阅方是**标签栏**，每次重建标签栏都要退订旧的，
   * 否则重渲染几次就有几个野订阅在更新早已移除的 DOM。
   */
  onChange(cb: (s: { index: number; total: number }) => void): () => void
}

let monacoPromise: Promise<Monaco | null> | null = null
let monacoRef: Monaco | null = null
let currentTheme = "gebai-dark"
let lightTheme = false

function baseUrl(): string {
  return (import.meta.env.BASE_URL || "/").replace(/\/$/, "")
}

/** Monaco vendor 目录（`public/vendor/monaco/vs`）。 */
export function monacoVsPath(): string {
  return `${baseUrl()}/vendor/monaco/vs`
}

/** 加载 Monaco（AMD loader，单例；失败/超时返回 null 触发降级）。 */
export function loadMonaco(timeoutMs = 25000): Promise<Monaco | null> {
  if (monacoPromise) return monacoPromise
  monacoPromise = new Promise<Monaco | null>((resolve) => {
    const w = window as unknown as Record<string, unknown>
    if (w.monaco) {
      monacoRef = w.monaco as Monaco
      resolve(monacoRef)
      return
    }
    const vs = monacoVsPath()
    w.MonacoEnvironment = {
      getWorkerUrl: (_moduleId: string, _label: string) => `${vs}/base/worker/workerMain.js`,
    }
    const done = () => {
      const m = (window as unknown as Record<string, unknown>).monaco as Monaco | undefined
      monacoRef = m ?? null
      if (m) defineTheme(m)
      resolve(monacoRef)
    }
    const script = document.createElement("script")
    script.src = `${vs}/loader.js`
    script.async = true
    script.onload = () => {
      const requireFn = w.require as ((deps: string[], cb: () => void) => void) | undefined
      try {
        ;(w.require as { config?: (o: unknown) => void }).config?.({ paths: { vs } })
        requireFn?.(["vs/editor/editor.main"], done)
      } catch {
        done()
      }
    }
    script.onerror = () => {
      console.warn("[files] Monaco 加载失败（vendor 缺失？），降级为轻量编辑器")
      resolve(null)
    }
    document.head.appendChild(script)
    setTimeout(() => {
      if (!monacoRef) resolve(null)
    }, timeoutMs)
  })
  return monacoPromise
}

/* --------------------------- 主题映射 --------------------------- */

function cssColor(name: string, fallback: string): string {
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
  if (!raw) return fallback
  const probe = document.createElement("span")
  probe.style.color = raw
  probe.style.position = "absolute"
  probe.style.opacity = "0"
  document.body.appendChild(probe)
  const computed = getComputedStyle(probe).color
  probe.remove()
  const m = /^rgba?\(([^)]+)\)/.exec(computed)
  if (!m) return fallback
  const [r, g, b] = m[1].split(",").map((s) => parseFloat(s))
  if ([r, g, b].some((n) => !Number.isFinite(n))) return fallback
  return `#${[r, g, b].map((n) => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, "0")).join("")}`
}

function alpha(hex: string, a: number): string {
  const v = Math.round(Math.max(0, Math.min(1, a)) * 255)
    .toString(16)
    .padStart(2, "0")
  return `${hex}${v}`
}

/** 依据当前 CSS 令牌重定义 Monaco 主题（light/dark 同名两套，按当前主题明暗选择）。 */
export function defineTheme(monaco: Monaco): void {
  const bg = cssColor("--bg-inset", "#0d1117")
  const fg = cssColor("--text", "#e6edf3")
  const muted = cssColor("--text-muted", "#8b949e")
  const faint = cssColor("--text-faint", "#6e7681")
  const accent = cssColor("--accent", "#6366f1")
  const border = cssColor("--border", "#262b3a")
  const elev = cssColor("--bg-elev", "#161a24")
  const success = cssColor("--success", "#3fb950")
  const danger = cssColor("--danger", "#f85149")
  const warning = cssColor("--warning", "#d29922")

  // 明暗判定：背景亮度（不依赖 data-theme，任何主题都能自适应）
  const rgb = bg.match(/\w\w/g) ?? []
  const lum = rgb.length === 3 ? (parseInt(rgb[0], 16) * 0.299 + parseInt(rgb[1], 16) * 0.587 + parseInt(rgb[2], 16) * 0.114) / 255 : 0
  lightTheme = lum > 0.5

  monaco.editor.defineTheme("gebai", {
    base: lightTheme ? "vs" : "vs-dark",
    inherit: true,
    rules: [
      { token: "", foreground: fg.slice(1), background: bg.slice(1) },
      { token: "comment", foreground: faint.slice(1), fontStyle: "italic" },
      { token: "keyword", foreground: accent.slice(1) },
      { token: "string", foreground: success.slice(1) },
      { token: "number", foreground: warning.slice(1) },
      { token: "regexp", foreground: warning.slice(1) },
      { token: "type", foreground: accent.slice(1) },
      { token: "type.identifier", foreground: accent.slice(1) },
      { token: "function", foreground: muted.slice(1) },
      { token: "variable", foreground: fg.slice(1) },
      { token: "tag", foreground: accent.slice(1) },
      { token: "attribute.name", foreground: warning.slice(1) },
      { token: "delimiter", foreground: muted.slice(1) },
      { token: "invalid", foreground: danger.slice(1) },
    ],
    colors: {
      "editor.background": bg,
      "editor.foreground": fg,
      "editorLineNumber.foreground": faint,
      "editorLineNumber.activeForeground": muted,
      "editorCursor.foreground": accent,
      "editor.selectionBackground": alpha(accent, lightTheme ? 0.22 : 0.3),
      "editor.inactiveSelectionBackground": alpha(accent, 0.14),
      "editor.selectionHighlightBackground": alpha(accent, 0.14),
      "editor.lineHighlightBackground": alpha(muted, lightTheme ? 0.09 : 0.07),
      "editor.lineHighlightBorder": "#00000000",
      "editorIndentGuide.background1": alpha(border, 0.75),
      "editorIndentGuide.activeBackground1": alpha(accent, 0.5),
      "editorBracketMatch.background": alpha(accent, 0.18),
      "editorBracketMatch.border": alpha(accent, 0.5),
      "editorWhitespace.foreground": alpha(faint, 0.5),
      "editorGutter.background": bg,
      "editorWidget.background": elev,
      "editorWidget.border": border,
      "editorSuggestWidget.background": elev,
      "editorSuggestWidget.selectedBackground": alpha(accent, 0.22),
      "editorHoverWidget.background": elev,
      "editorHoverWidget.border": border,
      "input.background": bg,
      "input.border": border,
      "scrollbar.shadow": "#00000000",
      "scrollbarSlider.background": alpha(muted, 0.22),
      "scrollbarSlider.hoverBackground": alpha(muted, 0.36),
      "scrollbarSlider.activeBackground": alpha(accent, 0.5),
      "minimap.background": bg,
      "menu.background": elev,
      "menu.border": border,
      "list.hoverBackground": alpha(muted, 0.12),
      "list.activeSelectionBackground": alpha(accent, 0.24),
      "diffEditor.insertedTextBackground": alpha(success, 0.16),
      "diffEditor.removedTextBackground": alpha(danger, 0.16),
      "diffEditor.insertedLineBackground": alpha(success, 0.09),
      "diffEditor.removedLineBackground": alpha(danger, 0.09),
      "diffEditorGutter.insertedLineBackground": alpha(success, 0.2),
      "diffEditorGutter.removedLineBackground": alpha(danger, 0.2),
    },
  })
}

/** 主题切换时重映射（main.ts 监听 gebai:theme-change / 本地切换）。 */
export function refreshEditorTheme(): void {
  if (!monacoRef) return
  defineTheme(monacoRef)
  monacoRef.editor.setTheme("gebai")
  currentTheme = "gebai"
}

export { currentTheme }

/* --------------------------- 编辑器实现 --------------------------- */

/** 大文件阈值：超过后关闭小地图与高级特性。 */
const LARGE_FILE_CHARS = 1_500_000

export async function createEditor(host: HTMLElement, opts: EditorOptions): Promise<EditorHandle> {
  const monaco = await loadMonaco()
  if (!monaco) return createFallbackEditor(host, opts)
  defineTheme(monaco)
  const large = opts.value.length > (opts.largeFileChars ?? LARGE_FILE_CHARS)
  const model = monaco.editor.createModel(opts.value, opts.language)
  const ed = monaco.editor.create(host, {
    model,
    theme: "gebai",
    readOnly: opts.readOnly,
    automaticLayout: true,
    minimap: { enabled: !large && (opts.minimap ?? true), maxColumn: 90, renderCharacters: false },
    fontFamily: '"JetBrains Mono", "Cascadia Code", Consolas, "Courier New", monospace',
    fontSize: 13,
    lineHeight: 20,
    tabSize: 2,
    renderWhitespace: "selection",
    renderLineHighlight: large ? "none" : "all",
    scrollBeyondLastLine: false,
    smoothScrolling: true,
    wordWrap: opts.wordWrap ? "on" : "off",
    bracketPairColorization: { enabled: !large },
    guides: { bracketPairs: !large, indentation: !large },
    stickyScroll: { enabled: false },
    unicodeHighlight: { ambiguousCharacters: false, invisibleCharacters: false },
    scrollbar: { verticalScrollbarSize: 11, horizontalScrollbarSize: 11, useShadows: false },
    overviewRulerBorder: false,
    contextmenu: true,
    quickSuggestions: !opts.readOnly,
    suggestOnTriggerCharacters: !opts.readOnly,
    formatOnPaste: !opts.readOnly,
    padding: { top: 8, bottom: 24 },
    fixedOverflowWidgets: true,
  })
  let blameCollection: { clear: () => void } | null = null

  return {
    kind: "monaco",
    getValue: () => model.getValue(),
    setValue: (v) => {
      model.setValue(v)
    },
    setLanguage: (lang) => {
      monaco.editor.setModelLanguage(model, lang || "plaintext")
    },
    setReadOnly: (ro) => ed.updateOptions({ readOnly: ro }),
    isReadOnly: () => ed.getOption(monaco.editor.EditorOption.readOnly),
    focus: () => ed.focus(),
    layout: () => ed.layout(),
    revealLine: (line, column = 1) => {
      ed.revealLineInCenter(line + 1)
      ed.setPosition({ lineNumber: line + 1, column })
      ed.focus()
    },
    getScrollTop: () => ed.getScrollTop(),
    setScrollTop: (top) => ed.setScrollTop(top),
    getCursor: () => {
      const pos = ed.getPosition()
      const sel = ed.getSelection()
      const len = sel ? model.getValueInRange(sel).length : 0
      return { line: pos?.lineNumber ?? 1, column: pos?.column ?? 1, selected: len }
    },
    onCursor: (cb) => {
      ed.onDidChangeCursorPosition((e) => {
        const sel = ed.getSelection()
        const len = sel ? model.getValueInRange(sel).length : 0
        cb({ line: e.position.lineNumber, column: e.position.column, selected: len })
      })
      ed.onDidChangeCursorSelection((e) => {
        cb({ line: e.selection.positionLineNumber, column: e.selection.positionColumn, selected: model.getValueInRange(e.selection).length })
      })
    },
    onChange: (cb) => {
      ed.onDidChangeModelContent(() => cb())
    },
    markClean: () => {
      /* Monaco 无需额外处理：脏标记由上层按内容比对维护 */
    },
    setBlame: (lines) => {
      blameCollection?.clear()
      if (!lines.length) return
      const decos = lines.map((l) => ({
        range: new monaco.Range(l.line, 1, l.line, 1),
        options: {
          isWholeLine: true,
          description: "git-blame",
          className: "fw-blame-line",
          hoverMessage: {
            value: `**${l.author || "未知"}** · ${l.uncommitted ? "未提交" : new Date(l.time).toLocaleString()}\n\n\`${l.hash.slice(0, 8)}\` ${l.summary || ""}`,
          },
        },
      }))
      const collectionFactory = (monaco.editor as unknown as { createDecorationsCollection?: (d: unknown[]) => { clear: () => void } }).createDecorationsCollection
      blameCollection = collectionFactory ? collectionFactory(decos) : null
    },
    dispose: () => {
      blameCollection?.clear()
      ed.dispose()
      model.dispose()
    },
  }
}

/** 降级编辑器：只读用 highlight.js 静态高亮，编辑用 textarea（功能对齐，体验降级）。 */
async function createFallbackEditor(host: HTMLElement, opts: EditorOptions): Promise<EditorHandle> {
  const wrap = document.createElement("div")
  wrap.className = "fw-fallback"
  const pre = document.createElement("pre")
  pre.className = "fw-fallback-code hljs"
  const area = document.createElement("textarea")
  area.className = "fw-fallback-edit"
  area.spellcheck = false
  area.value = opts.value
  let readOnly = opts.readOnly
  if (readOnly) area.style.display = "none"
  else pre.style.display = "none"
  wrap.appendChild(pre)
  wrap.appendChild(area)
  host.appendChild(wrap)

  const render = async () => {
    if (!readOnly) return
    try {
      const mod = (await import("highlight.js/lib/common")) as unknown as { default: { highlight: (c: string, o: { language: string }) => { value: string } } }
      const res = mod.default.highlight(area.value, { language: opts.language === "plaintext" ? "plaintext" : opts.language })
      pre.innerHTML = res.value
    } catch {
      pre.textContent = area.value
    }
  }
  await render()
  let cursorCb: CursorListener | null = null
  area.addEventListener("keyup", () => {
    if (!cursorCb) return
    const upto = area.value.slice(0, area.selectionStart)
    const lines = upto.split("\n")
    cursorCb({ line: lines.length, column: lines[lines.length - 1].length + 1, selected: Math.abs(area.selectionEnd - area.selectionStart) })
  })
  return {
    kind: "fallback",
    getValue: () => area.value,
    setValue: (v) => {
      area.value = v
      void render()
    },
    setLanguage: () => {
      void render()
    },
    setReadOnly: (ro) => {
      readOnly = ro
      area.readOnly = ro
      pre.style.display = ro ? "" : "none"
      area.style.display = ro ? "none" : ""
      void render()
    },
    isReadOnly: () => area.readOnly,
    focus: () => area.focus(),
    layout: () => {},
    revealLine: (line) => {
      const lines = area.value.split("\n")
      const before = lines.slice(0, line).join("\n").length
      area.setSelectionRange(before, before)
      area.scrollTop = Math.max(0, (line - 6) * 20)
    },
    getScrollTop: () => area.scrollTop,
    setScrollTop: (top) => {
      area.scrollTop = top
    },
    getCursor: () => ({ line: 1, column: 1, selected: 0 }),
    onCursor: (cb) => {
      cursorCb = cb
    },
    onChange: () => {
      /* 降级模式由上层 input 事件驱动 */
    },
    markClean: () => {},
    setBlame: () => {},
    dispose: () => wrap.remove(),
  }
}

/* --------------------------- 差异视图 --------------------------- */

export interface DiffOptions {
  original: string
  modified: string
  language: string
  /** 并列 / 行内 */
  inline?: boolean
}

export async function createDiffEditor(host: HTMLElement, opts: DiffOptions): Promise<DiffHandle> {
  const monaco = await loadMonaco()
  if (!monaco) {
    const pre = document.createElement("pre")
    pre.className = "fw-fallback-code"
    pre.textContent = `--- 原\n${opts.original}\n\n+++ 改\n${opts.modified}`
    host.appendChild(pre)
    return { kind: "fallback", layout: () => {}, dispose: () => pre.remove() }
  }
  defineTheme(monaco)
  const original = monaco.editor.createModel(opts.original, opts.language)
  const modified = monaco.editor.createModel(opts.modified, opts.language)
  const ed = monaco.editor.createDiffEditor(host, {
    theme: "gebai",
    readOnly: true,
    automaticLayout: true,
    renderSideBySide: !opts.inline,
    // 右侧概览尺 = 「差异都在哪」的地图（配合上一处/下一处按钮，是这个视图的核心导航手段）。
    // 普通编辑器关闭它保持干净；差异视图正需要它。
    renderOverviewRuler: true,
    ignoreTrimWhitespace: false,
    fontFamily: '"JetBrains Mono", "Cascadia Code", Consolas, monospace',
    fontSize: 13,
    lineHeight: 20,
    scrollBeyondLastLine: false,
    minimap: { enabled: false },
    renderLineHighlight: "none",
    padding: { top: 8, bottom: 16 },
    fixedOverflowWidgets: true,
    scrollbar: { verticalScrollbarSize: 11, horizontalScrollbarSize: 11, useShadows: false },
  })
  ed.setModel({ original, modified })

  /* ---- 差异块导航 ----
   * Monaco 不直接提供 goToNextDiff，但 getLineChanges() 给出全部差异块
   *（原/改两侧的行号区间），据此自己跳。两个关键取舍：
   *  1. 「当前块」以**视口顶部**判定而不是内部游标：用户滚到哪里，计数就显示哪一块
   *     （与 VSCode 的 diff 导航一致）；
   *  2. 纯删除块在 modified 侧行号为 0，此时改在 original 侧定位——
   *     两侧滚动是同步的（diff editor 自带同步滚动），露一边两边都会跟。
   */
  let idx = -1
  const navCbs = new Set<(s: { index: number; total: number }) => void>()
  const changes = (): Array<{ originalStartLineNumber: number; originalEndLineNumber: number; modifiedStartLineNumber: number; modifiedEndLineNumber: number }> =>
    (ed.getLineChanges() ?? []) as never

  /** 视口顶部所在/之后的第一个差异块（都与视口无关时为最后一块）。 */
  function currentIndex(): number {
    const cs = changes()
    if (!cs.length) return -1
    const range = ed.getModifiedEditor().getVisibleRanges()[0]
    const top = range ? range.startLineNumber : 1
    for (let i = 0; i < cs.length; i++) {
      if (cs[i].modifiedEndLineNumber >= top) return i
    }
    return cs.length - 1
  }

  function emit(): void {
    const total = changes().length
    const s = { index: total ? currentIndex() + 1 : 0, total }
    for (const cb of navCbs) cb(s)
  }

  function reveal(i: number): void {
    const cs = changes()
    const c = cs[i]
    if (!c) return
    idx = i
    const me = ed.getModifiedEditor()
    const oe = ed.getOriginalEditor()
    const hasMod = c.modifiedStartLineNumber >= 1
    const target = hasMod ? me : oe
    const start = hasMod ? c.modifiedStartLineNumber : c.originalStartLineNumber
    const end = hasMod ? Math.max(c.modifiedEndLineNumber, c.modifiedStartLineNumber) : Math.max(c.originalEndLineNumber, c.originalStartLineNumber)
    // 选中整块：目标一眼可见（仅 revealLine 时，块很长也不好认）。
    // 末列取该行**最大列**——单行变更若用 col 1 → col 1 是零宽选区（等于一个光标），
    // 编辑器不会画任何选区高亮，"跳过去了"就看不出来。
    const model = target.getModel()
    target.setSelection({ startLineNumber: start, startColumn: 1, endLineNumber: end, endColumn: model ? model.getLineMaxColumn(end) : 1 })
    target.revealLineInCenterIfOutsideViewport(start)
    // Monaco 只在**获焦**时画强选区高亮；顺便让后续按键（方向键、Ctrl+F）落到差异视图上。
    // 不抢表单焦点：在提交框/搜索框里打字时按 F7，不应把光标拽走。
    const active = document.activeElement as HTMLElement | null
    const inField = !!active && (active.tagName === "INPUT" || active.tagName === "TEXTAREA" || active.isContentEditable)
    if (!inField) target.focus()
    emit()
  }

  /** 从当前块出发到下一（dir=1）/ 上一（dir=-1）处；到头**回卷**（连点不会没反馈地卡住，同 IDEA）。 */
  function step(dir: 1 | -1): void {
    const cs = changes()
    if (!cs.length) return
    const base = idx < 0 ? (dir === 1 ? -1 : 0) : idx
    const next = base + dir
    reveal(next < 0 ? cs.length - 1 : next >= cs.length ? 0 : next)
  }

  // 滚动 / 差异重算后刷新计数
  const sub = ed.getModifiedEditor().onDidScrollChange(() => {
    if (idx >= 0) idx = currentIndex()
    emit()
  })
  const diffSub = ed.onDidUpdateDiff(() => {
    idx = -1
    emit()
  })
  emit()

  const nav: DiffNav = {
    next: () => step(1),
    prev: () => step(-1),
    state: () => ({ index: changes().length ? Math.max(1, (idx < 0 ? currentIndex() : idx) + 1) : 0, total: changes().length }),
    onChange: (cb) => {
      navCbs.add(cb)
      cb(nav.state())
      return () => navCbs.delete(cb)
    },
  }

  /* 键盘：捕获阶段挂在 host 上。
   * 必须用 capture——Monaco 的 diff editor **内置**了 F7 / Shift+F7（diffReview）
   * 并会 stopPropagation，冒泡阶段（宿主 main.ts 的全局快捷键）根本收不到：
   * 编辑器一获焦，F7 就变成 Monaco 自己行为（且与我们的计数不同步）。
   * 捕获先于 Monaco 自己的监听器，拦下并自己处理；Alt+↑↓ 一并支持。 */
  const onKeyDown = (e: KeyboardEvent): void => {
    const isNext = e.key === "F7" && !e.shiftKey
    const isPrev = e.key === "F7" && e.shiftKey
    const isAlt = e.altKey && (e.key === "ArrowDown" || e.key === "ArrowUp")
    if (!isNext && !isPrev && !isAlt) return
    e.preventDefault()
    e.stopPropagation()
    if (isPrev || e.key === "ArrowUp") nav.prev()
    else nav.next()
  }
  host.addEventListener("keydown", onKeyDown, true)

  return {
    kind: "monaco",
    nav,
    layout: () => ed.layout(),
    dispose: () => {
      host.removeEventListener("keydown", onKeyDown, true)
      sub.dispose()
      diffSub.dispose()
      ed.dispose()
      original.dispose()
      modified.dispose()
    },
  }
}

/** 是否已在当前页面加载出 Monaco（用于状态栏提示与测试）。 */
export function monacoReady(): boolean {
  return !!monacoRef
}
