import { startServer } from "@gebai/server"

/**
 * 桌面端宿主（scaffold）：
 * 同进程启动服务端；桌面 WebView 集成需在具体平台实现（Windows WebView2 /
 * macOS WKWebView / Linux WebKitGTK）。当前以「本地浏览器访问」作为跨平台兜底，
 * 后续可用原生 WebView 替换 openBrowser。
 */
export async function runDesktop(overrides: Parameters<typeof startServer>[0] = {}) {
  const { server, config } = await startServer(overrides)
  const url = `http://${config.host}:${config.port}`
  if (process.env.GEBAI_NO_OPEN !== "1") {
    await openBrowser(url)
  }
  console.log(`[gebai-desktop] Web UI: ${url}`)
  return { server, url }
}

async function openBrowser(url: string) {
  const platform = process.platform
  try {
    if (platform === "win32") {
      const { spawn } = await import("node:child_process")
      spawn("cmd", ["/c", "start", url], { stdio: "ignore", detached: true }).unref()
    } else if (platform === "darwin") {
      const { spawn } = await import("node:child_process")
      spawn("open", [url], { stdio: "ignore" })
    } else {
      const { spawn } = await import("node:child_process")
      spawn("xdg-open", [url], { stdio: "ignore" })
    }
  } catch {
    console.warn("[gebai-desktop] 无法自动打开浏览器，请手动访问", url)
  }
}

if (import.meta.main) {
  runDesktop().catch((err) => {
    console.error(err)
    process.exit(1)
  })
}
