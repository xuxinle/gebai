/**
 * 霜天子代理工具测试：用真实 TCP mock 控制通道 + 假 ToolContext（真实临时目录）验证
 * ① 查询类工具（tree/find/get/visual/metrics/wait）的透传与错误处理；
 * ② 写类工具（set/invoke/click/type/key）的参数整形与审批声明；
 * ③ run 工具的生命周期（自举判定/构建失败传播/启动握手/停止优雅退出）；
 * ④ capture 的截图落盘与图片块。
 */
import { afterEach, describe, expect, test } from "bun:test"
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { isAbsolute, join } from "node:path"
import { createServer, type Server, type Socket } from "node:net"
import { mkdtempSync } from "node:fs"
import type { ToolContext } from "@gebai/sdk"
import { tools } from "./shuangtian_tools"

// 平台差异（测试夹具面向 Windows/Linux 两平台；此前只在 POSIX 下跑，Windows 上夹具会拼坏路径）
const IS_WIN = process.platform === "win32"
const BOOTSTRAP_SCRIPT = IS_WIN ? "bootstrap.ps1" : "bootstrap.sh"
const LAUNCH_MARK = IS_WIN ? "Start-Process" : "setsid"

interface MockControl {
  port: number
  calls: Array<{ method: string; params: Record<string, unknown> }>
  close: () => Promise<void>
  setHandler: (handler: (method: string, params: Record<string, unknown>) => unknown | { __error: { code: string; message: string } }) => void
}

async function startControl(): Promise<MockControl> {
  const calls: Array<{ method: string; params: Record<string, unknown> }> = []
  let handler: (method: string, params: Record<string, unknown>) => unknown | { __error: { code: string; message: string } } = (method) => ({ method })
  const sockets = new Set<Socket>()
  const server: Server = createServer((socket) => {
    sockets.add(socket)
    socket.on("close", () => sockets.delete(socket))
    let buffer = Buffer.alloc(0)
    socket.on("data", (chunk: Buffer) => {
      buffer = Buffer.concat([buffer, chunk])
      while (buffer.length >= 4) {
        const length = buffer.readUInt32BE(0)
        if (buffer.length < 4 + length) break
        const message = JSON.parse(buffer.subarray(4, 4 + length).toString("utf8")) as {
          id: number
          method: string
          params: Record<string, unknown>
        }
        buffer = buffer.subarray(4 + length)
        // 客户端每次连接会自动前置一个合成 hello（服务端协议门所必需）；
        // 测试关注的是“用户请求”，故把它过滤出 calls 视图（仍会被应答——
        // 握手行为本身由 shuangtian_client.test.ts 的专项用例验证）。
        const synthetic_handshake =
          message.method === "hello" && (message.params ?? {}).client === "shuangtian-agent"
        if (!synthetic_handshake) calls.push({ method: message.method, params: message.params ?? {} })
        const result = handler(message.method, message.params ?? {})
        const payload =
          typeof result === "object" && result !== null && "__error" in result
            ? JSON.stringify({ id: message.id, ok: false, error: (result as { __error: unknown }).__error })
            : JSON.stringify({ id: message.id, ok: true, result })
        const encoded = Buffer.from(payload, "utf8")
        const header = Buffer.alloc(4)
        header.writeUInt32BE(encoded.length, 0)
        socket.write(Buffer.concat([header, encoded]))
      }
    })
  })
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve))
  const address = server.address()
  const port = typeof address === "object" && address !== null ? address.port : 0
  return {
    port,
    calls,
    setHandler: (next) => {
      handler = next
    },
    close: async () => {
      for (const socket of sockets) socket.destroy()
      await new Promise<void>((resolve) => server.close(() => resolve()))
    },
  }
}

interface FakeEnv {
  ctx: ToolContext
  commands: Array<{ cmd: string; workdir?: string }>
  home: string
}

/** 假 ToolContext：真实临时目录 + 记录型 runCommand（不真起进程）。 */
function makeCtx(
  home: string,
  responder: (cmd: string) => { stdout?: string; stderr?: string; code?: number } = () => ({}),
): FakeEnv {
  const workdir = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(workdir, { recursive: true })
  const commands: Array<{ cmd: string; workdir?: string }> = []
  const ctx: ToolContext = {
    projects: [],
    resolveProjectPath: (name: string) => join(home, name),
    getTodos: async () => [],
    setTodos: async () => {},
    registry: {
      schemas: () => [],
      resolve: () => undefined,
      getAgentNames: () => [],
    },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => ({ choice: null, cancelled: true }) as never,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
    user: "default",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home,
    env: {},
    sandboxed: false,
    // 绝对路径必须原样返回：Windows 的 `C:\…` 不以 `/` 开头，旧判定会把它拼到 workdir 后（夹具 bug）
    resolvePath: (p) => (isAbsolute(p) ? p : join(workdir, p)),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(join(p, ".."), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async (p) => {
      const { readdir } = await import("node:fs/promises")
      try {
        const entries = await readdir(p, { withFileTypes: true })
        return entries.map((entry) => ({ name: entry.name, path: join(p, entry.name), isDir: entry.isDirectory() })) as never
      } catch {
        return []
      }
    },
    deleteFile: async (p) => {
      const { rm } = await import("node:fs/promises")
      await rm(p, { force: true })
    },
    moveFile: async () => {},
    runCommand: async (cmd, opts) => {
      commands.push({ cmd, workdir: opts?.workdir })
      const outcome = responder(cmd)
      return { stdout: outcome.stdout ?? "", stderr: outcome.stderr ?? "", code: outcome.code ?? 0 }
    },
    uploadAttachment: async () => "",
    publish: () => {},
  }
  return { ctx, commands, home }
}

/** 造一个最小框架工程目录（含 st.pkg 与已自举的 st）。 */
function makeFramework(home: string): string {
  const root = join(home, "shuangtian")
  mkdirSync(join(root, "build", "bin"), { recursive: true })
  writeFileSync(join(root, "st.pkg"), JSON.stringify({ name: "shuangtian", version: "0.1.0" }))
  // 平台正确的二进制名：Windows 是 st.exe（stBinary 按平台拼名，写错就永远走自举）
  writeFileSync(join(root, "build", "bin", IS_WIN ? "st.exe" : "st"), "#!/bin/sh\nexit 0\n")
  return root
}

const controls: MockControl[] = []
afterEach(async () => {
  while (controls.length > 0) {
    const control = controls.pop()
    if (control) await control.close()
  }
})

async function control(): Promise<MockControl> {
  const server = await startControl()
  controls.push(server)
  return server
}

/** 取一个**当前无人监听**的端口（绑完立即释放）：用来制造“陈旧控制文件”。 */
async function closedPort(): Promise<number> {
  const server: Server = createServer()
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve))
  const address = server.address()
  const port = typeof address === "object" && address !== null ? address.port : 0
  await new Promise<void>((resolve) => server.close(() => resolve()))
  return port
}

describe("只读查询工具", () => {
  test("tree/find/get/visual/metrics 透传参数并返回结构化 data", async () => {
    const server = await control()
    server.setHandler((method, params) => ({ method, params, tree: { id: "root" } }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const target = `127.0.0.1:${server.port}`

    const tree = await tools.tree.execute({ depth: 2, target }, ctx)
    expect(tree.output).toContain("tree")
    expect(server.calls[0]).toEqual({ method: "tree", params: { depth: 2 } })

    await tools.find.execute({ selector: "Button[text~=保存]", limit: 5, target }, ctx)
    expect(server.calls[1].params).toEqual({ selector: "Button[text~=保存]", limit: 5 })

    await tools.get.execute({ id: "#btn-save", target }, ctx)
    expect(server.calls[2].params).toEqual({ id: "#btn-save" })

    await tools.visual.execute({ target }, ctx)
    expect(server.calls[3].method).toBe("visual")

    const metrics = await tools.metrics.execute({ target }, ctx)
    expect(metrics.data).toMatchObject({ method: "metrics" })
  })

  test("控制通道错误被转成可读文本（工具不抛异常）", async () => {
    const server = await control()
    server.setHandler(() => ({ __error: { code: "not_found", message: "未找到元素: #nope" } }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const result = await tools.get.execute({ id: "#nope", target: `127.0.0.1:${server.port}` }, ctx)
    expect(result.output).toContain("not_found")
    expect(result.output).toContain("解析失败".slice(0, 0) + "未找到元素")
  })

  test("无运行实例时给出可操作的指引", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const result = await tools.tree.execute({}, ctx)
    expect(result.output).toContain("未找到运行中的应用")
    expect(result.output).toContain("shuangtian_run")
  })

  test("apps：扫描运行期控制文件并校验连通性", async () => {
    const server = await control()
    server.setHandler(() => ({ app: { name: "mdeditor" }, backend: "headless" }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const dir = join(ctx.resolvePath(".shuangtian"))
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "mdeditor-control.json"), JSON.stringify({ port: server.port, pid: 4242 }))

    const result = await tools.apps.execute({}, ctx)
    expect(result.output).toContain("mdeditor")
    expect(result.output).toContain("可控制")
  })

  test("wait：超时返回 satisfied=false（不报错）", async () => {
    const server = await control()
    server.setHandler(() => ({ satisfied: false, elapsed_ms: 1000, detail: "等待超时: text" }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const result = await tools.wait.execute(
      { for: "text", text: "完成", timeout_ms: 200, target: `127.0.0.1:${server.port}` },
      ctx,
    )
    expect(result.data).toMatchObject({ satisfied: false })
  })
})

describe("写类工具", () => {
  test("set/invoke/click/type/key 需要审批（写与输入类）", () => {
    expect(tools.set.requiresApproval).toBe(true)
    expect(tools.invoke.requiresApproval).toBe(true)
    expect(tools.click.requiresApproval).toBe(true)
    expect(tools.type.requiresApproval).toBe(true)
    expect(tools.key.requiresApproval).toBe(true)
    // 只读类免审批
    for (const name of ["tree", "find", "get", "visual", "metrics", "capture", "wait", "apps"] as const) {
      expect(tools[name].requiresApproval ?? false).toBe(false)
    }
  })

  test("click 坐标与修饰键按协议整形（逻辑像素）", async () => {
    const server = await control()
    server.setHandler(() => ({ handled: true, hit: { id: "#btn" } }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    await tools.click.execute(
      { x: 120.5, y: 64, kind: "dblclick", ctrl: true, shift: true, target: `127.0.0.1:${server.port}` },
      ctx,
    )
    expect(server.calls[0].method).toBe("input.mouse")
    expect(server.calls[0].params).toMatchObject({
      kind: "dblclick",
      x: 120.5,
      y: 64,
      ctrl: true,
      shift: true,
      alt: false,
      button: 1,
    })
  })

  test("type 支持先聚焦指定组件再输入", async () => {
    const server = await control()
    server.setHandler(() => ({ handled: true, focused: "#editor" }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    await tools.type.execute({ text: "霜天", id: "#editor", target: `127.0.0.1:${server.port}` }, ctx)
    expect(server.calls[0].params).toEqual({ text: "霜天", id: "#editor" })
  })

  test("set 传 props 对象", async () => {
    const server = await control()
    server.setHandler(() => ({ changed: ["text"] }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const result = await tools.set.execute(
      { id: "#title", props: { text: "新标题" }, target: `127.0.0.1:${server.port}` },
      ctx,
    )
    expect(server.calls[0].params).toEqual({ id: "#title", props: { text: "新标题" } })
    expect(result.data).toMatchObject({ changed: ["text"] })
  })

  test("call 逃生门透传任意方法与参数", async () => {
    const server = await control()
    server.setHandler((method, params) => ({ method, params }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    await tools.call.execute(
      { method: "app", params: { action: "set_scale", scale: 2 }, target: `127.0.0.1:${server.port}` },
      ctx,
    )
    expect(server.calls[0]).toMatchObject({ method: "app", params: { action: "set_scale", scale: 2 } })
  })
})

describe("capture", () => {
  test("按指定路径落盘并返回图片块", async () => {
    const server = await control()
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const path = join(home, "shot.png")
    server.setHandler(() => {
      // 模拟服务端写盘（真实实现由应用侧写 PNG）
      writeFileSync(path, Buffer.from([0x89, 0x50, 0x4e, 0x47]))
      return { path, format: "png", region: { x: 0, y: 0, width: 1280, height: 800 } }
    })
    const result = await tools.capture.execute({ path, target: `127.0.0.1:${server.port}` }, ctx)
    expect(result.output).toContain(path)
    expect(result.blocks?.[0]).toMatchObject({ type: "image", path })
    expect(server.calls[0].params).toMatchObject({ encode: "file", path })
    expect(existsSync(path)).toBe(true)
  })

  test("region 参数只在同时给出宽高时下发", async () => {
    const server = await control()
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    server.setHandler(() => ({ path: join(home, "x.png"), format: "png" }))
    await tools.capture.execute({ x: 10, y: 20, width: 100, height: 50, path: join(home, "x.png"), target: `127.0.0.1:${server.port}` }, ctx)
    expect(server.calls[0].params.region).toEqual({ x: 10, y: 20, width: 100, height: 50 })
  })

  test("控制通道失败时返回文本且不带图片块", async () => {
    const server = await control()
    server.setHandler(() => ({ __error: { code: "invalid", message: "截图区域为空" } }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const result = await tools.capture.execute({ target: `127.0.0.1:${server.port}` }, ctx)
    expect(result.blocks).toBeUndefined()
    expect(result.output).toContain("invalid")
  })
})

describe("run 生命周期", () => {
  test("target=build：缺工具链时先自举，再构建", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = join(home, "shuangtian")
    mkdirSync(root, { recursive: true })
    writeFileSync(join(root, "st.pkg"), JSON.stringify({ name: "shuangtian" }))
    const { ctx, commands } = makeCtx(home, (cmd) =>
      cmd.includes("bootstrap") ? { stdout: "bootstrap ok" } : { stdout: "构建完成 [dev]" },
    )
    const result = await tools.run.execute({ action: "build", target: "gallery", framework: root }, ctx)
    expect(commands[0].cmd).toContain(BOOTSTRAP_SCRIPT)
    // `st` 用框架自带的绝对路径（工程可能不在框架目录下），构建发生在**工程目录**
    expect(commands[1].cmd).toContain("build gallery")
    expect(commands[1].cmd).toContain("--profile dev")
    expect(result.output).toContain("构建完成")
  })

  test("构建失败：把编译输出尾部原样透出（便于定位）", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const { ctx } = makeCtx(home, (cmd) =>
      cmd.includes("build") ? { code: 1, stderr: "error: 'foo' was not declared" } : {},
    )
    const result = await tools.run.execute({ action: "build", target: "gallery", framework: root }, ctx)
    expect(result.output).toContain("构建失败")
    expect(result.output).toContain("not declared")
  })

  test("交叉编译：--toolchain 透传给 st，产物目录与 .exe 后缀如实告知", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const commands: string[] = []
    const { ctx } = makeCtx(home, (cmd) => {
      commands.push(cmd)
      return { stdout: "构建完成 [release]" }
    })
    const result = await tools.run.execute(
      { action: "build", target: "gallery", profile: "release", toolchain: "mingw", framework: root },
      ctx,
    )
    // 命令行必须带上工具链（否则只是"普通构建"，用户以为交叉编译了）
    expect(commands.some((c) => c.includes("--toolchain=mingw"))).toBe(true)
    expect(result.output).toContain("交叉编译完成")
    expect(result.output).toContain("build/release-mingw/bin/gallery.exe")
  })

  test("交叉编译产物不能在本机启动：start 明确拒绝而非启动失败", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const { ctx } = makeCtx(home, () => ({ stdout: "构建完成 [release]" }))
    const result = await tools.run.execute(
      { action: "start", target: "gallery", toolchain: "mingw", framework: root },
      ctx,
    )
    expect(result.output).toContain("无法在本机启动")
    // 不能把工具链名渲染成对象（参数与内部引导变量曾同名遮蔽）
    expect(result.output).not.toContain("[object Object]")
  })

  test("action=init：在指定目录创建独立工程（并告知下一步）", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const { ctx, commands } = makeCtx(home, () => ({ stdout: "工程已创建: /tmp/app" }))
    const result = await tools.run.execute({ action: "init", project: join(home, "app"), name: "myapp", framework: root }, ctx)
    const init = commands.find((item) => item.cmd.includes("init"))
    expect(init).toBeDefined()
    expect(init!.cmd).toContain("--framework")
    expect(init!.cmd).toContain("myapp")
    expect(result.output).toContain("下一步")
  })

  test("独立工程：project 与 framework 是不同目录时，构建发生在工程目录", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const app = join(home, "app")
    mkdirSync(app, { recursive: true })
    writeFileSync(join(app, "st.pkg"), JSON.stringify({ name: "app", framework: { path: root } }))
    const { ctx, commands } = makeCtx(home, (cmd) =>
      cmd.includes("build") ? { stdout: "构建完成 [dev]" } : { stdout: "" },
    )
    const result = await tools.run.execute(
      { action: "build", target: "app", project: app, framework: root },
      ctx,
    )
    const build = commands.find((item) => item.cmd.includes("build app"))
    expect(build).toBeDefined()
    // workdir 必须是工程目录（否则 st 会去框架里找目标）
    expect(build!.workdir).toBe(app)
    expect(result.output).toContain("构建完成")
  })

  test("工程清单缺失：提示用 init 创建（而不是含糊失败）", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const { ctx } = makeCtx(home)
    const result = await tools.run.execute(
      { action: "build", target: "app", project: join(home, "missing"), framework: root },
      ctx,
    )
    expect(result.output).toContain("未找到工程清单")
    expect(result.output).toContain("action=init")
  })

  test("框架目录缺失：明确提示而非静默", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const { ctx } = makeCtx(home)
    const result = await tools.run.execute({ action: "build", framework: join(home, "nope") }, ctx)
    expect(result.output).toContain("未找到霜天框架")
  })

  test("action=start：脱离会话启动并等待控制端口，返回 port/pid/控制文件", async () => {
    const server = await control()
    server.setHandler(() => ({ app: { name: "mdeditor" }, backend: "headless", headless: true }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    mkdirSync(join(root, "build", "dev", "bin"), { recursive: true })
    writeFileSync(join(root, "build", "dev", "bin", IS_WIN ? "mdeditor.exe" : "mdeditor"), "#!/bin/sh\nexit 0\n")

    const { ctx, commands } = makeCtx(home, (cmd) => {
      if (cmd.includes(LAUNCH_MARK)) {
        // 模拟应用启动后写控制文件
        const controlDir = join(ctx.sessionWorkdir ?? ctx.workdir, ".shuangtian")
        mkdirSync(controlDir, { recursive: true })
        writeFileSync(join(controlDir, "mdeditor-control.json"), JSON.stringify({ port: server.port, pid: 777 }))
        return { stdout: "777" }
      }
      return { stdout: "构建完成 [dev]" }
    })
    const result = await tools.run.execute({ action: "start", target: "mdeditor", framework: root }, ctx)
    expect(result.output).toContain("已启动 mdeditor")
    expect(result.output).toContain(String(server.port))
    expect(result.data).toMatchObject({ ok: true, pid: 777, port: server.port })
    const launch = commands.find((item) => item.cmd.includes(LAUNCH_MARK))
    expect(launch?.cmd).toContain("--headless")
    expect(launch?.cmd).toContain("--control-file")
  })

  test("action=status：进程与通道状态一并汇报", async () => {
    const server = await control()
    server.setHandler(() => ({ ts: 1 }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const { ctx } = makeCtx(home, () => ({ stdout: "0" }))
    const dir = join(ctx.sessionWorkdir ?? ctx.workdir, ".shuangtian")
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "gallery-control.json"), JSON.stringify({ port: server.port, pid: 31337 }))
    const result = await tools.run.execute({ action: "status", target: "gallery", framework: root }, ctx)
    expect(result.output).toContain("存活")
    expect(result.output).toContain("响应正常")
  })

  test("action=start：陈旧控制文件不得被当成“本次启动的结果”（回归：误连旧实例）", async () => {
    // 真机踩到的坑：就绪轮询的判据曾是“文件存在 + 端口 > 0”，而**上一轮遗留的文件立即满足它**——
    // 刚起的进程还没写自己的端口，工具已经把旧实例的端口返回了。
    // 轻则握手失败，重则连到另一个实例并“成功”（后续 capture/tree 操作的全是它）。
    const server = await control()
    server.setHandler(() => ({ app: { name: "mdeditor" }, backend: "headless", headless: true }))
    const stale_port = await closedPort()
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    mkdirSync(join(root, "build", "dev", "bin"), { recursive: true })
    writeFileSync(join(root, "build", "dev", "bin", IS_WIN ? "mdeditor.exe" : "mdeditor"), "#!/bin/sh\nexit 0\n")

    const { ctx } = makeCtx(home, (cmd) => {
      if (cmd.includes(LAUNCH_MARK)) {
        // **延迟写入**：复现真实时序——进程启动到写控制文件之间有一段窗口，
        // 而轮询在这一窗口里看到的只能是上一轮遗留的文件（正是误连的根因）。
        // 同步写入会让这个用例失去鉴别力（旧实现也能“蒙对”）。
        setTimeout(() => {
          const controlDir = join(ctx.sessionWorkdir ?? ctx.workdir, ".shuangtian")
          mkdirSync(controlDir, { recursive: true })
          writeFileSync(
            join(controlDir, "mdeditor-control.json"),
            JSON.stringify({ port: server.port, pid: 777 }),
          )
        }, 600)
        return { stdout: "777" }
      }
      return { stdout: "构建完成 [dev]" }
    })
    // 预置一个陈旧文件（端口无人监听、pid 也不是本轮的）
    const dir = join(ctx.sessionWorkdir ?? ctx.workdir, ".shuangtian")
    mkdirSync(dir, { recursive: true })
    writeFileSync(
      join(dir, "mdeditor-control.json"),
      JSON.stringify({ port: stale_port, pid: 4242, token: "stale-token" }),
    )

    const result = await tools.run.execute({ action: "start", target: "mdeditor", framework: root }, ctx)
    expect(result.data).toMatchObject({ ok: true, pid: 777, port: server.port })
    expect(result.data).not.toMatchObject({ port: stale_port })
    expect(result.output).toContain("已启动 mdeditor")
  })

  test("action=start：已有存活实例时明确拒绝（不允许静默双实例）", async () => {
    // 静默起第二个实例的后果比“起不来”严重得多：两个进程抢同一个控制文件，
    // 后续每个工具调用都可能落到另一个实例上（截图/改属性看似成功却是另一个界面）。
    const server = await control()
    server.setHandler(() => ({ ok: true, ts: 1 }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    mkdirSync(join(root, "build", "dev", "bin"), { recursive: true })
    writeFileSync(join(root, "build", "dev", "bin", IS_WIN ? "mdeditor.exe" : "mdeditor"), "#!/bin/sh\nexit 0\n")
    const { ctx, commands } = makeCtx(home, () => ({ stdout: "构建完成 [dev]" }))
    const dir = join(ctx.sessionWorkdir ?? ctx.workdir, ".shuangtian")
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "mdeditor-control.json"), JSON.stringify({ port: server.port, pid: 5150 }))

    const result = await tools.run.execute({ action: "start", target: "mdeditor", framework: root }, ctx)
    expect(result.output).toContain("已有一个存活的")
    expect(result.output).toContain("action=stop")
    expect(result.data).toMatchObject({ ok: false, already_running: true, port: server.port })
    // 关键：**没有真的去启动**（否则就又是双实例）
    expect(commands.some((item) => item.cmd.includes(LAUNCH_MARK))).toBe(false)
  })

  test("控制文件对应实例已不在：快速失败并给出可行动提示（而非传输错误）", async () => {
    const server = await control()
    server.setHandler(() => ({ ts: 1 }))
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    // 进程名查询返回空（进程不存在）→ 文件陈旧
    const { ctx } = makeCtx(home, () => ({ stdout: "" }))
    const dir = join(ctx.sessionWorkdir ?? ctx.workdir, ".shuangtian")
    mkdirSync(dir, { recursive: true })
    writeFileSync(
      join(dir, "mdeditor-control.json"),
      JSON.stringify({ port: server.port, pid: 999_999, token: "tk" }),
    )
    const result = await tools.tree.execute({}, ctx)
    expect(result.output).toContain("已不在运行")
    expect(result.output).toContain("start")
    // 不应把“连不上”抛成难以理解的传输错误
    expect(result.output).not.toContain("transport")
    expect(server.calls).toHaveLength(0)
  })

  test("action=test：san 开关与退出码透传", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-tools-"))
    const root = makeFramework(home)
    const { ctx, commands } = makeCtx(home, () => ({ code: 0, stdout: "7 passed, 0 failed" }))
    const result = await tools.run.execute({ action: "test", san: true, framework: root }, ctx)
    expect(commands.some((item) => item.cmd.includes("test") && item.cmd.includes("--san"))).toBe(
      true,
    )
    expect(result.output).toContain("7 passed")
  })
})

describe("框架与提示词装配", () => {
  test("def 导出完整且工具命名与 def 一致", async () => {
    const mod = await import("./shuangtian")
    expect(mod.def.name).toBe("shuangtian")
    expect(mod.def.systemPrompt.length).toBeGreaterThan(200)
    expect(Object.keys(mod.def.tools ?? {}).sort()).toEqual(Object.keys(tools).sort())
    expect(mod.def.preload).toBe(false)
    expect(mod.def.description).toContain("无头")
    // 环境变量声明必须带前缀（前端面板白名单口径）
    for (const item of mod.def.envVars ?? []) {
      expect(item.name.startsWith("SHUANGTIAN")).toBe(true)
      expect(item.description.length).toBeGreaterThan(5)
    }
  })

  test("系统提示词写明关键约定（无头/逻辑坐标/截图核验/收尾）", async () => {
    const mod = await import("./shuangtian")
    const prompt = mod.def.systemPrompt
    expect(prompt).toContain("无头")
    expect(prompt).toContain("逻辑像素")
    expect(prompt).toContain("capture")
    expect(prompt).toContain("stop")
    expect(prompt).toContain("st-control/1")
  })

  test("README/客户端导出面可用（协议常量与错误类型）", async () => {
    const client = await import("./shuangtian_client")
    expect(typeof client.resolve_target).toBe("function")
    expect(typeof client.request).toBe("function")
    expect(client.ControlError.prototype).toBeInstanceOf(Error)
    // 工具文件中不应出现硬编码端口（端口一律来自控制文件或参数）
    const source = readFileSync(new URL("./shuangtian_tools.ts", import.meta.url), "utf8")
    expect(source).not.toMatch(/127\.0\.0\.1:9\d{3}/)
  })
})
