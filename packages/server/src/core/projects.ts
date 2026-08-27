import { isAbsolute, join, resolve } from "node:path"
import type { Tool, ToolContext } from "./types"
import { walkDirFiles } from "./tools"
import { resolveInSandbox } from "./paths"

/** 保留项目名：会话工作区（tmp）——project 参数可指定；预置项目名不得占用（启动校验拒绝）。 */
export const RESERVED_PROJECT_TMP = "tmp"

export const PROJECT_PARAM = {
  project: { type: "string", description: `项目名（预置清单项）、项目根路径（自由项目，绝对/相对均可）或保留名 ${RESERVED_PROJECT_TMP}（会话工作区）；传入时路径参数相对所选根解析（沙箱模式限定该根内），未传时相对会话工作目录` },
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
 * 切换到所选根（沙箱模式限定该根内，本地模式不限制），未传时相对路径以会话工作目录为基准（行为不变）。
 * 全局文件工具（read/write/edit/patch/ls/grep/glob/file/diff/sh/py）经全局注册统一包装
 * （ctx.projects/resolveProjectPath 由引擎聚合注入）；code/explore 的独有工具（search_symbols/analyze/git/
 * preview_server）同规则包装。
 */
export function projectAware(tool: Tool, opts: { workdir?: boolean } = {}): Tool {
  const parameters = { ...tool.parameters, properties: { ...tool.parameters.properties, ...PROJECT_PARAM } }
  return {
    ...tool,
    parameters,
    async execute(args, ctx) {
      const project = args.project ? String(args.project) : ""
      // 受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目——未传 project 且无绑定根时，
      // 拒绝自由路径（绑定根会话的路径基准即项目根，不受限）
      if (!project && ctx.env?.CODE_RESTRICT_PROJECTS === "true" && !ctx.boundProjectRoot) {
        return { output: "受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目（预置项目清单中的项目），请用 project 参数指定项目名后重试。" }
      }
      if (!project) return tool.execute(args, ctx)
      const root = resolveProjectRoot(project, ctx)
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
