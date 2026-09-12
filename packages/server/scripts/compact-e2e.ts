/**
 * 上下文压缩「接续质量」端到端验证（真实模型）：`bun run scripts/compact-e2e.ts`
 *
 * 为何需要独立脚本：压缩的单元/集成测试只覆盖「摘要输入构造正确」，但**接续质量**（压缩后模型
 * 还能不能接着干活、摘要是否保住了任务目标/文件路径/未完成事项）只能拿真实模型量。本脚本把这件事
 * 变成可重复的对照实验：
 *   A 基线会话——同样的长历史，不压缩直接问（确认测试本身有效：不问压缩也能答对）
 *   B 压缩会话——先压缩掉含「任务规格」的早期区间，再问同样的问题（检查摘要是否保住三要素）
 *
 * 判据：B 的回复应复述出「任务目标 / 两个文件路径 / 两条未完成事项」（正则弱匹配，避免措辞差异误判）；
 * 同时打印生成的摘要，便于评估 `SUMMARY_ITEM_LIMIT` 与块预算是否需要调整。
 *
 * 环境：读仓库 `.env` 的真实 LLM 配置（脚本调试模式按仓库根加载 `.env`；仅 `GEBAI_HOME` 指向临时目录
 * 做数据隔离，不碰真实用户数据）。历史规模用 `COMPACT_E2E_LINES` 覆盖（缺省 320 段 ≈ 7 万字符；
 * 小值用于冒烟，见 `COMPACT_E2E_LINES=60`）。
 */
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

const home = mkdtempSync(join(tmpdir(), "gebai-compact-e2e-"))
process.env.GEBAI_HOME = home
process.env.GEBAI_CRON_ENABLED = "false"
process.env.GEBAI_IDLE_TODO_ENABLED = "false"
process.env.GEBAI_GC_DISABLED = "1"
process.env.GEBAI_FEISHU_BOT_ENABLED = "false"
process.env.GEBAI_SCHEDULER = "off"

const { composeServer } = await import("../src/boot/compose")

const USER = "admin"
const LINES = Math.max(10, Number(process.env.COMPACT_E2E_LINES) || 320)

/** 埋入早期历史的「任务规格」——压缩后必须仍能被复述。 */
const GOAL = "重构 clipwin 的增量同步模块，把全量扫描改成基于 mtime 的增量 diff"
const FILE_A = "src/sync/incremental.ts"
const FILE_B = "src/sync/watcher.ts"
const PENDING_1 = "watcher 的 debounce 阈值还没接入配置项"
const PENDING_2 = "增量索引的单元测试只覆盖新增文件，删除与改名路径还没测"

const QUESTION = "继续之前的任务。请先用三行分别复述：①任务目标 ②涉及的文件路径 ③未完成事项（逐条列出）。然后说明你打算先做哪一项。"

const filler = (i: number) =>
  `第 ${i} 段排查记录：检查了 ${FILE_A} 的实现，确认 mtime 缓存命中逻辑与 watcher 事件顺序无冲突；` +
  `过程中整理了一份调用链笔记，涉及 ${FILE_B} 与索引构建流程：sync 按目录层级递归、遇符号链接跳过；` +
  `缓存以 路径:size:mtime 为键，命中复用旧索引，未命中重算哈希并写回；表上限 20000 条，超出按插入顺序淘汰。` +
  `（长上下文填充，用于把历史推到需要压缩的规模）`

/** 判据：五要素的弱匹配（措辞差异不算失败）。 */
const recall = (text: string) => ({
  目标: /增量/.test(text) && /mtime|扫描/.test(text),
  文件A: /incremental\.ts/.test(text),
  文件B: /watcher\.ts/.test(text),
  未完成1: /debounce|防抖|阈值/.test(text),
  未完成2: /删除|改名|重命名/.test(text),
})

async function main(): Promise<number> {
  const c = await composeServer({ auth: "local" })
  console.log(`[env] GEBAI_HOME=${c.config.gebaiHome}（临时目录）| 历史规模 ${LINES} 段`)

  /** 造一个「任务规格 + 长历史 + 近期消息」的会话；返回会话 id 与建议压缩区间上界。 */
  const seed = async (title: string): Promise<{ sid: string; compactEnd: number }> => {
    const s = await c.store.createSession(USER, title)
    const now = Date.now()
    await c.store.appendMessage(s.id, {
      id: "goal",
      role: "user",
      content:
        `任务规格（请牢记，后续继续做）：\n目标：${GOAL}\n涉及文件：${FILE_A}、${FILE_B}\n` +
        `未完成事项：\n1) ${PENDING_1}\n2) ${PENDING_2}\n做到一半我会让你继续。`,
      createdAt: now,
    } as never)
    await c.store.appendMessage(s.id, { id: "ack", role: "assistant", content: "收到，先按这个目标推进。", createdAt: now + 1 } as never)
    for (let i = 0; i < LINES; i++) {
      await c.store.appendMessage(s.id, {
        id: `f${i}`,
        role: i % 2 === 0 ? "user" : "assistant",
        content: filler(i),
        createdAt: now + 2 + i,
      } as never)
    }
    await c.store.appendMessage(s.id, { id: "recent", role: "user", content: "先停一下，等我下条消息再继续。", createdAt: now + 2 + LINES } as never)
    const loaded = await c.store.load(s.id, USER)
    const chars = (loaded?.messages ?? []).reduce((n, m) => n + String(m.content ?? "").length, 0)
    const compactEnd = Math.max(1, (loaded?.messages.length ?? 0) - 20)
    console.log(`[${title}] ${loaded?.messages.length} 条 / ${chars} 字符 | 计划压缩区间 [0, ${compactEnd})`)
    return { sid: s.id, compactEnd }
  }

  const ask = async (sid: string): Promise<string> => {
    await c.engine.run(sid, USER, QUESTION)
    const after = await c.store.load(sid, USER)
    return String([...(after?.messages ?? [])].reverse().find((m) => m.role === "assistant" && typeof m.content === "string")?.content ?? "")
  }

  // A：基线（不压缩）
  const a = await seed("压缩接续-A基线")
  const replyA = await ask(a.sid)
  const checkA = recall(replyA)
  console.log(`\n[A 基线·未压缩] 复述 ${JSON.stringify(checkA)}\n---\n${replyA.slice(0, 600)}\n`)

  // B：压缩后同一会话继续
  const b = await seed("压缩接续-B压缩")
  const t0 = Date.now()
  const result = await c.engine.compactSession(b.sid, USER, { from: 0, to: b.compactEnd })
  const bLoaded = await c.store.load(b.sid, USER)
  const charsAfter = (bLoaded?.messages ?? []).reduce((n, m) => n + String(m.content ?? "").length, 0)
  console.log(`[B 压缩] ${Date.now() - t0}ms，压缩 ${result.compacted} 条 → 余 ${bLoaded?.messages.length} 条 / ${charsAfter} 字符`)
  const summary = String(result.summary ?? "")
  console.log(`\n--- 生成的摘要（前 900 字符）---\n${summary.slice(0, 900)}\n`)
  const replyB = await ask(b.sid)
  const checkB = recall(replyB)
  console.log(`[B 压缩后·复述] ${JSON.stringify(checkB)}\n---\n${replyB.slice(0, 900)}\n`)

  const passA = Object.values(checkA).filter(Boolean).length
  const passB = Object.values(checkB).filter(Boolean).length
  console.log(`\n=== 结论 ===\nA 基线 ${passA}/5；B 压缩后 ${passB}/5；摘要长度 ${summary.length} 字符`)
  // 判定：压缩后仍复述出全部五要素即通过（基线用于说明测试有效，不参与判定）
  return passB === 5 ? 0 : 1
}

let code = 1
try {
  code = await main()
} catch (err) {
  console.error("验证失败：", err)
} finally {
  rmSync(home, { recursive: true, force: true })
}
process.exit(code)
