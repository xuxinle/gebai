/**
 * 消息列 DOM 窗口化——DOM 编排层（坐标算术见 virtual-window.ts）：
 *
 * DOM 结构（#messages 的直接子节点）：
 *   [.vz-spacer.top] [挂载中的块 .vz-block …] [.vz-spacer.bottom] [尾部活动区节点 …]
 *
 * - **块**是最小挂载单位（一段连续消息，划分规则见 history-chunk.ts）：未挂载时只有一侧
 *   spacer 的高度，挂载时是容器里的真实节点（高度实测）。已渲染节点的引用常驻——卸载只是
 *   从 DOM 摘下，重挂零重建（在途流 `run.el`、工具卡 `pendingTools`、子会话容器 body 等运行态
 *   引用因此不掉线）。
 * - **尾部活动区**（新消息 / 在途流 / 工具卡 / 异常提示）恒定挂载在末尾，其高度由 DOM 真实
 *   承担，不参与估高。
 * - 滚动位置稳定：以「视口顶部块 + 块内偏移」为锚点，DOM 变更（渲染实测、卸载、spacer 更新）
 *   后同帧把 `scrollTop` 修正回锚点位置——向下滚动时卸载的都是刚测过的真实高度（零补偿），
 *   向上滚动时新挂载块由估高变实测，差值由锚点补齐。
 * - 视口不可测（`clientHeight <= 0`，测试替身 / 容器隐藏）：区间计算**沿用最近一次可测高度**
 *   （从未测过时用假定屏高），不退化取全部块。后台会话的 pane 恒为 `hidden`，若按“不可测→
 *   全量”处理，长会话在后台期间会把整篇消息全挂上 DOM，切回时同步布局上百节点就是明显的卡顿；
 *   保持窗口化则后台只维护视口附近的块，切回是纯显隐。spacer 仍按模型高度更新（切回布局是新的），
 *   只是不做需要真实几何的“拽到底部 / 锚点补偿”。
 */

import { createVzModel, type VzModel, type VzRange, type VzSlotSeed } from "./virtual-window"

/** 视口上方预挂载余量（视口高度倍数）：与下方对称——上滚时也先渲染再进入视野，避免视口落进估高区域。 */
const MARGIN_ABOVE = 1.5
/** 视口下方预挂载余量：滚动主要向下，留足一屏半。 */
const MARGIN_BELOW = 1.5
/** 窗口收缩滞回带（视口高度倍数）：丢块要超出更紧的边距才生效——否则视口恰好停在边界上时，
 *  spacer 的 18px 间隙差就会让范围来回翻转，同一块反复挂载/卸载。 */
const RANGE_HYSTERESIS = 0.25
/** 贴底阈值（px）：与粘底跟随（sticky-scroll）同口径——距底不超过该值视为在底部。 */
const BOTTOM_THRESHOLD = 64
/** 锚点下钻最大层数（块 → 消息 → 正文 → 内容块…；越深越能察觉块内长高，过深则易随重渲染失效）。 */
const ANCHOR_MAX_DEPTH = 8

export interface VirtualizeOpts {
  container: HTMLElement
  /** 渲染第 index 号块：把该块的消息节点 append 进 host（同步；host 尚未入 DOM）。 */
  renderBlock: (index: number, host: HTMLElement) => void
}

export interface Virtualizer {
  /** 重设块表并落底（会话切换 / 重新加载）：清空容器（含尾部活动区）后建立 spacer。 */
  reset(seeds: VzSlotSeed[]): void
  /** 按当前滚动位置立即同步挂载集合（含锚定补偿）。 */
  sync(): void
  /** 尾部活动区挂载（恒在 spacer 之后，恒挂载）。 */
  appendTail(node: Node): void
  /** 当前视口顶部锚点（顶部 / 落在尾部活动区时返回 null）。 */
  anchor(): { key: string; offset: number } | null
  /** 按锚点恢复滚动位置（跨会话记忆）；返回落位后的 scrollTop，键不存在返回 null。 */
  scrollToAnchor(key: string, offset: number): number | null
  /** 块 key → 全渲染坐标（导航高亮）；不存在返回 null。 */
  posOfKey(key: string): number | null
  /** 块 key → 当前采用高度。 */
  heightOfKey(key: string): number | null
  /** 注入「程序回正落点」判定（意图驱动；返回 null = 不回正）。省略时用几何贴底判定：
   *  落点为绝对底部。落点由跟随核心给（含用户在容忍区内的距底位移）——
   *  窗口化不得自己写绝对底部，否则用户的小步微调会被每次内容增长抹掉（滚不出去）。 */
  setFollowSource(fn: (() => number | null) | null): void
  /** 注入「内容已变更」通知（贴底赋值后交跟随核心接手后续对齐）。 */
  setContentChangedHook(fn: (() => void) | null): void
  /** 容器内容原点（padding-top）：块坐标 → 屏幕坐标的换算偏移（导航高亮用）。 */
  contentTop(): number
}

export function createVirtualizer(opts: VirtualizeOpts): Virtualizer {
  const container = opts.container
  const model: VzModel = createVzModel()
  const padTop = document.createElement("div")
  const padBottom = document.createElement("div")
  padTop.className = "vz-spacer"
  padBottom.className = "vz-spacer"
  /** 已渲染块的容器（未渲染为 undefined；卸载只摘 DOM，引用常驻）。 */
  const blocks: Array<HTMLElement | undefined> = []
  const blockIndexOf = new Map<Element, number>()
  /** 容器内容原点（padding-top）：scrollTop 从 padding 盒顶起算，块坐标从首个块顶起算。 */
  let origin = 0
  let rafId = 0
  let syncing = false
  let resetting = false
  let followSource: (() => number | null) | null = null
  let contentChangedHook: (() => void) | null = null

  const raf = typeof requestAnimationFrame === "function" ? requestAnimationFrame.bind(globalThis) : (cb: () => void) => { cb(); return 0 }

  const roContainer = typeof ResizeObserver !== "undefined" ? new ResizeObserver(() => { measureMetrics(); sync() }) : null
  const roBlocks = typeof ResizeObserver !== "undefined"
    ? new ResizeObserver((entries) => {
        let changed = false
        // 批次：一次尺寸变更常涉多块（流式输出每 120ms 重解析、图片/图表批量长高），
        // 每块单独 rebuild 会把高度表重算多次（上百块时就是刷屏式抖动）。
        model.beginBatch()
        try {
          for (const entry of entries) {
            const index = blockIndexOf.get(entry.target)
            if (index === undefined || !model.slots[index]?.mounted) continue
            if (measureBlock(index)) changed = true
          }
        } finally {
          model.endBatch()
        }
        // 异步长高（图片/图表/懒渲染）：按锚点补回布局位移（跟随时钉底）；只重算 spacer 与位置，
        // 不改挂载集合（避免与本次尺寸变化互相触发）
        if (changed) applyLayout(mountedRange(), wantKeepBottom())
      })
    : null

  /** 读取容器度量（块间距 / 内容原点）：spacer 补偿与坐标换算依赖它们。 */
  function measureMetrics() {
    try {
      const cs = getComputedStyle(container)
      const gap = parseFloat(cs.rowGap || cs.gap || "0")
      model.setGap(Number.isFinite(gap) ? gap : 0)
      const pt = parseFloat(cs.paddingTop || "0")
      origin = Number.isFinite(pt) ? pt : 0
    } catch {
      /* 无样式环境（测试替身）：按 0 处理 */
    }
  }

  function measureBlock(index: number): boolean {
    const el = blocks[index]
    if (!el || !el.isConnected) return false
    const h = el.getBoundingClientRect?.().height ?? 0
    return model.measure(index, h)
  }

  function mountBlock(index: number) {
    let el = blocks[index]
    if (!el) {
      el = document.createElement("div")
      el.className = "vz-block"
      opts.renderBlock(index, el)
      blocks[index] = el
      blockIndexOf.set(el, index)
      model.markRendered(index)
      roBlocks?.observe(el)
    }
    // 插到**正确的顺序位置**：下一个已挂载的更大序号块之前（否则向上扩窗时新块会落到
    // 已挂载块之后，DOM 顺序与槽位顺序不一致 → 块自身的空间与上方 spacer 重复计算，
    // 锚点被推着来回补偿（滚动时来回跳））
    let before: Node = padBottom
    for (let j = index + 1; j < model.count(); j++) {
      const cand = blocks[j]
      if (cand && cand.isConnected) {
        before = cand
        break
      }
    }
    container.insertBefore(el, before)
    model.markMounted(index)
    measureBlock(index)
  }

  function unmountBlock(index: number) {
    const el = blocks[index]
    if (!el) return
    if (el.isConnected) {
      measureBlock(index) // 摘除前确认真实高度（RO 已缓存，此处兜底）
      el.remove()
    }
    model.markUnmounted(index)
  }

  /** 应用 spacer 高度（返回是否发生变化：无变化时不写样式，也免去一次布局失效）。 */
  function applyPad(el: HTMLElement, height: number): boolean {
    const px = height > 0 ? `${Math.round(height)}px` : "0px"
    const display = height > 0 ? "" : "none"
    if (el.style.height === px && el.style.display === display) return false
    el.style.display = display
    el.style.height = px
    return true
  }

  /* ---------- 元素级滚动锚定（自管，代替被关闭的 overflow-anchor） ----------
   *
   * 布局变更后要把视口内容钉在原处，**不能写回一个绝对位置**：超大卡片渲染耗时较长，
   * 这期间用户仍在滚动，帧初算出的绝对位置会把这期间的滚动抹掉（现象就是「跳」）。
   * 做法：维护一条基线（视口顶部所在子节点的屏幕位置 + 当时的 scrollTop），任何布局变更
   * 后用「实际位移 − 用户滚动量」得出纯布局位移并补回 scrollTop——用户自己的滚动照旧生效，
   * 且块内内容异步长高（图片/图表/懒渲染）同样被补回。 */

  let anchorEl: HTMLElement | null = null
  let anchorTop = 0
  let anchorScrollTop = 0

  /** 视口顶部所在元素（锚点）：从容器直接子节点逐层**下钻**到最早跨过视口顶部的后代。
   *  必须下钻到后代——块级锚点察觉不到「块内内容长高」（图片/图表/懒渲染/超大卡片内部），
   *  而块自身 top 不变、补偿为 0，长高就会把视口内容推下去（表现即滚动时的跳动）。 */
  function topVisibleElement(): HTMLElement | null {
    const top = container.getBoundingClientRect().top + 1
    let node: HTMLElement | null = null
    for (const child of Array.from(container.children) as HTMLElement[]) {
      if (child.classList.contains("vz-spacer")) continue
      if (child.getBoundingClientRect().bottom > top) {
        node = child
        break
      }
    }
    for (let depth = 0; node && depth < ANCHOR_MAX_DEPTH; depth++) {
      const next = (Array.from(node.children) as HTMLElement[]).find((c) => c.getBoundingClientRect().bottom > top)
      if (!next) break
      node = next
    }
    return node
  }

  /** 记下当前锚点基线（布局稳定后调用）。 */
  function noteAnchor() {
    const el = topVisibleElement()
    anchorEl = el
    anchorTop = el ? el.getBoundingClientRect().top : 0
    anchorScrollTop = container.scrollTop
  }

  /** 把自上一条基线以来的**布局位移**补回 scrollTop（用户滚动量已扣除，不丢用户滚动）。 */
  function compensateLayout() {
    const el = anchorEl
    if (!el || !el.isConnected) return
    const scrolled = container.scrollTop - anchorScrollTop
    const expected = anchorTop - scrolled
    const delta = el.getBoundingClientRect().top - expected
    if (Number.isFinite(delta) && Math.abs(delta) > 0.5) container.scrollTop += delta
  }

  /** 落定一次布局变更（spacer + 锚定补偿）并重建基线：粘底跟随时钉底（内容增长不把视口推离
   *  底部，并交跟随核心接手后续对齐），否则只补布局位移。
   *  容器隐藏（后台 pane）时 clientHeight 为 0：spacer 仍要按模型高度更新（切回时布局才是新的），
   *  只是跳过“把视口拽到底部/按锚点补偿”——不可测的几何上算出的偏移会错。 */
  function applyLayout(range: VzRange, keepBottom: boolean) {
    const measurable = container.clientHeight > 0
    const pad = model.padHeight(range)
    applyPad(padTop, pad.top)
    applyPad(padBottom, pad.bottom)
    if (!measurable) return
    if (keepBottom) {
      const target = pinTarget()
      if (Math.abs(target - container.scrollTop) > 0.5) {
        container.scrollTop = target
        contentChangedHook?.()
      }
    } else {
      compensateLayout()
    }
    noteAnchor()
  }

  /** 程序回正落点：跟随意图优先（落点含用户在容忍区内的距底位移）；未注入时退回绝对底部
   *  （测试环境）。容器自身滚动几何决定上限，故始终住在可滚范围内。 */
  function pinTarget(): number {
    if (followSource) {
      try {
        const t = followSource()
        if (t !== null && Number.isFinite(t)) return t
      } catch {
        /* 注入方异常：退回几何判定 */
      }
    }
    return container.scrollHeight
  }

  /** 是否应保持贴底（程序回正落点可用性）。 */
  function wantKeepBottom(): boolean {
    if (followSource) {
      try {
        return followSource() !== null
      } catch {
        /* 注入方异常：退回几何判定 */
      }
    }
    return isAtBottom()
  }

  /** 当前位置距底部距离（px）。 */
  function distanceToBottom(): number {
    return container.scrollHeight - container.scrollTop - container.clientHeight
  }

  /** 是否贴底（几何判定，与粘底跟随同阈值）。 */
  function isAtBottom(): boolean {
    const distance = distanceToBottom()
    return Number.isFinite(distance) && distance <= BOTTOM_THRESHOLD
  }

  /** 当前挂载块对应的区间（含包络）：ResizeObserver 只据它重算 spacer 与滚动位置，不改挂载集合。 */
  function mountedRange(): VzRange {
    let start = -1
    let end = 0
    for (let i = 0; i < model.count(); i++) {
      if (!model.slots[i].mounted) continue
      if (start < 0) start = i
      end = i + 1
    }
    return start < 0 ? { start: 0, end: 0 } : { start, end }
  }

  /** 卸载块与复位区间滞回（重设后首次 sync 应重新取窗）。 */
  let lastRange: VzRange = { start: 0, end: 0 }
  let hasRange = false

  /** 视口高度兜底（px）：容器不可测且从未测过时（隐藏/未布局）的假定屏高。 */
const FALLBACK_VIEWPORT_H = 720

/** 取挂载区间并施加滞回：扩窗随视口即时生效，**缩窗**要超出更紧的边距才生效。 */
  function computeRange(scrollTop: number, viewportH: number, keepBottom: boolean): VzRange {
    const r = model.rangeFor(scrollTop, viewportH, MARGIN_ABOVE, MARGIN_BELOW)
    if (!hasRange) {
      lastRange = r
      hasRange = true
      return r
    }
    if (r.start > lastRange.start) {
      const tighter = model.rangeFor(scrollTop, viewportH, Math.max(0, MARGIN_ABOVE - RANGE_HYSTERESIS), MARGIN_BELOW).start
      r.start = Math.max(lastRange.start, Math.min(r.start, tighter))
    }
    if (!keepBottom && r.end < lastRange.end) {
      const tighter = model.rangeFor(scrollTop, viewportH, MARGIN_ABOVE, Math.max(0, MARGIN_BELOW - RANGE_HYSTERESIS)).end
      r.end = Math.min(lastRange.end, Math.max(r.end, tighter))
    }
    lastRange = r
    return r
  }

  /** 最近一次可测的视口高度（px）：容器隐藏时为 0，据此回退而不是把 0 交给区间计算。 */
  let lastViewportH = 0

  /** 区间计算用的视口高度：可测则刷新缓存，不可测则用上次值，最后兜底假定屏高。 */
  function viewportHeight(): number {
    const h = container.clientHeight
    if (h > 0) lastViewportH = h
    return lastViewportH > 0 ? lastViewportH : FALLBACK_VIEWPORT_H
  }

  /** 贴底取窗：末尾往前覆盖视口 + 上方余量（不依赖当前 scrollTop，缓存页切回不会取错窗）。 */
  function bottomRange(viewportH: number): VzRange {
    const n = model.count()
    if (!n) return { start: 0, end: 0 }
    const span = viewportH * (1 + MARGIN_ABOVE)
    let start = n - 1
    let covered = 0
    while (start > 0 && covered < span) {
      start -= 1
      covered += model.heightAt(start)
    }
    return { start, end: n }
  }

  function sync() {
    if (syncing) return
    const n = model.count()
    if (!n) return
    syncing = true
    try {
      // 视口高度：容器隐藏（后台会话 pane 为 hidden）时 clientHeight 为 0——直接用 0 会让
      // 区间退化为「全部块」（见文件头安全阀），长会话在后台期间就把整篇消息全挂上 DOM，
      // 切回时要同步布局上百节点（卡顿的真正来源）。改用**最近一次可测的高度**（没有则用
      // 兜底屏高）：后台只维护视口附近的块，切回才是纯显隐。
      const viewportH = viewportHeight()
      const scrollTop = Math.max(0, container.scrollTop - origin)
      const keepBottom = resetting || wantKeepBottom()
      // 取窗：贴底时不能拿旧 scrollTop 算（缓存页切回时它还是上次离开的值，且旧值可能
      // 大于当前总高）——直接按「末尾往前一屏」取，与紧接着的 range.end = n 一致。
      // 否则窗口先开在错误位置、spacer 反复改写，位置被掰动（切回时抖几下）。
      const range = keepBottom ? bottomRange(viewportH) : computeRange(scrollTop, viewportH, keepBottom)
      // 贴底：窗口锚定到末尾块（滚动位置即将落到底部，底部不留在估高空白侧）
      if (keepBottom) range.end = n
      let mutated = false
      // 变更顺序很关键：**先挂载 → 再落 spacer → 最后卸载**。若先摘 DOM 再更新 spacer，
      // 布局会出现瞬时「变短」，浏览器把 scrollTop 钳到临时上限（每次回退一个滚轮步长，
      // 并让区间来回翻转 → 同一块反复挂载/卸载）；本顺序下布局只会瞬时「变长」（无害）。
      // 批次：一次窗口变更会挂载多块，每块实测都重建高度表是 O(块数²)——先攒起来，
      // 结束时重建一次（长会话切回时总高不再反复跳，消除视觉抖动）。
      model.beginBatch()
      try {
        for (let i = range.start; i < range.end; i++) {
          if (!model.slots[i].mounted) {
            mountBlock(i)
            mutated = true
          }
        }
      } finally {
        model.endBatch()
      }
      const pad = model.padHeight(range)
      applyPad(padTop, pad.top)
      applyPad(padBottom, pad.bottom)
      for (let i = 0; i < n; i++) {
        const slot = model.slots[i]
        if (slot.mounted && (i < range.start || i >= range.end)) {
          unmountBlock(i)
          mutated = true
        }
      }
      // 布局确有变更才补偿并重建基线（纯滚动帧不走这里）
      if (!mutated) return
      if (keepBottom) {
        const target = pinTarget()
        if (Math.abs(target - container.scrollTop) > 0.5) {
          container.scrollTop = target
          contentChangedHook?.() // 交跟随核心接手后续对齐（尾部继续增长时咬住底部）
        }
      } else {
        compensateLayout()
      }
      noteAnchor()
    } finally {
      syncing = false
    }
  }

  function schedule() {
    if (rafId) return
    rafId = raf(() => {
      rafId = 0
      sync()
    })
  }

  const onScroll = () => schedule()
  const onResize = () => {
    measureMetrics()
    schedule()
  }

  container.addEventListener("scroll", onScroll, { passive: true })
  if (typeof window !== "undefined" && typeof window.addEventListener === "function") window.addEventListener("resize", onResize)

  return {
    reset(seeds) {
      blocks.length = 0
      blockIndexOf.clear()
      roBlocks?.disconnect()
      model.setSlots(seeds)
      measureMetrics()
      container.replaceChildren(padTop, padBottom)
      hasRange = false // 新会话重新取窗（滞回基线不跨会话）
      // 初始全未挂载：顶部 spacer 撑起全高，先落底让窗口落在尾部（首屏只渲染尾部块）
      applyPad(padTop, model.totalHeight())
      applyPad(padBottom, 0)
      if (container.clientHeight > 0) container.scrollTop = container.scrollHeight
      roContainer?.disconnect()
      roContainer?.observe(container)
      // 重设后的首次 sync 强制落底（本帧只把窗口落在尾部；最终位置由调用方接着定：
      // 落底跟随或按锚点恢复阅读位置）——不能用跟随意图（上一会话可能处于阅读态）
      resetting = true
      try {
        sync()
      } finally {
        resetting = false
      }
      // 首批块实测后按样本定稿未渲染块的估高：此后滚动不再重算总高（布局稳定是位置稳定的前提）
      if (model.settleEstimates()) applyLayout(mountedRange(), true)
      noteAnchor()
      schedule()
    },
    sync,
    appendTail(node) {
      if (padBottom.parentNode !== container) container.appendChild(padBottom)
      container.appendChild(node)
      // 尾部追加属于布局变更：按锚点补回位移（跟随时钉底），否则新增内容会把视口往下推
      if (!syncing && model.count() && container.clientHeight > 0) {
        compensateLayout()
        if (wantKeepBottom()) applyLayout(mountedRange(), true)
        else noteAnchor()
      }
    },
    anchor() {
      const n = model.count()
      if (!n || !(container.clientHeight > 0)) return null
      const y = Math.max(0, container.scrollTop - origin)
      if (y <= 0) return null
      const loc = model.locate(y)
      // 视口顶部越过最后一个块（落在尾部活动区）：无块锚点（贴底语义由调用方判定）
      if (y > model.pos(n - 1) + model.heightAt(n - 1)) return null
      return { key: model.slots[loc.index].key, offset: loc.offset }
    },
    scrollToAnchor(key, offset) {
      const index = model.slots.findIndex((s) => s.key === key)
      if (index < 0) return null
      const want = Math.max(0, offset)
      // 目标块的坐标**在挂载前不稳定**（未实测块用估高）；先按当前坐标粗定位让 sync 把
      // 目标块挂上去，实测后再精确修正一次——两写一读是必要的（否则未挂载则无法测量），
      // 但不能再反复：修正值按**同一块**的实测高度算，不重算总高。
      container.scrollTop = origin + model.pos(index) + want
      sync() // 目标块可能未挂载：先挂载（实测高度替换估高）
      const refined = origin + model.pos(index) + Math.min(want, model.heightAt(index))
      if (Number.isFinite(refined) && Math.abs(refined - container.scrollTop) > 0.5) container.scrollTop = refined
      noteAnchor() // 落位后重建基线（后续异步布局变更按此补偿）
      return container.scrollTop
    },
    posOfKey(key) {
      const index = model.slots.findIndex((s) => s.key === key)
      return index < 0 ? null : model.pos(index)
    },
    heightOfKey(key) {
      const index = model.slots.findIndex((s) => s.key === key)
      return index < 0 ? null : model.heightAt(index)
    },
    contentTop: () => origin,
    setFollowSource(fn) {
      followSource = fn
    },
    setContentChangedHook(fn) {
      contentChangedHook = fn
    },
  }
}
