/** 进程入口逻辑：`gebai exec <script>` 子命令 + 脚本调试模式的 web dist 自动构建 + 默认启动。
 *  自原单文件 index.ts 的 import.meta.main 段拆分，行为不变。 */
import { existsSync, statSync } from "node:fs"
import { join } from "node:path"
import { loadConfig, isBinaryMode } from "../core/base/config"
import { walkDir } from "../core/base/paths"

/** `gebai exec` 子命令实现：动态 import 目标脚本（模块顶层执行；退出码由脚本自身 process.exit 决定）。 */
async function importExecScript(file: string): Promise<void> {
  const { resolve } = await import("node:path")
  const { pathToFileURL } = await import("node:url")
  try {
    await import(pathToFileURL(resolve(file)).href)
  } catch (err) {
    console.error(err)
    process.exit(1)
  }
}

/**
 * 脚本调试模式下：server 托管 `packages/web/dist` 构建产物，若 web 源码比
 * dist 新（或 dist 缺失），启动前自动执行 web 构建，避免「改了前端代码但
 * 页面仍是旧产物」的坑。二进制模式跳过（产物随二进制分发）。
 */
export async function ensureWebDistBuilt(): Promise<void> {
  const config = loadConfig()
  if (isBinaryMode()) return

  const distIndex = join(config.webDist, "index.html")
  const distMtime = existsSync(distIndex) ? statSync(distIndex).mtimeMs : 0
  if (!distMtime) console.log("[gebai] web dist 缺失，自动构建…")

  // 扫描 web 源码最新修改时间（src/ 递归 + 顶层入口/依赖清单）
  const webRoot = join(config.webDist, "..")
  let newestSrc = 0
  const check = (p: string) => {
    try {
      const m = statSync(p).mtimeMs
      if (m > newestSrc) newestSrc = m
    } catch {
      /* 文件不存在忽略 */
    }
  }
  await walkDir(join(webRoot, "src"), 4, async (p) => check(p))
  check(join(webRoot, "index.html"))
  check(join(webRoot, "package.json"))

  if (newestSrc > distMtime + 1000) {
    console.log("[gebai] 检测到 web 源码更新，自动构建（约 1s）…")
    const r = Bun.spawnSync({
      cmd: [process.execPath, "run", "--cwd", webRoot, "build"],
      stdout: "inherit",
      stderr: "inherit",
    })
    if (!r.success) {
      console.warn("[gebai] web 自动构建失败，继续使用现有 dist；可手动执行: bun run --cwd packages/web build")
    } else {
      console.log("[gebai] web 构建完成")
    }
  }
}

/** 进程主入口（index.ts 的 import.meta.main 调用）：exec 子命令分发或默认启动。
 *  exec 段两种 argv 布局：编译单文件形态 spawn [gebai, "exec", script]（argv[1]=内嵌虚拟入口，exec 在
 *  argv[2]）；容器形态（execPath=bun 跑 dist 产物）spawn 须带真实入口段 [bun, 入口, "exec", script]（exec 在 argv[3]） */
export async function runMain(startServer: () => Promise<unknown>): Promise<void> {
  const arg = process.argv[2] === "exec" ? 3 : process.argv[3] === "exec" ? 4 : 0
  if (arg && process.argv[arg]) {
    await importExecScript(process.argv[arg])
  } else {
    await ensureWebDistBuilt()
    startServer().catch((err) => {
      console.error(err)
      process.exit(1)
    })
  }
}
