/** 工具执行作用域（AsyncLocalStorage）：透明浏览器代理 fetch 垫片的判定原语。
 *
 * 引擎在执行工具（全局工具/子Agent 工具，`runToolInterruptible` 统一收口）时进入作用域并携带
 * 当前会话 id——垫片（`sub-agents/playwright/fetch_proxy.ts`，GEBAI_BROWSER_PROXY=1 时安装）
 * 据此判定「本次 fetch 处于工具执行内」并路由到该会话的浏览器上下文；作用域外（LLM 请求/
 * webhook/调度/启动逻辑）不受影响。core 层只提供作用域原语，垫片实现位于 playwright 子Agent
 * （浏览器会话与桥接的属主），避免 core → sub-agents 反向依赖。
 *
 * 嵌套执行（agent_run 在工具内跑完整引擎）经 `runWithoutFetchProxy` 豁免——嵌套引擎的 LLM
 * 流式请求恒直连，不被外层工具作用域连带代理。 */
import { AsyncLocalStorage } from "node:async_hooks"

const toolFetchScope = new AsyncLocalStorage<{ sessionId: string } | null>()

/** 在工具执行作用域内执行 fn（携带 sessionId，异步链上全部继承）。 */
export function runInToolFetchScope<T>(sessionId: string, fn: () => Promise<T>): Promise<T> {
  return toolFetchScope.run({ sessionId }, fn)
}

/** 清除工具执行作用域执行 fn——LLM 请求等必须直连的网络调用豁免浏览器代理。 */
export function runWithoutFetchProxy<T>(fn: () => Promise<T>): Promise<T> {
  return toolFetchScope.run(null, fn)
}

/** 当前异步上下文所属的工具执行会话 id（作用域外/被豁免时 undefined）。 */
export function currentToolFetchSession(): string | undefined {
  return toolFetchScope.getStore()?.sessionId ?? undefined
}
