#!/usr/bin/env bun
/**
 * 仓级资源租约的**两线真实现场**（跨演进线互斥的活体证据，2026-10-17 单）。
 *
 * 本仓真实现场（不是假想）：
 *   /home/x/code/gebai/.evolve            ← 歌白线（另一条线在跑）
 *   /home/x/code/gebai/shuangtian/.evolve ← 霜天线（本线）
 *   两者的 `git rev-parse --show-toplevel` 都 = /home/x/code/gebai
 * ⇒ 各有自己的 control.md，却共享**同一棵树、同一批文件、同一台机器**。
 *
 * 本脚本在**临时目录**里复刻这个结构（真 `git init`，两个子目录各是一条演进线），
 * 然后走 `dispatch` / `state` / `collect` 的**真公开面**，把回显原文贴出来：
 *   场景 A：同仓 A 根持 repo:<top>:cpu-exclusive → B 根派工必须被顺延且说清
 *   场景 B：不同 git 仓的 C 根 → 不受影响（防止把所有人互相锁死）
 *   场景 C：A 的 worker 被 collect → 共享表释放 → B 立刻能领
 *   场景 D：B 读态看见 A 的仓级租约（两份合并展示）
 *   场景 E：A 留下的死租约（已到期）→ B 读态跨线回收并报原因
 *
 * 会话控制面是**桩**（`ctx.sessions`，单进程里不能真建会话）：对租约判据它只提供三件事
 * （会话存在吗、在跑吗、消息尾），与真引擎同语义。桩与真引擎的差异写在报告末尾「自查」。
 *
 * 用法：bun run packages/agents/scripts/repo_lease_live_1017.ts
 */
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join } from "node:path"
import { spawnSync } from "node:child_process"
import type { ToolContext } from "@gebai/sdk"
import {
  def as shengshengDef,
  parseControl,
  renderControl,
  parseSharedLeases,
  DEFAULT_EXCLUSIVE_RESOURCE,
} from "../src/agents/shengsheng/shengsheng"

const tool = (n: string) => shengshengDef.tools![n]
const LINE = "=".repeat(78)
const say = (s = "") => console.log(s)
const block = (title: string, body: string) => say(`${LINE}\n${title}\n${LINE}\n${body}\n`)

interface Stub {
  ctx: ToolContext
  root: string
  sent: Array<{ sessionId: string; message: string }>
  created: Array<{ id: string }>
  sessions: Map<string, { id: string; messages: unknown[] }>
  running: Set<string>
  sessionEnv: Record<string, string>
}

/** 一条演进线=一个项目根 + 一份独立会话控制面桩（两条线的会话 id 必须不同）。 */
function lineStub(root: string, tag: string): Stub {
  const home = mkdtempSync(join(tmpdir(), `repoline-${tag}-`))
  const tmp = join(home, "users", "admin", "sessions", tag, "tmp")
  mkdirSync(tmp, { recursive: true })
  mkdirSync(join(root, ".evolve"), { recursive: true })
  const s: Stub = { root, sent: [], created: [], sessions: new Map(), running: new Set(), sessionEnv: {}, ctx: undefined as unknown as ToolContext }
  const ctx = {
    user: "admin",
    sessionId: `sess-${tag}`,
    workdir: tmp,
    sessionWorkdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p: string) => p,
    readFile: async (p: string) => readFileSync(p, "utf8"),
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async (p: string, content: string) => {
      mkdirSync(dirname(p), { recursive: true })
      writeFileSync(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async (p: string) => { try { const { rmSync } = await import("node:fs"); rmSync(p, { force: true }) } catch { /* 忽略 */ } },
    moveFile: async (from: string, to: string) => {
      const { mkdir, rename } = await import("node:fs/promises")
      await mkdir(dirname(to), { recursive: true })
      await rename(from, to)
    },
    // 仓级租约靠它拿 git 顶层：**真跑 shell**（本脚本的夹具是真 git 仓）
    runCommand: async (cmd: string, o?: { workdir?: string }) => {
      const r = spawnSync("/bin/sh", ["-c", cmd], { cwd: o?.workdir ?? tmp, encoding: "utf8", timeout: 5000 })
      return { stdout: r.stdout ?? "", stderr: r.stderr ?? "", code: r.status ?? 1 }
    },
    uploadAttachment: async (r: { path: string }) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => root,
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    sessions: {
      store: {
        createSession: async (_u: string, name?: string) => {
          const id = `${tag}-w${s.created.length + 1}`
          s.created.push({ id })
          s.sessions.set(id, { id, messages: [] })
          return { id, name }
        },
        setEnv: async () => ({}),
        load: async (id: string) => s.sessions.get(id) ?? null,
        delete: async (id: string) => s.sessions.delete(id),
      },
      engine: {
        isRunning: (id: string) => s.running.has(id),
        cancel: (id: string) => s.running.delete(id),
        runtimeOf: async () =>
          Object.fromEntries([...s.running].map((id) => [id, { startedAt: Date.now() - 30_000, pending: [], toolCalls: 1 }])),
      },
      run: async (id: string, _u: string, message: string) => {
        s.sent.push({ sessionId: id, message })
        s.running.add(id)
      },
    },
    tasks: { add: async () => ({}), list: async () => [], update: async () => null, notify: async () => ({}) },
  } as unknown as ToolContext
  Object.defineProperty(ctx, "env", { get: () => s.sessionEnv, configurable: true })
  s.ctx = ctx
  return s
}

const controlMd = (root: string) => readFileSync(join(root, ".evolve", "control.md"), "utf8")
const sharedFile = (repo: string) => join(repo, ".evolve-shared", "leases.json")
const sharedOf = (repo: string) => parseSharedLeases(readFileSync(sharedFile(repo), "utf8")).leases

/** 取 control.md 某一节原文（到下一个 `## ` 或文件末）。 */
function section(md: string, title: string): string {
  const lines = md.split("\n")
  const i = lines.findIndex((l) => new RegExp(`^##\\s+${title}\\s*$`).test(l))
  if (i < 0) return `（control.md 里没有「${title}」节）`
  const out = [lines[i]]
  for (let j = i + 1; j < lines.length; j++) {
    if (/^##\s+/.test(lines[j])) break
    out.push(lines[j])
  }
  return out.join("\n").trimEnd()
}

function gitInit(dir: string): void {
  mkdirSync(dir, { recursive: true })
  const r = spawnSync("git", ["init", "-q", dir], { encoding: "utf8" })
  if (r.status !== 0) throw new Error(`git init 失败：${r.stderr}`)
}

function gitTop(dir: string): string {
  const r = spawnSync("git", ["-C", dir, "rev-parse", "--show-toplevel"], { encoding: "utf8" })
  return (r.stdout ?? "").trim()
}

async function main(): Promise<void> {
  const base = mkdtempSync(join(tmpdir(), "repo-lease-two-lines-"))
  // ── 真实现场结构：一个 git 仓，其下两条演进线各一个项目根 ─────────────────
  const repo = join(base, "gebai")
  gitInit(repo)
  const rootA = join(repo, "line-gebai")       // 形如 /home/x/code/gebai
  const rootB = join(repo, "shuangtian")       // 形如 /home/x/code/gebai/shuangtian
  mkdirSync(rootA, { recursive: true })
  mkdirSync(rootB, { recursive: true })
  const A = lineStub(rootA, "a")
  const B = lineStub(rootB, "b")
  A.sessionEnv.SHENGSHENG_MAX_WORKERS = "6"
  B.sessionEnv.SHENGSHENG_MAX_WORKERS = "6"

  block("【现场】两条演进线 + 同一个 git 仓（真 git init，真 rev-parse）", [
    `git 仓：${repo}`,
    `A 根（歌白线）：${rootA}  → git rev-parse --show-toplevel = ${gitTop(rootA)}`,
    `B 根（霜天线）：${rootB}  → git rev-parse --show-toplevel = ${gitTop(rootB)}`,
    `同一棵树？ ${gitTop(rootA) === gitTop(rootB) ? "是（这正是本单要处理的现场）" : "否"}`,
    `共享租约表（应在 **git 顶层**）：${sharedFile(repo)}`,
  ].join("\n"))

  const ia = await tool("state").execute({ action: "init", root: rootA, verify: "bun test" }, A.ctx)
  const ib = await tool("state").execute({ action: "init", root: rootB, verify: "bun test" }, B.ctx)
  say(`【0】两条线各自 init：A ${ia.output.split("\n")[0]}｜B ${ib.output.split("\n")[0]}\n`)

  // ── 场景 A：同仓 A 占 → B 顺延 ────────────────────────────────────────────
  const ra = await tool("dispatch").execute({
    orders: [{ task: "歌白线：tsc --noEmit 全量门禁（负载敏感）", domain: "gate", exclusive: true, context: "全仓类型检查，自己就会打满多核" }],
    root: rootA,
  }, A.ctx)
  block("【A1】A 根 dispatch（exclusive: true）回显原文", ra.output)
  block("【A2】共享租约表原文（真从盘上读回，位于 git 顶层）", readFileSync(sharedFile(repo), "utf8"))
  say(`【A2】盘上资源名 = ${JSON.stringify(sharedOf(repo)[0]?.resource)}（带 git 顶层 ⇒ 两条线看到的是同一个键）`)
  say(`【A2】持有者根 = ${sharedOf(repo)[0]?.root}`)
  block("【A3】A 根 control.md「资源租约」节（本地镜像：同一资源名）", section(controlMd(rootA), "资源租约"))

  A.running.clear()   // A 的 worker 干完一息（会话不在跑）；池仍登记、没人 collect —— 最坏的对峙现场
  const rb = await tool("dispatch").execute({
    orders: [{ task: "霜天线：等 load<2 做 x11 双缓冲 20 轮 A/B", domain: "perf", fresh: true, exclusive: true, context: "测量要求机器干净：前后 uptime、期间无人构建" }],
    root: rootB,
  }, B.ctx)
  block("【A4】B 根 dispatch（同仓另一条线，同一资源）回显原文 —— 必须被顺延且说清", rb.output)
  say(`【A4】桩观测：B 注入消息 ${B.sent.length} 条（0 = 真没开跑）｜B 新建会话 ${B.created.length} 个`)
  block("【A5】B 根「种子队列」节（方向已退回，未丢）", section(controlMd(rootB), "种子队列"))
  block("【A6】B 根「资源租约」节（它没拿到，本地表不应凭空多一条）", section(controlMd(rootB), "资源租约"))

  // ── 场景 D：B 读态合并展示 A 的仓级租约 ──────────────────────────────────
  const stB = await tool("state").execute({ root: rootB }, B.ctx)
  block("【D1】B 根 state 读态全文（合并展示：本地 0 条 + 仓级 1 条，并写明仓级域路径）", stB.output)

  // ── 场景 C：A collect → 释放 → B 能领 ────────────────────────────────────
  const holder = sharedOf(repo)[0]
  A.running.clear()
  A.sessions.get(holder.sessionId)!.messages = [{ role: "assistant", content: "tsc --noEmit：0 error（全仓）。" }]
  const col = await tool("collect").execute({ action: "collect", alias: holder.alias, summary: "tsc --noEmit 0 error", root: rootA }, A.ctx)
  block("【C1】A 根 collect 回显原文（应显式报「租约已释放」，含仓级那一条）", col.output)
  block("【C2】collect 后共享租约表原文", readFileSync(sharedFile(repo), "utf8"))
  const rd = await tool("dispatch").execute({
    orders: [{ task: "霜天线：等 load<2 做 x11 双缓冲 20 轮 A/B", domain: "perf", fresh: true, exclusive: true }],
    root: rootB,
  }, B.ctx)
  block("【C3】B 根再派（应拿到）回显原文", rd.output)
  block("【C4】此刻共享租约表原文（持者已换成 B 根）", readFileSync(sharedFile(repo), "utf8"))

  // ── 场景 E：B 留下的死租约 → A 读态跨线回收 ───────────────────────────────
  const doc = JSON.parse(readFileSync(sharedFile(repo), "utf8")) as { version: number; leases: Array<Record<string, unknown>> }
  doc.leases[0].expireAt = Date.now() - 60_000
  writeFileSync(sharedFile(repo), `${JSON.stringify(doc, null, 2)}\n`)
  say(`【E0】把 B 那条的到期时刻改成 60 秒前（模拟“持有者悄悄死掉而登记没被碰”）：${new Date(Number(doc.leases[0].expireAt)).toLocaleString("zh-CN")}\n`)
  const stA = await tool("state").execute({ root: rootA }, A.ctx)
  block("【E1】A 根 state 读态全文（应报「仓级租约回收」+ 原因 + 是哪条线的）", stA.output)
  block("【E2】回收后共享租约表原文", readFileSync(sharedFile(repo), "utf8"))

  // ── 场景 B：不同 git 仓 → 互不相挡 ───────────────────────────────────────
  const other = join(base, "other-repo")
  gitInit(other)
  const rootC = join(other, "line-c")
  mkdirSync(rootC, { recursive: true })
  const C = lineStub(rootC, "c")
  C.sessionEnv.SHENGSHENG_MAX_WORKERS = "6"
  await tool("state").execute({ action: "init", root: rootC, verify: "bun test" }, C.ctx)
  // 让 A 先占住（重新领一条，保证“有人在占”这个前提真实存在）
  const ra2 = await tool("dispatch").execute({ orders: [{ task: "歌白线：再跑一次门禁", domain: "gate", fresh: true, exclusive: true }], root: rootA }, A.ctx)
  say(`【B0】${ra2.output.split("\n")[0]}`)
  say(`【B0】A 根现在持着：${sharedOf(repo).map((l) => `${l.resource}@${l.root}`).join("、") || "（无）"}`)
  const rc = await tool("dispatch").execute({
    orders: [{ task: "另一个仓的负载敏感测量（与本仓无关）", domain: "perf", fresh: true, exclusive: true }],
    root: rootC,
  }, C.ctx)
  block("【B1】另一个 git 仓的 C 根 dispatch 回显原文 —— 必须照派（不出现「未派」/「跨线」）", rc.output)
  say(`【B1】桩观测：C 注入消息 ${C.sent.length} 条（1 = 真派出去了）`)
  block("【B2】另一个仓自己的共享租约表原文（各仓各一份，不是一个全局表）", readFileSync(sharedFile(other), "utf8"))
  say(`【B2】两个仓的资源名不同键：\n  ${sharedOf(repo).map((l) => l.resource).join("\n  ")}\n  ${sharedOf(other).map((l) => l.resource).join("\n  ")}`)

  block("【自查】本脚本的桩与真引擎差在哪（结论可移置的边界）", [
    "· 会话控制面是桩：`load` 读 Map、`isRunning` 读 Set、`run` 只记录消息不真起事件循环。",
    "  租约判据只问三件（会话存在吗／在跑吗／到期了吗），桩对这三件与真引擎同语义；",
    "  桩不模拟「引擎拉起会话有延迟」——启动宽限那条判据由单测（LEASE_START_GRACE_MS）单独锁。",
    "· 两条线在本脚本里是**同进程内的两个 ctx**（真实现场是同一服务的两个会话，也可能是两个服务进程）。",
    "  这正是跨线回收要「两重印证」的原因：我看不到它的会话 ≠ 它的会话不存在（脚本里能看清，真环境里不能）。",
    "· git 是真 git（`git init` 真跑）：`rev-parse --show-toplevel` 的输出与真仓同形。",
  ].join("\n"))
  say(`\n夹具根（临时，可删）：${base}`)
}

await main()
