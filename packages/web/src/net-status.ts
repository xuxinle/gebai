/**
 * 网络连通性 → 界面动作的判据（纯函数，可单测）。
 *
 * ## 为什么需要它
 * 前端的运行态收敛原本只有三条触发路径：`visibilitychange`（回前台）、`pageshow`、以及一个
 * 10 秒定时自检。**没有任何一条监听浏览器自己的网络事件**（`online`/`offline`）——后果很具体：
 *
 * - 断网瞬间：WS 可能还没察觉（心跳最长 10s 才判死），界面仍显示「已连接」，用户以为一切正常，
 *   发出的消息石沉大海；
 * - 网络恢复瞬间：**最长要等满 10 秒**才被下一次定时自检捞到，界面在这段时间里持续显示旧的运行态；
 *   用户看到的就是"网回来了但界面还卡着"。
 *
 * 浏览器给的 `online`/`offline` 是**最及时的信号**（恢复时早于任何定时器），所以应当：
 * ① 恢复时**立即**自检收敛（不等定时器）；
 * ② 断开时**立即如实**把状态标成断开（不谎报已连接），并让 SDK 主动断开以便走退避重连
 *    ——否则会出现"网早没了、WS 对象还 OPEN、心跳攒着不判死"的半死连接。
 *
 * ## 为什么不只依赖 navigator.onLine
 * `navigator.onLine === true` 只说明**有网络接口**，不代表能连到服务端（公司网需要认证、
 * 代理挂了、服务端重启中）。所以它只用作**提前量**：真值仍以自检（`client.verify()`）的
 * 服务端应答为准。这条边界写在这里，免得有人把 onLine 当权威。
 */

/** 网络事件类型。 */
export type NetEvent = "online" | "offline"

/** 一次网络事件应当触发的动作。 */
export interface NetAction {
  /** 是否立即自检（向服务端核对真相）。 */
  verify: boolean
  /** 是否让 SDK 主动断开（走退避重连）。 */
  dropConnection: boolean
  /** 状态栏文案（null = 不改动）。 */
  label: string | null
  /** 文案是否为"异常态"（决定颜色/呼吸动画）。 */
  warn: boolean
}

/**
 * 网络事件 → 动作。
 *
 * `offline`：立即标断开 + 主动断连。为何要主动断：浏览器判定离线后，WS 对象往往仍是 OPEN，
 * 心跳定时器也照跑，服务端那头却早已收不到——这种"半死连接"不会自己 close，
 * 只能主动 `close()` 逼它走 `onclose → 退避重连`；网络恢复后由退避重连 + online 自检双保险接回。
 *
 * `online`：立即自检。此时 WS 可能还在重连中（自检内部会先 ping，不通就判死继续退避），
 * 也可能已重连成功但事件补偿还没跑——自检把两条路径都收敛到服务端真值。
 * 文案不写"已连接"：是否真的连上由自检结果决定，先如实写"恢复中"。
 */
export function actionForNetEvent(ev: NetEvent): NetAction {
  if (ev === "offline") {
    return { verify: false, dropConnection: true, label: "网络已断开，等待恢复…", warn: true }
  }
  return { verify: true, dropConnection: false, label: "网络已恢复，正在核对状态…", warn: false }
}

/**
 * 是否值得为一次自检结果重写状态栏文案。
 *
 * 为何要这个判据：自检每 10 秒跑一次，若每次都写一遍文案，状态栏会持续被重写——
 * 而 `setConn` 会触发一次 DOM 写入与布局，且在"正在显示具体错误信息"（如导出失败）
 * 时把那条信息冲掉。所以只在**连接态确实变化**时才写。
 */
export function shouldRewriteConnLabel(prev: string, next: string): boolean {
  return prev !== next
}

/** 网络事件的浏览器来源（可注入，便于无 DOM 环境测试）。 */
export interface NetSource {
  addEventListener: (type: string, fn: () => void) => void
  removeEventListener: (type: string, fn: () => void) => void
  onLine?: boolean
}

/**
 * 订阅网络连通性变化。
 *
 * 返回解绑函数（与 `fw/dom.ts` 的 `on()` 同约定），便于组件/页面销毁时回收——
 * 这类全局监听忘了解绑，会在页面反复初始化时堆叠，一次断网触发 N 次自检。
 */
export function bindNetworkEvents(
  source: NetSource | null | undefined,
  handler: (ev: NetEvent) => void,
): () => void {
  if (!source || typeof source.addEventListener !== "function") return () => {}
  const onOnline = (): void => handler("online")
  const onOffline = (): void => handler("offline")
  source.addEventListener("online", onOnline)
  source.addEventListener("offline", onOffline)
  return () => {
    source.removeEventListener?.("online", onOnline)
    source.removeEventListener?.("offline", onOffline)
  }
}
