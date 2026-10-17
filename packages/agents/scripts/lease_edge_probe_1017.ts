#!/usr/bin/env bun
/**
 * 边界探针④：**持有者会话仍在跑**时的 idle 判据（会话可达、isRunning 为真）
 * ——确认本节说的“两重确认”不会把“正在跑的持有者”误回收。
 */
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef, parseControl, renderControl, LEASE_START_GRACE_MS } from "../src/agents/shengsheng/shengsheng"

const tool = (n: string) => shengshengDef.tools![n]
const root = mkdtempSync(join(tmpdir(), "lease-edge-probe-"))
mkdirSync(join(root, ".evolve"), { recursive: true })
const home = mkdtempSync(join(tmpdir(), "lease-edge-home-"))
let n = 0
const sessions = new Map<string, { id: string; messages: unknown[] }>()
const running = new Set<string>()
const ctx = {
  user: "admin", sessionId: "s-edge", workdir: home, sessionWorkdir: home, home, env: {}, sandboxed: false,
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
      createSession: async (_u: string, name?: string) => { const id = `e${++n}`; sessions.set(id, { id, messages: [] }); return { id, name } },
      setEnv: async () => ({}),
      load: async (id: string) => sessions.get(id) ?? null,
      delete: async (id: string) => sessions.delete(id),
    },
    engine: {
      isRunning: (id: string) => running.has(id),
      cancel: (id: string) => running.delete(id),
      runtimeOf: async () => Object.fromEntries([...running].map((id) => [id, { startedAt: Date.now() - 30_000, pending: [], toolCalls: 1 }])),
    },
    run: async (id: string) => { running.add(id) },
  },
  tasks: { add: async () => ({}), list: async () => [], update: async () => null, notify: async () => ({}) },
} as unknown as ToolContext
const sessionEnv: Record<string, string> = { SHENGSHENG_MAX_WORKERS: "6" }
Object.defineProperty(ctx, "env", { get: () => sessionEnv, configurable: true })
const md = () => readFileSync(join(root, ".evolve", "control.md"), "utf8")

await tool("state").execute({ action: "init", root, verify: "bun test" }, ctx)
await tool("dispatch").execute({ orders: [{ task: "负载敏感测量的长任务", domain: "a", exclusive: true }], root }, ctx)
const l = parseControl(md()).leases[0]
// 把起租时刻推到宽限之外（>3 分钟）——但对“仍在跑”的会话，仍然**不得**回收
const st = parseControl(md())
st.leases[0].start = Date.now() - (LEASE_START_GRACE_MS + 10 * 60_000)
writeFileSync(join(root, ".evolve", "control.md"), renderControl(st))
const r = await tool("state").execute({ root }, ctx)
console.log("【边界④】持有者会话仍在跑 + 起租已 13 分钟 → state 原文片段：")
console.log((r.output as string).split("\n").filter((x) => /租约|回收|呼吸/.test(x)).join("\n"))
console.log(`\n盘上租约条数 = ${parseControl(md()).leases.length}（1 = 仍在跑的持有者没有被误回收）`)

// 反向：同一张租约，会话一停（仍可达、池里没标 idle）→ 过了宽限就该回收
running.clear()
const r2 = await tool("state").execute({ root }, ctx)
console.log("\n【边界④-反】同一张租约、会话停下（池里仍标 running）→ state 原文片段：")
console.log((r2.output as string).split("\n").filter((x) => /租约|回收|呼吸/.test(x)).join("\n"))
console.log(`盘上租约条数 = ${parseControl(md()).leases.length}（0 = 真空闲被回收）`)
void l
