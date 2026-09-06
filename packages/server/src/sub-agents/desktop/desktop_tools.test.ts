import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import type { ToolContext } from "../../core/base/types"
import {
  screenshotTool,
  windowListTool,
  windowFocusTool,
  windowStateTool,
  typeTextTool,
  keyPressTool,
  mouseClickTool,
  mouseScrollTool,
  mouseDragTool,
  clipboardReadTool,
  clipboardWriteTool,
  screenInfoTool,
  detectSensitive,
} from "./desktop_tools"
import { def as desktopDef } from "./desktop"

// 工具按 process.platform 分支（win32 走 PowerShell，linux 走 xdotool 等）：
// 大部分用例断言 Windows 行为，统一 mock 为 win32（linux 专属用例内部自行覆盖并恢复）
const ORIGINAL_PLATFORM = process.platform
beforeAll(() => {
  Object.defineProperty(process, "platform", { value: "win32" })
})
afterAll(() => {
  Object.defineProperty(process, "platform", { value: ORIGINAL_PLATFORM })
})

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => { throw new Error("未知预置项目") },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "ok", archive: { runId: "r", agents: ["x"], input: "", output: "ok", messages: [] } }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    waitForCapture: async () => null,
  }
  return { ...base, ...overrides }
}

/** 解码 PowerShell -EncodedCommand，便于断言脚本内容。 */
function decodeCmd(cmd: string): string {
  const m = cmd.match(/-EncodedCommand (\S+)/)
  return m ? Buffer.from(m[1], "base64").toString("utf16le") : cmd
}

describe("desktop tools", () => {
  test("screenshot rejects in sandboxed (server deployment) mode", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, { sandboxed: true })
    await expect(screenshotTool.execute({}, c)).rejects.toThrow(/本地\/桌面/)
  })

  test("screenshot captures full screen and returns image block", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        // 只记录主截图命令（探测命令 identify/convert 忽略，不影响断言）
        if (cmd.includes("powershell")) seenCmd = cmd
        const script = decodeCmd(cmd)
        const m = script.match(/'([^']+\.png)'/)
        if (m) await c.writeFile(m[1], "")
        return { stdout: "", stderr: "", code: 0 }
      },
    })
    const r = await screenshotTool.execute({}, c)
    expect(seenCmd).toContain("powershell")
    expect(seenCmd).toContain("-EncodedCommand")
    const script = decodeCmd(seenCmd)
    // 全屏 = 虚拟屏幕（覆盖所有显示器，副屏可为负坐标）；脚本声明 DPI 感知（物理像素坐标）
    expect(script).toContain("[System.Windows.Forms.SystemInformation]::VirtualScreen")
    expect(script).toContain("SetProcessDPIAware")
    expect(r.blocks?.[0]).toMatchObject({ type: "image", name: expect.stringMatching(/^screenshot_\d+\.png$/) })
    // 文件已落盘
    const { readdir } = await import("node:fs/promises")
    const files = await readdir(c.workdir)
    expect(files.some((f) => f.startsWith("screenshot_"))).toBe(true)
  })

  test("screenshot validates region format", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const r = await screenshotTool.execute({ region: "abc" }, ctx(home))
    expect(r.output).toContain("region 格式错误")
  })

  test("screenshot accepts negative-coordinate region (副屏)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        if (cmd.includes("powershell")) seenCmd = cmd
        const script = decodeCmd(cmd)
        const m = script.match(/'([^']+\.png)'/)
        if (m) await c.writeFile(m[1], "")
        return { stdout: "STAT -2560,0 2560x1440 mean=60.0 colors=500", stderr: "", code: 0 }
      },
    })
    const r = await screenshotTool.execute({ region: "-2560,0,2560,1440" }, c)
    const script = decodeCmd(seenCmd)
    expect(script).toContain("New-Object System.Drawing.Rectangle(-2560, 0, 2560, 1440)")
    expect(r.output).toContain("原点 (-2560,0)")
  })

  test("screenshot with region uses Rectangle bounds", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        if (cmd.includes("powershell")) seenCmd = cmd
        const script = decodeCmd(cmd)
        const m = script.match(/'([^']+\.png)'/)
        if (m) await c.writeFile(m[1], "")
        return { stdout: "STAT 10,20 800x600 mean=60.0 colors=500", stderr: "", code: 0 }
      },
    })
    await screenshotTool.execute({ region: "10,20,800,600" }, c)
    expect(decodeCmd(seenCmd)).toContain("New-Object System.Drawing.Rectangle(10, 20, 800, 600)")
  })

  test("window_list parses TSV output into aligned table with foreground marker and bounds", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async () => ({
        stdout: "1234\tnotepad\t*\t100,100,800,600\t无标题 - 记事本\n5678\tchrome\t\t1920,0,1200,900\tGitHub - 工作",
        stderr: "",
        code: 0,
      }),
    })
    const r = await windowListTool.execute({}, c)
    expect(r.output).toContain("共 2 个窗口")
    expect(r.output).toContain("notepad")
    expect(r.output).toContain("chrome")
    // 前台标记解析：* 行进程进入「当前前台」行；bounds 列原样保留
    expect(r.output).toContain("当前前台: notepad")
    expect(r.output).toContain("100,100,800,600")
  })

  test("window_focus requires pid or title", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const r = await windowFocusTool.execute({}, ctx(home))
    expect(r.output).toContain("pid 或 title")
  })

  test("type_text embeds text via base64 into PowerShell clipboard command", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已输入 3 字符（剪贴板已恢复原内容）", stderr: "", code: 0 }
      },
    })
    const r = await typeTextTool.execute({ text: "你好GEBAI" }, c)
    expect(r.output).toContain("已输入")
    expect(r.output).toContain("内容预览")
    expect(seenCmd).toContain("powershell")
    // 脚本内通过 base64 注入文本，避免引号转义；剪贴板写入回验重试、粘贴后延时恢复
    const script = decodeCmd(seenCmd)
    expect(script).toContain(Buffer.from("你好GEBAI", "utf8").toString("base64"))
    expect(script).toContain("Get-Clipboard -Raw")
    expect(script).toContain("finally")
  })

  test("type_text clipboard verifies write-back and delays restore before paste", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已输入 3 字符（剪贴板已恢复原内容）", stderr: "", code: 0 }
      },
    })
    await typeTextTool.execute({ text: "测试文本" }, c)
    const script = decodeCmd(seenCmd)
    // 写入回验：Set-Clipboard 后读回比对（剪贴板管理软件拦截写入时静默失败防护）
    expect(script).toContain("$setOk")
    expect(script).toContain("-ceq $wantN")
    // 粘贴后延时恢复：目标应用异步消费剪贴板，立即恢复会粘出旧内容
    const pasteIdx = script.indexOf("SendWait")
    const restoreIdx = script.indexOf("Set-Clipboard -Value $old")
    const sleepBetween = script.slice(pasteIdx).match(/Start-Sleep -Milliseconds (\d+)/)
    expect(pasteIdx).toBeGreaterThan(-1)
    expect(restoreIdx).toBeGreaterThan(pasteIdx)
    expect(Number(sleepBetween?.[1])).toBeGreaterThanOrEqual(500)
  })

  test("type_text clipboard surfaces write failure instead of fake success", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async () => ({
        stdout: "输入失败: 剪贴板写入未生效（可能被剪贴板管理软件拦截），未执行粘贴，原剪贴板未被修改",
        stderr: "",
        code: 0,
      }),
    })
    const r = await typeTextTool.execute({ text: "测试文本" }, c)
    // 失败必须诚实上报，不追加成功语义文案（已输入/内容预览）
    expect(r.output).toContain("输入失败")
    expect(r.output).not.toContain("已输入")
    expect(r.output).not.toContain("内容预览")
  })

  test("type_text aborts on sensitive key patterns (密钥泄漏防护)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let ran = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        ran = cmd
        return { stdout: "", stderr: "", code: 0 }
      },
    })
    const r = await typeTextTool.execute({ text: "FEISHU_APP_SECRET=sk-abcdef0123456789" }, c)
    expect(r.output).toContain("敏感信息")
    expect(r.output).toContain("mode=\"keys\"")
    expect(ran).toBe("") // 未执行任何命令
  })

  test("type_text keys mode types ASCII via SendKeys without touching clipboard", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已输入 5 字符（keys 模式）", stderr: "", code: 0 }
      },
    })
    const r = await typeTextTool.execute({ text: "a+b%c", mode: "keys" }, c)
    const script = decodeCmd(seenCmd)
    expect(script).toContain("SendWait")
    expect(script).not.toContain("Get-Clipboard")
    // 特殊字符逐字符 {} 转义（+ 和 % 不会被解释为修饰键）
    expect(script).toContain(Buffer.from("a{+}b{%}c", "utf8").toString("base64"))
    expect(r.output).toContain("keys 模式")
  })

  test("type_text keys mode rejects non-ASCII", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let ran = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        ran = cmd
        return { stdout: "", stderr: "", code: 0 }
      },
    })
    const r = await typeTextTool.execute({ text: "你好", mode: "keys" }, c)
    expect(r.output).toContain("仅支持 ASCII")
    expect(ran).toBe("")
  })

  test("clipboard_read shows content and warns on sensitive patterns", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "APP_SECRET=sk-abcdef0123456789\n", stderr: "", code: 0 }
      },
    })
    const r = await clipboardReadTool.execute({}, c)
    expect(decodeCmd(seenCmd)).toContain("Get-Clipboard")
    expect(r.output).toContain("剪贴板内容")
    expect(r.output).toContain("⚠️ 检测到疑似敏感信息")
  })

  test("clipboard_read empty clipboard reports cleanly", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async () => ({ stdout: "（剪贴板为空）", stderr: "", code: 0 }),
    })
    const r = await clipboardReadTool.execute({}, c)
    expect(r.output).toContain("剪贴板为空")
    expect(r.output).not.toContain("⚠️")
  })

  test("screenshot parses STAT metadata and warns on black frame (A1 黑帧检测)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async (cmd) => {
        const script = decodeCmd(cmd)
        const m = script.match(/'([^']+\.png)'/)
        if (m) await c.writeFile(m[1], "")
        return { stdout: "STAT 0,0 1920x1080 mean=3.2 colors=4", stderr: "", code: 0 }
      },
    })
    const r = await screenshotTool.execute({}, c)
    expect(r.output).toContain("1920x1080")
    expect(r.output).toContain("平均亮度 3.2")
    expect(r.output).toContain("疑似黑屏")
  })

  test("screenshot dark solid frame warns as non-real image", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async (cmd) => {
        const script = decodeCmd(cmd)
        const m = script.match(/'([^']+\.png)'/)
        if (m) await c.writeFile(m[1], "")
        return { stdout: "STAT 0,0 800x600 mean=25.0 colors=3", stderr: "", code: 0 }
      },
    })
    const r = await screenshotTool.execute({}, c)
    expect(r.output).toContain("单一暗色")
  })

  test("screenshot normal frame carries size metadata without warning", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async (cmd) => {
        const script = decodeCmd(cmd)
        const m = script.match(/'([^']+\.png)'/)
        if (m) await c.writeFile(m[1], "")
        return { stdout: "STAT 0,0 1920x1080 mean=128.4 colors=2000", stderr: "", code: 0 }
      },
    })
    const r = await screenshotTool.execute({}, c)
    expect(r.output).toContain("1920x1080")
    expect(r.output).not.toContain("⚠️")
  })

  test("screen_info lists monitors with resolution and primary flag", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async () => ({
        stdout: "\\\\.\\DISPLAY1\t0\t0\t1920\t1080\tTrue\n\\\\.\\DISPLAY2\t1920\t0\t2560\t1440\tFalse",
        stderr: "",
        code: 0,
      }),
    })
    const r = await screenInfoTool.execute({}, c)
    expect(r.output).toContain("共 2 个显示器")
    expect(r.output).toContain("1920x1080")
    expect(r.output).toContain("2560x1440")
    expect(r.output).toContain("（主屏）")
  })

  test("detectSensitive matches key/value and sk- patterns, misses plain text", () => {
    expect(detectSensitive("sk-abcdef0123456789xyz")).toMatch(/\*\*\*\*/)
    expect(detectSensitive("api_key=xxxxxxxxxxxxxxxxxxxxxxxx")).not.toBeNull()
    expect(detectSensitive("Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9")).not.toBeNull()
    expect(detectSensitive("今天天气不错")).toBeNull()
    expect(detectSensitive("Hello world 123")).toBeNull()
  })

  test("detectSensitive long-token heuristic: 40+ 混合非纯hex 才命中", () => {
    // 40+ 字母数字混合且非纯十六进制 → 命中（疑似随机令牌）
    expect(detectSensitive("ghp_Ab12Cd34Ef56Gh78Ij90Kl12Mn34Op56Qr78")).not.toBeNull()
    // 纯十六进制 40 位（git commit sha）→ 不命中
    expect(detectSensitive("commit 14d529dfafa48fdbf1a0e3f2c4a5b6c7d8e9f0a1")).toBeNull()
    // 40+ 纯字母（英文长句片段，无数字混排）→ 不命中
    expect(detectSensitive("thequickbrownfoxjumpsoverthelazydogandrunsaway")).toBeNull()
  })

  test("key_press sends SendKeys command", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "ok", stderr: "", code: 0 }
      },
    })
    await keyPressTool.execute({ keys: "^c" }, c)
    expect(decodeCmd(seenCmd)).toContain("SendWait")
    expect(decodeCmd(seenCmd)).toContain(Buffer.from("^c", "utf8").toString("base64"))
  })

  test("mouse_click builds click command", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已left点击 (100, 200)", stderr: "", code: 0 }
      },
    })
    const r = await mouseClickTool.execute({ x: 100, y: 200 }, c)
    expect(r.output).toContain("(100, 200)")
    expect(decodeCmd(seenCmd)).toContain("SetCursorPos(100, 200)")
  })

  test("command failure surfaces stderr", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    const c = ctx(home, {
      runCommand: async () => ({ stdout: "", stderr: "access denied", code: 1 }),
    })
    const r = await screenshotTool.execute({}, c)
    expect(r.output).toContain("截图失败")
    expect(r.output).toContain("access denied")
  })

  test("window_focus title is base64-injected (no PowerShell interpolation surface)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "未找到匹配窗口", stderr: "", code: 0 }
      },
    })
    // 恶意 title：若裸插值会执行 calc
    await windowFocusTool.execute({ title: "$(Start-Process calc)" }, c)
    const script = decodeCmd(seenCmd)
    expect(script).not.toContain("Start-Process calc")
    // 标题以 base64 注入脚本内解码后匹配
    expect(script).toContain(Buffer.from("$(Start-Process calc)", "utf8").toString("base64"))
    expect(script).toContain("MainWindowTitle.Contains($t)")
  })

  test("window_focus verifies foreground and surfaces activation failure honestly", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return {
          stdout: "激活失败: Windows 前台锁定拦截（后台进程禁止夺取焦点）。请手动点击一次目标窗口后重试，或改用其他验证通道确认窗口状态",
          stderr: "",
          code: 0,
        }
      },
    })
    const r = await windowFocusTool.execute({ pid: 999 }, c)
    const script = decodeCmd(seenCmd)
    // 激活后复核前台窗口（GetForegroundWindow 比对），Alt 击键缓解重试一次
    expect(script).toContain("GetForegroundWindow")
    expect(script).toContain("keybd_event(0x12")
    // 失败诚实上报：不虚报「已激活」
    expect(r.output).toContain("激活失败")
    expect(r.output).not.toContain("已激活")
  })

  test("mouse_click double uses the defined Add-Type class", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已double点击 (1, 2)", stderr: "", code: 0 }
      },
    })
    const r = await mouseClickTool.execute({ x: 1, y: 2, button: "double" }, c)
    const script = decodeCmd(seenCmd)
    expect(script).toContain("GebaiMouse2")
    expect(script).not.toContain("[GebaiMouse]::")
    expect(r.output).toContain("double")
  })

  test("Linux key_press rejects shell metacharacters", async () => {
    const original = process.platform
    Object.defineProperty(process, "platform", { value: "linux" })
    try {
      const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
      let ran = ""
      const c = ctx(home, {
        runCommand: async (cmd) => {
          if (cmd.includes("command -v")) return { stdout: "/usr/bin/xdotool", stderr: "", code: 0 }
          ran = cmd
          return { stdout: "", stderr: "", code: 0 }
        },
      })
      const r = await keyPressTool.execute({ keys: "a; touch /tmp/pwn; echo" }, c)
      expect(r.output).toContain("非法字符")
      expect(ran).toBe("") // 未执行任何命令
    } finally {
      Object.defineProperty(process, "platform", { value: original })
    }
  })

  test("mouse_scroll sends wheel event with signed units and direction mapping", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已向下滚动 3 格 (100, 200)", stderr: "", code: 0 }
      },
    })
    await mouseScrollTool.execute({ x: 100, y: 200, direction: "down", amount: 3 }, c)
    const script = decodeCmd(seenCmd)
    expect(script).toContain("SetCursorPos(100, 200)")
    expect(script).toContain("mouse_event(0x0800, 0, 0, -360") // 垂直滚轮 flag + 向下负值（每格 120）
    // 水平向右：HWHEEL flag + 正值
    await mouseScrollTool.execute({ x: 1, y: 2, direction: "right", amount: 2 }, c)
    expect(decodeCmd(seenCmd)).toContain("mouse_event(0x1000, 0, 0, 240")
  })

  test("mouse_drag presses, interpolates and releases between endpoints", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已拖拽 (10, 20) → (110, 220)", stderr: "", code: 0 }
      },
    })
    const r = await mouseDragTool.execute({ from_x: 10, from_y: 20, to_x: 110, to_y: 220 }, c)
    const script = decodeCmd(seenCmd)
    expect(script).toContain("mouse_event(0x0002") // 左键按下
    expect(script).toContain("mouse_event(0x0004") // 左键抬起
    expect(script).toContain("$steps = 12") // 插值移动（适配依赖真实轨迹的目标）
    expect(r.output).toContain("(10, 20) → (110, 220)")
  })

  test("window_state maps actions to ShowWindow codes", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已最小化: notepad (PID 1234) - 无标题", stderr: "", code: 0 }
      },
    })
    await windowStateTool.execute({ action: "minimize", pid: 1234 }, c)
    expect(decodeCmd(seenCmd)).toContain("ShowWindow($p.MainWindowHandle, 6)")
    await windowStateTool.execute({ action: "close", pid: 1234 }, c)
    expect(decodeCmd(seenCmd)).toContain("ShowWindow($p.MainWindowHandle, 0)") // SW_CLOSE → WM_CLOSE 优雅关闭
    const r = await windowStateTool.execute({ action: "restore" }, ctx(home))
    expect(r.output).toContain("pid 或 title")
  })

  test("clipboard_write verifies write-back and previews content", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已写入", stderr: "", code: 0 }
      },
    })
    const r = await clipboardWriteTool.execute({ text: "给用户复制的内容" }, c)
    const script = decodeCmd(seenCmd)
    expect(script).toContain("Set-Clipboard -Value $want")
    expect(script).toContain("-ceq $want") // 写入回验（剪贴板管理软件拦截防护）
    expect(r.output).toContain("已写入 8 字符")
    expect(r.output).toContain("给用户复制的内容")
    expect(r.output).not.toContain("⚠️")
  })

  test("clipboard_write warns (not aborts) on sensitive content and surfaces failure", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-desktop-"))
    let seenCmd = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        seenCmd = cmd
        return { stdout: "已写入", stderr: "", code: 0 }
      },
    })
    // 写剪贴板是「交给用户粘贴」的场景：敏感值告警但不中止（与 type_text 拦截不同）
    const r = await clipboardWriteTool.execute({ text: "sk-abcdef0123456789" }, c)
    expect(seenCmd).not.toBe("")
    expect(r.output).toContain("⚠️ 内容含疑似敏感信息")
    // 写入未生效诚实上报
    const fail = ctx(home, {
      runCommand: async () => ({ stdout: "写入失败: 剪贴板写入未生效（可能被剪贴板管理软件拦截），原剪贴板可能已被部分修改", stderr: "", code: 0 }),
    })
    const r2 = await clipboardWriteTool.execute({ text: "x" }, fail)
    expect(r2.output).toContain("写入失败")
    expect(r2.output).not.toContain("内容预览")
  })
})

describe("desktop definition", () => {
  test("tool names conform to sub-agent tool naming rules", () => {
    for (const t of Object.keys(desktopDef.tools ?? {})) {
      expect(t).toMatch(/^[a-zA-Z0-9_]+$/)
      expect(t).not.toMatch(/[.\-:]/)
      expect(`desktop_${t}`.length).toBeLessThanOrEqual(40)
    }
  })

  test("input/click/window tools require approval; screenshot/list do not", () => {
    expect(desktopDef.requiresApproval).toMatchObject({
      type_text: true,
      key_press: true,
      mouse_click: true,
      mouse_scroll: true,
      mouse_drag: true,
      window_focus: true,
      window_state: true,
      clipboard_write: true,
    })
    expect(desktopDef.requiresApproval?.screenshot).toBeFalsy()
    expect(desktopDef.requiresApproval?.window_list).toBeFalsy()
    expect(desktopDef.requiresApproval?.locate_image).toBeFalsy()
    expect(desktopDef.requiresApproval?.wait_for).toBeFalsy()
  })

  test("systemPrompt 覆盖新能力与多显示器语义", () => {
    const p = desktopDef.systemPrompt
    expect(p).toContain("desktop_locate_image")
    expect(p).toContain("desktop_wait_for")
    expect(p).toContain("mouse_scroll")
    expect(p).toContain("mouse_drag")
    expect(p).toContain("window_state")
    expect(p).toContain("虚拟屏幕")
    expect(p).toContain("clipboard_write")
  })

  test("systemPrompt 覆盖检测分层后端与配对语义（detect 条目）", () => {
    const p = desktopDef.systemPrompt
    expect(p).toContain("ultralytics YOLO ONNX") // 即插即用（元数据自适应）
    expect(p).toContain("自动读模型元数据")
    expect(p).toContain("sidecar") // GPU 分层后端
    expect(p).toContain("配对 OCR 文本") // 检测×OCR 配对输出
    expect(p).toContain("desktop_locate 为主") // 定位纪律：检测给类型不给语义
  })

  test("preload off and tools registered (独有工具 only——编排用全局 agent_run，不复刻)", () => {
    expect(desktopDef.preload).toBe(false)
    expect(Object.keys(desktopDef.tools ?? {}).sort()).toEqual(
      [
        "clipboard_read",
        "clipboard_write",
        "detect",
        "key_press",
        "locate",
        "locate_image",
        "mouse_click",
        "mouse_drag",
        "mouse_move",
        "mouse_scroll",
        "ocr",
        "screen_info",
        "screenshot",
        "type_text",
        "wait_for",
        "window_focus",
        "window_list",
        "window_move",
        "window_state",
      ].sort(),
    )
  })
})
