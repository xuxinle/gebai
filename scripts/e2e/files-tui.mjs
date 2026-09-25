/**
 * 终端独占模式（全屏 TUI：vi / watch / less / htop）实测。
 *
 * 用法：bun run scripts/e2e/files-tui.mjs [baseUrl]
 * 检查：备用屏进入/退出与主屏恢复、按键直达程序、Ctrl+C 退出、TUI 中改尺寸（SIGWINCH）、
 *      备用屏内搜索高亮可见且位置对齐、鼠标上报时的右键/中键归属、滚轮、Shift 绕过上报选词、
 *      备用屏往返后滚动历史仍在（用可见行验证，不用 viewport.scrollTop——xterm 6 走自绘滚动条）。
 */
import { chromium } from "playwright"
import { mkdirSync } from "node:fs"

const BASE = process.argv[2] ?? "http://localhost:3000"
/** 失败现场截图目录（缺省 /tmp/gebai-e2e；不进仓库，也不污染工作目录）。 */
const ARTIFACTS = process.env.E2E_ARTIFACTS ?? "/tmp/gebai-e2e"
mkdirSync(ARTIFACTS, { recursive: true })
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
  await page.waitForTimeout(150)
}
const run = async (cmd, wait = 900) => {
  await focus()
  await page.keyboard.type(cmd)
  await page.keyboard.press("Enter")
  await page.waitForTimeout(wait)
}
const lines = () => page.$$eval(`${ACTIVE} .xterm-rows > div`, (rs) => rs.map((r) => r.textContent || ""))
const text = async () => (await lines()).join("\n")
/** 只有内容的行（终端可见 12 行，内容不满屏时底部是空行——取尾行会取到空）。 */
const filled = async () => (await lines()).map((l) => l.trim()).filter(Boolean)
/** 整行仅两个数字的行 = stty size 输出（不会误抽屏幕上的其它数字）。 */
const sizeRow = async () => (await lines()).map((l) => l.trim()).find((l) => /^\d+ \d+$/.test(l)) ?? ""
const cols = async () => Number((await sizeRow()).split(/\s+/)[1] ?? 0)
const visibleNums = async () => (await lines()).join(" ").match(/\b\d{1,3}\b/g)?.map(Number) ?? []
const firstNum = async () => (await visibleNums())[0] ?? -1
const esc = async () => (await filled()).slice(-2).join(" | ").slice(0, 150)
const box = () =>
  page.$eval(`${ACTIVE} .xterm-screen`, (el) => {
    const r = el.getBoundingClientRect()
    return { x: r.x, y: r.y, w: r.width, h: r.height }
  })

/* ---------- 1. 主屏历史 + 备用屏往返 ---------- */
await run("clear; seq 1 120", 1500)
const beforeTui = await firstNum()
check("主屏有滚动历史（可见首行不是 1）", beforeTui > 1, `可见首个数字=${beforeTui}`)

await run("watch -n 0.3 'stty size'", 2200)
const inWatch = await text()
check("watch 进入备用屏（铺满、无滚动缓冲）", /Every 0\.3s/.test(inWatch), await esc())

// TUI 中改窗口尺寸：watch 里跑 stty size，缩窄后列数应变小、还原后回来
const size1 = await sizeRow()
await page.setViewportSize({ width: 1000, height: 620 })
await page.waitForTimeout(2500)
const size2 = await sizeRow()
await page.setViewportSize({ width: 1440, height: 760 })
await page.waitForTimeout(2500)
const size3 = await sizeRow()
check("TUI 中改窗口尺寸：程序收到 SIGWINCH 并按新尺寸重排", !!size1 && !!size2 && size1 !== size2 && size3 === size1, `${size1} → ${size2} → ${size3}`)

await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(1200)
await run("echo AFTER-WATCH", 900)
check("Ctrl+C 退出 watch，shell 回到可用状态", (await text()).includes("AFTER-WATCH"), await esc())

/* ---------- 2. 滚动历史仍在（用可见行，不看 viewport.scrollTop） ---------- */
await focus()
await page.mouse.move((await box()).x + 200, (await box()).y + 60)
await page.mouse.wheel(0, -1500)
await page.waitForTimeout(700)
const scrollUp = await firstNum()
check("备用屏往返后主屏历史仍可回滚（滚轮上滚）", scrollUp >= 1 && scrollUp < 120, `上滚后首个数字=${scrollUp}`)
await focus()
await page.mouse.wheel(0, 3000)
await page.waitForTimeout(700)
const scrolledDown = await firstNum()
check("滚到底部回到底行", scrolledDown > scrollUp, `回底首个数字=${scrolledDown}`)

/* ---------- 3. less：全屏 + 内部搜索 + q 退出 ---------- */
await run("less -R packages/web/src/files/term-keys.ts", 1600)
const lessRows = await lines()
check("less 进入备用屏（内容铺满整屏，无回显行）", lessRows.filter((l) => l.trim()).length >= lessRows.length - 1, `${lessRows.filter((l) => l.trim()).length}/${lessRows.length} 行有内容`)
await focus()
await page.keyboard.type("/selectAll")
await page.keyboard.press("Enter")
await page.waitForTimeout(900)
check("less 内部搜索生效", /selectAll/i.test(await text()), await esc())
await focus()
await page.keyboard.press("q")
await page.waitForTimeout(1000)
await run("echo AFTER-LESS", 800)
check("q 退出 less 并回到 shell", (await text()).includes("AFTER-LESS"), await esc())

/* ---------- 4. 备用屏内的面板搜索：高亮必须可见且位置对齐 ---------- */
// 先取真实列数（单元格宽度 = 屏宽 / 列数）：进入 less 后就看不到 stty 的输出了
await run("stty size", 800)
const termCols = Number((await sizeRow()).split(/\s+/)[1] ?? 0) || 172
await run("printf 'AAAFINDMEZZZ\\nBBB\\nCCC\\nDDD\\n' | less", 1500)
await focus()
await page.keyboard.press("Control+f")
await page.waitForTimeout(300)
await page.keyboard.type("FINDME")
await page.waitForTimeout(1200)
const deco = await page.evaluate(
  ({ sel, termCols }) => {
    const screenEl = document.querySelector(`${sel} .xterm-screen`)
    const screenW = screenEl.getBoundingClientRect().width
    const ds = [...document.querySelectorAll(`${sel} .xterm-decoration`)]
    const rows = [...document.querySelectorAll(`${sel} .xterm-rows > div`)]
    const idx = rows.findIndex((r) => (r.textContent || "").includes("AAAFINDMEZZZ"))
    const rowRect = idx >= 0 ? rows[idx].getBoundingClientRect() : null
    const scr = screenEl.getBoundingClientRect()
    return {
      count: ds.length,
      visible: ds.filter((d) => getComputedStyle(d).display !== "none").length,
      left: ds[0]?.style.left,
      top: ds[0]?.style.top,
      bg: ds[0] ? getComputedStyle(ds[0]).backgroundColor : null,
      expectedLeftPx: rowRect ? Math.round(rowRect.left - scr.left + 3 * (screenW / termCols)) : null,
      termCols,
      expectedTopPx: rowRect ? Math.round(rowRect.top - scr.top) : null,
      rowText: idx >= 0 ? rows[idx].textContent : null,
      screenW: Math.round(screenW),
    }
  },
  { sel: ACTIVE, termCols },
)
check("备用屏内 Ctrl+F 的命中装饰可见", deco.count > 0 && deco.visible > 0, `deco=${deco.count} 可见=${deco.visible} bg=${deco.bg}`)
const leftOk = Math.abs(Number.parseInt(deco.left ?? "-1", 10) - (deco.expectedLeftPx ?? -99)) <= 2
const topOk = Math.abs(Number.parseInt(deco.top ?? "-1", 10) - (deco.expectedTopPx ?? -99)) <= 2
check("备用屏内高亮位置与文字对齐（left/top = 匹配起点）", leftOk && topOk, `deco(${deco.left},${deco.top}) 期望(${deco.expectedLeftPx},${deco.expectedTopPx}) 列=${deco.termCols} 行=${deco.rowText}`)
await page.screenshot({ path: `${ARTIFACTS}/tui-less-search.png` })
await page.keyboard.press("Escape")
await page.waitForTimeout(300)
await focus()
await page.keyboard.press("q")
await page.waitForTimeout(900)

/* ---------- 5. vi：编辑落盘 + 鼠标上报时的右键/中键归属 ---------- */
await run("rm -f /tmp/gebai-tui-vi.txt; vi /tmp/gebai-tui-vi.txt", 1600)
await focus()
await page.keyboard.type("iHELLO-VI-中文")
await page.keyboard.press("Escape")
await page.waitForTimeout(400)
await page.keyboard.type(":wq")
await page.keyboard.press("Enter")
await page.waitForTimeout(1200)
await run("cat /tmp/gebai-tui-vi.txt", 900)
check("vi 内输入中文并 :wq 落盘", (await text()).includes("HELLO-VI-中文"), await esc())

await run("vi -c 'set mouse=a' /tmp/gebai-tui-vi.txt", 1800)
const b = await box()
await page.mouse.click(b.x + 60, b.y + 40, { button: "right" })
await page.waitForTimeout(700)
check("鼠标上报开启时右键归程序（面板菜单不弹出）", !(await page.evaluate(() => !!document.querySelector(".fw-menu-pop"))))
await page.keyboard.down("Shift")
await page.mouse.click(b.x + 60, b.y + 40, { button: "right" })
await page.keyboard.up("Shift")
await page.waitForTimeout(700)
const shiftMenu = await page.evaluate(() => {
  const pop = document.querySelector(".fw-menu-pop")
  return pop ? [...pop.querySelectorAll(".fw-menu-item")].map((i) => i.textContent.trim()).slice(0, 3) : null
})
check("Shift+右键强制调出面板菜单（绕过鼠标上报）", !!shiftMenu, JSON.stringify(shiftMenu))
await page.keyboard.press("Escape")
await page.waitForTimeout(300)

// 中键：上报开启时归程序（不该粘贴我们剪贴板里的内容）
await page.evaluate(() => navigator.clipboard.writeText("echo MIDDLE-SHOULD-NOT-PASTE"))
const before = await text()
await page.mouse.click(b.x + 120, b.y + 60, { button: "middle" })
await page.waitForTimeout(800)
check("鼠标上报开启时中键归程序（不注入剪贴板内容）", !(await text()).includes("MIDDLE-SHOULD-NOT-PASTE"), await esc())
// Shift 拖动仍可选词（xterm 自带的绕过规则）
await page.keyboard.down("Shift")
await page.mouse.move(b.x + 20, b.y + 30)
await page.mouse.down()
await page.mouse.move(b.x + 200, b.y + 30, { steps: 8 })
await page.mouse.up()
await page.keyboard.up("Shift")
await page.waitForTimeout(400)
const selLen = await page.evaluate((sel) => document.querySelector(`${sel} .xterm-selection div`) ? 1 : 0, ACTIVE)
check("Shift+拖动在鼠标上报程序内仍能选文本", selLen === 1, `选区层=${selLen}`)
await page.keyboard.press("Escape")
await page.waitForTimeout(300)
await focus()
await page.keyboard.type(":q!")
await page.keyboard.press("Enter")
await page.waitForTimeout(1000)

// 未开启鼠标上报时：右键出菜单、中键粘贴
await run("cat /tmp/gebai-tui-vi.txt", 800)
await page.mouse.click(b.x + 100, b.y + 80, { button: "right" })
await page.waitForTimeout(600)
check("未开启鼠标上报时右键照常出面板菜单", await page.evaluate(() => !!document.querySelector(".fw-menu-pop")))
await page.keyboard.press("Escape")
await page.waitForTimeout(300)
await page.evaluate(() => navigator.clipboard.writeText("echo MIDDLE-PASTES"))
await page.mouse.click(b.x + 150, b.y + (await box()).h - 12, { button: "middle" })
await page.waitForTimeout(700)
check("未开启鼠标上报时中键粘贴", (await text()).includes("echo MIDDLE-PASTES"), await esc())
await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(400)

/* ---------- 6. Ctrl+K 必须留给 shell（readline 删至行尾） ---------- */
await focus()
await page.keyboard.type("printf 'KEEPKILL'") // 不回车：光标行（行尾）上应有这段文本
await page.waitForTimeout(400)
const beforeCtrlK = (await lines()).find((l) => l.includes("KEEPKILL")) ?? ""
await page.keyboard.press("Control+a") // readline 回到行首（面板未占这个键）
await page.keyboard.press("Control+k") // readline kill-line：删掉整行
await page.waitForTimeout(500)
const afterCtrlK = (await lines()).find((l) => l.includes("KEEPKILL")) ?? ""
check(
  "Ctrl+K 归 shell（readline 删至行尾；未被面板当成清屏抢走）",
  beforeCtrlK.includes("KEEPKILL") && afterCtrlK === "",
  `前=${JSON.stringify(beforeCtrlK.trim().slice(-24))} 后=${JSON.stringify(afterCtrlK.trim().slice(-24))}`,
)
await focus()
await page.keyboard.press("Control+c")
await page.waitForTimeout(300)

/* ---------- 7. Ctrl+Shift+K 清屏 ---------- */
await run("clear; seq 1 60", 1200)
await focus()
await page.keyboard.press("Control+Shift+k")
await page.waitForTimeout(600)
const cleared = (await lines()).filter((l) => l.trim())
check("Ctrl+Shift+K 清屏（面板动作，不动 shell 的 Ctrl+K）", cleared.length <= 2, `剩余非空行=${cleared.length}`)

/* ---------- 8. htop（全屏 + 颜色 + q 退出） ---------- */
await run("htop", 2500)
const htopText = await text()
if (/command not found/.test(htopText)) {
  check("htop 全屏渲染（本机未安装，跳过）", true, "未安装")
} else {
  check("htop 全屏渲染（备用屏 + 满屏内容）", htopText.split("\n").filter((l) => l.trim()).length > 5, htopText.split("\n")[0]?.slice(0, 50))
  await page.screenshot({ path: `${ARTIFACTS}/tui-htop.png` })
  await focus()
  await page.keyboard.press("q")
  await page.waitForTimeout(1200)
  await run("echo AFTER-HTOP", 900)
  check("htop 退出后回到可用 shell", (await text()).includes("AFTER-HTOP"), await esc())
}

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
