/**
 * @plantuml/core 类型声明（服务端后端渲染用；与前端 web 的 plantuml-core.d.ts 独立）：
 * 包本身无类型定义（TeaVM 编译产物，仅导出 render/renderToString）。
 */
declare module "@plantuml/core" {
  export function renderToString(lines: string[], onSuccess: (svg: string) => void, onError: (msg: string) => void): void
  export function render(lines: string[], targetId: string, opts?: { dark?: boolean }): void
}

declare module "@plantuml/core/viz-global.js" {
  /** UMD 副作用模块：注入 globalThis.Viz（Graphviz WASM 工厂）。 */
  const _: void
  export default _
}
