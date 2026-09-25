/**
 * 终端行内编辑实测：光标移动（←→ / Home / End / Ctrl+A·E / Ctrl+←→ / Alt+B·F）与
 * 删除（Backspace / Delete / Ctrl+W / Ctrl+U / Ctrl+K / Alt+Backspace），
 * 覆盖**宽字符（中文）**、**折行长行**、**边界**与 TUI（vim 插入/普通模式）。
 *
 * 三层验证（任一层都能独立失败，避免“看起来对”）：
 *  ① 字节层：`stty -icanon -echo; cat -v` 看按键发出什么字节（关掉行规程的删词/删行，否则
 *     回显会被 tty 自己擦掉——那是正确行为，不是缺陷）
 *  ② 行为层：readline 里真的编辑 + 回车执行，看命令输出（端到端，用户看得见的那一层）
 *  ③ 视觉层：xterm 光标元素的 left/top 随移动变化（含折行时跨行）
 *
 * 用法：bun run scripts/e2e/files-edit.mjs [baseUrl]
 */
import { chromium } from "playwright"

const BASE = process.argv[2] ?? "http://localhost:3000"
const ACTIVE = ".fw-pty-view:not([hidden])"
const results = []
const check = (name, ok, detail = "") => {
  results.push({ name, ok, detail })
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? "  — " + detail : ""}`)
}

const browser = await chromium.launch()
const ctx = await browser.newContext({ viewport: { width: 1440, height: 760 }, permissions: ["clipboard-read", "clipboard-write"] })
const page = await ctx.newPage()
const errors = []
page.on("pageerror", (e) => errors.push(`pageerror: ${e.message}`))
page.on("console", (m) => {
  if (m.type() === "error") errors.push(`console.error: ${m.text().slice(0, 160)}`)
})

const closeAllSessions = async () => {
  await page.evaluate(async () => {
    const ws = new WebSocket(`${location.origin.replace(/^http/, "ws")}/ws`)
    await new Promise((res) => (ws.onopen = res))
    const req = (type, payload = {}) =>
      new Promise((resolve) => {
        const id = `c${Math.random()}`
        const onMsg = (ev) => {
          const m = JSON.parse(ev.data)
          if (m.id !== id) return
          ws.removeEventListener("message", onMsg)
          resolve(m)
        }
        ws.addEventListener("message", onMsg)
        ws.send(JSON.stringify({ type, id, payload }))
      })
    const list = await req("term.list")
    for (const s of list?.payload?.sessions ?? []) await req("term.close", { id: s.id })
    ws.close()
  })
}

await page.goto(`${BASE}/files`, { waitUntil: "domcontentloaded" })
await page.evaluate(() => {
  localStorage.setItem("gebai.ui.dockView", "terminal")
  localStorage.setItem("gebai.ui.dockVisible2", "1")
  localStorage.removeItem("gebai.ui.termSessions")
})
await closeAllSessions()
await page.goto(`${BASE}/files`, { waitUntil: "domcontentloaded" })
await page.waitForSelector(`${ACTIVE} .xterm`, { timeout: 30000 })
await page.waitForTimeout(2500)

const focus = async () => {
  await page.click(`${ACTIVE} .xterm-screen`)
  await page.waitForTimeout(120)
}
const run = async (cmd, wait = 800) => {
  await focus()
  await page.keyboard.type(cmd)
  await page.keyboard.press("Enter")
  await page.waitForTimeout(wait)
}
const keys = async (list) => {
  await focus()
  for (const k of list) await page.keyboard.press(k)
  await page.waitForTimeout(300)
}
/** 屏幕上「有内容的行」。 */
const filled = () => page.$$eval(`${ACTIVE} .xterm-rows > div`, (rs) => rs.map((r) => (r.textContent || "").trim()).filter(Boolean))
const screenText = async () => (await filled()).join("\n")
/** 拼接所有行（折行输出被拆成多行，拼起来才能看到完整字符串）。 */
const screenJoined = () => page.$$eval(`${ACTIVE} .xterm-rows > div`, (rs) => rs.map((r) => (r.textContent || "")).join(""))
const clean = () => run("clear", 600)
/** xterm 光标元素的位置（left/top，相对屏幕）。 */
const cursorRect = () =>
  page.evaluate((sel) => {
    const c = document.querySelector(`${sel} .xterm-cursor`)
    if (!c) return null
    const r = c.getBoundingClientRect()
    const scr = document.querySelector(`${sel} .xterm-screen`).getBoundingClientRect()
    return { left: Math.round(r.left - scr.left), top: Math.round(r.top - scr.top), w: Math.round(r.width) }
  }, ACTIVE)
/**
 * `cat file; echo MARK` 之后，MARK 行的上一行（= 文件内容，避免误取提示符行）。
 *
 * **从底部找**：同一屏里可能留着上一段 cat 的输出（同一个 MARK 出现两次），
 * 取第一个会拿到上一段的内容——实测踩过（表现为「文件没更新」，其实是断言取错了行）。
 */
const lineBeforeMark = async (mark) => {
  const ls = await filled()
  const i = ls.map((l) => l === mark).lastIndexOf(true)
  return i > 0 ? ls[i - 1] : "<未找到 MARK>"
}
/** 光标所在的是可视第几行（0 起）。 */
const cursorRowIndex = () =>
  page.evaluate((sel) => {
    const c = document.querySelector(`${sel} .xterm-cursor`)
    const rows = [...document.querySelectorAll(`${sel} .xterm-rows > div`)]
    if (!c) return -1
    const cy = c.getBoundingClientRect().top
    return rows.findIndex((r) => Math.abs(r.getBoundingClientRect().top - cy) < 3)
  }, ACTIVE)

/* ==================== ① 字节层 ==================== */

// -icanon：关掉行规程的“删词（Ctrl+W）/删行（Ctrl+U）/退格擦字”，否则回显被 tty 自己擦掉
await run("stty -icanon -echo; cat -v", 900)
await keys(["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "Delete", "Backspace"])
const b1 = await screenJoined()
check(
  "方向键 / Home / End / Delete / Backspace 发出标准字节",
  ["^[[D", "^[[C", "^[[A", "^[[B", "^[[H", "^[[F", "^[[3~", "^?"].every((s) => b1.includes(s)),
  (await filled()).slice(-1)[0]?.slice(0, 100),
)
await keys(["Control+a", "Control+e", "Control+w", "Control+u", "Control+k"])
const b2 = await screenJoined()
check(
  "Ctrl+A/E/W/U/K 原样透传（shell 的读行键一个都没被面板截走）",
  ["^A", "^E", "^W", "^U", "^K"].every((s) => b2.includes(s)),
  (await filled()).slice(-1)[0]?.slice(0, 100),
)
await keys(["Alt+b", "Alt+f", "Control+ArrowLeft", "Control+ArrowRight", "Control+Backspace", "Alt+Backspace", "Shift+ArrowLeft", "Control+Delete"])
const b3 = await screenJoined()
check(
  "Alt+B/F、Ctrl+←/→、Ctrl+Backspace、Alt+Backspace、Shift+←、Ctrl+Delete 均透传",
  ["^[b", "^[f", "^[[1;5D", "^[[1;5C", "^H", "^[^?", "^[[1;2D", "^[[3;5~"].every((s) => b3.includes(s)),
  (await filled()).slice(-1)[0]?.slice(0, 120),
)
await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(400)
await run("stty icanon echo", 500)

/* ==================== ② 行为层（readline） ==================== */

await clean()
await focus()
await page.keyboard.type("echo AB")
await keys(["ArrowLeft", "ArrowLeft"]) // 光标移到 A 之前
await page.keyboard.type("XY")
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
check("←← 后插入落在光标处（echo AB → echo XYAB）", (await screenText()).includes("XYAB"), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo AB")
await keys(["ArrowLeft"]) // 光标移到 A、B 之间
await page.keyboard.type("XY")
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
check("← 后插入落在 A、B 之间（echo AB → echo AXYB）", (await screenText()).includes("AXYB"), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo ABCDEF")
await keys(["Backspace", "Backspace"])
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
const bsRows = await filled()
check("Backspace 删光标前（ABCDEF → 输出 ABCD，且不回显 ABCDEF）", bsRows.includes("ABCD") && !bsRows.some((l) => l.includes("echo ABCDEF")), bsRows.slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo ABCDEF")
await keys(["ArrowLeft", "ArrowLeft", "Delete"]) // 光标在 E 上 → 删 E
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
check("Delete 删光标处（ABCDEF + ←← + Del → ABCDF）", (await screenText()).includes("ABCDF"), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo TAIL")
await keys(["Home"])
await page.keyboard.type("HEAD ")
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
check("Home 行首插入（echo TAIL → HEAD echo TAIL）", (await screenText()).includes("HEAD echo TAIL"), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo ONE TWO THREE")
await keys(["Control+w"]) // 删前一词
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
const cw = await screenJoined()
check("Ctrl+W 删前一词（THREE 被删）", /ONE TWO\s/.test(cw) && !/ONE TWO THREE/.test(cw), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo alpha beta")
await keys(["Alt+Backspace"]) // readline 的 backward-kill-word
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
check("Alt+Backspace 删前一词（beta 被删）", !/alpha beta/.test(await screenJoined()), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo left right")
await keys(["Control+ArrowLeft", "Control+ArrowLeft"]) // 按词跳到行首
await page.keyboard.type("X")
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
check("Ctrl+← 按词移动（插入点落在词边界上）", /Xecho|Xleft/.test(await screenJoined()), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("NOT-A-COMMAND-XXXX")
await keys(["Control+u"])
const afterU = (await filled()).slice(-1)[0] ?? ""
await page.keyboard.press("Enter")
await page.waitForTimeout(600)
check("Ctrl+U 清掉整行（且不执行任何命令）", !afterU.includes("NOT-A-COMMAND") && !(await screenText()).includes("command not found"), `Ctrl+U 后=${JSON.stringify(afterU.slice(-40))}`)

await clean()
await focus()
await page.keyboard.type("echo KEEPKILL")
await keys(["Control+a", "Control+k"]) // 行首按 Ctrl+K = 清空整行
const afterK = (await filled()).slice(-1)[0] ?? ""
await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(400)
check("Ctrl+K 删至行尾（行首按下 = 清空该行）", !afterK.includes("KEEPKILL"), `Ctrl+K 后=${JSON.stringify(afterK.slice(-40))}`)

// 边界：空行上的 Backspace / Delete / Home / End 不应破坏提示符，且 shell 仍可用
await clean()
await keys(["Backspace", "Backspace", "Delete", "Home", "End"])
await run("echo EDGE-OK", 700)
check("空行上的 Backspace/Delete/Home/End 是安全的空操作", (await screenText()).includes("EDGE-OK"), (await filled()).slice(-2)[0])

// 历史导航
await clean()
await run("echo HIST-ONE", 600)
await run("echo HIST-TWO", 600)
await keys(["ArrowUp"])
await page.keyboard.press("Enter")
await page.waitForTimeout(700)
check("↑ 取回上一条命令并可执行", (await screenJoined()).split("HIST-TWO").length - 1 >= 3, (await filled()).slice(-2)[0])

/* ==================== ③ 折行长行编辑 ==================== */

const LONG = "A".repeat(200)
await clean()
await focus()
await page.keyboard.type(`echo ${LONG}`) // 命令本身折成多行
await page.waitForTimeout(300)
const wrapRows = await page.evaluate((sel) => document.querySelectorAll(`${sel} .xterm-rows > div`).length, ACTIVE)
await keys(["Home"])
const homeRow = await cursorRowIndex()
await keys(["End"])
const endRow = await cursorRowIndex()
check("折行长行：Home 回到逻辑行首（首行）、End 回到行尾（末行）", homeRow === 0 && endRow > homeRow, `Home 在第 ${homeRow} 行、End 在第 ${endRow} 行，共 ${wrapRows} 行`)

await keys(["Control+u"]) // 清掉
await focus()
await page.keyboard.type(`echo ${LONG}`)
await page.keyboard.type("TAIL") // 行尾追加
await page.keyboard.press("Enter")
await page.waitForTimeout(900)
check("折行长行：行尾追加内容完整（200 个 A + TAIL）", (await screenJoined()).includes(`${LONG}TAIL`), `长度=${(await screenJoined()).length}`)

await clean()
await focus()
await page.keyboard.type(`echo ${LONG}`)
await keys(Array.from({ length: 10 }, () => "Backspace")) // 跨折行边界回删
await page.keyboard.type("ZZZ")
await page.keyboard.press("Enter")
await page.waitForTimeout(900)
const joined = await screenJoined()
check("折行长行：跨行 Backspace 删 10 个后追加（190 个 A + ZZZ）", joined.includes(`${"A".repeat(190)}ZZZ`) && !joined.includes("A".repeat(191)), `是否命中 190A+ZZZ=${joined.includes(`${"A".repeat(190)}ZZZ`)}`)

/* ==================== ④ 宽字符（中文） ==================== */

await clean()
await focus()
await page.keyboard.type("echo 中文测试")
await keys(["Backspace", "Backspace"])
await page.keyboard.press("Enter")
await page.waitForTimeout(800)
const cjkRows = await filled()
check("中文整字删除（Backspace×2 → 输出「中文」，且不回显「测试」）", cjkRows.includes("中文") && !cjkRows.some((l) => l.includes("测试")), cjkRows.slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo 甲乙丙")
await keys(["ArrowLeft", "ArrowLeft"]) // 光标移到「乙」之前
await page.keyboard.type("丁")
await page.keyboard.press("Enter")
await page.waitForTimeout(800)
check("中文按字数左右移动 + 插入（甲乙丙 + ←← + 丁 → 甲丁乙丙）", (await screenJoined()).includes("甲丁乙丙"), (await filled()).slice(-2)[0])

await clean()
await focus()
await page.keyboard.type("echo 甲乙丙")
await keys(["ArrowLeft", "Delete"]) // 光标在「丙」上 → 删掉整字
await page.keyboard.press("Enter")
await page.waitForTimeout(800)
const cjkDel = await screenJoined()
check("中文 Delete 删光标处整字（不残留半个字）", cjkDel.includes("甲乙") && !cjkDel.includes("甲乙丙"), (await filled()).slice(-2)[0])

// 折行边界上的中文删除（宽字符 × 列数最容易出现半字残留）：
// 用 shell 自校验——把编辑结果交给 wc -c（UTF-8 下每字 3 字节，locale 无关）
await clean()
await focus()
await page.keyboard.type(`echo ${"中".repeat(100)}`) // 100 个宽字符 = 200 列 > 面板列数
await keys(Array.from({ length: 3 }, () => "Backspace"))
await page.keyboard.type(" | wc -c")
await page.keyboard.press("Enter")
await page.waitForTimeout(900)
const cjkWrapRows = await filled()
check("折行处的中文删除不残留半字（97 字 × 3 字节 + 换行 = 292）", cjkWrapRows.includes("292"), `屏幕尾行=${JSON.stringify(cjkWrapRows.slice(-2).join(" | "))}`)

// 折行长行中间插入后，回显的命令行必须与真实缓冲一致（把整行注释掉，屏幕只剩这一行可数）
await clean()
await focus()
await page.keyboard.type(`echo X${"A".repeat(200)}`)
await keys(["Home"])
await page.keyboard.type("#")
await page.keyboard.press("Enter")
await page.waitForTimeout(900)
const echoRows = await filled()
const aCount = echoRows.join("").split("A").length - 1
check("折行长行中间插入后回显无错位（注释掉整行，A 恰好 200 个）", aCount === 200 && echoRows.join("").includes("#echo X"), `A 个数=${aCount}`)

/* ==================== ④b 其它输入/删除路径（输入法整段提交 / 双宽 emoji / Alt+D / 跨折行 Delete） ==================== */

// 输入法整段提交（CompositionEvent 的提交结果走 textarea 的 input 通道，不经过单键 keydown）：
// 光标在中段时提交，必须落在光标处而不是行尾
await clean()
await focus()
await page.keyboard.type("echo AB")
await keys(["ArrowLeft"])
await page.keyboard.insertText("中文") // 模拟输入法上屏
await page.keyboard.press("Enter")
await page.waitForTimeout(800)
check("输入法整段提交落在光标处（echo AB + ← + 「中文」 → A中文B）", (await screenJoined()).includes("A中文B"), (await filled()).slice(-2)[0])

// 双宽 emoji（代理对 + 4 字节 UTF-8）：Backspace 必须整字删
await clean()
await focus()
await page.keyboard.type("echo 😀😀")
await keys(["Backspace"])
await page.keyboard.type(" | wc -c")
await page.keyboard.press("Enter")
await page.waitForTimeout(800)
check("emoji 整字删除（😀😀 删一个 → 4 字节 + 换行 = 5）", (await filled()).includes("5"), (await filled()).slice(-2).join(" | "))

// Alt+D 删后一个词（readline kill-word 的前向版）
await clean()
await focus()
await page.keyboard.type("echo one two")
await keys(["Alt+b", "Alt+b"]) // 光标到「one」前
await keys(["Alt+d"]) // 删掉「one 」
await page.keyboard.press("Enter")
await page.waitForTimeout(800)
const altD = await filled()
check("Alt+D 删后一个词（one 被删，只剩 two）", altD.includes("two") && !altD.some((l) => l.includes("one two")), altD.slice(-2)[0])

// 跨折行边界的 Delete：删掉折行接缝处的字符，回显不能错位
await clean()
await focus()
await page.keyboard.type(`echo ${"B".repeat(199)}`)
await keys(["ArrowLeft", "ArrowLeft"]) // 光标移到倒数第二个 B 之前
await keys(["Delete"]) // 删掉光标处那个 B（199 → 198）
await keys(["End"]) // 归位行尾：不归位的话后面补的文本会插在“最后一个 B”之前（readline 的既有行为）
await page.keyboard.type(" | wc -c")
await page.keyboard.press("Enter")
await page.waitForTimeout(900)
check("折行接缝处 Delete 正确（198 个 B + 换行 = 199）", (await filled()).includes("199"), (await filled()).slice(-2).join(" | "))

/* ==================== ⑤ TUI（vim）行内编辑 ==================== */

await clean()
await run("rm -f /tmp/gebai-edit.txt", 500)
await run("vi /tmp/gebai-edit.txt", 1500)
await focus()
await page.keyboard.type("iabcdef") // 插入模式
await keys(["Home"])
await page.keyboard.type("XYZ")
await keys(["End", "Backspace", "Backspace"]) // 删掉 ef
await keys(["ArrowLeft", "Delete"]) // 光标落在 c|d 之间 → 删 d
await page.keyboard.press("Escape")
await page.waitForTimeout(300)
await page.keyboard.type(":wq")
await page.keyboard.press("Enter")
await page.waitForTimeout(1000)
await run("cat /tmp/gebai-edit.txt; echo END-MARK", 900)
const vimFile1 = await lineBeforeMark("END-MARK")
check("vim 插入模式：Home/End/←/Backspace/Delete 全部生效（XYZabc）", vimFile1 === "XYZabc", `文件内容=${JSON.stringify(vimFile1)}`)

await run("vi /tmp/gebai-edit.txt", 1500)
await focus()
// 先按 0 回到行首：vim 会用 viminfo 恢复上次的光标位置，不显式归位的话
// 「按几次 →」与期望值就对不上（不是缺陷，是 vim 的既有行为）
await keys(["0", "x"]) // 删首字符 X → YZabc
await keys(["ArrowRight", "ArrowRight"]) // 光标停在 a 上
await page.keyboard.type("D") // 删到行尾 → YZ
await page.keyboard.type(":wq")
await page.keyboard.press("Enter")
await page.waitForTimeout(1000)
await run("cat /tmp/gebai-edit.txt; echo END-MARK", 900)
const vimFile2 = await lineBeforeMark("END-MARK")
check("vim 普通模式：x / → / D 生效（XYZabc → YZ）", vimFile2 === "YZ", `文件内容=${JSON.stringify(vimFile2)}`)

check("控制台无错误", errors.length === 0, errors.slice(0, 3).join(" | "))

await closeAllSessions()
await browser.close()

const failed = results.filter((r) => !r.ok)
console.log(`\n=== ${results.length - failed.length}/${results.length} 通过 ===`)
if (failed.length) {
  console.log("未通过：")
  for (const f of failed) console.log(` - ${f.name}${f.detail ? "  " + f.detail : ""}`)
  process.exitCode = 1
}
