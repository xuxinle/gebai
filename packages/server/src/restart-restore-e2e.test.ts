/**
 * 重启后「在运行会话的会话级审批姿态」恢复（端到端）：真实起服务 → 重启前后两个进程视图。
 *
 * 为何要有这一条：restoreRunningSessions 的单测只证明函数内部分支正确，而真正会坏的是**接线**——
 * 谁的 tmpdir、启动时是否真的被调用、写的是不是会话内存态 env（前端读到的就是它）。
 * 这里用两个真实实例（同一 GEBAI_HOME、同一临时 tmpdir）走一遍：
 *   ① 第一个实例建会话并设审批跳过；
 *   ② 模拟重启前的登记（工具在自杀前写 running.json）；
 *   ③ 第二个实例启动 → 会话内存态 env 里应重新出现 GEBAI_APPROVAL_SKIP（前端 GET /env 可见）。
 */
import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { restartDir } from "./core/tools/restart"
import type { ServerHandle } from "./index"

let home = ""
let tmp = ""
let first: ServerHandle | null = null
let second: ServerHandle | null = null
let base = ""
const originalTmp = {
  TEMP: process.env.TEMP,
  TMP: process.env.TMP,
  TMPDIR: process.env.TMPDIR,
  NODE_ENV: process.env.NODE_ENV,
  CH_DISABLE: process.env.GEBAI_CHANNELS_DISABLE,
  XIANGQI: process.env.XIANGQI_AUTO,
  /**
   * 启动前的全部 `GEBAI_` 变量快照（**不是几个具名变量**）。
   *
   * 为何要整类快照：本用例把 NODE_ENV 换成 production（要的就是那条真实启动路径），
   * 于是 `loadConfig` 会把**仓库 `.env` 整份**注入 process.env——而 bun 的 preload
   * 只在进程启动瞬间清一次，用例内注入的会留给**同一分片的后续文件**。
   * 实测：`.env` 里 `GEBAI_PERSONA_OUTBOUND_REVIEW=false` 会让同分片的出站审校用例
   * 整批断言变成 “disabled”（现象像功能坏了，实际是测试污染）。
   */
  GEBAI: Object.fromEntries(Object.entries(process.env).filter(([k]) => k.startsWith("GEBAI_")).map(([k, v]) => [k, v as string])),
  /** 访问口令（启动前快照；用例内要删掉才能走免登录路径）。 */
  passwordHash: process.env.GEBAI_ADMIN_PASSWORD_HASH,
}

/** 让本进程的 tmpdir() 指向测试目录（restartDir 与恢复逻辑都按它定位登记文件）。 */
function useTmpDir(dir: string) {
  if (process.platform === "win32") {
    process.env.TEMP = dir
    process.env.TMP = dir
  } else {
    process.env.TMPDIR = dir
  }
}

beforeAll(async () => {
  home = mkdtempSync(join(tmpdir(), "gebai-restore-e2e-home-"))
  tmp = mkdtempSync(join(tmpdir(), "gebai-restore-e2e-tmp-"))
  useTmpDir(tmp)
  // 启动期恢复只在本机模式（且非测试进程）执行——本用例要的正是那条真实路径
  process.env.NODE_ENV = "production"
  // 访问口令：`NODE_ENV=production` 让 loadConfig 把**仓库 `.env` 整份**读进来，
  // 若部署方在 `.env` 里设了 `GEBAI_ADMIN_PASSWORD_HASH`（本地模式=访问口令），
  // 本实例就会真的要求登录——而本用例要验证的是「启动期会话审批姿态恢复」，
  // 所有 REST 请求都没有凭证，会全部 401（表现为 POST /sessions 拿不到 id）。
  // 本地模式免登录才是这个用例的前提，故在启动前显式清除；afterAll 里恢复原值。
  originalTmp.passwordHash = process.env.GEBAI_ADMIN_PASSWORD_HASH
  delete process.env.GEBAI_ADMIN_PASSWORD_HASH
  // 仓库 .env 可能真的配了飞书与渠道：本用例只验证启动期 env 恢复，任何对外连接都不应发生
  process.env.GEBAI_CHANNELS_DISABLE = "feishu,local"
  // 相期窗口：拟人模式已在 config 覆盖里关掉，但再显式上一道锁——该 exe 的启动器会「接管」
  // 同 exe 的现有实例（枚举+kill），测试绝不得关掉用户桌面上正在用的提醒窗口
  process.env.XIANGQI_AUTO = "off"
  const { startServer } = await import("./index")
  // 测试环境隔离：本机安装可能真的开了拟人模式/渠道（会拉起相期窗口、接管用户现有实例、连铁飞书）——
  // 本用例只验证「启动期恢复会话审批姿态」，一切对外副作用一律关掉。
  first = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0, personaEnabled: false, feishuBotEnabled: false, adminPasswordHash: undefined })
  base = `http://127.0.0.1:${first.server.port}`
})

afterAll(() => {
  for (const h of [first, second]) {
    try {
      h?.gc?.stop()
      h?.tasks?.stop()
      h?.todos?.stop()
      h?.server.stop(true)
    } catch {
      /* 已停 */
    }
  }
  process.env.NODE_ENV = originalTmp.NODE_ENV
  // 恢复访问口令（上面为免登录路径删掉了）
  if (originalTmp.passwordHash === undefined) delete process.env.GEBAI_ADMIN_PASSWORD_HASH
  else process.env.GEBAI_ADMIN_PASSWORD_HASH = originalTmp.passwordHash
  // 把 loadConfig 从仓库 .env 注入的 `GEBAI_` 变量全部抹掉，再恢复本用例开工前的值——
  // 否则它们会泄漏给同分片的后续测试文件（“本地 .env 改了断言”的那类幽灵失败）。
  for (const k of Object.keys(process.env)) {
    if (k.startsWith("GEBAI_")) delete process.env[k]
  }
  for (const [k, v] of Object.entries(originalTmp.GEBAI)) process.env[k] = v
  if (originalTmp.CH_DISABLE === undefined) delete process.env.GEBAI_CHANNELS_DISABLE
  else process.env.GEBAI_CHANNELS_DISABLE = originalTmp.CH_DISABLE
  if (originalTmp.XIANGQI === undefined) delete process.env.XIANGQI_AUTO
  else process.env.XIANGQI_AUTO = originalTmp.XIANGQI
  if (originalTmp.TEMP === undefined) delete process.env.TEMP
  else process.env.TEMP = originalTmp.TEMP
  if (originalTmp.TMP === undefined) delete process.env.TMP
  else process.env.TMP = originalTmp.TMP
  if (originalTmp.TMPDIR === undefined) delete process.env.TMPDIR
  else process.env.TMPDIR = originalTmp.TMPDIR
  rmSync(home, { recursive: true, force: true })
  rmSync(tmp, { recursive: true, force: true })
})

describe("重启后在运行会话的审批姿态恢复（启动期真实接线）", () => {
  test("新实例启动时按 running.json 恢复会话内存态 env（含非本会话），且登记被一次性消费", async () => {
    // 两个会话：a 开着审批跳过（重启后要恢复），b 没开（不得凭空写入）
    const a = (await (await fetch(`${base}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "a" }) })).json()) as { id: string }
    const b = (await (await fetch(`${base}/api/v1/sessions`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: "b" }) })).json()) as { id: string }
    expect(a.id).toMatch(/^[0-9a-f]{32}$/)

    // 重启前登记（真实布局：{tmpdir}/gebai-restart/running.json）
    mkdirSync(restartDir(tmp), { recursive: true })
    writeFileSync(join(restartDir(tmp), "running.json"), JSON.stringify({ at: Date.now(), sessions: [{ sessionId: a.id, user: "admin", approvalSkip: true }, { sessionId: b.id, user: "admin" }] }), "utf8")

    first!.gc?.stop()
    first!.tasks?.stop()
    first!.todos?.stop()
    first!.server.stop(true)
    first = null

    const { startServer } = await import("./index")
    second = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0, personaEnabled: false, feishuBotEnabled: false, adminPasswordHash: undefined })
    base = `http://127.0.0.1:${second.server.port}`

    // 启动期恢复是监听建立后立即 await 的（先于续跑消费），查询前缓存已就位
    const envA = (await (await fetch(`${base}/api/v1/sessions/${a.id}/env`)).json()) as Array<{ name: string; value: string; source: string }>
    const skip = envA.find((e) => e.name === "GEBAI_APPROVAL_SKIP")
    expect(skip).toMatchObject({ value: "true", source: "session" })

    // 未开审批跳过的会话不写入（服务端不凭空造 env）
    const envB = (await (await fetch(`${base}/api/v1/sessions/${b.id}/env`)).json()) as Array<{ name: string }>
    expect(envB.some((e) => e.name === "GEBAI_APPROVAL_SKIP")).toBe(false)

    // 一次性消费：登记文件已删除，再启动不会重复写
    expect(() => readFileSync(join(restartDir(tmp), "running.json"), "utf8")).toThrow()
  })
})
