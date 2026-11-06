/**
 * 微框架 · 模块生命周期（资源登记）：把「监听器/定时器/观察者」的回收收成一份。
 *
 * 现状问题：面板代码里 `addEventListener` / `setInterval` / `ResizeObserver` 分散注册，
 * 关闭面板时靠人工记得逐个清理——漏一个就是"关掉面板后回调还在跑"或"面板开合十几次
 * 监听器堆叠、按钮点一次触发十次"。这类问题在浏览器里不会报错，只会表现为莫名其妙的怪行为。
 *
 * 这里提供 `Scope`：注册即登记，`dispose()` 一次性回收。**不接管 DOM**（DOM 由各面板自己建），
 * 只接管"需要被记住的副作用"。纯函数式、零依赖，可单测。
 */

export interface Scope {
  /** 登记一个解绑函数（`on()`/`makeDraggable().dispose` 的返回值可直接塞进来）。 */
  add: (dispose: () => void) => void
  /** 登记定时器（interval/timeout），dispose 时自动 clear。 */
  timer: (handle: ReturnType<typeof setInterval> | ReturnType<typeof setTimeout>) => void
  /** 登记 ResizeObserver（dispose 时 disconnect）。 */
  observer: (obs: { disconnect: () => void }) => void
  /** 是否已销毁（晚到的异步回调可据此跳过副作用）。 */
  disposed: () => boolean
  /** 回收全部登记的资源（幂等：重复调用无副作用）。 */
  dispose: () => void
  /** 当前登记数量（测试与泄漏排查用）。 */
  size: () => number
}

/** 建一个资源作用域。 */
export function createScope(): Scope {
  const disposers: Array<() => void> = []
  let done = false
  const run = (fn: () => void): void => {
    try {
      fn()
    } catch {
      /* 单个清理失败不阻断其余清理 */
    }
  }
  return {
    add: (dispose) => {
      if (done) {
        // 已销毁后再登记：立刻执行（避免"忘了检查 disposed"造成泄漏）
        run(dispose)
        return
      }
      disposers.push(dispose)
    },
    timer: (handle) => {
      const fn = (): void => {
        clearInterval(handle as ReturnType<typeof setInterval>)
        clearTimeout(handle as ReturnType<typeof setTimeout>)
      }
      if (done) run(fn)
      else disposers.push(fn)
    },
    observer: (obs) => {
      const fn = (): void => obs.disconnect()
      if (done) run(fn)
      else disposers.push(fn)
    },
    disposed: () => done,
    dispose: () => {
      if (done) return
      done = true
      for (const fn of disposers.splice(0).reverse()) run(fn)
    },
    size: () => disposers.length,
  }
}

/**
 * 轮询器：按固定间隔执行任务，**单飞**（上一轮还没结束就跳过本轮，避免任务堆叠）。
 * 目标是「面板打开期间静默同步」这类场景：面板关闭必须停、页面隐藏可暂停、任务在跑不叠加。
 */
export interface Poller {
  start: () => void
  stop: () => void
  /** 立即跑一次（打开面板时用）。 */
  now: () => void
}

export function createPoller(fn: () => void | Promise<void>, intervalMs: number, opts: { scope?: Scope } = {}): Poller {
  let handle: ReturnType<typeof setInterval> | null = null
  let inFlight = false
  const tick = (): void => {
    if (inFlight) return // 单飞：上一轮没结束就跳过
    if (typeof document !== "undefined" && document.visibilityState === "hidden") return // 后台标签页不跑
    inFlight = true
    Promise.resolve()
      .then(fn)
      .catch(() => {})
      .finally(() => {
        inFlight = false
      })
  }
  const poller: Poller = {
    start: () => {
      if (handle !== null) return
      handle = setInterval(tick, intervalMs)
      opts.scope?.timer(handle)
    },
    stop: () => {
      if (handle === null) return
      clearInterval(handle)
      handle = null
    },
    now: () => void tick(),
  }
  return poller
}
