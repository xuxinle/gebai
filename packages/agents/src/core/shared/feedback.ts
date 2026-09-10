/** 用户反馈查询工具（反馈闭环；自 server core/tools/extras.ts 迁入 agents——self_optimize 域）：
 *  读取 {GEBAI_HOME}/users/{user}/feedback/ 分片 JSON（按时间倒序），反馈数据进入 Agent 上下文。
 *  存储布局与 server feedback.ts 同源（写入端在引擎 REST/WS，读取端在此）。 */
import { readFile } from "node:fs/promises"
import { join } from "node:path"
import type { FeedbackInfo, Tool } from "@gebai/sdk"
import { schema, walkDir } from "@gebai/sdk/node"

async function readFeedback(home: string, userId: string): Promise<FeedbackInfo[]> {
  const base = join(home, "users", userId, "feedback")
  const out: FeedbackInfo[] = []
  await walkDir(base, 5, async (p) => {
    if (!p.endsWith(".json")) return
    try {
      const fb = JSON.parse(await readFile(p, "utf8")) as FeedbackInfo
      if (fb.id) out.push(fb)
    } catch {
      /* 跳过损坏文件 */
    }
  })
  return out.sort((a, b) => b.createdAt - a.createdAt)
}

export const readFeedbackTool: Tool = {
  name: "read_feedback",
  description:
    "读取用户提交的反馈（本用户，按时间倒序）。用于自我优化等场景了解用户对既往输出的评价（点赞/点踩/文字反馈/建议）与改进点。",
  parameters: schema(
    {
      limit: { type: "integer", description: "返回条数（默认 10，上限 50）" },
      session_id: { type: "string", description: "可选：仅返回该会话的反馈" },
    },
  ),
  outputSchema: schema({
    items: {
      type: "array",
      description: "反馈列表（按时间倒序）",
      items: schema({
        type: { type: "string", description: "thumbs_up/thumbs_down/suggestion/text" },
        createdAt: { type: "integer", description: "毫秒时间戳" },
        sessionId: { type: "string" },
        messageId: { type: "string" },
        label: { type: "string" },
        subAgent: { type: "string" },
        text: { type: "string" },
      }, ["type", "createdAt"]),
    },
  }, ["items"]),
  async execute(args, ctx) {
    const list = await readFeedback(ctx.home, ctx.user)
    const filtered = args.session_id ? list.filter((f) => f.sessionId === String(args.session_id)) : list
    const n = Math.min(Math.max(Number(args.limit ?? 10) || 10, 1), 50)
    const items = filtered.slice(0, n)
    if (!items.length) return { output: args.session_id ? `该会话暂无反馈记录。` : "暂无反馈记录。", data: { items: [] } }
    const label = (t: string) => (t === "thumbs_up" ? "👍" : t === "thumbs_up" ? "👍" : t === "thumbs_down" ? "👎" : t === "suggestion" ? "建议" : "文字")
    return {
      output:
        `用户反馈（最近 ${items.length} 条${args.session_id ? `，会话 ${String(args.session_id)}` : ""}）：\n` +
        items
          .map((f) => {
            const parts = [
              `- [${new Date(f.createdAt).toISOString().slice(0, 19).replace("T", " ")}] ${label(f.type)}`,
              f.sessionId ? `会话 ${f.sessionId}` : "",
              f.messageId ? `消息 ${f.messageId}` : "",
              f.label ? `标签 ${f.label}` : "",
              f.subAgent ? `子Agent ${f.subAgent}` : "",
            ].filter(Boolean)
            const body = f.text ? `\n  ${f.text.slice(0, 500)}` : ""
            return `${parts.join("，")}${body}`
          })
          .join("\n"),
      data: { items },
    }
  },
}
