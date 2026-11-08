/**
 * 启动阶段的弱网容错：超时包装、启动画面状态、离线态重试。
 *
 * ## 为什么需要
 * 启动链是**串行**的（`awaitCustomBoot` → 令牌恢复 → 工具元数据/外部身份 → `connect` → 会话列表 → 消息），
 * 而弱网/桥接路由器链路下两个环节都会「挂住而不是失败」：
 *
 * - **REST 无超时**：`fetch` 默认不超时，TCP 通但服务端不应答时永久悬挂（实测 20s 未返回）；
 * - **首连失败即终止**：`await client.connect()` 一抛错，后续的 `bindConnectionVerify()`（自检与
 *   运行态收敛的唯一点位）压根不会执行——此后网络恢复也没有任何路径把页面救回来，只能手动刷新。
 *
 * 于是手机端断网时刷新一次，看到的就是「启动动画转一圈 → 掉进死页面且永不自愈」。
 *
 * ## 本模块的对策
 * - `withTimeout`：给任何启动期 Promise 加**上限**，超时按失败处理（降级而不是挂死）；
 * - `bootStatus`：把「连接中 / 第 n 次重试 / 离线可重试」写到启动画面上，弱网下用户能分辨
 *   「在重连」与「卡死」；
 * - `bindBootRetry`：离线态下把启动画面的「重试连接」按钮接到重新初始化入口。
 */

/** 给 Promise 加超时：超时 reject（不取消底层操作，只是不再等它）。ms<=0 表示不限制。 */
export function withTimeout<T>(p: Promise<T>, ms: number, label: string): Promise<T> {
  if (!(ms > 0)) return p
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label}超时（${ms}ms）`)), ms)
    p.then(
      (v) => {
        clearTimeout(timer)
        resolve(v)
      },
      (e) => {
        clearTimeout(timer)
        reject(e)
      },
    )
  })
}

/** 启动画面状态文案与重试按钮的可达句柄（启动完成后自然失效：元素已移除）。 */
interface SplashRefs {
  status: HTMLElement | null
  retry: HTMLButtonElement | null
}

function splashRefs(): SplashRefs {
  if (typeof document === "undefined") return { status: null, retry: null }
  return {
    status: document.querySelector<HTMLElement>("#gb-splash .gb-splash-status"),
    retry: document.querySelector<HTMLButtonElement>("#gb-splash .gb-splash-retry"),
  }
}

/**
 * 写启动画面状态：`text` 为空串即清除。
 *
 * `retry` 为 true 时同时露出「重试连接」按钮（离线态专用）——不给按钮的话，用户在死页面里
 * 唯一能做的动作就是猜「要不要刷新」，而刷新只会重新走一遍同一条卡住的路径。
 */
export function bootStatus(text: string, retry = false): void {
  const { status, retry: btn } = splashRefs()
  if (status) status.textContent = text
  if (btn) btn.hidden = !retry
}

/** 把启动画面的重试按钮接到给定的重新初始化入口（多次调用以最后一次为准）。 */
export function bindBootRetry(run: () => void): void {
  const { retry } = splashRefs()
  if (!retry) return
  retry.onclick = () => {
    bootStatus("正在重新连接…", false)
    run()
  }
}

/** 启动画面是否仍在（未隐藏）：决定失败时是写启动画面还是写状态栏。 */
export function splashVisible(): boolean {
  if (typeof document === "undefined") return false
  return !!document.getElementById("gb-splash")
}

/** 启动期 REST 的宽松上限（毫秒）：弱网下宁可降级也不挂死，但给足够窗口正常加载。 */
export const BOOT_REST_TIMEOUT_MS = 8_000
/** 首连的上限（毫秒）：SDK 自身有 8s 建连超时，这里多留一拍余量。 */
export const BOOT_CONNECT_TIMEOUT_MS = 10_000
