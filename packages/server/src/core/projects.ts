import { isAbsolute, join, resolve } from "node:path"
import type { Tool, ToolContext } from "./types"
import { walkDirFiles } from "./tools"
import { resolveInSandbox } from "./paths"
import type { ToolSchema } from "@gebai/sdk"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

/** 保留项目名：会话工作区（tmp）——project 参数与 project 工具均可指定；预置项目名不得占用（启动校验拒绝）。 */
export const RESERVED_PROJECT_TMP = "tmp"

export const PROJECT_PARAM = {
  project: { type: "string", description: `项目名（预置清单项）、项目根路径（自由项目，绝对/相对均可）或保留名 ${RESERVED_PROJECT_TMP}（会话工作区——设定项目根后访问会话文件用）；传入时路径参数相对所选根解析（沙箱模式限定该根内）` },
}

/** 会话粘性项目根（DESIGN「项目机制」）：project 工具 action=use 设定后，本会话（含其派生的新会话执行，
 *  如 agent_run explore）内未传 project 参数的文件工具以该根为基准——自由路径项目不必每次重复传长路径。
 *  key = user:sessionId；受限模式（CODE_RESTRICT_PROJECTS=true）下 use 仅接受预置项目名（见 project 工具）。 */
const sessionProjectRoots = new Map<string, string>()

/** 测试用：清空会话粘性项目根表。 */
export function _resetSessionProjectRootsForTest(): void {
  sessionProjectRoots.clear()
}

/** 会话删除时释放粘性根条目（进程内无界增长防护，engine.forgetSession 调用；按 sessionId 后缀匹配
 *  ——key 为 user:sessionId，forgetSession 侧无 user 上下文，与 shTaskServices 清理同惯例）。 */
export function clearSessionProjectRoots(sessionId: string): void {
  for (const k of [...sessionProjectRoots.keys()]) {
    if (k.endsWith(`:${sessionId}`)) sessionProjectRoots.delete(k)
  }
}

/** project 参数值是否为路径形态（区别于预置项目名）：绝对路径或含路径分隔/以 . ~ 开头的相对路径。 */
function looksLikePath(v: string): boolean {
  return isAbsolute(v) || /^[.~]/.test(v) || /[/\\]/.test(v)
}

/** 解析 project 参数（预置项目名/路径形态/保留名 tmp，DESIGN「项目机制」）：保留名 tmp → 会话工作区
 *  （引擎注入的 sessionWorkdir，未注入时回退 ctx.workdir——新会话绑定项目根时两者不同，tmp 恒指会话工作区）。
 *  路径形态——本地模式绝对路径直用、相对按进程 cwd 解析；沙箱模式限定用户数据目录内（越界/绝对拒绝，
 *  与引擎预置项目根 resolveAgentProjectRoot 同规则）。非路径形态走 ctx.resolveProjectPath（预置项目名，
 *  未知名抛「未知预置项目」）。 */
export function resolveProjectRoot(project: string, ctx: ToolContext): string {
  if (project === RESERVED_PROJECT_TMP) return ctx.sessionWorkdir ?? ctx.workdir
  if (looksLikePath(project)) {
    if (ctx.sandboxed) return resolveInSandbox(join(ctx.home, "users", ctx.user), project)
    return isAbsolute(project) ? project : resolve(process.cwd(), project)
  }
  return ctx.resolveProjectPath(project)
}

/**
 * 为工具添加可选 project 参数（预置项目名/项目根路径/保留名 tmp）：传入时把路径解析基准与工作目录
 * 切换到所选根（沙箱模式限定该根内，本地模式不限制）；未传时若会话设定了粘性项目根（project 工具
 * action=use）则以该根为基准，否则行为不变。code/explore 等以项目为操作域的子Agent 复用
 * （ctx.projects/resolveProjectPath 由引擎聚合注入）；主会话装载子Agent 后与全局同名的工具
 * 经引擎会话注册表视图以本包装版呈现（同名合并，见 DESIGN「装载工具会话可见性」）。
 */
export function projectAware(tool: Tool, opts: { workdir?: boolean } = {}): Tool {
  const parameters = { ...tool.parameters, properties: { ...tool.parameters.properties, ...PROJECT_PARAM } }
  return {
    ...tool,
    parameters,
    async execute(args, ctx) {
      const project = args.project ? String(args.project) : ""
      const stickyRoot = !project ? sessionProjectRoots.get(`${ctx.user}:${ctx.sessionId}`) : undefined
      // 受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目——未传 project 且无粘性根/绑定根时，
      // 拒绝自由路径（粘性根设定时已按同规则校验：受限模式仅接受预置项目名）
      if (!project && !stickyRoot && ctx.env?.CODE_RESTRICT_PROJECTS === "true" && !ctx.boundProjectRoot) {
        return { output: "受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目（预置项目清单中的项目），请用 project 参数指定项目名后重试。" }
      }
      const root = project ? resolveProjectRoot(project, ctx) : stickyRoot
      if (!root) return tool.execute(args, ctx)
      const rest = { ...args }
      delete rest.project
      const pctx: ToolContext = {
        ...ctx,
        workdir: opts.workdir ? root : ctx.workdir,
        resolvePath: (p) => (ctx.sandboxed ? resolveInSandbox(root, p) : resolve(root, p)),
        listFiles: () => walkDirFiles(root),
      }
      return tool.execute(rest, pctx)
    },
  }
}

/** 会话项目设定（预置项目与自由路径项目统一入口，DESIGN「项目机制」）：action=use 设定会话默认项目根
 *  （project 参数同文件工具：预置项目名、项目根路径或保留名 tmp），此后未传 project 参数的文件工具以该根为基准——
 *  自由路径项目一次设定、后续调用免重复传根路径；action=list 查看预置清单与当前设定；action=clear 清除。 */
export const projectTool: Tool = {
  name: "project",
  description: `查看/设定会话默认项目根：use 设定后未传 project 参数的文件工具（read/edit/grep/sh 等）都以该根为基准，自由路径项目不必每次重复传根路径；list 查看预置项目清单与当前设定；clear 清除设定。目标项目可用预置项目名（预置清单项）、项目根路径或保留名 ${RESERVED_PROJECT_TMP}（切回会话工作区）。`,
  card: { titleParams: ["action", "project"] },
  parameters: schema(
    {
      action: { type: "string", enum: ["use", "list", "clear"], description: "操作（默认 list）" },
      project: { type: "string", description: `use 时的目标项目：预置项目名、项目根路径（绝对/相对）或保留名 ${RESERVED_PROJECT_TMP}（会话工作区）` },
    },
    [],
  ),
  outputSchema: schema(
    {
      current: { type: "string", description: "当前会话默认项目根（未设定为 null）" },
      presets: { type: "array", description: "预置项目清单", items: schema({ name: { type: "string" }, path: { type: "string" }, description: { type: "string" } }, ["name", "path"]) },
    },
    [],
  ),
  async execute(args, ctx) {
    const key = `${ctx.user}:${ctx.sessionId}`
    const action = String(args.action ?? "list")
    if (action === "clear") {
      const had = sessionProjectRoots.delete(key)
      return { output: had ? "已清除会话默认项目根（文件工具回到会话工作目录基准）。" : "当前未设定会话默认项目根。", data: { current: null, presets: ctx.projects } }
    }
    if (action === "list") {
      const cur = sessionProjectRoots.get(key)
      const presets = ctx.projects.length ? `预置项目:\n${ctx.projects.map((p) => `- ${p.name}${p.description ? `: ${p.description}` : ""}（${p.path}）`).join("\n")}` : "（无预置项目——预置清单未配置）"
      return {
        output: `${presets}\n保留项目名: ${RESERVED_PROJECT_TMP}（会话工作区——project 参数传 ${RESERVED_PROJECT_TMP} 可随时访问）\n${cur ? `当前会话默认项目根: ${cur}` : "当前未设定默认项目根（文件工具以会话工作目录为基准；用 project action=use 设定）"}`,
        data: { current: cur ?? null, presets: ctx.projects },
      }
    }
    const project = String(args.project ?? "")
    if (!project) return { output: "use 需要传 project 参数（预置项目名、项目根路径或保留名 tmp）。" }
    // 受限模式：仅允许设定预置清单内项目（与文件工具 project 参数同规则；绑定根会话不受限）
    if (ctx.env?.CODE_RESTRICT_PROJECTS === "true" && !ctx.boundProjectRoot && !ctx.projects.some((p) => p.name === project)) {
      return { output: "受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许设定预置清单内的预置项目。" }
    }
    let root: string
    try {
      root = resolveProjectRoot(project, ctx)
    } catch (err) {
      return { output: `项目根解析失败: ${(err as Error).message}` }
    }
    sessionProjectRoots.set(key, root)
    let existsNote = ""
    try {
      const { stat } = await import("node:fs/promises")
      const st = await stat(root)
      if (!st.isDirectory()) existsNote = "\n（注意：该路径不是目录）"
    } catch {
      existsNote = "\n（注意：该目录当前不存在——新建项目场景可忽略）"
    }
    return {
      output: `已设定会话默认项目根: ${root}${existsNote}\n（此后未传 project 参数的文件工具以该根为基准，相对路径在此根内解析；会话工作区文件用 project 参数传 ${RESERVED_PROJECT_TMP} 访问；清除用 action=clear，查看用 action=list）`,
      data: { current: root, presets: ctx.projects },
    }
  },
}
