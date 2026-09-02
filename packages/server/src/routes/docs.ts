/** OpenAPI 文档路由：/api/docs（公开，无敏感信息，便于集成方调试）。 */
import type { RouteCtx } from "./context"

export function registerDocsRoutes(rc: RouteCtx): void {
  const { app } = rc

  app.get("/api/docs", (c) =>
    c.json({
      openapi: "3.0.3",
      info: { title: "歌白智能体 API", version: "1.0.0", description: "歌白智能体 REST API（WebSocket 实时通道见 /ws）" },
      paths: {
        "/api/health": { get: { summary: "健康检查", responses: { "200": { description: "ok" } } } },
        "/api/v1/auth/login": { post: { summary: "登录（多用户模式）", responses: { "200": { description: "token" } } } },
        "/api/v1/auth/logout": { post: { summary: "登出", responses: { "200": { description: "ok" } } } },
        "/api/v1/auth/exchange": { post: { summary: "外部身份兑换令牌（同源集成扩展点）", responses: { "200": { description: "token+user" } } } },
        "/api/v1/auth/external-config": { get: { summary: "外部身份扩展点探测", responses: { "200": { description: "enabled/storageKey/autocreate" } } } },
        "/api/v1/users": { get: { summary: "用户列表（管理员）" }, post: { summary: "创建用户（管理员）" } },
        "/api/v1/sessions": { get: { summary: "会话列表" }, post: { summary: "创建会话" } },
        "/api/v1/sessions/{id}": { get: { summary: "会话详情" }, delete: { summary: "删除会话" }, patch: { summary: "重命名会话" } },
        "/api/v1/sessions/{id}/restore": { post: { summary: "从回收站恢复会话（GC 归档保留期内）" } },
        "/api/v1/sessions/{id}/prompt": { post: { summary: "发送消息（同步 JSON：等待任务完成返回最终回复；autoApprove 控制审批姿态）" } },
        "/api/v1/chat": { post: { summary: "单 HTTP 一站式对话（缺省自动建会话，带 sessionId 续聊；返回 sessionId+最终回复；autoApprove 支持自动审批）" } },
        "/api/v1/sessions/{id}/attachments": { post: { summary: "上传附件（multipart）" } },
        "/api/v1/sessions/{id}/cancel": { post: { summary: "取消任务" } },
        "/api/v1/sessions/{id}/approval": { post: { summary: "审批决策" } },
        "/api/v1/sessions/{id}/choice": { post: { summary: "选择决策（ask 选项询问分支）" } },
        "/api/v1/sessions/{id}/draw": { post: { summary: "画图渲染结果回传（show 图表分支）" } },
        "/api/v1/sessions/{id}/compact": { post: { summary: "主动压缩上下文" } },
        "/api/v1/sessions/{id}/truncate": { post: { summary: "截断会话消息" } },
        "/api/v1/sessions/{id}/env": { get: { summary: "会话环境变量" }, put: { summary: "设置会话环境变量" } },
        "/api/v1/sessions/{id}/todos": { get: { summary: "会话待办清单" } },
        "/api/v1/sessions/{id}/files": { get: { summary: "会话临时文件列表" } },
        "/api/v1/sessions/{id}/files/content": { get: { summary: "读取文件内容" } },
        "/api/v1/sessions/{id}/files/download": { get: { summary: "下载单文件" }, post: { summary: "多选打包下载（zip）" } },
        "/api/v1/sessions/{id}/files/preview": { get: { summary: "文件预览（会话相对/项目绝对路径，点击弹窗查看用；?render=office 返回 docx/xlsx/xlsm/pptx 阅读视图 HTML）" } },
        "/api/v1/tools": { get: { summary: "工具集查询" }, patch: { summary: "工具启停" } },
        "/api/v1/sub-agents": { get: { summary: "子Agent 能力列表" } },
        "/api/v1/feedback": { get: { summary: "反馈查询（管理员可全部）" }, post: { summary: "提交反馈" } },
        "/api/v1/webhooks": { get: { summary: "Webhook 列表" }, post: { summary: "注册 Webhook" } },
        "/api/v1/webhooks/{id}": { delete: { summary: "删除 Webhook" } },
        "/api/v1/mini-tools": { get: { summary: "HTML 小工具列表（公用 + 本人私有）" } },
        "/api/v1/mini-tools/{name}": { get: { summary: "读取 HTML 小工具（含源码）" }, delete: { summary: "删除 HTML 小工具（?scope=private|public）" } },
      },
    }),
  )
}
