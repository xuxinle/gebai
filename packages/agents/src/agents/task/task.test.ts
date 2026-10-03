import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import type { Task, TaskCreateInput, TaskFileEntry, TaskNotifyMessage, TaskQueueView, TaskService, TaskUpdateInput, ToolContext } from "@gebai/sdk"
import { def, name, tools } from "./task"

function task(over: Partial<Task> = {}): Task {
  return {
    id: "t1",
    user: "default",
    kind: "scheduled",
    runner: "script",
    schedule: "0 9 * * *",
    script: "echo hi",
    enabled: true,
    createdAt: 1,
    updatedAt: 1,
    state: "idle",
    runCount: 0,
    ...over,
  }
}

function emptyQueue(): TaskQueueView {
  return { user: "default", limit: 5, running: [], entries: [], busy: false }
}

/** 记录调用的服务替身（默认全部返回空结果，用例按需覆盖）。 */
function service(over: Partial<TaskService> = {}): TaskService {
  return {
    add: async () => task(),
    list: async () => [],
    get: async () => null,
    remove: async () => false,
    update: async () => null,
    run: async () => null,
    cancel: async () => false,
    stop: async () => false,
    queue: async () => emptyQueue(),
    runs: async () => [],
    notify: async () => ({ taskId: "t1", delivered: 1, errors: [] }),
    files: async () => [],
    readFile: async () => "",
    writeFile: async () => ({ path: "x", size: 0, mtimeMs: 0, dir: false }),
    deleteFile: async () => false,
    ...over,
  }
}

function ctx(home: string, tasks?: TaskService): ToolContext {
  return {
    user: "default",
    sessionId: "s1",
    workdir: home,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => p,
    readFile: async () => "x",
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (ref) => ref.name,
    publish: () => {},
    registry: {
      schemas: () => [],
      resolve: () => undefined,
      getAgentNames: () => [],
    },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
    projects: [],
    resolveProjectPath: () => home,
    getTodos: async () => [],
    setTodos: async () => {},
    ...(tasks ? { tasks } : {}),
  }
}

describe("task sub-agent", () => {
  test("def 结构与命名空间：task_* 工具（add/list/update/run/cancel/remove/files/notify）", () => {
    expect(def.name).toBe("task")
    expect(name).toBe("task")
    expect(Object.keys(tools).sort()).toEqual(["add", "cancel", "files", "list", "notify", "remove", "run", "update"])
    expect(def.preload).toBe(false)
    // 任务 = 无人值守的任意命令/会话执行：创建/修改/删除/执行/取消均需审批（防多用户模式绕过审批边界）
    expect(def.requiresApproval).toEqual({ add: true, update: true, remove: true, run: true, cancel: true, files: true, notify: false })
    for (const t of ["add", "update", "remove", "run", "cancel", "files"]) expect(tools[t].requiresApproval).toBe(true)
    expect(tools.list.requiresApproval).toBeFalsy()
    expect(tools.list.safeMode).toBe(true)
    // 主动通知：无人值守场景等不到人工审批，故免审；投递目标限定为用户已配置的通道，安全模式下不提供
    expect(tools.notify.requiresApproval).toBe(false)
    expect(tools.notify.safeMode).toBe(false)
  })

  test("能力未启用（ctx.tasks 缺省）：各工具明确提示且不抛错", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-task-subagent-"))
    try {
      const c = ctx(home)
      for (const [tool, args] of [
        [tools.add, { runner: "script", script: "echo" }],
        [tools.list, {}],
        [tools.update, { id: "t1", enabled: false }],
        [tools.run, { id: "t1" }],
        [tools.cancel, { id: "t1" }],
        [tools.remove, { id: "t1" }],
        [tools.files, { id: "t1", op: "list" }],
        [tools.notify, { text: "hi" }],
      ] as const) {
        const r = await (tool as { execute: (a: Record<string, unknown>, x: ToolContext) => Promise<{ output: string }> }).execute(args as Record<string, unknown>, c)
        expect(r.output).toContain("未启用")
      }
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("add：参数透传与 notify/agents 归一，输出含任务 id", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-task-subagent-"))
    try {
      let captured: TaskCreateInput | undefined
      let updatedPatch: TaskUpdateInput | undefined
      const c = ctx(
        home,
        service({
          add: async (input) => {
            captured = input
            return task({ id: "new1", name: "daily" })
          },
          update: async (_id, patch) => {
            updatedPatch = patch
            return task({ id: "t1", name: "daily" })
          },
        }),
      )
      const r = await tools.add.execute(
        {
          kind: "scheduled",
          runner: "script",
          name: "daily",
          schedule: "0 9 * * *",
          timezone: "Asia/Shanghai",
          script: "echo hi",
          timeout_ms: 1000,
          max_consecutive_errors: 5,
          notify_on: "model",
          notify: [{ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/x", secret: "s3cr3t", at: ["ou_a"] }],
          env: { REPORT_DIR: "data", PORT: 8080 },
          front: true,
          run_now: false,
        },
        c,
      )
      expect(captured).toMatchObject({
        kind: "scheduled",
        runner: "script",
        name: "daily",
        schedule: "0 9 * * *",
        timezone: "Asia/Shanghai",
        script: "echo hi",
        timeoutMs: 1000,
        maxConsecutiveErrors: 5,
        notifyOn: "model",
        front: true,
        runNow: false,
      })
      // 工具面用 snake_case 入参，契约是 camelCase：secret/at 原样透传（secret 不下发脱敏占位）
      expect(captured!.notify).toEqual([{ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/x", secret: "s3cr3t", at: ["ou_a"] }])
      // env：任务级环境变量透传（非字符串值字符串化，模型常把端口写成数字）
      expect(captured!.env).toEqual({ REPORT_DIR: "data", PORT: "8080" })
      expect(r.output).toContain("new1")

      // env 非对象：明确报错（不静默丢失用户配置）
      await expect(tools.add.execute({ runner: "script", script: "echo", env: "A=1" }, c)).rejects.toThrow(/env 须为/)

      // update：env 传空对象清除，未传不改动
      // update：env 传空对象清除，未传不改动
      await tools.update.execute({ id: "t1", env: { ONLY: "1" } }, c)
      expect(updatedPatch!.env).toEqual({ ONLY: "1" })
      await tools.update.execute({ id: "t1", name: "renamed" }, c)
      expect(updatedPatch!.env).toBeUndefined()

      // agents：单值也归一为数组；prompt 型目标与预载名单透传
      await tools.add.execute({ runner: "prompt", prompt: "跑一次巡检", target: "sticky", agents: ["code"] }, c)
      expect(captured).toMatchObject({ runner: "prompt", prompt: "跑一次巡检", target: "sticky", agents: ["code"] })
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("list：任务清单 + 队列概览（额度/排队顺序/置顶/等待原因）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-task-subagent-"))
    try {
      const c = ctx(
        home,
        service({
          list: async () => [
            task({ id: "t1", name: "日报", kind: "scheduled", runner: "script", schedule: "@daily", state: "running", runCount: 3, lastStatus: "error", lastError: "exit 1" }),
            task({ id: "t2", kind: "idle", runner: "prompt", prompt: "闲时跑", schedule: undefined, script: undefined }),
          ],
          queue: async () => ({
            user: "default",
            limit: 5,
            running: [{ taskId: "t1", name: "日报", kind: "scheduled", runner: "script", sessionId: "sess-1", startedAt: 1 }],
            entries: [
              { taskId: "t3", user: "default", kind: "manual", runner: "prompt", name: "批量", source: "manual", priority: 999, enqueuedAt: 2, front: true, position: 1, waiting: "目标会话正在运行，等待中" },
            ],
            busy: true,
          }),
        }),
      )
      const r = await tools.list.execute({}, c)
      expect(r.output).toContain("并发额度: 1/5")
      expect(r.output).toContain("t1")
      expect(r.output).toContain("日报")
      expect(r.output).toContain("运行中")
      expect(r.output).toContain("t3")
      expect(r.output).toContain("置顶")
      expect(r.output).toContain("等待中")
      expect(r.output).toContain("exit 1")
      // 类别过滤透传（scheduled/manual/idle）：只筛任务清单，队列概览始终为全量
      const only = await tools.list.execute({ kind: "idle" }, c)
      expect(only.output).toContain("t2")
      expect(only.output).not.toContain("- t1 |")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("update：补丁映射与不存在分支", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-task-subagent-"))
    try {
      let patch: TaskUpdateInput | undefined
      const c = ctx(
        home,
        service({
          update: async (_id, p) => {
            patch = p
            return task({ id: "t1", name: "改名后", enabled: false })
          },
        }),
      )
      const r = await tools.update.execute({ id: "t1", name: "改名后", enabled: false, schedule: "0 10 * * *", timeout_ms: 2000, notify_on: "auto", agents: ["code"] }, c)
      expect(patch).toMatchObject({ name: "改名后", enabled: false, schedule: "0 10 * * *", timeoutMs: 2000, notifyOn: "auto", agents: ["code"] })
      expect(r.output).toContain("已更新")
      expect(r.output).toContain("t1")

      const c2 = ctx(home, service({ update: async () => null }))
      const missing = await tools.update.execute({ id: "nope", enabled: true }, c2)
      expect(missing.output).toContain("不存在")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("run/cancel/remove：入队、出队与终止、删除分支", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-task-subagent-"))
    try {
      const c = ctx(
        home,
        service({
          run: async (_id, opts) =>
            opts?.front === true
              ? { task: task({ id: "t1", name: "置顶任务" }), queued: true, position: 1 }
              : { task: task({ id: "t1", name: "普通任务" }), queued: true, position: 3 },
          cancel: async (id) => id === "t1",
          stop: async (id) => id === "t1",
          remove: async (id) => id === "t1",
        }),
      )
      const queued = await tools.run.execute({ id: "t1" }, c)
      expect(queued.output).toContain("已入队")
      expect(queued.output).toContain("队列位置: 3")
      const front = await tools.run.execute({ id: "t1", front: true }, c)
      expect(front.output).toContain("队列位置: 1")

      // 未入队（已在运行/已停用等）：如实回显原因
      const c2 = ctx(home, service({ run: async () => ({ task: task({ id: "t1" }), queued: false, reason: "任务正在运行中" }) }))
      const blocked = await tools.run.execute({ id: "t1" }, c2)
      expect(blocked.output).toContain("未入队")
      expect(blocked.output).toContain("任务正在运行中")

      const c3 = ctx(home, service())
      expect((await tools.run.execute({ id: "nope" }, c3)).output).toContain("不存在")

      expect((await tools.cancel.execute({ id: "t1" }, c)).output).toContain("已出队")
      expect((await tools.cancel.execute({ id: "t1", mode: "stop" }, c)).output).toContain("已请求终止")
      expect((await tools.cancel.execute({ id: "nope" }, c3)).output).toContain("不在排队中")
      expect((await tools.cancel.execute({ id: "nope", mode: "stop" }, c3)).output).toContain("未在运行中")

      expect((await tools.remove.execute({ id: "t1" }, c)).output).toContain("已删除")
      expect((await tools.remove.execute({ id: "nope" }, c3)).output).toContain("不存在")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("files：列表/读/写/删与错误分支", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-task-subagent-"))
    try {
      const entries: TaskFileEntry[] = [
        { path: "scripts", size: 0, mtimeMs: 0, dir: true },
        { path: "scripts/run.sh", size: 12, mtimeMs: 1, dir: false },
      ]
      let written: { path: string; content: string } | undefined
      const c = ctx(
        home,
        service({
          files: async () => entries,
          readFile: async (_id, path) => (path === "scripts/run.sh" ? "echo hello\n" : (() => { throw new Error(`文件不存在: ${path}`) })()),
          writeFile: async (_id, path, content) => {
            written = { path, content }
            return { path, size: content.length, mtimeMs: 2, dir: false }
          },
          deleteFile: async (_id, path) => path === "scripts/run.sh",
        }),
      )
      const listed = await tools.files.execute({ id: "t1", op: "list" }, c)
      expect(listed.output).toContain("[目录] scripts")
      expect(listed.output).toContain("scripts/run.sh")
      expect(listed.output).toContain("12 字节")

      const read = await tools.files.execute({ id: "t1", op: "read", path: "scripts/run.sh" }, c)
      expect(read.output).toContain("echo hello")
      await expect(tools.files.execute({ id: "t1", op: "read", path: "missing.sh" }, c)).rejects.toThrow("文件不存在")

      const write = await tools.files.execute({ id: "t1", op: "write", path: "scripts/new.sh", content: "echo new\n" }, c)
      expect(written).toEqual({ path: "scripts/new.sh", content: "echo new\n" })
      expect(write.output).toContain("已写入")

      expect((await tools.files.execute({ id: "t1", op: "delete", path: "scripts/run.sh" }, c)).output).toContain("已删除")
      expect((await tools.files.execute({ id: "t1", op: "delete", path: "missing.sh" }, c)).output).toContain("不存在")

      // 缺 path / 未知 op：明确提示而非静默
      expect((await tools.files.execute({ id: "t1", op: "read" }, c)).output).toContain("缺少 path")
      expect((await tools.files.execute({ id: "t1", op: "wat" }, c)).output).toContain("不支持的操作")

      const empty = ctx(home, service({ files: async () => [] }))
      expect((await tools.files.execute({ id: "t1", op: "list" }, empty)).output).toContain("为空")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("notify：正文/标题/@ 人透传，id 缺省时不传（由服务端按执行会话推断）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-task-subagent-"))
    try {
      const calls: Array<{ input: TaskNotifyMessage; id?: string }> = []
      const c = ctx(
        home,
        service({
          notify: async (input, id) => {
            calls.push({ input, id })
            return { taskId: "t1", delivered: 1, errors: [] }
          },
        }),
      )
      const r = await tools.notify.execute({ text: "  磁盘占用 92%  ", title: "巡检告警", at: ["ou_a"] }, c)
      expect(r.output).toContain("已推送")
      expect(r.output).toContain("t1")
      expect(calls[0].input).toEqual({ text: "磁盘占用 92%", title: "巡检告警", at: ["ou_a"] })
      expect(calls[0].id).toBeUndefined()

      await tools.notify.execute({ text: "x", id: "t9" }, c)
      expect(calls[1].id).toBe("t9")

      // 缺正文：明确提示且不调服务
      expect((await tools.notify.execute({}, c)).output).toContain("缺少 text")
      expect(calls).toHaveLength(2)

      // 服务错误（无通道/安全模式等）：转述原因不抛错
      const bad = ctx(
        home,
        service({
          notify: async () => {
            throw new Error("安全模式：通知投递已限制")
          },
        }),
      )
      expect((await tools.notify.execute({ text: "x" }, bad)).output).toContain("通知失败：安全模式")
      // 部分通道失败：保留成功数并附问题
      const partial = ctx(home, service({ notify: async () => ({ taskId: "t1", delivered: 1, errors: ["feishu: 通知投递失败: HTTP 500"] }) }))
      const pr = await tools.notify.execute({ text: "x" }, partial)
      expect(pr.output).toContain("1 个通道")
      expect(pr.output).toContain("HTTP 500")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
