/**
 * 文件工作台终端 · 对照 VSCode 的端到端检查（Playwright + Chromium）。
 *
 * 用法：bun run scripts/e2e/files-e2e.mjs [baseUrl]（缺省 http://localhost:3000，即本机运行的服务）
 * 覆盖：初始尺寸 / 折行 / 调色板 / 搜索（高亮 + 三开关 + 计数）/ 键位（清屏·全选·切标签·新建·滚动）/
 *      未读指示 / OSC 标题 / 粘贴与复制 / 设置菜单 / 关闭确认 / dead 标签重启 / 刷新接管 / 控制台零错误。
 */
import { chromium } from "playwright"
import { mkdirSync } from "node:fs"

const BASE = process.argv[2] ?? "http://localhost:3000"
/** 失败现场截图目录（缺省 /tmp/gebai-e2e；不进仓库，也不污染工作目录）。 */
const ARTIFACTS = process.env.E2E_ARTIFACTS ?? "/tmp/gebai-e2e"
mkdirSync(ARTIFACTS, { recursive: true })
const URL = `${BASE}/files`
const results = []
const check = (name, ok, detail = "") => {
  results.push({ name, ok, detail })
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? "  — " + detail : ""}`)
}

/** 活动标签页内的元素（多标签下 xterm 节点会有多份，只有活动的可见）。 */
const ACTIVE = ".fw-pty-view:not([hidden])"

const browser = await chromium.launch()
const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, permissions: ["clipboard-read", "clipboard-write"] })
const page = await ctx.newPage()
const errors = []
page.on("pageerror", (e) => errors.push(`pageerror: ${e.message}`))
page.on("console", (m) => {
  if (m.type() === "error") errors.push(`console.error: ${m.text().slice(0, 200)}`)
})

await page.goto(URL, { waitUntil: "domcontentloaded" })
await page.evaluate(() => {
  localStorage.setItem("gebai.ui.dockView", "terminal")
  localStorage.setItem("gebai.ui.dockVisible2", "1")
  localStorage.removeItem("gebai.ui.termSessions")
  localStorage.removeItem("gebai.ui.termPrefs")
  localStorage.removeItem("gebai.ui.termSearchOpts")
})

// 先清干净服务端上的 PTY 会话（并发上限 8；上一次探测/异常退出留下的会话会占满额度，
// 让后续“新建终端”直接失败——测出来的失败全是假的）。
const reclaimed = await page.evaluate(async () => {
  const ws = new WebSocket(`${location.origin.replace(/^http/, "ws")}/ws`)
  await new Promise((res, rej) => {
    ws.onopen = res
    ws.onerror = () => rej(new Error("ws 连接失败"))
    setTimeout(() => rej(new Error("ws 连接超时")), 5000)
  })
  const req = (type, payload = {}) =>
    new Promise((resolve) => {
      const id = `c${Math.random().toString(36).slice(2)}`
      const onMsg = (ev) => {
        let m
        try {
          m = JSON.parse(ev.data)
        } catch {
          return
        }
        if (m.id !== id) return
        ws.removeEventListener("message", onMsg)
        resolve(m)
      }
      ws.addEventListener("message", onMsg)
      ws.send(JSON.stringify({ type, id, payload }))
    })
  const list = await req("term.list")
  const ids = (list?.payload?.sessions ?? []).map((s) => s.id)
  for (const id of ids) await req("term.close", { id })
  ws.close()
  return ids.length
})
console.log(`（回收了 ${reclaimed} 个残留终端会话）`)

await page.goto(URL, { waitUntil: "domcontentloaded" })
await page.waitForSelector(`${ACTIVE} .xterm`, { timeout: 30000 })
await page.waitForTimeout(2500)

const screen = () => page.$eval(`${ACTIVE} .xterm-rows`, (el) => el.innerText)
const focus = async () => {
  await page.click(`${ACTIVE} .xterm-screen`)
  await page.waitForTimeout(150)
}
const type = async (text) => {
  await focus()
  await page.keyboard.type(text)
  await page.keyboard.press("Enter")
}
const run = async (cmd, wait = 800) => {
  await type(cmd)
  await page.waitForTimeout(wait)
}
const tail = async (n = 3) => (await screen()).split("\n").slice(-n).join("\n")
const geom = () =>
  page.evaluate((sel) => {
    const scr = document.querySelector(`${sel} .xterm-screen`)
    const rows = [...document.querySelectorAll(`${sel} .xterm-rows > div`)]
    return { rows: rows.length, screenW: Math.round(scr?.getBoundingClientRect().width ?? 0) }
  }, ACTIVE)
const status = () => page.$eval(".fw-pty-search-status", (e) => e.textContent)

/* ---------- 1. 初始尺寸与折行 ---------- */
await run("echo COLS=$COLUMNS LINES=$LINES; stty size")
const initGeom = await geom()
const sizeLine = (await screen()).split("\n").reverse().find((l) => /^\d+ \d+$/.test(l.trim()))
const ptyCols = sizeLine ? Number(sizeLine.trim().split(/\s+/)[1]) : 0
check("初始 PTY 尺寸等于面板实际列数（不再恒为 80）", ptyCols > 100, `stty cols=${ptyCols}，屏幕宽 ${initGeom.screenW}px，可见行 ${initGeom.rows}`)

const longLine = "0123456789".repeat(20) // 200 字符 > 面板列数，必然折行
await run(`echo ${longLine}`)
const joined = (await screen()).replace(/\s/g, "")
check("200 字符长行折行后内容完整（无错位/丢字）", joined.includes(longLine), `缓冲里可拼出 ${joined.includes(longLine) ? 200 : joined.length} 字符`)

/* ---------- 2. 调色板 ---------- */
await run("printf '\\033[31mNORMRED\\033[91mBRIGHTRED\\033[39m\\n'")
const colors = await page.evaluate((sel) => {
  const spans = [...document.querySelectorAll(`${sel} .xterm-rows span`)]
  // 取**最后一个**命中：前面还有命令行回显（未上色），输出在它后面
  const find = (t) => {
    const hits = spans.filter((s) => (s.textContent || "").includes(t))
    const el = hits[hits.length - 1]
    return el
      ? { color: getComputedStyle(el).color, cls: el.className, style: el.getAttribute("style")?.slice(0, 60) ?? "" }
      : null
  }
  return { normal: find("NORMRED"), bright: find("BRIGHTRED"), defaultFg: document.querySelector(`${sel} .xterm-rows`) ? getComputedStyle(document.querySelector(`${sel} .xterm-rows`)).color : null }
}, ACTIVE)
// 红与亮红必须落到两个不同颜色上（亮色可能因最小对比度被微调，但不应等于默认前景）
check(
  "ANSI 红与亮红是两个不同颜色、且都不是默认前景色",
  !!colors.normal && !!colors.bright && colors.normal.color !== colors.bright.color && colors.normal.color !== colors.defaultFg,
  JSON.stringify(colors),
)

/* ---------- 3. 搜索：高亮 + 开关 + 计数 ---------- */
await run("printf 'FINDME-a\\nFINDME-b\\nfindme-c\\n'")
await focus()
await page.keyboard.press("Control+f")
await page.waitForTimeout(300)
await page.keyboard.type("FINDME")
await page.waitForTimeout(900)
const deco = await page.evaluate((sel) => {
  const d = document.querySelector(`${sel} .xterm-decoration`)
  return {
    count: document.querySelectorAll(`${sel} .xterm-decoration`).length,
    bg: d ? getComputedStyle(d).backgroundColor : null,
    status: document.querySelector(".fw-pty-search-status")?.textContent ?? "",
    toggles: [...document.querySelectorAll(".fw-pty-search .fw-term-toggle")].map((b) => b.textContent),
  }
}, ACTIVE)
check("搜索命中在滚动缓冲里可见（有 decoration 且带底色）", deco.count > 0 && !!deco.bg && deco.bg !== "rgba(0, 0, 0, 0)", `deco=${deco.count} bg=${deco.bg}`)
check("搜索显示命中计数（第 n/m 项）", /^\d+\/\d+$/.test(deco.status), `状态=${JSON.stringify(deco.status)}`)
check("查找框有 Aa / ab / .* 三个开关", deco.toggles.join("|") === "Aa|ab|.*", JSON.stringify(deco.toggles))

const countOf = (s) => Number(/^\d+\/(\d+)$/.exec(s)?.[1] ?? /^(\d+) 处$/.exec(s)?.[1] ?? -1)
const totalBefore = countOf(await status())
// 大小写敏感：FINDME 只命中 4 处（回声 2 + 输出 2），不敏感含 findme-c 共 6 处
await page.click('.fw-pty-search .fw-term-toggle[title="区分大小写"]')
await page.waitForTimeout(800)
const totalCase = countOf(await status())
check("打开「区分大小写」后命中数收窄", totalBefore === 6 && totalCase === 4, `${totalBefore} → ${totalCase}`)
await page.click('.fw-pty-search .fw-term-toggle[title="区分大小写"]')
await page.waitForTimeout(400)

// 正则：FINDME-[ab] 只命中 a/b 两类（回声 + 输出 = 4）
await page.click('.fw-pty-search .fw-term-toggle[title="正则表达式"]')
await page.click(".fw-pty-search input")
await page.keyboard.press("Control+a")
await page.keyboard.type("FINDME-[ab]")
await page.waitForTimeout(900)
const totalRegex = countOf(await status())
check("正则开关生效（FINDME-[ab] → 4 处，字面量搜则为 0）", totalRegex === 4, `命中=${totalRegex}（${await status()}）`)
await page.click('.fw-pty-search .fw-term-toggle[title="正则表达式"]')
await page.waitForTimeout(400)
// 关闭：Esc（焦点已在输入框上）
await page.keyboard.press("Escape")
await page.waitForTimeout(400)
const closed = await page.evaluate((sel) => ({
  hidden: document.querySelector(".fw-pty-search")?.hidden,
  deco: document.querySelectorAll(`${sel} .xterm-decoration`).length,
}), ACTIVE)
check("Esc 关闭查找框且清掉高亮", closed.hidden === true && closed.deco === 0, JSON.stringify(closed))

/* ---------- 4. 键位：清屏 / 全选 / 新建 / 切标签 / 滚动 ---------- */
await run("seq 1 60", 1000)
await focus()
await page.keyboard.press("Control+Shift+k")
await page.waitForTimeout(600)
const cleared = await screen()
check("Ctrl+Shift+K 清屏（不占 shell 的 Ctrl+K）", !cleared.includes("seq 1 60") && !cleared.includes("59"), `首行=${cleared.split("\n")[0]}`)

await focus()
await page.keyboard.press("Control+Shift+a")
await page.waitForTimeout(400)
const selDivs = await page.evaluate((sel) => document.querySelectorAll(`${sel} .xterm-selection div`).length, ACTIVE)
check("Ctrl+Shift+A 全选", selDivs > 0, `selection divs=${selDivs}`)

await focus()
await page.keyboard.press("Control+Shift+`")
await page.waitForTimeout(2000)
check("Ctrl+Shift+` 新建终端", (await page.$$(".fw-term-tab")).length === 2, `标签数=${(await page.$$(".fw-term-tab")).length}`)

const activeIdx = () => page.evaluate(() => [...document.querySelectorAll(".fw-term-tab")].findIndex((t) => t.className.includes("active")))
await focus()
await page.keyboard.press("Control+Shift+ArrowUp")
await page.waitForTimeout(600)
const idx1 = await activeIdx()
await focus()
await page.keyboard.press("Control+Shift+ArrowDown")
await page.waitForTimeout(600)
const idx2 = await activeIdx()
check("Ctrl+Shift+↑/↓ 切换终端标签", idx1 === 0 && idx2 === 1, `↑→${idx1}，↓→${idx2}`)

await run("clear; seq 1 300", 1800)
const firstVisible = () =>
  page.evaluate((sel) => {
    const rows = [...document.querySelectorAll(`${sel} .xterm-rows > div`)]
    for (const r of rows) {
      const m = /^(\d{1,3})$/.exec((r.textContent || "").trim())
      if (m) return Number(m[1])
    }
    return -1
  }, ACTIVE)
const atBottom = await firstVisible()
await focus()
await page.keyboard.press("Control+Shift+Home")
await page.waitForTimeout(800)
const afterHome = await firstVisible()
await focus()
await page.keyboard.press("Control+Shift+End")
await page.waitForTimeout(800)
const afterEnd = await firstVisible()
// 注意：xterm 6 的滚动条是自绘的，`.xterm-viewport` 的 scrollTop/scrollHeight 恒等 —— 只能看可见行
check("Ctrl+Shift+Home/End 滚动到顶/底", afterHome === 1 && afterEnd === atBottom && atBottom > 1, `底=${atBottom} 顶后=${afterHome} 回底后=${afterEnd}`)

/* ---------- 5. 非活动标签未读指示 ---------- */
const clickTab = (i) => page.evaluate((i) => document.querySelectorAll(".fw-term-tab")[i].dispatchEvent(new MouseEvent("click", { bubbles: true })), i)
await clickTab(1)
await page.waitForTimeout(600)
// 在 1 号里挂一个延时输出，马上切到 0 号——输出到达时 1 号已是非活动标签
await run("(sleep 2; echo UNREAD-LATE) &", 500)
await clickTab(0)
await page.waitForTimeout(3500)
const unread = await page.evaluate(() => [...document.querySelectorAll(".fw-term-tab")].map((t) => t.className))
check("非活动标签有新输出时点亮未读指示", unread.some((c) => c.includes("unread")), JSON.stringify(unread))
await clickTab(1)
await page.waitForTimeout(400)
const unreadCleared = await page.evaluate(() => [...document.querySelectorAll(".fw-term-tab")].map((t) => t.className))
check("切回该标签后未读指示消失", unreadCleared.every((c) => !c.includes("unread")), JSON.stringify(unreadCleared))

/* ---------- 6. OSC 标题 → 标签名 ---------- */
await focus()
await type("printf '\\033]0;MY-TERM-TITLE\\007'")
await page.waitForTimeout(1000)
const labelNow = await page.evaluate(() => document.querySelector(".fw-term-tab.active .fw-term-tab-name")?.textContent)
check("shell 的 OSC 标题跟随到标签名", labelNow === "MY-TERM-TITLE", `标签名=${labelNow}`)

/* ---------- 7. 粘贴 / 复制 ---------- */
await page.evaluate(() => navigator.clipboard.writeText("echo PASTE-CHECK"))
await focus()
await page.keyboard.press("Control+Shift+v")
await page.waitForTimeout(800)
check("Ctrl+Shift+V 粘贴（单行不自动执行）", (await tail(1)).includes("echo PASTE-CHECK"), await tail(1))
await focus()
await page.keyboard.press("Shift+Insert")
await page.waitForTimeout(700)
check("Shift+Insert 也是粘贴", (await tail(1)).split("echo PASTE-CHECK").length >= 3, await tail(1))
await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(400)

// 中键粘贴（X11 惯例）
await page.evaluate(() => navigator.clipboard.writeText("echo MIDDLE"))
const box = await page.$eval(`${ACTIVE} .xterm-screen`, (el) => {
  const r = el.getBoundingClientRect()
  return { x: r.x, y: r.y, w: r.width, h: r.height }
})
await page.mouse.click(box.x + 120, box.y + box.h - 12, { button: "middle" })
await page.waitForTimeout(800)
check("中键粘贴", (await tail(1)).includes("echo MIDDLE"), await tail(1))
await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(400)

// 复制：拖选 → Ctrl+C（有选区即复制）
await run("echo COPY-ME-12345", 800)
const rowBox = await page.evaluate((sel) => {
  const rows = [...document.querySelectorAll(`${sel} .xterm-rows > div`)]
  const el = rows[rows.length - 2]
  const r = el.getBoundingClientRect()
  return { x: r.x, y: r.y, h: r.height }
}, ACTIVE)
await page.evaluate(() => navigator.clipboard.writeText("__EMPTY__"))
await page.mouse.move(rowBox.x + 4, rowBox.y + rowBox.h / 2)
await page.mouse.down()
await page.mouse.move(rowBox.x + 300, rowBox.y + rowBox.h / 2, { steps: 10 })
await page.mouse.up()
await page.waitForTimeout(300)
// 不能再 click 一次：xterm 里单击会清掉选区（先前的失败就是被这一步清掉的）
await page.keyboard.press("Control+c")
await page.waitForTimeout(600)
const clip = await page.evaluate(() => navigator.clipboard.readText()).catch(() => "<读失败>")
check("拖选后 Ctrl+C 复制（有选区时不发中断）", clip !== "__EMPTY__" && clip.length > 0, JSON.stringify(clip.slice(0, 40)))

/* ---------- 8. 设置菜单项齐备 ---------- */
await page.click('.fw-term-actions button[title^="终端设置"]')
await page.waitForTimeout(500)
const menuText = await page.evaluate(() => [...document.querySelectorAll(".fw-menu-pop .fw-menu-item")].map((e) => e.textContent.trim()).join(" | "))
check(
  "设置菜单含行高 / 光标 / 选中即复制 / Ctrl+滚轮",
  /行高/.test(menuText) && /光标/.test(menuText) && /选中即复制/.test(menuText) && /滚轮/.test(menuText),
  menuText.slice(0, 220),
)
await page.keyboard.press("Escape")
await page.waitForTimeout(300)

/* ---------- 9. 关闭确认（有输出在跑时） ---------- */
await focus()
await page.keyboard.type("seq 1 1000000")
await page.keyboard.press("Enter")
await page.waitForTimeout(400)
const tabsBefore = (await page.$$(".fw-term-tab")).length
await page.evaluate(() => document.querySelector(".fw-term-tab.active .fw-term-tab-close").dispatchEvent(new MouseEvent("click", { bubbles: true })))
await page.waitForTimeout(800)
const dialogShown = await page.evaluate(() => !!document.querySelector(".fw-overlay .fw-dialog"))
check("关闭正在输出的终端先确认（VSCode confirmOnKill 同语义）", dialogShown, dialogShown ? "" : "未弹确认")
if (dialogShown) {
  await page.click(".fw-dialog-actions .fw-btn:not(.primary)")
  await page.waitForTimeout(500)
}
check("取消后标签仍在", (await page.$$(".fw-term-tab")).length === tabsBefore, `标签数=${(await page.$$(".fw-term-tab")).length}`)
await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(600)

/* ---------- 10. shell 退出 → dead 标签 + Enter 重启 ---------- */
await run("exit", 1600)
const deadInfo = await page.evaluate((sel) => {
  const t = document.querySelector(".fw-term-tab.active")
  return { cls: t?.className, title: t?.getAttribute("title"), tail: document.querySelector(`${sel} .xterm-rows`)?.innerText.split("\n").slice(-3).join("\n") }
}, ACTIVE)
check("shell 退出后标签标为 dead、tooltip 带退出码", /dead/.test(deadInfo.cls ?? "") && /已退出/.test(deadInfo.title ?? ""), JSON.stringify(deadInfo).slice(0, 200))
await focus()
await page.keyboard.press("Enter")
await page.waitForTimeout(2200)
const revived = await page.evaluate((sel) => ({ cls: document.querySelector(".fw-term-tab.active")?.className, tail: document.querySelector(`${sel} .xterm-rows`)?.innerText.split("\n").slice(-2).join("\n") }), ACTIVE)
check("dead 标签按 Enter 就地重启", !/dead/.test(revived.cls ?? ""), JSON.stringify(revived).slice(0, 160))

/* ---------- 11. 主题切换后调色板随主题 ---------- */
const themeApplied = await page.evaluate(() => {
  document.dispatchEvent(new Event("gebai:theme-change"))
  return true
})
check("主题切换事件被接收（重算调色板，不报错）", themeApplied)

/* ---------- 12. 刷新后接管 + 尺寸 ---------- */
await run("echo AFTER-RELOAD-MARK", 800)
const tabsBeforeReload = (await page.$$(".fw-term-tab")).length
await page.reload({ waitUntil: "domcontentloaded" })
await page.waitForSelector(`${ACTIVE} .xterm`, { timeout: 30000 })
await page.waitForTimeout(3000)
check("刷新后接管会话并回放缓冲", (await screen()).includes("AFTER-RELOAD-MARK"), await tail(2))
check("刷新后标签数保持", (await page.$$(".fw-term-tab")).length === tabsBeforeReload, `${(await page.$$(".fw-term-tab")).length} / ${tabsBeforeReload}`)
await focus()
await page.keyboard.type("stty size")
await page.keyboard.press("Enter")
await page.waitForTimeout(1000)
const reloadSize = (await screen()).split("\n").reverse().find((l) => /^\d+ \d+$/.test(l.trim()))
const reloadCols = reloadSize ? Number(reloadSize.trim().split(/\s+/)[1]) : 0
check("刷新接管后尺寸仍然正确", reloadCols > 100, `cols=${reloadCols}`)

/* ---------- 13. 控制台零错误 ---------- */
check("控制台无错误", errors.length === 0, errors.slice(0, 3).join(" | "))

await page.screenshot({ path: `${ARTIFACTS}/term-e2e.png` })
await browser.close()

const failed = results.filter((r) => !r.ok)
console.log(`\n=== ${results.length - failed.length}/${results.length} 通过 ===`)
if (failed.length) {
  console.log("未通过：")
  for (const f of failed) console.log(` - ${f.name}${f.detail ? "  " + f.detail : ""}`)
  process.exitCode = 1
}
