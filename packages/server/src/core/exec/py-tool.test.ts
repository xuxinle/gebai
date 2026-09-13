/**
 * py 工具桥测试（仅本地模式）：协议走回环 socket，stdout/stderr 完全归用户输出。
 * 需要宿主机 python 解释器——缺失时相关用例跳过（不伪装成失败）。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { buildPyBridgeScript, pyTool, _resetPythonCmdCache } from "./py-tool"
import { jsTool } from "./js-tool"
import { BRIDGE_TOOL_MAX_CALLS } from "./tool-bridge"
import type { Tool, ToolContext, ToolResult } from "../base/types"

/** 解释器可用性探测：与产品同口径逐个候选试 `--version`（Windows 上 "python3" 可能是失效的商店别名）。 */
const PY = ((): string | null => {
  for (const cand of ["python3", "python", "py"]) {
    try {
      const r = Bun.spawnSync([cand, "--version"], { stdout: "pipe", stderr: "pipe" })
      if ((r.exitCode ?? 1) === 0) return cand
    } catch {
      /* 候选不可用 */
    }
  }
  return null
})()

function mkTool(
  name: string,
  execute: (args: Record<string, unknown>, ctx: ToolContext) => Promise<ToolResult>,
  extra: Partial<Tool> = {},
): Tool {
  return { name, description: "", parameters: { type: "object", properties: {} }, ...extra, execute } as Tool
}

interface CtxOpts {
  sandboxed?: boolean
  safeMode?: boolean
  env?: Record<string, string>
  extraTools?: Record<string, Tool>
}

/** 测试 ctx：`runCommand` 对 `--version` 探测按真实可用性应答，对脚本执行按真实 python 跑（旧路径/降级用）。 */
function ctx(home: string, opts: CtxOpts = {}): ToolContext {
  const tmp = join(home, "tmp")
  mkdirSync(tmp, { recursive: true })
  const tools: Record<string, Tool> = {
    echo: mkTool("echo", async (args) => ({ output: `echo:${JSON.stringify(args)}`, data: args })),
    boom: mkTool("boom", async () => {
      throw new Error("爆炸")
    }),
    need_arg: mkTool("need_arg", async () => ({ output: "ok" }), {
      parameters: { type: "object", properties: { path: { type: "string" } }, required: ["path"] } as Tool["parameters"],
    }),
    py: mkTool("py", async () => ({ output: "嵌套 py 不应执行" })),
    ...(opts.extraTools ?? {}),
  }
  return {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    home,
    env: opts.env ?? {},
    sandboxed: opts.sandboxed ?? false,
    ...(opts.safeMode ? { safeMode: true } : {}),
    resolvePath: (p) => resolve(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async (p, content) => {
      const { writeFile } = await import("node:fs/promises")
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async (cmd, o) => {
      if (cmd.endsWith("--version")) {
        const cand = cmd.split(" ")[0]
        try {
          const r = Bun.spawnSync([cand, "--version"], { stdout: "pipe", stderr: "pipe" })
          return { stdout: r.stdout.toString(), stderr: r.stderr.toString(), code: r.exitCode ?? 1 }
        } catch {
          return { stdout: "", stderr: "", code: 1 }
        }
      }
      const m = /^(\S+) -X utf8 "(.+)"$/.exec(cmd)
      if (!m) return { stdout: "", stderr: `bad cmd: ${cmd}`, code: 1 }
      const r = Bun.spawnSync([m[1], "-X", "utf8", m[2]], {
        cwd: o?.workdir,
        env: { ...process.env, PYTHONUTF8: "1" }, // 桩模拟产品侧执行：解释器查找用真实 PATH（不受 ctx.env 影响）
        stdout: "pipe",
        stderr: "pipe",
      })
      return { stdout: r.stdout.toString(), stderr: r.stderr.toString(), code: r.exitCode ?? 1 }
    },
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => {
      throw new Error("未知预置项目")
    },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: {
      schemas: () => Object.keys(tools).map((name) => ({ name, description: "", parameters: { type: "object", properties: {} } })),
      resolve: (name) => (tools[name] ? { name, tool: tools[name] } : undefined),
      getAgentNames: () => [],
    },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    waitForCapture: async () => null,
  }
}

function tmpHome(tag: string): string {
  return mkdtempSync(join(tmpdir(), `gebai-py-${tag}-`))
}

/** 删除测试临时目录：被杀子进程（超时/中断用例）的 cwd 就是该目录，Windows 上进程终止后句柄释放
 *  有延迟，立即 rm 会 EBUSY（并行负载下句柄回收更慢，更易触发）——短重试后放弃（残留无害）。 */
async function rmTemp(dir: string): Promise<void> {
  for (let i = 0; i < 40; i += 1) {
    try {
      rmSync(dir, { recursive: true, force: true })
      return
    } catch {
      await Bun.sleep(25)
    }
  }
}

describe("py 工具桥（本地模式）", () => {
  test("桥脚本生成：注入 tools/ctx/input，env 不落盘，非法标识符工具名不生成函数", () => {
    const script = buildPyBridgeScript({
      port: 12345,
      token: "tok",
      toolNames: ["read", "class", "1bad", "sh"],
      ctx: { user: "u", workdir: "C:/tmp", env: { SECRET_TOKEN: "leak-me" }, messages: [] },
      input: { k: 1 },
      userCode: 'print("hi")\n',
    })
    expect(script).toContain("_G_TOOL_NAMES = _g_json.loads")
    expect(script).toContain('\\"read\\"') // 名单经双重 stringify 注入为 Python 字符串字面量
    expect(script).toContain('\\"class\\"') // 名单原样注入，Python 侧按 keyword/builtins 过滤
    expect(script).toContain("_G_CTX = _g_json.loads")
    expect(script).toContain("_G_INPUT = _g_json.loads")
    expect(script).toContain('print(\\"hi\\")') // 用户代码作为 Python 字符串字面量注入（不再嵌入源码本体）
    expect(script).not.toContain("leak-me") // ctx.env 不落盘（运行时引用 os.environ）
    expect(script).not.toContain("SECRET_TOKEN") // ctx.env 不落盘（含键名也不落盘，运行时引用 os.environ）
    // JSON 字面量里的 true/false/null 不是合法 Python 字面量——注入数据一律经 json.loads 解析
    expect(script).toContain('_g_json.loads("{\\"k\\":1}")')
  })

  test.if(!!PY)("工具名即函数 + tools.call + 返回值 + print 输出", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-fn")
    const c = ctx(home)
    const r = await pyTool.execute(
      {
        code: [
          'r = echo({"msg": "fn"})',
          'print("call:", r["output"])',
          'print("attr:", r.output)',
          'r2 = tools.call("echo", {"msg": "ns"})',
          'print("input:", input)',
          'result = {"fn": r["data"]["msg"], "ns": r2["data"]["msg"], "input": input}',
        ].join("\n"),
        input: { k: 1 },
      },
      c,
    )
    expect(r.output).toContain("[返回值]")
    expect(r.output).toContain('call: echo:{"msg":"fn"}')
    const data = r.data as { exitCode: number; result: { fn: string; ns: string; input: unknown }; calls: Array<{ name: string; ok: boolean }>; stdout: string }
    expect(data.exitCode).toBe(0)
    expect(data.result).toEqual({ fn: "fn", ns: "ns", input: { k: 1 } })
    expect(data.calls.map((x) => x.name)).toEqual(["echo", "echo"])
    expect(data.stdout).toContain("attr: echo:")
    expect(data.stdout).toContain("input: {'k': 1}") // input 以 JSON 注入变量（非 stdin）
    await rmTemp(home)
  })

  test.if(!!PY)("fd 直写/子进程输出与工具调用交织：协议不被污染（不借用 stdio 的核心收益）", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-stdio")
    const c = ctx(home)
    const r = await pyTool.execute(
      {
        code: [
          "import os, sys, subprocess",
          'print("via-print")',
          'os.write(1, b"via-fd\\n")',
          'subprocess.run([sys.executable, "-c", "print(\'via-child\')"])',
          'r = echo({"msg": "after"})',
          'print("tool:", r["output"])',
          'result = {"closed": True}',
        ].join("\n"),
      },
      c,
    )
    const data = r.data as { stdout: string; result: unknown; calls: Array<{ name: string; ok: boolean }> }
    expect(data.stdout).toContain("via-print")
    expect(data.stdout).toContain("via-fd")
    expect(data.stdout).toContain("via-child")
    expect(data.stdout).toContain('tool: echo:{"msg":"after"}')
    expect(data.calls.length).toBe(1)
    expect(data.result).toEqual({ closed: true })
    await rmTemp(home)
  })

  test.if(!!PY)("工具异常为 _G_ToolError 可 try/except 容错；未知工具与缺参即时拒绝", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-err")
    const c = ctx(home)
    const r = await pyTool.execute(
      {
        code: [
          "msgs = []",
          "try:",
          "    boom({})",
          "except Exception as e:",
          '    msgs.append("boom:" + type(e).__name__)',
          "try:",
          '    tools.call("nope", {})',
          "except Exception as e:",
          '    msgs.append("unknown:" + str(e))',
          "try:",
          '    need_arg({})',
          "except Exception as e:",
          '    msgs.append("missing:" + str(e))',
          "print(chr(10).join(msgs))",
          'result = msgs',
        ].join("\n"),
      },
      c,
    )
    const data = r.data as { result: string[]; calls: Array<{ name: string; ok: boolean; error?: string }> }
    expect(data.result[0]).toBe("boom:_G_ToolError")
    expect(data.result[1]).toContain("未知工具: nope")
    expect(data.result[2]).toContain("缺少必填参数")
    expect(data.calls.filter((x) => !x.ok).length).toBe(3)
    await rmTemp(home)
  })

  test.if(!!PY)("嵌套守卫：py 内不能再调 py（分发层拒绝）；脚本可继续", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-nest")
    const c = ctx(home)
    const r = await pyTool.execute(
      {
        code: ['try:', '    tools.call("py", {"code": "print(1)"})', "except Exception as e:", '    print("nested:", str(e))', 'result = "done"'].join("\n"),
      },
      c,
    )
    expect(r.output).toContain("不能再调用 py")
    expect((r.data as { result: string }).result).toBe("done")
    await rmTemp(home)
  })

  test.if(!!PY)("链式重入：py 内可调 js（首次进入放行，真实 js 桥执行）", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-py-js")
    const c = ctx(home, { extraTools: { js: jsTool } })
    const r = await pyTool.execute(
      { code: ['r = js({"code": "return 42"})', 'print("js output:", r["output"][:40])', 'result = r["data"]["result"]'].join("\n") },
      c,
    )
    const data = r.data as { result: number; calls: Array<{ name: string; ok: boolean }> }
    expect(data.result).toBe(42) // py→js 真实执行（链 ["py"] 不含 js → 放行）
    expect(data.calls).toEqual([{ name: "js", ok: true }])
    await rmTemp(home)
  }, 30000)

  test.if(!!PY)("链式重入：py→js→py 被分发层硬拒（py 已在链）", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-py-js-py")
    const c = ctx(home, { extraTools: { js: jsTool, py: pyTool } })
    const jsCode = [
      "try {",
      '  const r = await tools.call("py", { code: "print(1)" })',
      '  return "py-ran:" + r.output',
      "} catch (e) {",
      '  return "py-rejected:" + e.message',
      "}",
    ].join("\n")
    const r = await pyTool.execute({ code: ['r = js({' + JSON.stringify("code") + ": " + JSON.stringify(jsCode) + "})", 'result = r["data"]["result"]'].join("\n") }, c)
    const res = (r.data as { result: string }).result
    expect(res).toContain("py-rejected") // 二层 py 被拒（硬拒：js 侧拿到 reject）
    expect(res).toContain("不能再调用 py")
    await rmTemp(home)
  }, 40000)

  test.if(!!PY)("链式重入：js→py 首次放行（py 注桥）、py 内再调 js 被硬拒（js 已在链）", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-js-py-js")
    const c = ctx(home, { extraTools: { js: jsTool, py: pyTool } })
    const pyCode = [
      "lines = []",
      "try:",
      '    js({"code": "return 1"})',
      '    lines.append("js-ran")',
      "except Exception as e:",
      '    lines.append("js-rejected: " + str(e))',
      'result = lines',
    ].join("\n")
    const jsCode = ['const r = await tools.call("py", { code: ' + JSON.stringify(pyCode) + " })", "return r.data.result"].join("\n")
    const r = await jsTool.execute({ code: jsCode }, c)
    const res = (r.data as { result: string[] }).result
    expect(res.length).toBe(1)
    expect(res[0]).toContain("js-rejected") // py 内调 js：js 已在链 → 硬拒
    expect(res[0]).toContain("不能再调用 js")
    await rmTemp(home)
  }, 40000)

  test.if(!!PY)("工具调用总数超上限被拒（防脚本放大）", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-cap")
    const c = ctx(home)
    const r = await pyTool.execute(
      {
        code: [
          "n = 0",
          'err = ""',
          `for i in range(${BRIDGE_TOOL_MAX_CALLS + 5}):`,
          "    try:",
          '        echo({"i": i})',
          "        n += 1",
          "    except Exception as e:",
          "        err = str(e)",
          "        break",
          'result = {"ok_calls": n, "err": err}',
        ].join("\n"),
      },
      c,
    )
    const res = (r.data as { result: { ok_calls: number; err: string } }).result
    expect(res.ok_calls).toBe(BRIDGE_TOOL_MAX_CALLS)
    expect(res.err).toContain("超上限")
    await rmTemp(home)
  }, 60000)

  test.if(!!PY)("超时终止：sleep 脚本按 timeout 杀进程树并返回超时结果", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-timeout")
    const c = ctx(home)
    const r = await pyTool.execute({ code: "import time\ntime.sleep(30)\nprint('never')", timeout: 1 }, c)
    const data = r.data as { timedOut?: boolean; exitCode: number }
    expect(data.timedOut).toBe(true)
    expect(data.exitCode).toBe(124)
    expect(r.output).toContain("[timed out after 1s]")
    await rmTemp(home)
  }, 30000)

  test.if(!!PY)("strict：非 0 退出抛工具级错误（SystemExit 非 0 视为失败）", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-strict")
    const c = ctx(home)
    await expect(pyTool.execute({ code: "import sys\nsys.exit(3)", strict: true }, c)).rejects.toThrow(/exit 3/)
    const ok = await pyTool.execute({ code: 'print("fine")', strict: true }, c)
    expect(ok.output).toContain("fine")
    await rmTemp(home)
  })

  test.if(!!PY)("沙箱模式：不注入工具桥（纯脚本执行，工具名不可用）", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-sandbox")
    const c = ctx(home, { sandboxed: true })
    const r = await pyTool.execute(
      { code: ['try:', '    echo({"msg": "x"})', '    print("bridge-on")', "except NameError:", '    print("no-bridge")'].join("\n") },
      c,
    )
    expect(r.output).toContain("no-bridge")
    expect((r.data as { calls?: unknown }).calls).toBeUndefined()
    await rmTemp(home)
  })

  test.if(!!PY)("桥不可用降级：解释器找不到时不退回 stdio 桥，改纯脚本执行并说明", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-degrade")
    // PATH 指向不存在目录：spawn 找不到解释器 → 桥通道未建立 → 降级（runCommand 桩自身按真实 PATH 执行，可跑通）
    const c = ctx(home, { env: { PATH: join(home, "no-such-bin") } })
    const r = await pyTool.execute({ code: 'print("legacy-ran")' }, c)
    expect(r.output).toContain("脚本桥不可用")
    expect(r.output).toContain("legacy-ran")
    await rmTemp(home)
  })

  test.if(!!PY)("py 桥重入（链含 py）：不注桥，纯脚本执行", async () => {
    _resetPythonCmdCache()
    const home = tmpHome("bridge-reenter")
    const c = ctx(home)
    c.bridgeLangs = ["py"]
    const r = await pyTool.execute({ code: 'print("plain")' }, c)
    expect(r.output).toContain("plain")
    expect((r.data as { calls?: unknown }).calls).toBeUndefined()
    await rmTemp(home)
  })

  test("审批姿态：py 恒需审批（requiresApproval 为函数形态且恒 true）", () => {
    const ra = pyTool.requiresApproval as (args: Record<string, unknown>, ctx?: unknown) => boolean
    expect(typeof pyTool.requiresApproval).toBe("function")
    expect(ra({ code: "print(1)", approval: false })).toBe(true)
    expect(ra({})).toBe(true)
    expect(pyTool.parameters.properties).toHaveProperty("approval")
    expect(pyTool.parameters.properties).toHaveProperty("code")
    expect((pyTool.outputSchema as { required: string[] }).required).toEqual(["stdout", "stderr", "exitCode"])
  })

  test("空 code 拒绝", async () => {
    const home = tmpHome("bridge-empty")
    const r = await pyTool.execute({ code: "   " }, ctx(home))
    expect(r.output).toContain("code 不能为空")
    await rmTemp(home)
  })
})
