/**
 * 相期桌面窗口的拉起决策（方案 B：窗口作为 WS 客户端连**主服务端口**的 `/xiangqi`）。
 *
 * 拆成「纯决策」与「执行」两层：决策部分不碰文件系统之外的东西、不 spawn，
 * 因此三种部署形态（源码 / dist / 自定义 GEBAI_HOME）各自会探测到哪个 exe
 * 全部可以在 `bun test` 里断言——这段逻辑以前写在 compose 里靠日志人肉排，
 * 结果是一处上溯级数写错（少一级）导致**静默失败**了很久。
 *
 * ## 三条形态下 exe 在哪
 *
 * | 形态 | 服务自身位置 | exe 位置 |
 * |---|---|---|
 * | 源码（`bun run dev`） | `<repo>/packages/server/src/boot` | `<repo>/shuangtian/build/<档>/bin/` |
 * | dist（`bun build`） | `<repo>/packages/server/dist` | 同上 |
 * | 二进制 / 桌面（`~/.gebai`） | 任意 | 优先 `{GEBAI_HOME}/shuangtian/build/<档>/bin/`，否则按仓库标记向上找 |
 *
 * 「向上找仓库标记」是这里唯一稳的判据：不数级数（数过一次，数少了），
 * 而是逐级上溯找 `shuangtian/examples/xiangqi/` 这个目录——它在源码树里一定存在，
 * 在发行产物里一定不存在（那时靠 `{GEBAI_HOME}` 下的部署副本或 `XIANGQI_BIN`）。
 */
import { existsSync } from "node:fs"
import { dirname, isAbsolute, join, resolve } from "node:path"
import { resolveDataPath } from "@gebai/sdk/node"
import { XIANGQI_HOME_DIRNAME } from "@gebai/agents"

/** 可执行文件名：Windows 要带 `.exe`（`spawn` 不会替你补后缀）。 */
export const XINGQI_EXE_NAME = process.platform === "win32" ? "xiangqi.exe" : "xiangqi"

/** 构建档探测顺序：dev 主用；quick/release 是发行与快速验证档；debug 兜底。 */
export const XIANGQI_PROFILES = ["dev", "quick", "release", "debug"] as const

/** 仓库标记：源码树里 `shuangtian/examples/xiangqi/` 必然存在（发行产物里没有）。 */
const REPO_MARKER = ["shuangtian", "examples", "xiangqi"] as const

/** 上溯深度上限（防止病态路径下无限循环；正常仓库 6 级足够，留足余量）。 */
const MAX_UP = 16

export interface XiangqiProbeInput {
  /** 部署数据根（`config.gebaiHome`）。 */
  gebaiHome: string
  /** 服务自身所在目录（生产传 `import.meta.dirname`，测试注入假位置）。 */
  moduleDir: string
  /** `XIANGQI_BIN` 覆盖（未设 = undefined/空串）。 */
  override?: string
  /** 探测哪些构建档（按序取第一个存在的）。 */
  profiles?: readonly string[]
  /** 文件存在判定（测试注入；缺省 `existsSync`）。 */
  exists?: (p: string) => boolean
  /** PATH 查找（测试注入；缺省 `Bun.which`）。返回绝对路径或 null。 */
  which?: (name: string) => string | null
}

export interface XiangqiBinResult {
  /** 找到的 exe 绝对路径；`null` = 一个都没找到。 */
  bin: string | null
  /** 仓库根（由标记定位；未找到为 null）。exe 与 UI 目录都从这里推。 */
  repoRoot: string | null
  /** **按序试过的全部候选**——找不到时日志必须把它们列全（只报一个会误导排查方向）。 */
  candidates: string[]
  /** 命中来自 `XIANGQI_BIN` 覆盖（此时不再探测别的）。 */
  fromOverride: boolean
}

/** 逐级上溯找仓库根（以 `shuangtian/examples/xiangqi` 为标记）。 */
function findRepoRoot(start: string, exists: (p: string) => boolean): string | null {
  let cur = resolve(start)
  for (let i = 0; i < MAX_UP; i++) {
    if (exists(join(cur, ...REPO_MARKER))) return cur
    const parent = dirname(cur)
    if (parent === cur) break
    cur = parent
  }
  return null
}

/**
 * 探测相期 exe：`XIANGQI_BIN` 覆盖 > `{GEBAI_HOME}/shuangtian/build/<档>` >
 * 「从服务自身目录上溯找仓库根」>「从 `{GEBAI_HOME}` 上溯找仓库根」>
 * `{GEBAI_HOME}/xiangqi(.exe)` 直查 > `PATH`（Bun.which）。
 *
 * 后两路是后备：使用者把单个 exe 放进 GEBAI_HOME（不随仓库/部署副本）或装进 PATH
 * 时仍能找到——不同使用者的本地部署形态各异，探测面宁宽勿狭。
 *
 * 为什么后两路不合并成一路：`GEBAI_HOME` 可能与仓库同根（源码形态）、可能是仓库下的
 * 子目录（本地实例 `.gebai-local`）、也可能完全在别处（`~/.gebai` 部署）——
 * 从任意一个点**上溯找标记**都能收敛到仓库根，比硬编码相对级数可靠。
 */
export function resolveXiangqiBin(input: XiangqiProbeInput): XiangqiBinResult {
  const exists = input.exists ?? existsSync
  const profiles = input.profiles ?? XIANGQI_PROFILES
  const override = input.override?.trim()
  if (override) {
    const abs = isAbsolute(override) ? override : resolve(override)
    const ok = exists(abs)
    return {
      bin: ok ? abs : null,
      repoRoot: ok ? findRepoRoot(dirname(abs), exists) : null,
      candidates: [abs],
      fromOverride: true,
    }
  }

  const candidates: string[] = []
  const push = (p: string): void => {
    if (!candidates.includes(p)) candidates.push(p)
  }

  // ① 与 GEBAI_HOME 同根/同树的部署副本（桌面形态常见：数据根里放一份 shuangtian）
  for (const profile of profiles) push(join(input.gebaiHome, "shuangtian", "build", profile, "bin", XINGQI_EXE_NAME))

  // ② 从服务自身目录上溯找仓库根（源码/dist 形态主路径）
  const fromModule = findRepoRoot(input.moduleDir, exists)
  // ③ 从 GEBAI_HOME 上溯找仓库根（源码形态下同一结果；`.gebai-local` 这类子目录也能收敛）
  const fromHome = findRepoRoot(input.gebaiHome, exists)
  const roots = [fromModule, fromHome].filter((r): r is string => r !== null)
  for (const root of roots) {
    for (const profile of profiles) push(join(root, "shuangtian", "build", profile, "bin", XINGQI_EXE_NAME))
  }

  // ④ GEBAI_HOME 直查（后备：使用者把单个 exe 放数据根，不随仓库/部署副本）
  push(join(input.gebaiHome, XINGQI_EXE_NAME))

  // ⑤ PATH（后备：装进了系统/用户 PATH 的场景）——which 返回的就是绝对路径
  const which = input.which ?? ((name: string) => Bun.which(name) ?? null)
  const onPath = which(XINGQI_EXE_NAME)
  if (onPath) push(onPath)

  for (const candidate of candidates) {
    if (exists(candidate)) {
      return { bin: candidate, repoRoot: roots[0] ?? findRepoRoot(dirname(candidate), exists), candidates, fromOverride: false }
    }
  }
  return { bin: null, repoRoot: fromModule ?? fromHome, candidates, fromOverride: false }
}

/** 相期 UI 资源目录（`main.xml` + `app.js`）：仓库内的 `shuangtian/examples/xiangqi/ui`。 */
export function resolveXiangqiUiDir(repoRoot: string | null, exists: (p: string) => boolean = existsSync): string | null {
  if (repoRoot === null) return null
  const dir = join(repoRoot, "shuangtian", "examples", "xiangqi", "ui")
  return exists(join(dir, "main.xml")) ? dir : null
}

export interface XiangqiPlanInput extends Omit<XiangqiProbeInput, "moduleDir"> {
  /** 服务**实际监听端口**（`Bun.serve` 返回的 `server.port`；`port=0` 时由 OS 分配）。 */
  port: number
  /** 服务自身所在目录（`import.meta.dirname`）。 */
  moduleDir: string
  /** 环境变量（生产传 `process.env`；测试注入）。 */
  env: Record<string, string | undefined>
}

/**
 * 拉起决策（纯函数，无副作用）：
 * `action: "skip"` 时 `reason` 说明为什么不起（每条都有对应的日志），
 * `action: "launch"` 时 `bin`/`args` 就是 `spawn` 的全部输入。
 */
export type XiangqiLaunchPlan =
  | { action: "skip"; reason: "persona-off" | "not-local" | "auto-off" }
  | { action: "skip"; reason: "bin-not-found"; candidates: string[]; searchedFrom: string[] }
  | {
      action: "launch"
      bin: string
      args: string[]
      url: string
      home: string
      uiDir: string | null
      candidates: string[]
    }

export function planXiangqiLaunch(input: XiangqiPlanInput, exists: (p: string) => boolean = existsSync): XiangqiLaunchPlan {
  const auto = String(input.env.XIANGQI_AUTO ?? "").trim().toLowerCase()
  if (auto === "off") return { action: "skip", reason: "auto-off" }
  const probe = resolveXiangqiBin({ ...input, exists })
  if (probe.bin === null) {
    const searchedFrom = [input.moduleDir, input.gebaiHome]
    return { action: "skip", reason: "bin-not-found", candidates: probe.candidates, searchedFrom }
  }
  const uiDir = resolveXiangqiUiDir(probe.repoRoot, exists)
  const home = xiangqiHomeFor(input.gebaiHome, input.env)
  const args = ["--agent", `ws://127.0.0.1:${input.port}/xiangqi`, "--home", home]
  // `--ui` 显式给绝对路径：exe 的资源探测是相对 cwd 的，而宿主进程的 cwd 与 exe 所在的
  // 仓库树没有任何关系（实测：从仓库根起服务时窗口「装载失败：读不到
  // examples/xiangqi/ui/main.xml」后立刻退出——TCP 都还没连）。
  if (uiDir !== null) args.push("--ui", uiDir)
  return {
    action: "launch",
    bin: probe.bin,
    args,
    url: `ws://127.0.0.1:${input.port}/xiangqi`,
    home,
    uiDir,
    candidates: probe.candidates,
  }
}

/**
 * 相期数据根：`XIANGQI_HOME` → `{GEBAI_HOME}/data/xiangqi` → `<cwd>/data/xiangqi`。
 *
 * **公式只有一份**：目录名常量与 `homeFromEnv` 同源于 agents 侧（相期控制器自己算的就是
 * 读写箱件的位置）。本函数只是把 GEBAI_HOME **显式传进去**的版本（宿主在启动期就需知道
 * 路径，而 `homeFromEnv` 只读 process.env，测试注入不便）。
 *
 * 两边算出不同目录的后果是文件兼底各写各的（推卡降级写一处、答复轮询读另一处），
 * 而 WS 通的时候看不出任何异常——所以这里**不再自己拼字符串**，统一走 `resolveDataPath`。
 */
export { XIANGQI_HOME_DIRNAME }

export function xiangqiHomeFor(gebaiHome: string, env: Record<string, string | undefined>): string {
  const explicit = env.XIANGQI_HOME?.trim()
  if (explicit) return explicit
  const home = (gebaiHome || env.GEBAI_HOME || "").trim()
  return resolveDataPath(home === "" ? process.cwd() : home, XIANGQI_HOME_DIRNAME, XIANGQI_HOME_DIRNAME)
}
