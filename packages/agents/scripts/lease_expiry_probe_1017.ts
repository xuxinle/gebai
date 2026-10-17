#!/usr/bin/env bun
/**
 * 探针：**到期**这条兜底判据到底在哪生效？
 *
 * 设计意图（shengsheng.ts Lease 注释）：`expireAt` 是兜底，防「持有者悄悄死掉而登记没被碰」，
 * 然后一条死租约会**永久堵住院子**。而堵住的恰好是「要独占才敢做」的那类活。
 *
 * 所以要问的是：一条**已到期**的租约，
 *   a) `state` 读态会回收（已知会，见 lease_accept_live_1017.ts §7）；
 *   b) **直接 `dispatch`（不经 state）**会不会也被挡住？
 * 若 (b) 仍被挡，则「到期」不是自清，而是「等下一次读态」——而读态是模型的选择，不是机制。
 *
 * 用法：bun run packages/agents/scripts/lease_expiry_probe_1017.ts
 */
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef, parseControl, renderControl } from "../src/agents/shengsheng/shengsheng"

const tool = (n: string) => shengshengDef.tools![n]
const root = mkdtempSync(join(tmpdir(), "lease-expiry-probe-"))
mkdirSync(join(root, ".evolve"), { recursive: true })
const home = mkdtempSync(join(tmpdir(), "lease-expiry-home-"))
const created: string[] = []
const sent: string[] = []
const sessions = new Map<string, { id: string; messages: unknown[] }>()
const running = new Set<string>()

const ctx = {
  user: "admin", sessionId: "s-probe", workdir: home, sessionWorkdir: home, home, env: {}, sandboxed: false,
  resolvePath: (p: string) => p,
  readFile: async (p: string) => readFileSync(p, "utf8"),
  readBinaryFile: async () => new Uint8Array(),
  writeFile: async (p: string, c: string) => { mkdirSync(join(p, ".."), { recursive: true }); writeFileSync(p, c) },
  listFiles: async () => [], listDir: async () => [], deleteFile: async () => {}, moveFile: async () => {},
  runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
  uploadAttachment: async (r: { path: string }) => r.path, publish: () => {}, projects: [],
  resolveProjectPath: () => root, getTodos: async () => [], setTodos: async () => {},
  registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
  listSubAgentDefs: () => [], loadSubAgent: async () => {},
  waitForChoice: async () => null, waitForEnv: async () => false, waitForDraw: async () => ({ ok: true }),
  sessions: {
    store: {
      createSession: async (_u: string, name?: string) => {
        const id = `p${created.length + 1}`
        created.push(id); sessions.set(id, { id, messages: [] }); return { id, name }
      },
      setEnv: async () => ({}),
      load: async (id: string) => sessions.get(id) ?? null,
      delete: async (id: string) => sessions.delete(id),
    },
    engine: {
      isRunning: (id: string) => running.has(id),
      cancel: (id: string) => running.delete(id),
      runtimeOf: async () => Object.fromEntries([...running].map((id) => [id, { startedAt: Date.now() - 30_000, pending: [], toolCalls: 1 }])),
    },
    run: async (id: string, _u: string, _m: string) => { sent.push(id); running.add(id) },
  },
  tasks: { add: async () => ({}), list: async () => [], update: async () => null, notify: async () => ({}) },
} as unknown as ToolContext
const sessionEnv: Record<string, string> = { SHENGSHENG_MAX_WORKERS: "8" }
Object.defineProperty(ctx, "env", { get: () => sessionEnv, configurable: true })

const md = () => readFileSync(join(root, ".evolve", "control.md"), "utf8")

await tool("state").execute({ action: "init", root, verify: "bun test" }, ctx)
await tool("dispatch").execute({ orders: [{ task: "甲：负载敏感门禁", domain: "a", exclusive: true }], root }, ctx)
const lease = parseControl(md()).leases[0]
console.log(`A 领到租约：${lease.resource} 持者 ${lease.alias}（到期 ${new Date(lease.expireAt).toLocaleTimeString("zh-CN")}）`)

// 把到期时刻改到过去（真 parse→render 往返），**不调用 state**（不触发读态回收）
const st = parseControl(md())
st.leases[0].expireAt = Date.now() - 1_000
writeFileSync(join(root, ".evolve", "control.md"), renderControl(st))
running.clear()
console.log(`B 盘上租约已过期（expireAt=${new Date(st.leases[0].expireAt).toLocaleString("zh-CN")}），且**未**调用 state`)

const r = await tool("dispatch").execute({ orders: [{ task: "乙：另一路负载敏感测量", domain: "b", fresh: true, exclusive: true }], root }, ctx)
console.log("\nC 直接 dispatch（不经 state）的回显原文：\n" + r.output)
console.log(`\nD 注入 worker 的消息数 = ${sent.length}（1 = 乙真被一条已经过期的租约挡住了）`)
console.log(`E 派完盘上租约：\n${md().split("\n").filter((l) => l.startsWith("| cpu")).join("\n") || "（空）"}`)

// 对照：先读一次 state（回收），再 dispatch
const st1 = await tool("state").execute({ root }, ctx)
console.log(`\nF 先 state 再 dispatch：state 原文片段\n${(st1.output as string).split("\n").filter((l) => /回收|已到期|租约/.test(l)).join("\n")}`)
const r2 = await tool("dispatch").execute({ orders: [{ task: "乙：另一路负载敏感测量", domain: "b", fresh: true, exclusive: true }], root }, ctx)
console.log(`\nG 第二次 dispatch 回显原文：\n${r2.output}`)
console.log(`\nH 注入 worker 的消息数 = ${sent.length}`)
console.log(`\n夹具根：${root}`)
