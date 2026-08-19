import { startServer } from "@gebai/server"

/**
 * 桌面端固定端口：不易冲突（避开常见开发端口与 Windows 临时端口段 49152+）。核心目的是保持
 * origin（协议+主机+端口）稳定——浏览器/WebView 的 localStorage 按源隔离，端口每次随机变化
 * 会让环境变量（gebai.ui.env）、主题、快捷键等全部浏览器本地数据跨重启「凭空丢失」。
 * 端口被占（如重复启动）直接报错退出，不回退随机端口——随机 origin 正是数据丢失的根因。
 */
const DESKTOP_PORT = 47896

/**
 * 桌面端宿主（浏览器形态）：同进程启动服务端并自动打开系统默认浏览器；
 * 原生窗口形态见 launcher/（tao/wry 启动器，spawn 本二进制并连接 WebView，
 * 以 GEBAI_NO_OPEN=1 走此入口时不打开浏览器）。
 */
export async function runDesktop(overrides: Parameters<typeof startServer>[0] = {}) {
  // 固定端口（见 DESKTOP_PORT 注释）；显式 GEBAI_PORT 或调用方 overrides.port 优先
  const port = overrides.port ?? (process.env.GEBAI_PORT ? Number(process.env.GEBAI_PORT) : DESKTOP_PORT)
  let started: Awaited<ReturnType<typeof startServer>>
  try {
    started = await startServer({ ...overrides, port })
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    // Bun 的占用报错形如 "Failed to start server. Is port N in use?"，EADDRINUSE 在 code 属性而非 message
    const code = (err as { code?: string }).code
    if (code === "EADDRINUSE" || /EADDRINUSE|in use|Failed to (listen|serve|bind|start)/i.test(msg)) {
      throw new Error(
        `桌面端固定端口 ${port} 被占用（可能已有 GEBAI 实例在运行）。不会自动换端口——端口变化会使浏览器本地数据（环境变量/主题等）丢失。请结束占用进程，或设 GEBAI_PORT 指定其他端口。原始错误: ${msg}`,
      )
    }
    throw err
  }
  const { server } = started
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
