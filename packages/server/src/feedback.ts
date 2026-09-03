import { readFile } from "node:fs/promises"
import { dirname, join } from "node:path"
import type { FeedbackInfo, FeedbackInput } from "@gebai/sdk"
import { feedbackPath, walkDir } from "./core/base/paths"

/** 递归扫描反馈分片目录，返回该用户全部反馈（按时间倒序）。 */
export async function readFeedback(home: string, userId: string): Promise<FeedbackInfo[]> {
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

/**
 * 反馈上下文关联（DESIGN「用户反馈」：自动关联使用的模型与子Agent）：按反馈指向的会话记录取
 * model（消息落盘时由 provider capabilities 携带）与 subAgent（会话当前装载的子Agent 名单）。
 * 尽力关联不阻断提交——会话不存在/已删除/读取异常时返回空对象，反馈照常写入。
 */
export async function feedbackContext(
  store: Pick<import("./core/session/store").SessionStore, "load">,
  userId: string,
  fb: Pick<FeedbackInput, "sessionId" | "messageId">,
): Promise<{ model?: string; subAgent?: string }> {
  try {
    const session = await store.load(fb.sessionId, userId)
    if (!session) return {}
    const out: { model?: string; subAgent?: string } = {}
    const msg = session.messages.find((m) => m.id === fb.messageId)
    if (msg?.model) out.model = msg.model
    if (session.loadedSubAgents?.length) out.subAgent = session.loadedSubAgents.join(",")
    return out
  } catch {
    return {}
  }
}

/**
 * 写入一条反馈（分片存储：{GEBAI_HOME}/users/{user}/feedback/YYYY-MM-DD/{h0}/{h1}/{id}.json）。
 * ctx 为 feedbackContext 关联出的 model/subAgent（可缺省）。返回反馈 id。
 * REST 与 WS 通道共用，避免分片策略改版时漏改。
 */
export async function writeFeedback(
  home: string,
  userId: string,
  fb: FeedbackInput,
  ctx: { model?: string; subAgent?: string } = {},
): Promise<string> {
  const { writeFile, mkdir } = await import("node:fs/promises")
  const id = crypto.randomUUID().replace(/-/g, "")
  const file = feedbackPath(home, userId, id)
  await mkdir(dirname(file), { recursive: true })
  await writeFile(
    file,
    JSON.stringify({ ...fb, id, userId, createdAt: Date.now(), ...ctx.model ? { model: ctx.model } : {}, ...ctx.subAgent ? { subAgent: ctx.subAgent } : {} }, null, 2),
  )
  return id
}
