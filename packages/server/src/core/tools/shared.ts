/** tools/ 目录内部共享助手与注册契约（自 core/tools.ts 拆分）。 */
import type { ToolSchema } from "@gebai/sdk"
import type { Tool } from "../base/types"

export function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

/** 全局工具注册条目（tools/ 目录文件的零注册契约）：凡导出 `export const globalTools: GlobalToolEntry[]`
 *  的文件即被聚合器（tools/index.ts）自动收集——dev 形态运行时扫描本目录（重启生效），dist/--compile
 *  形态由 scripts/build-tools.ts 构建期生成静态注册表（tools/bundle.generated.ts）。新增全局工具 = 新文件，
 *  不改任何中央注册表（与子 Agent 同款扩展模型）。 */
export interface GlobalToolEntry {
  /** 全局工具名（[a-z][a-z0-9_]*，须与 Tool.name 一致；重名在聚合时抛错） */
  name: string
  /** 工具定义；工厂形式（`() => Tool`）在聚合时实例化（todo 等带闭包状态的工具） */
  tool: Tool | (() => Tool)
  /** projectAware 包装声明（DESIGN「项目机制」，聚合器统一施加）：true=默认会话相对路径；
   *  "workdir"=附 workdir 参数（sh/py）；缺省不包装。 */
  project?: boolean | "workdir"
}
