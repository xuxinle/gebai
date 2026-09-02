/** 编排类全局工具（flow/tool_schemas）——自 core/tools.ts 按域拆分。 */
import type { Tool } from "../base/types"
import { runFlow, scanFlowApprovals } from "../exec/flow"
import { schema, type GlobalToolEntry } from "./shared"

export function makeFlowTool(): Tool {
  const flow: Tool = {
    name: "flow",
    description:
      "数据流编排：一次调用执行多步工具链（把工具视为函数做动态编程），减少与模型的往返与词元消耗。**适用边界**：本工具是声明式管道，只做稳定的固定流程编排（引用映射/条件分支/循环）；超出表达式语言能力的高阶逻辑（复杂字符串/数据变换、参数需动态计算、错误捕获与分支恢复、条件重试策略、跨步骤聚合统计）**一律改用 js 工具**（完整 JS 语言能力，工具同样像内置函数直接调用），不要在 flow 里硬凑复杂表达式。支持：\n" +
      "- **步骤**：steps 为工具步骤或循环分组列表（分组须声明 foreach 或 while）。\n" +
      "- **引用**：`{{s1.data.xxx}}` 引用步骤结构化输出（各工具 data 结构先用 tool_schemas 批量查询），`{{s1.output}}` 引用文本输出。params 值恰为一个 `{{表达式}}` 时保留原始类型（数字/数组/对象原样传递），混排字符串按文本拼接——**字符串值混排进脚本源码须自带引号**（写 `'item:{{item}}'`，裸拼 `'item:' + {{item}}` 会把值当标识符、执行报错）；**多行字段（如 data.stdout）混排会带入换行**，拼进脚本源码字符串会语法错误：多行文本建议走 stdin（input 参数）或三引号包裹，引用单值字段（exitCode/length）不受影响；**对象/数组直接映射给 sh/py 的 input 参数时以 JSON 文本（双引号）传入 stdin，脚本 `json.loads` 可直接解析**（不会出现 Python repr 单引号形态）。根名：步骤 id（缺省自动编号 s1/s2…）、`prev`（上一实际执行步骤）、`item`/`index`（foreach 当前项/序号）、`iteration`（while 轮次）、`input`（flow 参数 input）。路径访问 `.字段`/`[下标]`/`.length`。\n" +
      "- **input 显式映射**：`{ 目标参数名: \"{{源}}\" }`，解析后覆盖 params 同名字段并抑制自动注入——字段改名、多对一汇聚（多个步骤输出映射进同一工具的不同参数）都用它表达；**非对象形式**（如 `input: \"{{item}}\"`）等价于 `{ input: \"{{item}}\" }`，直接给工具的 input 参数传值（脚本 stdin）。\n" +
      "- **when 条件分支**：表达式为假时跳过该步（不中断）。支持两种写法（等价）：裸表达式 `gen.data.ok == true` 或 `{{表达式}}` 包裹/混排 `{{gen.data.ok}} == true`。运算：`==`/`!=`/`>`/`>=`/`<`/`<=`、`&&`/`||`/`!`、括号、函数 `len()`/`contains()`/`exists()`；空数组视为假；引用不存在的路径解析为 undefined/空串（不报错，需判定存在性用 `exists()`）。\n" +
      "- **foreach 数据循环**（一对多扇出）：值为数组（逐项——**可直接写 JSON 数组文本如 `\"[1,2,3]\"`**，或表达式/`{{引用}}` 求值为数组，脚本 stdout 的 JSON 数组文本同样自动解析）或正整数（按次），体内经 `{{item}}`/`{{index}}` 引用；**快照语义**——迭代次数固定为求值时的长度，循环体修改源数组不影响遍历；**嵌套时内层 `{{item}}`/`{{index}}` 遮蔽外层同名引用**（外层值需提前映射到中间步骤）；组结果 data = 每轮末步 data 的数组。\n" +
      "- **while 条件循环**（do-while：先执行一轮再判断，条件可引用本组最新结果如 `{{g.data.exitCode}}`，适合重试直到成功）：为真继续下一轮，达上限停止；`maxLoops` 默认 10、上限 50；需前置判断时配 when。**组 data = 最后一轮末步的 data（单轮结果，非数组，无 `.length`）**——`iteration` 引用轮次，`{{g.data.xxx}}` 始终取最新轮结果。\n" +
      "- **失败语义**：**工具级异常才中断** flow（调用不存在的工具、strict 脚本非 0 退出等），错误信息含失败位置、原因与**已执行步骤清单**（判断副作用、安全续接重试用）；sh/py **非 0 退出码默认是正常结果**（exitCode 在 data，可用 when 判定，或给该步传 strict: true 转为中断）；`optional: true` 的步骤任何失败（执行失败/参数模板错误等）都不中断（记录错误继续）。\n" +
      "- **自动注入**（未显式 input 映射时保留旧版语义）：当前步为脚本工具（sh/py）时上一步输出经 stdin（input 参数）传入；其余工具按参数名映射注入上一步 JSON 输出，兜底注入 input 参数。注入物恒为上一步**文本输出**——上一步为 js 时即日志 + `[返回值]` 前缀的混排文本（非干净 JSON），下游要干净结果请显式映射 `{{id.data.result}}`。\n" +
      "- **结果透传**：内部工具产生的图片/图表/文件等富内容块去重限量（10 个）透传到 flow 结果（与 js 编排一致，UI/模型可见，编排 show/图片类工具产物不丢）；`agent_run` 步骤的新会话存档透传到本工具调用记录供历史回放（多个 agent_run 步骤保留最后一个）。\n" +
      "- **审批与安全**：内部任一工具需审批则整个 flow 提交一次审批（通过后依次执行）；安全模式下风险工具在 step 层同规则拦截。\n" +
      "- **规模上限**：单次工具调用总数 ≤ 100、foreach ≤ 50 项、分组嵌套 ≤ 4 层；超限请拆分多次调用。",
    requiresApproval: (args, ctx) => scanFlowApprovals(args.steps, ctx),
    parameters: schema(
      {
        steps: {
          type: "array",
          description: "步骤列表：工具步骤 { id?, tool, params?, input?, when?, optional? } 或循环分组 { id?, foreach|while, maxLoops?, steps }",
          items: { type: "object", properties: { tool: { type: "string" }, params: { type: "object" } }, required: ["tool"] },
        },
        input: { description: "初始输入（任意类型），步骤中经 {{input}} 引用" },
        timeout: { type: "number", description: "flow 整体执行超时（秒）：步骤间累计时间超限即中止并返回已执行部分（防循环失控）；缺省不限制；单步慢请用各工具自身 timeout 参数" },
      },
      ["steps"],
    ),
    async execute(args, ctx) {
      return runFlow({ steps: args.steps, input: args.input, timeout: args.timeout }, ctx)
    },
  }
  return flow
}
/** 批量获取工具的输入参数与结构化输出 schema（DESIGN「工具双输出」）：编排（flow）前理解输出结构，避免逐个试调。 */
export const toolSchemasTool: Tool = {
  name: "tool_schemas",
  description:
    "批量获取工具的输入参数 schema 与结构化输出（data）schema。编写 flow 数据流编排前先用本工具了解相关工具的输出结构（引用 {{步骤id.data.字段}} 的前提）。tools 传工具名列表（可含子Agent 命名空间工具，如 code_read）；省略时返回全部已启用工具的输出结构概要（不含输入参数，紧凑一行一个）。无 outputSchema 的工具仅有文本 output（无结构化 data 可引用）。",
  parameters: schema({
    tools: { type: "array", items: { type: "string" }, description: "工具名列表（省略 = 全部已启用工具的输出概要）" },
  }),
  async execute(args, ctx) {
    const all = ctx.registry.schemas()
    const names = Array.isArray(args.tools) ? args.tools.map(String).filter(Boolean) : []
    if (!names.length) {
      const lines = all.map((s) => {
        const os = ctx.registry.resolve(s.name)?.tool.outputSchema
        return `- ${s.name}: ${os ? JSON.stringify(os) : "（仅文本 output，无结构化 data）"}`
      })
      return { output: `已启用工具（${all.length} 个）的输出结构：\n${lines.join("\n")}`, data: { tools: all.map((s) => ({ name: s.name, outputSchema: ctx.registry.resolve(s.name)?.tool.outputSchema ?? null })) } }
    }
    const entries = names.map((name) => {
      const s = all.find((x) => x.name === name)
      if (!s) return { name, error: "未知或未启用的工具" }
      return { name, description: s.description, parameters: s.parameters, outputSchema: ctx.registry.resolve(name)?.tool.outputSchema ?? null }
    })
    return { output: JSON.stringify(entries, null, 2), data: { tools: entries } }
  },
}

export const globalTools: GlobalToolEntry[] = [
  { name: "flow", tool: () => makeFlowTool() },
  { name: "tool_schemas", tool: toolSchemasTool },
]
