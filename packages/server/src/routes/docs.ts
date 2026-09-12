/** OpenAPI 文档路由：/api/docs（公开，无敏感信息，便于集成方调试）。
 *
 * **端点表由路由注册自动生成**（遍历 `app.routes`），摘要取自下方 `SUMMARIES` 补充表——
 * 手写清单容易与实现漂移（新增端点忘记登记、删掉的端点仍列在文档里），生成器保证
 * 「有哪些端点」永远与代码一致，摘要只影响可读性。未登记摘要的端点仍会列出（method + path），
 * 响应说明留空并在 `x-summary-covered` 标注不完整，便于集成方据实判断。
 */
import type { RouteCtx } from "./context"

/** 摘要补充表（键为 OpenAPI 形态路径，即 `:param` 已转 `{param}`）。 */
const SUMMARIES: Record<string, Record<string, string>> = {
  "/api/health": { get: "健康检查（探活；含进程启动标识 bootId）" },
  "/api/docs": { get: "OpenAPI 文档（端点表由路由注册自动生成）" },
  "/api/v1/auth/login": { post: "登录（服务模式）" },
  "/api/v1/auth/register": { post: "注册（服务模式，受 GEBAI_SIGNUP_MODE 控制）" },
  "/api/v1/auth/logout": { post: "登出" },
  "/api/v1/auth/me": { get: "当前登录用户" },
  "/api/v1/auth/exchange": { post: "外部身份兑换令牌（同源集成扩展点）" },
  "/api/v1/auth/external-config": { get: "外部身份扩展点探测" },
  "/api/v1/users": { get: "用户列表（管理员）", post: "创建用户（管理员）" },
  "/api/v1/users/{id}": { patch: "更新用户（管理员）", delete: "删除用户（管理员）" },
  "/api/v1/sessions": { get: "会话列表", post: "创建会话" },
  "/api/v1/sessions/{id}": { get: "会话详情", delete: "删除会话", patch: "重命名/置顶会话" },
  "/api/v1/sessions/{id}/restore": { post: "从回收站恢复会话（GC 归档保留期内）" },
  "/api/v1/sessions/{id}/prompt": { post: "发送消息（同步 JSON：等待任务完成返回最终回复；autoApprove 控制审批姿态）" },
  "/api/v1/chat": { post: "单 HTTP 一站式对话（缺省自动建会话，带 sessionId 续聊；返回 sessionId+最终回复）" },
  "/api/v1/sessions/{id}/attachments": { post: "上传附件（multipart）" },
  "/api/v1/sessions/{id}/cancel": { post: "取消任务" },
  "/api/v1/sessions/{id}/approval": { post: "审批决策" },
  "/api/v1/sessions/{id}/choice": { post: "选择决策（ask 选项询问分支）" },
  "/api/v1/sessions/{id}/draw": { post: "画图渲染结果回传（show 图表分支）" },
  "/api/v1/sessions/{id}/compact": { post: "主动压缩上下文" },
  "/api/v1/sessions/{id}/truncate": { post: "截断会话消息" },
  "/api/v1/sessions/{id}/env": { get: "会话环境变量", put: "设置会话环境变量" },
  "/api/v1/sessions/{id}/todos": { get: "会话待办清单" },
  "/api/v1/sessions/{id}/files": { get: "会话临时文件列表" },
  "/api/v1/sessions/{id}/files/content": { get: "读取文件内容" },
  "/api/v1/sessions/{id}/files/download": { get: "下载单文件", post: "多选打包下载（zip）" },
  "/api/v1/sessions/{id}/files/preview": { get: "文件预览（会话相对/项目绝对路径；?render=office 返回 Office 阅读视图 HTML）" },
  "/api/v1/env/catalog": { get: "环境变量目录（前端面板白名单）" },
  "/api/v1/tools": { get: "工具集查询", patch: "工具启停" },
  "/api/v1/sub-agents": { get: "子Agent 能力列表" },
  "/api/v1/feedback": { get: "反馈查询（管理员可全部）", post: "提交反馈" },
  "/api/v1/webhooks": { get: "Webhook 列表", post: "注册 Webhook" },
  "/api/v1/webhooks/{id}": { delete: "删除 Webhook" },
  "/api/v1/cron": { get: "定时任务清单", post: "创建定时任务" },
  "/api/v1/cron/{id}": { patch: "修改定时任务", delete: "删除定时任务" },
  "/api/v1/cron/{id}/run": { post: "手动触发定时任务" },
  "/api/v1/todos": { get: "用户级待办清单", post: "新增待办", patch: "清单级重排" },
  "/api/v1/todos/{id}": { patch: "更新待办", delete: "删除待办" },
  "/api/v1/todos/{id}/run": { post: "立即执行待办（新建会话）" },
  "/api/v1/oauth/feishu/callback": { get: "飞书用户授权回调（写回会话令牌）" },
  "/api/v1/roots": { get: "文件工作台根清单" },
  "/api/v1/roots/resolve": { post: "根解析（路径 → 所属根）" },
  "/api/v1/fs/list": { get: "目录列举" },
  "/api/v1/fs/tree": { get: "目录树快照" },
  "/api/v1/fs/stat": { get: "路径元信息（含 etag）" },
  "/api/v1/fs/read": { get: "文本读取" },
  "/api/v1/fs/raw": { get: "原始字节读取" },
  "/api/v1/fs/office": { get: "Office 阅读视图转换" },
  "/api/v1/fs/archive": { get: "压缩包条目列表" },
  "/api/v1/fs/search": { get: "内容搜索" },
  "/api/v1/fs/download": { get: "文件下载" },
  "/api/v1/fs/write": { put: "写入文件（etag 乐观锁）" },
  "/api/v1/fs/mkdir": { post: "新建目录" },
  "/api/v1/fs/rename": { post: "重命名" },
  "/api/v1/fs/move": { post: "移动" },
  "/api/v1/fs/copy": { post: "复制" },
  "/api/v1/fs/delete": { post: "删除（进回收站）" },
  "/api/v1/fs/upload": { post: "上传" },
  "/api/v1/fs/archive/extract": { post: "解压" },
  "/api/v1/fs/trash": { get: "回收站列表" },
  "/api/v1/fs/trash/restore": { post: "回收站恢复" },
  "/api/v1/fs/trash/purge": { post: "回收站彻底清除" },
  "/api/v1/git/status": { get: "工作区状态" },
  "/api/v1/git/diff": { get: "变更差异" },
  "/api/v1/git/compare": { get: "任意两端对比" },
  "/api/v1/git/file-diff": { get: "单文件差异" },
  "/api/v1/git/content": { get: "端点内容（并列视图两侧文本）" },
  "/api/v1/git/show": { get: "指定提交的文件内容" },
  "/api/v1/git/log": { get: "提交日志" },
  "/api/v1/git/commit": { get: "提交详情", post: "创建提交" },
  "/api/v1/git/commit-file-diff": { get: "提交内单文件差异" },
  "/api/v1/git/file-history": { get: "单文件历史" },
  "/api/v1/git/blame": { get: "逐行追溯" },
  "/api/v1/git/refs": { get: "引用（分支/标签/远程/暂存/冲突）" },
  "/api/v1/git/branches": { get: "分支列表" },
  "/api/v1/git/branch": { post: "分支写操作（建/删/改名/切换）" },
  "/api/v1/git/tags": { get: "标签列表" },
  "/api/v1/git/tag": { post: "标签写操作" },
  "/api/v1/git/remotes": { get: "远程列表" },
  "/api/v1/git/remote": { post: "远程写操作" },
  "/api/v1/git/repos": { get: "仓库清单（工作台根下的仓库）" },
  "/api/v1/git/stash": { get: "暂存区列表", post: "暂存区写操作" },
  "/api/v1/git/ignore": { post: "写入 .gitignore" },
  "/api/v1/git/conflicts/resolve": { post: "解决冲突（按块取侧）" },
  "/api/v1/git/conflicts": { get: "冲突列表" },
  "/api/v1/git/stage": { post: "暂存" },
  "/api/v1/git/unstage": { post: "取消暂存" },
  "/api/v1/git/discard": { post: "丢弃改动" },
  "/api/v1/git/checkout": { post: "切换分支/检出" },
  "/api/v1/git/merge": { post: "合并" },
  "/api/v1/git/rebase": { post: "变基" },
  "/api/v1/git/cherry-pick": { post: "拣选提交" },
  "/api/v1/git/revert": { post: "回滚提交" },
  "/api/v1/git/reset": { post: "重置" },
  "/api/v1/git/fetch": { post: "拉取远程引用" },
  "/api/v1/git/pull": { post: "拉取" },
  "/api/v1/git/push": { post: "推送" },
  "/api/v1/git/init": { post: "初始化仓库" },
}

/** Hono 路由的路径参数形态 `:id` → OpenAPI 的 `{id}`；根路径保持 `/`。 */
export function openApiPath(path: string): string {
  return path.replace(/:([A-Za-z0-9_]+)/g, "{$1}")
}

/** 从已注册路由收集 OpenAPI paths（只取 `/api/` 域；`HEAD` 由 Hono 自动派生，跳过）。 */
export function collectPaths(routes: ReadonlyArray<{ path: string; method: string }>): {
  paths: Record<string, Record<string, unknown>>
  total: number
  covered: number
} {
  const paths: Record<string, Record<string, unknown>> = {}
  let total = 0
  let covered = 0
  for (const r of routes) {
    const method = String(r.method || "").toLowerCase()
    if (!r.path.startsWith("/api/") || method === "head" || method === "all") continue
    const p = openApiPath(r.path)
    const summary = SUMMARIES[p]?.[method]
    const entry: Record<string, unknown> = {}
    if (summary) {
      covered++
      entry.summary = summary
      entry.responses = { "200": { description: "ok" } }
    } else {
      entry.description = "（未登记摘要）"
      entry.responses = { "200": { description: "ok" } }
    }
    const slot = (paths[p] ??= {})
    if (!slot[method]) slot[method] = entry
    total++
  }
  return { paths, total, covered }
}

export function registerDocsRoutes(rc: RouteCtx): void {
  const { app } = rc

  app.get("/api/docs", (c) => {
    // 惰性收集：请求时读 app.routes，故注册顺序无关（docs 之后仍有 static 等注册）
    const { paths, total, covered } = collectPaths(app.routes as unknown as Array<{ path: string; method: string }>)
    return c.json({
      openapi: "3.0.3",
      info: {
        title: "歌白智能体 API",
        version: "1.0.0",
        description: `歌白智能体 REST API（端点表由路由注册自动生成；WebSocket 实时通道见 /ws）。摘要覆盖 ${covered}/${total}。`,
      },
      "x-endpoints-total": total,
      "x-summary-covered": covered,
      paths,
    })
  })
}
