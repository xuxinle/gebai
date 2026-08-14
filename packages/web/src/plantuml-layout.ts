/** PlantUML 布局兜底默认参数（纯函数，与服务端 `injectPlantUmlLayout` 同规则）：
 *  @startuml 类图源码未显式设置节点间距时注入 ranksep/nodesep（防密集图节点拥挤、连线杂乱）；
 *  其余图型（mindmap/wbs/gantt/salt/json/yaml 等）布局由结构决定，不注入。 */
export function injectPlantUmlLayout(code: string): string {
  const m = code.match(/@start(\w*)/)
  if (!m || m[1] !== "uml") return code
  if (/skinparam\s+(?:ranksep|nodesep)/i.test(code)) return code
  const layout = "skinparam ranksep 80\nskinparam nodesep 40"
  const endIdx = code.search(/@enduml\b/)
  if (endIdx < 0) return `${code.trimEnd()}\n${layout}\n@enduml`
  return `${code.slice(0, endIdx)}${layout}\n${code.slice(endIdx)}`
}
