import { startServer } from "@gebai/server"

/**
 * 桌面端宿主（浏览器形态）：同进程启动服务端并自动打开系统默认浏览器；
 * 原生窗口形态见 launcher/（tao/wry 启动器，spawn 本二进制并连接 WebView，
 * 以 GEBAI_NO_OPEN=1 走此入口时不打开浏览器）。
 */
export async function runDesktop(overrides: Parameters<typeof startServer>[0] = {}) {
  // 桌面形态默认 OS 分配空闲端口（port=0），根治与 dev 服务/其他实例的 3000 端口冲突；
  // 显式 GEBAI_PORT 或调用方 overrides.port 优先
  const port = overrides.port ?? (process.env.GEBAI_PORT ? Number(process.env.GEBAI_PORT) : 0)
  const { server } = await startServer({ ...overrides, port })
  const url = `http://${server.hostname}:${server.port}`
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
