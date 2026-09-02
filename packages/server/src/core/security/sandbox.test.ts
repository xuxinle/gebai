import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { decodeOutput, Sandbox } from "./sandbox"
import { sessionPath } from "../base/paths"

describe("decodeOutput", () => {
  test("utf-8 bytes decode as-is", () => {
    expect(decodeOutput(Buffer.from("hello 世界", "utf8"))).toBe("hello 世界")
  })

  test("gbk bytes fall back to gbk decoding when utf-8 yields replacement chars", () => {
    // GBK 编码的「中文输出」
    const gbk = Buffer.from([0xd6, 0xd0, 0xce, 0xc4, 0xca, 0xe4, 0xb3, 0xf6])
    expect(decodeOutput(gbk)).toBe("中文输出")
  })

  test("empty buffer yields empty string", () => {
    expect(decodeOutput(Buffer.alloc(0))).toBe("")
  })
})

describe("Sandbox 豁免用户（auth=none 默认用户不受用户沙箱控制）", () => {
  test("enforcedFor: 豁免用户不受约束，其余用户受约束；全局关闭时均不受约束", () => {
    const on = new Sandbox({ home: "/tmp/h", enabled: true, isExempt: (u) => u === "default" })
    expect(on.isExempt("default")).toBe(true)
    expect(on.isExempt("alice")).toBe(false)
    expect(on.enforcedFor("default")).toBe(false)
    expect(on.enforcedFor("alice")).toBe(true)
    const off = new Sandbox({ home: "/tmp/h", enabled: false, isExempt: (u) => u === "default" })
    expect(off.enforcedFor("default")).toBe(false)
    expect(off.enforcedFor("alice")).toBe(false)
  })

  test("resolvePath: 豁免用户绝对路径/../ 放行（本地模式语义），其余用户仍被沙箱拒绝", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-exempt-"))
    const sid = "0123456789abcdef0123456789abcdef"
    const sb = new Sandbox({ home, enabled: true, isExempt: (u) => u === "default" })
    try {
      // 豁免用户（默认用户）：绝对路径直接使用、../ 可越界（相对路径基准 = 会话 tmp/，../ 越界到会话根）
      // （Windows 下 resolve("/etc/passwd") 为盘符根路径，断言与解析器同口径）
      expect(sb.resolvePath("default", sid, "/etc/passwd")).toBe(resolve("/etc/passwd"))
      expect(sb.resolvePath("default", sid, "../secret")).toBe(resolve(join(sessionPath(home, "default", sid), "tmp"), "../secret"))
      // 其余用户（含多用户模式同名 default 之外的用户）：沙箱拒绝
      expect(() => sb.resolvePath("alice", sid, "/etc/passwd")).toThrow()
      expect(() => sb.resolvePath("alice", sid, "../secret")).toThrow()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("exec: 豁免用户脚本子进程不剔除敏感变量（操作者本人环境），其余用户仍脱敏", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-exempt-exec-"))
    const sb = new Sandbox({ home, enabled: true, isExempt: (u) => u === "default" })
    try {
      const cmd = 'node -e "console.log(process.env.SECRET_KEY || \'EMPTY\')"'
      // 豁免用户（默认用户）：SECRET_KEY 可见
      const exempt = await sb.exec(cmd, { env: { SECRET_KEY: "s3cret" }, user: "default" })
      expect(exempt.stdout.trim()).toBe("s3cret")
      // 非豁免用户：敏感变量被剔除（防任意用户经 sh/py 外泄服务端密钥）
      const restricted = await sb.exec(cmd, { env: { SECRET_KEY: "s3cret" }, user: "alice" })
      expect(restricted.stdout.trim()).toBe("EMPTY")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("Sandbox.exec (Windows 编码)", () => {
  test("cmd output with chinese decodes correctly", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const r = await sb.exec('echo 中文输出测试')
      expect(r.code).toBe(0)
      // chcp 65001 后 cmd 输出 UTF-8：解码后包含原文本（可能有换行/回车差异）
      expect(r.stdout.replace(/\r?\n/g, "")).toContain("中文输出测试")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("exec: cwd 不存在时自动创建（会话 tmp/ 缺失不再全体 ENOENT）；spawnBackground 同语义", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-cwd-"))
    const sb = new Sandbox({ home, enabled: false })
    const sid = "0123456789abcdef0123456789abcdef"
    const cwd = sb.workdir("default", sid) // 从未创建的会话 tmp 目录
    try {
      const r = await sb.exec("echo ok", { cwd })
      expect(r.code).toBe(0)
      expect(r.stdout.trim()).toBe("ok")
      // 后台任务：cwd 与 logPath 父目录缺失同样自动创建
      const { existsSync } = await import("node:fs")
      const logPath = join(cwd, "sh-tasks", "t1.log")
      const h = sb.spawnBackground("echo bg", { cwd, logPath })
      await h.exited
      expect(existsSync(logPath)).toBe(true)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("node --version output is captured (not swallowed)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const r = await sb.exec("node --version")
      expect(r.code).toBe(0)
      expect(r.stdout.trim()).toMatch(/^v\d+\./)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("abort signal kills the running command and returns interrupted result", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const controller = new AbortController()
      const started = Date.now()
      const p = sb.exec('node -e "setTimeout(()=>{}, 30000)"', { signal: controller.signal })
      // 等子进程真正启动后再中断（立即 abort 会在 spawn 完成前杀掉，仍可接受但时序不确定）
      await new Promise((r) => setTimeout(r, 300))
      controller.abort()
      const r = await p
      // 中断立即返回（远早于 30 秒），code 124 + [interrupted by user] 标记，与超时区分
      expect(Date.now() - started).toBeLessThan(10000)
      expect(r.code).toBe(124)
      expect(r.stderr).toContain("[interrupted by user]")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("timeout kills the running command with timed-out marker", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const started = Date.now()
      const r = await sb.exec('node -e "setTimeout(()=>{}, 30000)"', { timeoutMs: 400 })
      expect(Date.now() - started).toBeLessThan(10000)
      expect(r.code).toBe(124)
      expect(r.stderr).toContain("[timed out after 400ms]")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
