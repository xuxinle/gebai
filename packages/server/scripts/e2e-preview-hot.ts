/**
 * preview_server 前端热重建验证（A 方案）：直接调用 code 子Agent 的 preview_server 工具，
 * 断言 ①默认开启热重建（子进程日志出现 vite build --watch 与 dev-reload 广播）
 * ②改动 packages/web 源码后 dist 自动重建（产物 hash 变化）③无需重启预览服务。
 *
 * 用法：bun run packages/server/scripts/e2e-preview-hot.ts
 */
import { mkdtempSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { makePreviewServerTool } from "@gebai/agents/src/core/code-tools"

const WEB = join(import.meta.dir, "..", "..", "web")
const PORT = 3993
const tmpDir = mkdtempSync(join(tmpdir(), "gebai-preview-hot-"))

let passed = 0
const failures: string[] = []
const check = (name: string, cond: boolean, detail = ""): void => {
  if (cond) {
    passed++
    console.log(`  ✓ ${name}`)
  } else {
    failures.push(name)
    console.log(`  ✗ ${name}${detail ? ` —— ${detail}` : ""}`)
  }
}
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

/** index.html 引用的全部产物名（js + css）——任一变化即 dist 重建的判据（CSS 变更进独立 css bundle，只盯 main-*.js 会漏判）。 */
async function assetsOf(port: number): Promise<string> {
  const html = await fetch(`http://127.0.0.1:${port}/`).then((r) => r.text()).catch(() => "")
  return (html.match(/assets\/[^"']+\.(?:js|css)/g) ?? []).sort().join(",")
}

const tool = makePreviewServerTool({ tmpDir, timeoutMs: 90_000, intervalMs: 300 })
// 探针用入口导入的样式表；内容必须是**能活过压缩的真实声明**（注释会被 vite 删掉，产物字节不变、测不出重建）
const probeFile = join(WEB, "src", "css", "base.css")
const originalProbe = await Bun.file(probeFile).text()
const marker = `\n.hot-reload-probe-${Date.now()} { color: #123456; }\n`

try {
  console.log("=== 启动预览服务（默认 hot）===")
  const started = await tool.execute({ action: "start", port: PORT })
  console.log(started.output)
  check("启动成功并声明已开启前端热重建", started.output.includes("已开启前端热重建"), started.output.slice(0, 200))
  check("启动信息给出访问 URL 与停止指引", /http:\/\/127\.0\.0\.1:3993/.test(started.output) && started.output.includes("action=stop"))

  // ① 页面可用
  const first = await assetsOf(PORT)
  check("页面可访问且含主产物", first.includes("main-"), first.slice(0, 120))
  console.log(`  重建前产物: ${first}`)

  // ② 子进程日志：dev-reload / build:watch 已生效（热重建接线证据）
  const log = join(tmpDir, `gebai-preview-${PORT}.log`)
  let logText = ""
  for (let i = 0; i < 30; i++) {
    logText = await Bun.file(log).text().catch(() => "")
    if (logText.includes("built") || logText.includes("dev-reload")) break
    await sleep(500)
  }
  check("预览服务日志出现构建 watcher 活动（vite build --watch）", /built in|dev-reload/.test(logText), logText.split("\n").slice(-3).join(" | ").slice(0, 200))

  // ③ 改动前端源码 → dist 自动重建（产物名变化）→ 无需重启预览服务
  // 注意：watch 构建期间 clean-dist 会短暂清空 dist（页面此刻无产物引用）——必须等到「非空且不同于原集合」，
  // 否则空集合会被误判为重建成功
  await Bun.write(probeFile, originalProbe + marker)
  let rebuilt = first
  for (let i = 0; i < 90; i++) {
    await sleep(1000)
    const cur = await assetsOf(PORT)
    if (cur !== "" && cur !== first) {
      rebuilt = cur
      break
    }
  }
  check(
    "改前端源码后 dist 自动重建（产物集合变化且非空）",
    rebuilt !== first && rebuilt.includes("main-"),
    `before=${first}\n     after=${rebuilt}`,
  )
  console.log(`  重建后产物: ${rebuilt}`)

  // ④ 无需重启：PID 不变（服务进程未重启）
  const state = await Bun.file(join(tmpDir, "gebai-preview.json")).json() as Array<{ port: number; pid: number; hot?: boolean }>
  const entry = state.find((e) => e.port === PORT)
  check("状态文件记录 hot 标记", entry?.hot === true, JSON.stringify(entry))
  check("重建期间服务进程未重启（PID 不变）", !!entry?.pid)

  // ⑤ 还原探针（触发一次回收重建；源码回到原样）
  await Bun.write(probeFile, originalProbe)
  let restored = ""
  for (let i = 0; i < 90; i++) {
    await sleep(1000)
    const cur = await assetsOf(PORT)
    if (cur !== "" && cur === first) {
      restored = cur
      break
    }
  }
  check("还原源码后产物回到原集合（未留残留改动）", restored === first, `expected=${first}\n     got=${restored}`)

  console.log("\n=== 停止预览服务 ===")
  const stopped = await tool.execute({ action: "stop", port: PORT })
  check("停止成功", stopped.output.includes("已停止"), stopped.output)
  await sleep(1000)
  const alive = await fetch(`http://127.0.0.1:${PORT}/`).then(() => true).catch(() => false)
  check("停止后端口不再响应", alive === false)

  console.log(`\n通过 ${passed} 项断言${failures.length ? `，失败 ${failures.length} 项` : ""}`)
  for (const f of failures) console.log(` - ${f}`)
  process.exit(failures.length ? 1 : 0)
} catch (err) {
  console.error("验证失败:", err)
  await tool.execute({ action: "stop", port: PORT }).catch(() => null)
  await Bun.write(probeFile, originalProbe).catch(() => null)
  process.exit(1)
}
