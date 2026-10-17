#!/usr/bin/env bun
/**
 * 独占资源租约的**活体验收**脚本（2026-10-17 单：shengsheng-lease-accept）。
 *
 * 目的：不是再写一遍单测，而是把「代码已落地、机制从未跑过」变成「有证据证明它真拦得住」：
 *   · 走**真公开面**：`dispatch` / `state` / `collect` 三个工具的真实 execute（不调内部函数）；
 *   · 磁盘上是**真文件**：夹具根 `<root>/.evolve/control.md` 是真写出的 markdown，
 *     「资源租约」表原文从盘上读回来贴（不是把内存对象 print 一遍）；
 *   · 每一次判据都用**返回原文**说话（回显是模型唯一能看到的东西）。
 *
 * 会话控制面是**桩**（`ctx.sessions`）：本脚本跑在单进程里，不能去创建真会话。
 * 桩对租约判据只提供三件事：会话存在与否、是否在跑、消息尾——与真引擎同语义。
 * 桩与真引擎的差异及其对结论的影响，写在报告「自查」一节。
 *
 * 用法：bun run packages/agents/scripts/lease_accept_live_1017.ts [--fixture <dir>]
 * 夹具根缺省新建临时目录（不碰 /home/x/code/gebai 的 .evolve/control.md）。
 */
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import {
  def as shengshengDef,
  parseControl,
  renderControl,
  resourceKey,
  DEFAULT_EXCLUSIVE_RESOURCE,
  DEFAULT_LEASE_MINUTES,
} from "../src/agents/shengsheng/shengsheng"

const tool = (n: string) => shengshengDef.tools![n]
const LINE = "=".repeat(78)

// ── 夹具根与会话控制面桩 ────────────────────────────────────────────────────
interface Stub {
  ctx: ToolContext
  root: string
  home: string
  created: Array<{ id: string; name?: string }>
  sent: Array<{ sessionId: string; message: string }>
  sessions: Map<string, { id: string; messages: unknown[]; ctxTokens?: number }>
  running: Set<string>
  sessionEnv: Record<string, string>
}

function stub(root: string): Stub {
  const home = mkdtempSync(join(tmpdir(), "lease-accept-"))
  const sessionDir = join(home, "users", "admin", "sessions", "s-live")
  const tmp = join(sessionDir, "tmp")
  mkdirSync(tmp, { recursive: true })
  mkdirSync(join(root, ".evolve"), { recursive: true })

  const s: Stub = {
    home,
    root,
    created: [],
    sent: [],
    sessions: new Map(),
    running: new Set(),
    sessionEnv: {},
    ctx: undefined as unknown as ToolContext,
  }

  const ctx = {
    user: "admin",
    sessionId: "s-live",           // 本会话自己的 id：回收判据会跳过“自己的”租约（见 shengsheng.ts:1587）
    workdir: tmp,
    sessionWorkdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p: string) => p,
    readFile: async (p: string) => readFileSync(p, "utf8"),
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async (p: string, content: string) => {
      mkdirSync(join(p, ".."), { recursive: true })
      writeFileSync(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
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
          const id = `live${s.created.length + 1}`
          s.created.push({ id, name })
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
        // 模拟 worker 干完一息就停（真引擎里是事件循环退出）：不清 running 的话，
        // 后续 collect/state 的“会话在跑”判据会一直为真——那是**桩的产物**，不是机制行为。
      },
    },
    tasks: { add: async () => ({}), list: async () => [], update: async () => null, notify: async () => ({}) },
  } as unknown as ToolContext
  Object.defineProperty(ctx, "env", { get: () => s.sessionEnv, configurable: true })
  s.ctx = ctx
  return s
}

// ── 盘上读原文（证据只认磁盘，不认内存） ─────────────────────────────────────
const controlMd = (root: string) => readFileSync(join(root, ".evolve", "control.md"), "utf8")

/** 取「资源租约」节原文（含节标题，到下一个 `## ` 或文件末为止）。 */
function leaseSection(md: string): string {
  const lines = md.split("\n")
  const i = lines.findIndex((l) => /^##\s+资源租约\s*$/.test(l))
  if (i < 0) return "（control.md 里没有「资源租约」节）"
  const out = [lines[i]]
  for (let j = i + 1; j < lines.length; j++) {
    if (/^##\s+/.test(lines[j])) break
    out.push(lines[j])
  }
  return out.join("\n").trimEnd()
}

const say = (s: string) => console.log(s)
const block = (title: string, body: string) => say(`${LINE}\n${title}\n${LINE}\n${body}\n`)

/** 磁盘真值：把池里某个 worker 标 idle（模拟“收藏路径绕过/旧版工具/人工改盘”留下的现场）。
 *  刻意走 parse → render 真往返（与工具同一条读写路径），不手拼 markdown。 */
function markIdleOnDisk(root: string, alias: string): void {
  const st = parseControl(controlMd(root))
  const w = st.workers.find((x) => x.alias === alias)
  if (!w) throw new Error(`池中无 ${alias}`)
  w.status = "idle"
  writeFileSync(join(root, ".evolve", "control.md"), renderControl(st))
}

/** 磁盘真值：把某条租约的 start 前推（模拟“租约已领了一段时间”——启动宽限判据要用）。 */
function ageLeaseOnDisk(root: string, resource: string, ms: number): void {
  const st = parseControl(controlMd(root))
  const l = st.leases.find((x) => x.resource === resource)
  if (!l) throw new Error(`租约表无 ${resource}`)
  l.start = Date.now() - ms
  writeFileSync(join(root, ".evolve", "control.md"), renderControl(st))
}

async function main(): Promise<void> {
  const argIdx = process.argv.indexOf("--fixture")
  const root = argIdx > 0 ? process.argv[argIdx + 1] : mkdtempSync(join(tmpdir(), "lease-fixture-root-"))
  mkdirSync(join(root, ".evolve"), { recursive: true })
  const s = stub(root)
  say(`夹具根（临时，不碰生产）：${root}`)
  say(`本会话 id：${s.ctx.sessionId}｜缺省租期 ${DEFAULT_LEASE_MINUTES} 分钟｜缺省资源名 ${DEFAULT_EXCLUSIVE_RESOURCE}`)
  say(`resourceKey("  CPU-Exclusive ")=${JSON.stringify(resourceKey("  CPU-Exclusive "))}｜resourceKey("cpu-exclusive")=${JSON.stringify(resourceKey("cpu-exclusive"))}`)

  // ── 0. init：让夹具根有真实 control.md ──────────────────────────────────
  const init = await tool("state").execute({ action: "init", root, verify: "bun test" }, s.ctx)
  block("【0】init（夹具根立坐标）", init.output as string)
  const seeded = "负载敏感测量 A：x11 双缓冲基线跑 20 轮 A/B（前后 uptime、期间无人构建）"
  await tool("state").execute({ action: "seed_add", direction: seeded, priority: "P1", root }, s.ctx)
  block("【0】control.md 现有节标题（证明「资源租约」节此刻是空的、由渲染器写出）", controlMd(root).split("\n").filter((l) => l.startsWith("## ")).join("\n") + "\n\n" + leaseSection(controlMd(root)))

  // ── 1. 真派一单：resource 带空格与大写（顺带验归一） ─────────────────────
  s.sessionEnv.SHENGSHENG_MAX_WORKERS = "12"   // 本脚本要连开多个 worker；容量上限不是本单判据
  const r1 = await tool("dispatch").execute({
    orders: [
      {
        task: seeded,
        domain: "x11",
        resource: "  CPU-Exclusive ",     // ← 故意带空格+大写：若归一失效，一个空格就能绕开租约
        context: "必须在机器干净时跑：20 轮 A/B 前后 uptime，期间不得有人构建/测试",
        deliver: "A/B 基线数据 + uptime 记录",
      },
    ],
    root,
  }, s.ctx)
  block("【1a】dispatch#1（resource=\"  CPU-Exclusive \"）回显原文", r1.output as string)
  block("【1b】夹具根 .evolve/control.md「资源租约」节原文（真从盘上读回）", leaseSection(controlMd(root)))
  const lease1 = parseControl(controlMd(root)).leases[0]
  say(`【1c】盘上租约行解析：resource=${JSON.stringify(lease1?.resource)} alias=${lease1?.alias} sid=${lease1?.sessionId} ` +
      `start=${new Date(lease1?.start ?? 0).toISOString()} expireAt=${new Date(lease1?.expireAt ?? 0).toISOString()} ` +
      `租期=${Math.round(((lease1?.expireAt ?? 0) - (lease1?.start ?? 0)) / 60000)} 分钟`)
  say(`【1c】归一判据：resourceKey(盘上)=${JSON.stringify(resourceKey(lease1?.resource ?? ""))}｜` +
      `与 "cpu-exclusive" 同名=${resourceKey(lease1?.resource ?? "") === resourceKey("cpu-exclusive")}`)

  // ── 2. 第二个请求同一资源（另一个 worker、另一个领域）必须被阻 ────────────
  s.running.clear()          // worker#1 干完一息（真引擎里会话转空闲）——但**池未标 idle、也没人 collect**
  const second = "负载敏感测量 B：pty 竞态复现的最终门禁 st test（要求负载干净）"
  const r2 = await tool("dispatch").execute({
    orders: [{ task: second, domain: "pty", fresh: true, resource: "cpu-exclusive", context: "最终门禁：必须独占，否则数字作废" }],
    root,
  }, s.ctx)
  block("【2a】dispatch#2（resource=\"cpu-exclusive\"，不同会话）回显原文", r2.output as string)
  say(`【2b】桩观测：注入 worker 的消息条数 = ${s.sent.length}（1 = 第二个真没开跑）｜新建会话数 = ${s.created.length}`)
  block("【2c】此刻「资源租约」节原文（仍是第一位持者，未被顶掉）", leaseSection(controlMd(root)))
  block("【2d】此刻「种子队列」节原文（被挡的方向退回种子，未丢）", md_section(controlMd(root), "种子队列"))

  // ── 2e. 反向的对照组：不声明资源的普通单照派（租约不挡无关工单） ─────────
  const plain = await tool("dispatch").execute({ orders: [{ task: "读码定位符号 st_build 的入口", domain: "docs", fresh: true }], root }, s.ctx)
  block("【2e】对照组：不声明 resource 的普通单照派（证明租约只挡“申请同一资源的人”）", plain.output as string)
  say(`【2e】注入消息条数 = ${s.sent.length}（2 = 普通单真派出去了）`)

  // ── 3. 释放：collect 后第三单能拿到 ──────────────────────────────────────
  const holderAlias = lease1.alias
  const holderSid = lease1.sessionId
  s.running.clear()
  s.sessions.get(holderSid)!.messages = [
    { role: "assistant", content: "门禁 184 passed / 0 failed；前后 uptime 0.3/0.4；期间无他人构建。" },
  ]
  const collected = await tool("collect").execute({ action: "collect", alias: holderAlias, summary: "门禁 184 passed / 0 failed（前后 uptime 已记）", root }, s.ctx)
  block("【3a】collect 回显原文（应显式报「租约已释放」）", collected.output as string)
  block("【3b】collect 后「资源租约」节原文", leaseSection(controlMd(root)))
  const r3 = await tool("dispatch").execute({ orders: [{ task: second, domain: "pty", fresh: true, resource: "cpu-exclusive" }], root }, s.ctx)
  block("【3c】dispatch#3（同一资源，第三位申请者）回显原文——应当成功", r3.output as string)
  block("【3d】此刻「资源租约」节原文（新持者已在表上）", leaseSection(controlMd(root)))
  const lease3 = parseControl(controlMd(root)).leases[0]
  say(`【3d】持者更替：${holderAlias} → ${lease3?.alias}｜资源名 ${JSON.stringify(lease3?.resource)}`)

  // ── 4. 逾期回收之一：持有者转 idle（池标 idle） ──────────────────────────
  s.running.clear()
  markIdleOnDisk(root, lease3.alias)
  const st1 = await tool("state").execute({ root }, s.ctx)
  block("【4a】state 读态（持有者已转 idle）原文片段——应显式报「租约回收」与原因", grepLines(st1.output as string, /租约|回收|cpu-exclusive|呼吸/))
  block("【4b】回收后「资源租约」节原文", leaseSection(controlMd(root)))
  const r4 = await tool("dispatch").execute({ orders: [{ task: "测量丁：另一路 A/B", domain: "perf", fresh: true, resource: "cpu-exclusive" }], root }, s.ctx)
  block("【4c】回收后再派（应拿到）原文", r4.output as string)

  // ── 5. 逾期回收之二：仅“会话可达但不在运行”——启动宽限须知 ───────────────
  const lease4 = parseControl(controlMd(root)).leases[0]
  s.running.clear()
  // ⑤a 的关键在于：**池里没标 idle**（worker 仍在池中、status=running），只有“会话此刻不在跑”这一条真值。
  // 刚领的租约落在启动宽限（3 分钟）内 → **不得**因“瞬时不在跑”就回收（否则刚派出去的单会被自己抹掉）。
  const stFresh = await tool("state").execute({ root }, s.ctx)
  block(`【5a】刚领租约、会话不在跑（未过启动宽限 3 分钟）→ state 全文（**不应**回收）`, stFresh.output as string)
  say(`【5a】盘上仍有租约：${parseControl(controlMd(root)).leases.length} 条（宽限内“不在跑”≠“已下班”）`)
  // 此刻距领租约仅 dispatch 内置的 ~1.5s 早错窗口——远小于 3 分钟启动宽限
  ageLeaseOnDisk(root, lease4.resource, 4 * 60_000)   // 把 start 前推到 4 分钟前（真 parse→render 往返）
  const stAged = await tool("state").execute({ root }, s.ctx)
  block("【5b】同一租约、start 前推到 4 分钟前 → state 全文（**应**回收，原因：已过启动宽限）", stAged.output as string)
  block("【5b】回收后「资源租约」节原文", leaseSection(controlMd(root)))

  // ── 6. 逾期回收之三：持有者会话被外部删除 ────────────────────────────────
  const r6 = await tool("dispatch").execute({ orders: [{ task: "测量戊：基线复测", domain: "perf2", fresh: true, exclusive: true }], root }, s.ctx)
  const lease6 = parseControl(controlMd(root)).leases[0]
  block("【6a】dispatch#6（exclusive:true 简写）回显原文", r6.output as string)
  say(`【6a】exclusive:true 落成的资源名 = ${JSON.stringify(lease6?.resource)}（应等于 ${JSON.stringify(DEFAULT_EXCLUSIVE_RESOURCE)}）`)
  s.running.clear()
  s.sessions.delete(lease6.sessionId)      // ← 外部删除（服务重启/人工清理）：控制面从此 load 不到它
  // 先**只读盘上租约条数**再读态：回收是 state 的副作用，副作用前后两个数字才构成证据。
  const before6 = parseControl(controlMd(root)).leases.length
  const st6 = await tool("state").execute({ root }, s.ctx)
  say(`【6b】读态前盘上租约 = ${before6} 条｜读态后 = ${parseControl(controlMd(root)).leases.length} 条`)
  block("【6b】会话被外部删除后 state 原文片段（应报「持有者会话已不存在」）", grepLines(st6.output as string, /租约|回收|不存在|呼吸/))
  block("【6b】回收后「资源租约」节原文", leaseSection(controlMd(root)))
  const r6b = await tool("dispatch").execute({ orders: [{ task: "测量己：再测一次", domain: "perf3", fresh: true, exclusive: true }], root }, s.ctx)
  block("【6c】回收后再派（应拿到）原文", r6b.output as string)
  // 租约表现已被 perf3-1 占住——到期那一档要的是一张**自己的**新租约，得先把它收掉
  s.running.clear()
  const lease6b = parseControl(controlMd(root)).leases[0]
  await tool("collect").execute({ action: "retire", alias: lease6b.alias, mode: "idle", reason: "为到期档腾出资源", root }, s.ctx)
  say(`【6d】腾资源后盘上租约 = ${parseControl(controlMd(root)).leases.length} 条`)

  // ── 7. 逾期回收之四：真到期（env SHENGSHENG_LEASE_MINUTES=1，真等 62 秒） ─
  // 租期设成 1 分钟（leaseMinutes 的下限就是 1；0 会被夹到 1）——到期兜底才能在一息内实测
  s.sessionEnv.SHENGSHENG_LEASE_MINUTES = "1"
  const r7 = await tool("dispatch").execute({ orders: [{ task: "测量庚：到期兜底", domain: "perf4", fresh: true, exclusive: true }], root }, s.ctx)
  const lease7 = parseControl(controlMd(root)).leases[0]
  if (!lease7) {
    say("【7a】❌ 未拿到新租约（上一档没收干净）——到期档无从测起")
    block("【7a】dispatch#7 回显原文", r7.output as string)
  }
  say(`【7a】SHENGSHENG_LEASE_MINUTES=${JSON.stringify(s.sessionEnv.SHENGSHENG_LEASE_MINUTES)} → 盘上租期 = ${Math.round(((lease7?.expireAt ?? 0) - (lease7?.start ?? 0)) / 60000)} 分钟；expireAt=${new Date(lease7?.expireAt ?? 0).toLocaleString("zh-CN")}`)
  block("【7a】dispatch#7 回显原文", r7.output as string)
  // 刻意让持有者会话**仍在跑**：到期兜底必须不依赖“已经下班”，否则它只是 idle 判据的副本。
  s.running.clear()
  s.running.add(lease7.sessionId)
  say(`【7b】等待到期（约 62 秒）…现在 ${new Date().toLocaleTimeString("zh-CN")}`)
  await Bun.sleep(62_000)
  const st7 = await tool("state").execute({ root }, s.ctx)
  say(`【7c】等待结束 ${new Date().toLocaleTimeString("zh-CN")}｜到期时刻 ${new Date(lease7?.expireAt ?? 0).toLocaleTimeString("zh-CN")}｜会话仍在跑=${s.running.has(lease7?.sessionId ?? "")}`)
  block("【7c】到期后 state 原文片段（会话仍在跑，但租约已到期 → 必须回收并说「已到期」）", grepLines(st7.output as string, /租约|回收|已到期|呼吸/))
  block("【7c】回收后「资源租约」节原文", leaseSection(controlMd(root)))

  // ── 8. retire 释放（另一条释放路径） ─────────────────────────────────────
  const r8 = await tool("dispatch").execute({ orders: [{ task: "测量辛：retire 释放", domain: "perf5", fresh: true, exclusive: true }], root }, s.ctx)
  const lease8 = parseControl(controlMd(root)).leases[0]
  s.running.clear()
  const rt = await tool("collect").execute({ action: "retire", alias: lease8.alias, mode: "idle", reason: "领域已终", root }, s.ctx)
  block("【8】retire mode=idle 回显原文（应显式报「租约已释放」）", rt.output as string)
  say(`【8】retire 后租约条数 = ${parseControl(controlMd(root)).leases.length}`)
  void r8

  say(`\n夹具根：${root}\n（control.md 全文留在此处，可复核）`)
}

function md_section(md: string, name: string): string {
  const lines = md.split("\n")
  const i = lines.findIndex((l) => new RegExp(`^##\\s+${name}\\s*$`).test(l))
  if (i < 0) return "（无此节）"
  const out = [lines[i]]
  for (let j = i + 1; j < lines.length; j++) {
    if (/^##\s+/.test(lines[j])) break
    out.push(lines[j])
  }
  return out.join("\n").trimEnd()
}

function grepLines(text: string, re: RegExp): string {
  const hit = text.split("\n").filter((l) => re.test(l))
  return hit.length ? hit.join("\n") : "（无匹配行）"
}

await main()
