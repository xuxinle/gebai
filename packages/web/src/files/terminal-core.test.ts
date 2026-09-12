/**
 * 终端面板纯逻辑测试：ANSI SGR 解析、控制字符（`\r` / `\b` / `\n` / `\t`）处理、输出缓冲、命令历史。
 *
 * 不依赖 DOM（web 包无 jsdom）：只测 `terminal-core.ts` 的纯函数与纯类；DOM 与网络部分在
 * `terminal.ts`，需要页面环境，不做单测。
 */
import { describe, expect, test } from "bun:test"
import {
  TERM_MAX_LINES,
  TermBuffer,
  applyAnsi,
  fgColorOf,
  newAnsiState,
  pathTail,
  pushHistory,
  samePath,
} from "./terminal-core"

/* ------------------------------ ANSI ------------------------------ */

describe("applyAnsi · SGR", () => {
  test("前景色 30-37 与 0 重置", () => {
    const { runs } = applyAnsi("\x1b[31m红\x1b[0m普通")
    expect(runs).toEqual([
      { text: "红", color: "red", bold: false },
      { text: "普通", color: null, bold: false },
    ])
  })

  test("亮色 90-97、粗体开（1）、粗体关（22）、前景重置（39）", () => {
    const st = newAnsiState()
    expect(applyAnsi("\x1b[1;92mOK", st).runs).toEqual([{ text: "OK", color: "brightGreen", bold: true }])
    expect(applyAnsi("\x1b[22m再说", st).runs).toEqual([{ text: "再说", color: "brightGreen", bold: false }])
    expect(applyAnsi("\x1b[39m普通", st).runs).toEqual([{ text: "普通", color: null, bold: false }])
  })

  test("颜色码映射表", () => {
    expect(fgColorOf(30)).toBe("black")
    expect(fgColorOf(37)).toBe("white")
    expect(fgColorOf(90)).toBe("brightBlack")
    expect(fgColorOf(97)).toBe("brightWhite")
    expect(fgColorOf(39)).toBeNull()
    expect(fgColorOf(1)).toBeNull()
  })

  test("未知转义序列整体忽略且不可见（CSI 非 m、OSC、未知 SGR 码）", () => {
    const { runs, state } = applyAnsi("a\x1b[?25lb\x1b]0;标题\x07c\x1b[999md")
    expect(runs.map((r) => r.text).join("")).toBe("abcd")
    expect(state.color).toBeNull()
    expect(state.pending).toBe("")
  })

  test("256 色 / 真彩不误判：38;5;31 不把 31 当成红色", () => {
    const a = applyAnsi("\x1b[38;5;31mX")
    expect(a.runs).toEqual([{ text: "X", color: null, bold: false }])
    const b = applyAnsi("\x1b[38;2;255;0;0mY")
    expect(b.runs).toEqual([{ text: "Y", color: null, bold: false }])
  })

  test("转义序列跨分片：半截序列留到下一片再拼", () => {
    const st = newAnsiState()
    const first = applyAnsi("\x1b[3", st)
    expect(first.runs).toEqual([])
    expect(st.pending).toBe("\x1b[3")
    expect(applyAnsi("1m红色", st).runs).toEqual([{ text: "红色", color: "red", bold: false }])

    const st2 = newAnsiState()
    expect(applyAnsi("\x1b]0;ti", st2).runs).toEqual([])
    expect(st2.pending).toBe("\x1b]0;ti")
    expect(applyAnsi("tle\x07正文", st2).runs).toEqual([{ text: "正文", color: null, bold: false }])
  })

  test("控制字符不落进可见文本（防漏网时渲染成豆腐块）", () => {
    expect(applyAnsi("a\r\nb\tc").runs.map((r) => r.text).join("")).toBe("abc")
  })
})

/* ------------------------------ 缓冲 ------------------------------ */

describe("TermBuffer · 控制字符", () => {
  test("\\r 回到行首覆盖当前行（写得更短时按终端语义保留旧尾字符）", () => {
    const buf = new TermBuffer()
    buf.write("abc\rxy")
    expect(buf.text()).toBe("xyc")
  })

  test("\\r\\n 正常换行，\\b 退格覆盖", () => {
    const buf = new TermBuffer()
    buf.write("abc\r\n")
    buf.write("12\b\b34")
    expect(buf.lines().map((l) => l.text)).toEqual(["abc", "34"])
  })

  test("进度条式重写：多次 \\r 只留最后一次内容", () => {
    const buf = new TermBuffer()
    buf.write("10%\r20%\r30%")
    expect(buf.text()).toBe("30%")
  })

  test("\\t 推进到下一个 8 列制表位", () => {
    const buf = new TermBuffer()
    buf.write("ab\tc")
    expect(buf.text()).toBe("ab      c")
  })

  test("单行超长按列截断并标记（避免无换行输出撑爆横向布局）", () => {
    const buf = new TermBuffer({ maxCols: 10 })
    buf.write("0123456789abcdef\nnext")
    const [long, next] = buf.lines()
    expect(long!.text).toBe("0123456789")
    expect(long!.text.length).toBe(10)
    expect(long!.truncated).toBe(true)
    expect(next!.text).toBe("next")
    expect(next!.truncated).toBe(false)
  })
})

describe("TermBuffer · 缓冲策略与样式", () => {
  test("ANSI 颜色随行冻结，同色相邻字符合并成一个 run", () => {
    const buf = new TermBuffer()
    buf.write("\x1b[32mOK\x1b[0m done\n")
    const line = buf.lines()[0]!
    expect(line.text).toBe("OK done")
    expect(line.runs).toEqual([
      { text: "OK", color: "green", bold: false },
      { text: " done", color: null, bold: false },
    ])
  })

  test("粗体开关同样切分 run", () => {
    const buf = new TermBuffer()
    buf.write("\x1b[1m粗\x1b[22m细\n")
    expect(buf.lines()[0]!.runs).toEqual([
      { text: "粗", color: null, bold: true },
      { text: "细", color: null, bold: false },
    ])
  })

  test("回溯上限：最旧行丢弃，trimmedLines 递增（视图据此裁 DOM 顶部）", () => {
    const buf = new TermBuffer({ maxLines: 3 })
    buf.write("1\n2\n3\n4\n")
    expect(buf.lines().map((l) => l.text)).toEqual(["2", "3", "4", ""])
    expect(buf.trimmedLines).toBe(1)
    expect(TERM_MAX_LINES).toBeGreaterThan(0)
  })

  test("clear 重置缓冲、裁剪计数与颜色状态", () => {
    const buf = new TermBuffer({ maxLines: 2 })
    buf.write("\x1b[31ma\nb\nc\n")
    expect(buf.trimmedLines).toBe(1)
    buf.clear()
    expect(buf.trimmedLines).toBe(0)
    expect(buf.lines().length).toBe(1)
    expect(buf.text()).toBe("")
    buf.write("普通")
    expect(buf.lines()[0]!.runs).toEqual([{ text: "普通", color: null, bold: false }])
  })
})

/* ------------------------------ 历史与路径 ------------------------------ */

describe("pushHistory", () => {
  test("重复命令提到末尾，不产生第二条", () => {
    expect(pushHistory(["a", "b"], "a")).toEqual(["b", "a"])
  })

  test("空白提交不入历史（且不改动已有内容）", () => {
    expect(pushHistory(["a"], "   ")).toEqual(["a"])
    expect(pushHistory([], "")).toEqual([])
  })

  test("上限：只保留最近 max 条，最旧丢弃", () => {
    let list: string[] = []
    for (let i = 0; i < 105; i++) list = pushHistory(list, `c${i}`, 100)
    expect(list.length).toBe(100)
    expect(list[0]).toBe("c5")
    expect(list[99]).toBe("c104")
  })

  test("超过上限的旧列表先被裁到上限内", () => {
    expect(pushHistory(["a", "b", "c"], "d", 2)).toEqual(["c", "d"])
  })
})

describe("路径工具", () => {
  test("pathTail 取尾段（分隔符两种、末尾多余分隔符、空值）", () => {
    expect(pathTail("/a/b/c")).toBe("c")
    expect(pathTail("C:\\work\\proj\\")).toBe("proj")
    expect(pathTail("")).toBe(".")
    expect(pathTail("/")).toBe("/")
  })

  test("samePath：分隔符统一，Windows 盘符路径忽略大小写，POSIX 保持敏感", () => {
    expect(samePath("C:\\A\\B", "c:/a/b")).toBe(true)
    expect(samePath("/a/b/", "/a/b")).toBe(true)
    expect(samePath("/a/b", "/a/B")).toBe(false)
    expect(samePath("/a/b", "/a/c")).toBe(false)
  })
})
