declare module "@plantuml/core" {
  /** 渲染进 DOM 元素（异步）；lines 为按行拆分的 PlantUML 源码。 */
  export function render(lines: string[], targetId: string, opts?: { dark?: boolean }): void
  /** 以回调方式交付 SVG 字符串；错误消息经 onError 回调。 */
  export function renderToString(lines: string[], onSuccess: (svg: string) => void, onError: (msg: string) => void): void
}
