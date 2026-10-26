/**
 * `pythonCommand` 的回归测试。
 *
 * ## 这条用例防的是什么
 *
 * Windows 上 `ctx.runCommand` 走 PowerShell（`shell: true`）。而 PowerShell 里
 * **以引号开头的行是字符串表达式，不是命令**：
 *
 * ```
 * "C:\...\python.exe" -X utf8 a.py     → Unexpected token '-X'
 * & "C:\...\python.exe" -X utf8 a.py   → 正常执行
 * ```
 *
 * 实测后果：整条命令根本没启动，stdout 空、stderr 只有一段 ParserError，
 * 而用户看到的是「找不到可用的 Python 解释器」——解释器明明就在。
 * 这个缺陷同时影响 **合成、pip 安装** 两条路（都拼了 `"python" ...`）。
 *
 * ## 判据为何要落在「首字符」而不是「包含 &」
 *
 * `&` 出现在路径或参数里是可能的；真正决定 PowerShell 怎么解析的，
 * 是命令**以什么开头**。所以断言必须是 `startsWith("& ")`。
 *
 * POSIX shell 里 `&` 是后台运算符，**加前缀会把命令丢到后台**（stdout 立刻空），
 * 所以另一侧必须断言 **不带** `&`——两侧都要守，否则修 Windows 会坏 Linux。
 */
import { describe, expect, test } from "bun:test"
import { pythonCommand } from "./tts"

const isWin = process.platform === "win32"

describe("pythonCommand：PowerShell 调用运算符", () => {
  test("Windows 下以 `& ` 开头（否则 PowerShell 当字符串表达式，命令不启动）", () => {
    const cmd = pythonCommand("C:\\Python313\\python.exe", "-X utf8 a.py")
    if (isWin) expect(cmd.startsWith("& ")).toBe(true)
    // 非 Windows 平台不要求 &，但要能拼出完整命令（下方另有专测）
    expect(cmd).toContain("-X utf8 a.py")
  })

  test("POSIX 下不得带 `&`（那是后台运算符，会把命令丢到后台）", () => {
    const cmd = pythonCommand("/usr/bin/python3", "-X utf8 a.py")
    if (!isWin) {
      expect(cmd.startsWith("&")).toBe(false)
      expect(cmd.startsWith('"/usr/bin/python3"')).toBe(true)
    }
    // 两侧共同约束：解释器路径必须带引号（与平台无关）
    expect(cmd).toContain('"')
  })

  test("解释器路径带空格时仍被完整引用（不引用会被拆成两个 token）", () => {
    const cmd = pythonCommand("C:\\Program Files\\Py\\python.exe", "-V")
    expect(cmd).toContain('"C:\\Program Files\\Py\\python.exe"')
    // 引用必须把整个路径包住：出现裸空格前缀说明引号错位
    expect(cmd).not.toMatch(/(^|[^"])\sC:\\Program Files/)
  })

  test("参数原样拼接（脚本路径、-m pip install 两种调用都走同一函数）", () => {
    expect(pythonCommand("/p/python", '-m pip install -q sherpa-onnx')).toBe(
      isWin ? '& "/p/python" -m pip install -q sherpa-onnx' : '"/p/python" -m pip install -q sherpa-onnx',
    )
    expect(pythonCommand("/p/python", '"/t/a b.py"')).toContain('"/t/a b.py"')
  })
})
