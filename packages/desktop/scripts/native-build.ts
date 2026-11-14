/**
 * 原生桌面宿主构建：**构建霜天应用 `gebai_desktop`（release），再把它与侧车打包成
 * `dist/gebai-desktop.exe`**。
 *
 * ## 三步的顺序与理由
 *
 * 1. `st build gebai_desktop --profile release` —— 编出宿主（自绘窗口 + 整窗 WebView2）。
 *    宿主源码在 `shuangtian/apps/gebai_desktop/`（与框架其它示例同一位置：它依赖框架的
 *    `ui::WebView` / `app::Application`，属于框架工程的一部分，不是 npm 包）。
 * 2. `scripts/pack-desktop.ts` —— 把 `dist/gebai.exe`（`bun --compile` 产物，由
 *    `server:build` 生成）追加到宿主 exe 尾部，写出 trailer。
 * 3. 产物落在 `dist/gebai-desktop.exe`（与原 Rust 启动器同名，下游脚本不必改）。
 *
 * ## 为何要先检查两个前置产物
 *
 * 缺少任一个时 `st build` / 打包的报错都**离真正原因很远**（“找不到工程清单”、
 * “找不到宿主 exe”）。这里先点名缺什么、该跑哪条命令。
 *
 * ## 环境变量（可选）
 *
 * - `GEBAI_DESKTOP_PROFILE`：构建档（`dev`/`release`，缺省 `release`）。
 * - `GEBAI_DESKTOP_FRAMEWORK`：霜天框架根（缺省仓库下 `shuangtian/`）。
 * - `GEBAI_DESKTOP_SKIP_BUILD=1`：只重新打包、不重建宿主（迭代打包时用）。
 */
import { existsSync, statSync } from "node:fs"
import { join } from "node:path"

const desktopDir = join(import.meta.dirname, "..")
const repoRoot = join(desktopDir, "..", "..")
const framework = process.env.GEBAI_DESKTOP_FRAMEWORK ?? join(repoRoot, "shuangtian")
const profile = process.env.GEBAI_DESKTOP_PROFILE ?? "release"
const st = join(framework, "build", "bin", process.platform === "win32" ? "st.exe" : "st")

if (!existsSync(join(framework, "st.pkg"))) {
  console.error(`[native-build] 找不到霜天框架：${framework}\n可设 GEBAI_DESKTOP_FRAMEWORK 指向框架根（含 st.pkg）`)
  process.exit(1)
}
if (!existsSync(st)) {
  console.error(`[native-build] 找不到霜天工具链：${st}\n先在框架目录跑一次 bootstrap（build\\bin\\st.exe 由它自举）`)
  process.exit(1)
}

/** 起一个子进程并等它结束；非零退出直接退出本脚本（不留半成品）。 */
function run(cmd, args, cwd) {
  const result = Bun.spawnSync({ cmd: [cmd, ...args], cwd, stdout: "inherit", stderr: "inherit" })
  if (!result.success) {
    console.error(`[native-build] 失败：${cmd} ${args.join(" ")}（在 ${cwd}）`)
    process.exit(result.exitCode ?? 1)
  }
}

if (process.env.GEBAI_DESKTOP_SKIP_BUILD !== "1") {
  console.log(`[native-build] 构建宿主 gebai_desktop（${profile}）…`)
  run(st, ["build", "gebai_desktop", `--profile=${profile}`], framework)
}

// 侧车由 server:build 产出；它不在时打包会在追加阶段写成半截文件，所以先拦。
const sidecar = process.env.GEBAI_DESKTOP_SIDECAR ?? join(desktopDir, "dist", "gebai.exe")
if (!existsSync(sidecar)) {
  console.error(
    `[native-build] 找不到侧车 ${sidecar}\n先构建服务端二进制：bun run --cwd packages/desktop server:build`,
  )
  process.exit(1)
}
console.log(`[native-build] 侧车 ${(statSync(sidecar).size / 1048576).toFixed(1)} MB：${sidecar}`)

run(process.execPath, [join(desktopDir, "scripts", "pack-desktop.ts")], desktopDir)
