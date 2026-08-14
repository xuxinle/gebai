import { readFile } from "node:fs/promises"
import { dirname, join } from "node:path"
import type { FeedbackInfo, FeedbackInput } from "@gebai/sdk"
import { feedbackPath, walkDir } from "./core/paths"

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
 * 写入一条反馈（分片存储：{GEBAI_HOME}/users/{user}/feedback/YYYY-MM-DD/{h0}/{h1}/{id}.json）。
 * 返回反馈 id。REST 与 WS 通道共用，避免分片策略改版时漏改。
 */
export async function writeFeedback(home: string, userId: string, fb: FeedbackInput): Promise<string> {
  const { writeFile, mkdir } = await import("node:fs/promises")
  const id = crypto.randomUUID().replace(/-/g, "")
  const file = feedbackPath(home, userId, id)
  await mkdir(dirname(file), { recursive: true })
  await writeFile(file, JSON.stringify({ ...fb, id, userId, createdAt: Date.now() }, null, 2))
  return id
}
