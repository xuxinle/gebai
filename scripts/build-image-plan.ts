/**
 * 镜像构建裁剪计划器：把**裁剪档案**（JSON）与 CLI 覆盖合并成一份构建期环境文件
 * （`GEBAI_BUILD_*` 与 `PLAN_*`），供 Dockerfile 构建阶段 source 后驱动各构建脚本、
 * 前端 vendor 拷贝与运行期系统包安装。
 *
 * 用法（宿主侧 `docker/build.sh --print-plan` 与镜像构建阶段都调用同一份逻辑）：
 *   bun run scripts/build-image-plan.ts --profile docker/profiles/minimal.json --out /tmp/gebai-plan.env
 *   bun run scripts/build-image-plan.ts --profile minimal --set assets.d2=0 --set web.vendor=monaco
 *   bun run scripts/build-image-plan.ts --profile-b64 <base64>     # 档案经 build-arg 传入（Docker 阶段）
 *   bun run scripts/build-image-plan.ts --print-plan               # 只打印计划与报告，不写文件
 *   bun run scripts/build-image-plan.ts --emit-args --profile X     # 只输出 Dockerfile 指令级 build-arg（KEY=VAL 每行）
 *
 * 档案 schema（缺省即全开/默认，`name` 必填；未知键/类型不符直接报错——配错档案却按默认面构建比失败更难排查）：
 *   {
 *     "name": "minimal", "description": "最小 API 服务",
 *     "sub_agents": { "enable": ["code"], "disable": [], "preload": ["code"] },
 *     "tools": { "disable": ["show"] },
 *     "assets": { "web_ui": true, "cv": true, "d2": true, "analyzer": true, "browser": true, "ripgrep": true },
 *     "web": { "vendor": ["monaco", "xterm"] },
 *     "system": { "git": true, "python": true, "bubblewrap": true, "fonts": true, "tzdata": true, "procps": true, "chromium": false },
 *     "image": { "base": "ubuntu:24.04", "user": "gebai", "uid": 1000, "data_dir": "/data", "port": 3000, ... },
 *     "prompt": { ... }    // 运行期（GEBAI_PROFILE）段：构建期忽略，使同一份档案从构建到运行贯穿
 *   }
 *
 * `image` 段（镜像本体定制）分两类落地：
 * - **指令级**（基础镜像 / 运行用户与 uid / 数据目录 / 端口 / 时区 / 标签 / 健康检查）：Dockerfile 的
 *   FROM・USER・VOLUME・EXPOSE・ENV・LABEL・HEALTHCHECK 由 `--emit-args` 输出的 build-arg 驱动（Docker
 *   指令无法在构建过程中条件化，故须在宿主侧由 `docker/build.sh` 先算好传入）。
 * - **构建步骤级**（额外 apt 包 / apt 源 / npm 源 / 代理）：写进计划文件（`PLAN_*`），由构建阶段 source 后使用。
 *
 * 名单中的子Agent 名与工具名在构建脚本侧校验（`build-subagents.ts` / `build-tools.ts` 对未知名
 * 报错退出），本脚本只做档案结构与字段级校验。
 */
import { existsSync, readdirSync, readFileSync, writeFileSync } from "node:fs"
import { isAbsolute, join, resolve } from "node:path"

const ROOT = join(import.meta.dirname, "..")

/** 内嵌资产开关（对应各 build 脚本的 `GEBAI_BUILD_*`）。 */
const ASSET_KEYS = ["web_ui", "cv", "d2", "analyzer", "browser", "ripgrep"] as const
type AssetKey = (typeof ASSET_KEYS)[number]
/** 前端 vendor 组（`packages/web/scripts/build-vendor.ts` 的裁剪单位）。 */
const VENDOR_GROUPS = ["monaco", "plantuml", "mermaid", "echarts", "d2js", "xterm", "tree_sitter"] as const
/** 运行期系统能力组（每组对应一组 apt 包）。 */
const SYSTEM_GROUPS = ["git", "python", "bubblewrap", "fonts", "tzdata", "procps", "chromium"] as const
type SystemKey = (typeof SYSTEM_GROUPS)[number]

/** 组 → apt 包。基础设施包（ca-certificates/curl/tini）固定包含，不提供裁剪：出站 HTTPS、
 *  健康探针与 PID 1 收尸是运行的最低前提，裁掉只会让容器「看起来能起、实际不可用」。 */
const SYSTEM_PACKAGES: Record<Exclude<SystemKey, "chromium">, string[]> = {
  git: ["git"],
  python: ["python3", "python3-venv", "python3-pip"],
  bubblewrap: ["bubblewrap"],
  fonts: ["fonts-noto-cjk", "fontconfig"],
  tzdata: ["tzdata"],
  procps: ["procps"],
}
const BASE_PACKAGES = ["ca-certificates", "curl", "tini"]

export interface BuildPlan {
  name: string
  description?: string
  subAgents: { enable?: string[]; disable?: string[]; preload?: string[] }
  tools: { disable?: string[] }
  assets: Record<AssetKey, boolean>
  vendor: string[]
  system: Record<SystemKey, boolean>
  image: ImageSpec
}

/** 镜像本体定制（`image` 段）：默认值即当前交付口径（Ubuntu 24.04 + 非 root 的 uid 1000 `gebai` + /data）。 */
export interface ImageSpec {
  /** 基础镜像（构建阶段与运行阶段同源；内网/私有仓库可指向自备镜像）。 */
  base: string
  /** 仅取 bun 可执行文件的来源镜像。 */
  bunImage: string
  /** apt 源替换（内网镜像；缺省不动基础镜像自带的源）。 */
  aptMirror?: string
  /** npm 源（bun install 用 `BUN_CONFIG_REGISTRY`；内网镜像）。 */
  npmRegistry?: string
  /** 构建期 HTTP(S) 代理（apt/npm/模型下载；缺省走宿主 docker 预定义代理 build-arg）。 */
  proxy?: string
  /** 容器内运行用户（非 root；`runAsRoot` 时忽略建用户）。 */
  user: string
  uid: number
  gid: number
  /** 运行用户家目录。 */
  home: string
  /** 运行用户登录 shell。 */
  shell: string
  /** 显式以 root 运行：服务模式的沙箱与脚本隔离都以「非特权用户」为前提，root 下部分工具会拒绝执行。 */
  runAsRoot: boolean
  /** 数据根（`GEBAI_HOME` / 挂载卷）。 */
  dataDir: string
  /** `GEBAI_MODE`：服务模式 server（缺省）/ 单用户容器可用 local。 */
  mode: string
  /** `GEBAI_HOST` 监听地址。 */
  host: string
  /** `GEBAI_PORT` 监听端口（同时作为 EXPOSE）。 */
  port: number
  /** 容器时区（写 `TZ`；设置时自动确保 tzdata 被安装）。 */
  tz?: string
  /** 裁剪组与固定基础设施之外**额外**安装的 apt 包。 */
  extraPackages: string[]
  /** 额外镜像标签（键 → 值）。 */
  labels: Record<string, string>
  /** 健康检查：false = 不写入 HEALTHCHECK（改用 `--target runtime-nohealthcheck`）。 */
  healthcheck: { interval: string; timeout: string; startPeriod: string; retries: number; path: string } | false
  /** 浏览器来源：`download` 从下载源拉（可指内网镜像）/ `local` 用预置目录（零网络）/ `off` 不装。 */
  browserSource: "download" | "local" | "off"
  /** `local` 时的预置目录（相对构建上下文；须是 playwright 本地缓存布局，如 `chromium-1243/`）。 */
  browserDir: string
  /** `download` 时的下载源（`PLAYWRIGHT_DOWNLOAD_HOST`；内网自建 CDN 镜像）。 */
  browserDownloadHost?: string
  /** 浏览器系统依赖：`auto`（在线走 `--with-deps`）/ 路径（按预置清单装，配合内网 apt 源）/ `off`（跳过）。 */
  browserDeps: string
  /** bun 供给：`image`（从 `bun_image` 取）/ `local`（预置文件，零网络）。 */
  bunSource: "image" | "local"
  /** bun 预置目录（相对构建上下文；约定放可执行的 `bun`）。 */
  bunDir: string
  /** node 供给：`auto`（跟随浏览器需求：启用则 image，否则 off）/ `image` / `local` / `apt` / `off`。
   *  浏览器桥接是 `Bun.spawn(["node", driver])`，没有 node 就没有浏览器能力。 */
  nodeSource: "auto" | "image" | "local" | "apt" | "off"
  /** `image` 通道的 node 来源镜像。 */
  nodeImage: string
  /** node 预置目录（相对构建上下文；约定放可执行的 `node`）。 */
  nodeDir: string
}

function fail(msg: string): never {
  throw new Error(msg)
}

function strList(v: unknown, where: string): string[] | undefined {
  if (v === undefined) return undefined
  if (!Array.isArray(v) || v.some((x) => typeof x !== "string")) fail(`${where} 必须是字符串数组`)
  return (v as string[]).map((s) => s.trim()).filter(Boolean)
}

function boolValue(v: unknown, where: string): boolean {
  if (typeof v !== "boolean") fail(`${where} 必须是布尔值（true/false）`)
  return v
}

function intValue(v: unknown, where: string, min: number, max: number): number {
  if (typeof v !== "number" || !Number.isInteger(v)) fail(`${where} 必须是整数`)
  if (v < min || v > max) fail(`${where} 须在 ${min}~${max} 之间（收到 ${v}）`)
  return v
}

function strValue(v: unknown, where: string): string {
  if (typeof v !== "string" || !v.trim()) fail(`${where} 必须是非空字符串`)
  return v.trim()
}

function obj(v: unknown, where: string): Record<string, unknown> | undefined {
  if (v === undefined) return undefined
  if (!v || typeof v !== "object" || Array.isArray(v)) fail(`${where} 必须是对象`)
  return v as Record<string, unknown>
}

function checkKeys(o: Record<string, unknown>, allowed: readonly string[], where: string): void {
  for (const k of Object.keys(o)) {
    if (!allowed.includes(k)) fail(`${where} 含未知字段 "${k}"（可用: ${allowed.join(" / ")}）`)
  }
}

/** 默认镜像口径（与交付镜像一致：Ubuntu 24.04、非 root 的 uid/gid 1000 `gebai`、数据根 /data、端口 3000）。 */
function defaultImage(): ImageSpec {
  return {
    base: "ubuntu:24.04",
    bunImage: "oven/bun:1.4.2",
    user: "gebai",
    uid: 1000,
    gid: 1000,
    home: "/home/gebai",
    shell: "/bin/bash",
    runAsRoot: false,
    dataDir: "/data",
    mode: "server",
    host: "0.0.0.0",
    port: 3000,
    extraPackages: [],
    labels: {},
    healthcheck: { interval: "30s", timeout: "5s", startPeriod: "20s", retries: 3, path: "/api/health" },
    browserSource: "download",
    browserDir: "docker/browsers",
    browserDeps: "auto",
    bunSource: "image",
    bunDir: "docker/bun",
    nodeSource: "auto",
    nodeImage: "node:22-slim",
    nodeDir: "docker/node",
  }
}

const IMAGE_KEYS = [
  "base",
  "bun_image",
  "apt_mirror",
  "npm_registry",
  "proxy",
  "user",
  "uid",
  "gid",
  "home",
  "shell",
  "run_as_root",
  "data_dir",
  "mode",
  "host",
  "port",
  "tz",
  "extra_packages",
  "labels",
  "healthcheck",
  "browser_source",
  "browser_dir",
  "browser_download_host",
  "browser_deps",
  "bun_source",
  "bun_dir",
  "node_source",
  "node_image",
  "node_dir",
] as const
const HEALTHCHECK_KEYS = ["interval", "timeout", "start_period", "retries", "path"] as const
/** 浏览器来源（`image.browser_source`）。 */
const BROWSER_SOURCES = ["download", "local", "off"] as const
/** 运行时来源（bun：`image`/`local`；node 多 `auto`/`apt`/`off`）。 */
const BUN_SOURCES = ["image", "local"] as const
const NODE_SOURCES = ["auto", "image", "local", "apt", "off"] as const
/** Docker 标签名允许的字符（过宽的键会在 Dockerfile 里生成非法 LABEL）。 */
const LABEL_KEY_RE = /^[a-zA-Z0-9][a-zA-Z0-9._-]*$/
/** 时长值（Docker HEALTHCHECK 接受的时长格式，如 30s / 1m30s）。 */
const DURATION_RE = /^\d+(ns|us|ms|s|m|h)(\d+(ns|us|ms|s|m|h))*$/

/** 解析 `image` 段（未知字段 / 类型不符 / 非法值一律报错）。 */
function parseImage(v: unknown, label: string): ImageSpec {
  const img = defaultImage()
  const raw = obj(v, `${label} 档案 image`)
  if (!raw) return img
  checkKeys(raw, IMAGE_KEYS, `${label} 档案 image`)
  const W = (k: string): string => `${label} 档案 image.${k}`
  if (raw.base !== undefined) img.base = strValue(raw.base, W("base"))
  if (raw.bun_image !== undefined) img.bunImage = strValue(raw.bun_image, W("bun_image"))
  if (raw.apt_mirror !== undefined) img.aptMirror = strValue(raw.apt_mirror, W("apt_mirror"))
  if (raw.npm_registry !== undefined) img.npmRegistry = strValue(raw.npm_registry, W("npm_registry"))
  if (raw.proxy !== undefined) img.proxy = strValue(raw.proxy, W("proxy"))
  if (raw.user !== undefined) {
    img.user = strValue(raw.user, W("user"))
    if (!/^[a-z_][a-z0-9_-]*$/.test(img.user)) fail(`${W("user")} 须匹配 [a-z_][a-z0-9_-]*（收到 "${img.user}"）`)
    // 家目录未显式指定时跟随用户名：改名而不改家目录会得到不一致的镜像
    if (raw.home === undefined) img.home = `/home/${img.user}`
  }
  if (raw.uid !== undefined) img.uid = intValue(raw.uid, W("uid"), 1, 4294967294)
  if (raw.gid !== undefined) img.gid = intValue(raw.gid, W("gid"), 1, 4294967294)
  if (raw.home !== undefined) img.home = strValue(raw.home, W("home"))
  if (raw.shell !== undefined) img.shell = strValue(raw.shell, W("shell"))
  if (raw.run_as_root !== undefined) img.runAsRoot = boolValue(raw.run_as_root, W("run_as_root"))
  if (raw.data_dir !== undefined) {
    img.dataDir = strValue(raw.data_dir, W("data_dir"))
    if (!img.dataDir.startsWith("/")) fail(`${W("data_dir")} 必须是绝对路径（收到 "${img.dataDir}"）`)
  }
  if (raw.mode !== undefined) img.mode = strValue(raw.mode, W("mode"))
  if (raw.host !== undefined) img.host = strValue(raw.host, W("host"))
  if (raw.port !== undefined) img.port = intValue(raw.port, W("port"), 1, 65535)
  if (raw.tz !== undefined) img.tz = strValue(raw.tz, W("tz"))
  if (raw.extra_packages !== undefined) {
    const pkgs = strList(raw.extra_packages, W("extra_packages"))
    for (const p of pkgs ?? []) {
      // 包名直接进 apt 命令行（`apt-get install $PLAN_SYSTEM_PACKAGES`）：含空白/元字符会改变命令含义
      if (!/^[a-z0-9][a-z0-9+.-]*(:[a-z0-9-]+)?$/.test(p)) fail(`${W("extra_packages")} 含非法包名 "${p}"`)
    }
    img.extraPackages = pkgs ?? []
  }
  const labels = obj(raw.labels, W("labels"))
  if (labels) {
    for (const [k, val] of Object.entries(labels)) {
      if (!LABEL_KEY_RE.test(k)) fail(`${W("labels")} 含非法标签名 "${k}"（允许字母数字开头，后可含字母数字与 . _ -）`)
      img.labels[k] = strValue(val, `${W("labels")}.${k}`)
    }
  }
  if (raw.healthcheck !== undefined) {
    if (raw.healthcheck === false) {
      // 禁用健康检查：Docker 指令无法在构建中条件化，改由 `--target runtime-nohealthcheck` 选择无 HEALTHCHECK 阶段
      img.healthcheck = false
    } else if (raw.healthcheck === true) {
      // 保持默认参数
    } else {
      const hc = obj(raw.healthcheck, W("healthcheck"))!
      checkKeys(hc, HEALTHCHECK_KEYS, W("healthcheck"))
      const base = img.healthcheck as Exclude<ImageSpec["healthcheck"], false>
      const dur = (key: "interval" | "timeout" | "start_period", cur: string): string => {
        if (hc[key] === undefined) return cur
        const s = strValue(hc[key], `${W("healthcheck")}.${key}`)
        if (!DURATION_RE.test(s)) fail(`${W("healthcheck")}.${key} 须为时长（如 30s / 1m30s），收到 "${s}"`)
        return s
      }
      img.healthcheck = {
        interval: dur("interval", base.interval),
        timeout: dur("timeout", base.timeout),
        startPeriod: dur("start_period", base.startPeriod),
        retries: hc.retries === undefined ? base.retries : intValue(hc.retries, `${W("healthcheck")}.retries`, 1, 100),
        path: hc.path === undefined ? base.path : strValue(hc.path, `${W("healthcheck")}.path`),
      }
      // 间隔/超时/启动期/重试写在 Dockerfile 的 HEALTHCHECK 指令里，而 Dockerfile 解析阶段不做变量
      // 展开（实测报 "invalid duration ${...}"）——这几个值构建期注入不了，不静默忽略而是明确告知
      if (hc.interval !== undefined || hc.timeout !== undefined || hc.start_period !== undefined || hc.retries !== undefined) {
        console.warn(
          `[build-image-plan] ${label}: healthcheck 的 interval/timeout/start_period/retries 无法由构建参数注入` +
            "（Dockerfile 指令在解析阶段固定，实测不接受变量展开）——镜像将使用 30s/5s/20s/3；" +
            "需要调整请在运行期覆盖（compose 的 healthcheck 段或 docker run --health-interval/--health-timeout/--health-retries/--health-start-period）。" +
            "path 与端口仍按本档案生效。",
        )
      }
    }
  }
  // 数据根与挂载卷不能是 / 或系统目录：那会把 chown -R 打到整个文件系统上
  if (img.dataDir === "/") fail(`${W("data_dir")} 不能是根目录`)
  // 浏览器：来源 / 预置目录 / 下载源 / 系统依赖
  if (raw.browser_source !== undefined) {
    const src = strValue(raw.browser_source, W("browser_source"))
    if (!(BROWSER_SOURCES as readonly string[]).includes(src)) {
      fail(`${W("browser_source")} 须为 ${BROWSER_SOURCES.join(" / ")}（收到 "${src}"）`)
    }
    img.browserSource = src as ImageSpec["browserSource"]
  }
  if (raw.browser_dir !== undefined) img.browserDir = strValue(raw.browser_dir, W("browser_dir"))
  if (img.browserDir.startsWith("/") && img.browserSource === "local") {
    // local 预置目录须在构建上下文内（Docker 只能 COPY 上下文里的文件），绝对路径传不进去
    fail(`${W("browser_dir")} 须为构建上下文内的相对路径（Docker 只能拷贝上下文内的文件）`)
  }  if (raw.browser_download_host !== undefined) img.browserDownloadHost = strValue(raw.browser_download_host, W("browser_download_host"))
  if (raw.browser_deps !== undefined) {
    img.browserDeps = raw.browser_deps === false ? "off" : raw.browser_deps === true ? "auto" : strValue(raw.browser_deps, W("browser_deps"))
  }
  // 运行时供给：bun（构建必需）与 node（浏览器桥接必需）
  if (raw.bun_source !== undefined) {
    const src = strValue(raw.bun_source, W("bun_source"))
    if (!(BUN_SOURCES as readonly string[]).includes(src)) fail(`${W("bun_source")} 须为 ${BUN_SOURCES.join(" / ")}（收到 "${src}"）`)
    img.bunSource = src as ImageSpec["bunSource"]
  }
  if (raw.bun_dir !== undefined) img.bunDir = relPath(strValue(raw.bun_dir, W("bun_dir")), W("bun_dir"))
  if (raw.node_source !== undefined) {
    const src = strValue(raw.node_source, W("node_source"))
    if (!(NODE_SOURCES as readonly string[]).includes(src)) fail(`${W("node_source")} 须为 ${NODE_SOURCES.join(" / ")}（收到 "${src}"）`)
    img.nodeSource = src as ImageSpec["nodeSource"]
  }
  if (raw.node_image !== undefined) img.nodeImage = strValue(raw.node_image, W("node_image"))
  if (raw.node_dir !== undefined) img.nodeDir = relPath(strValue(raw.node_dir, W("node_dir")), W("node_dir"))
  return img
}

/** 预置目录须在构建上下文内（Docker 只能 COPY 上下文里的文件）。 */
function relPath(v: string, where: string): string {
  if (v.startsWith("/")) fail(`${where} 须为构建上下文内的相对路径（Docker 只能拷贝上下文内的文件）`)
  return v
}
function defaultPlan(): BuildPlan {
  return {
    name: "full",
    subAgents: {},
    tools: {},
    assets: Object.fromEntries(ASSET_KEYS.map((k) => [k, true])) as Record<AssetKey, boolean>,
    vendor: [...VENDOR_GROUPS],
    system: Object.fromEntries(SYSTEM_GROUPS.map((k) => [k, k === "chromium" ? false : true])) as Record<SystemKey, boolean>,
    image: defaultImage(),
  }
}

/** 档案解析与结构校验（未知键/类型不符直接报错）。 */
export function parseProfile(raw: unknown, label: string): BuildPlan {
  const root = obj(raw, `${label} 档案`)!
  checkKeys(root, ["name", "description", "sub_agents", "tools", "assets", "web", "system", "image", "prompt"], `${label} 档案`)
  const name = root.name
  if (typeof name !== "string" || !name.trim()) fail(`${label} 档案缺少 name（字符串）`)
  const plan = defaultPlan()
  plan.name = (name as string).trim()
  if (root.description !== undefined) {
    if (typeof root.description !== "string") fail(`${label} 档案 description 必须是字符串`)
    plan.description = root.description.trim() || undefined
  }

  const subs = obj(root.sub_agents, `${label} 档案 sub_agents`)
  if (subs) {
    checkKeys(subs, ["enable", "disable", "preload"], `${label} 档案 sub_agents`)
    plan.subAgents = {
      enable: strList(subs.enable, `${label} 档案 sub_agents.enable`),
      disable: strList(subs.disable, `${label} 档案 sub_agents.disable`),
      preload: strList(subs.preload, `${label} 档案 sub_agents.preload`),
    }
    if (plan.subAgents.enable?.length && plan.subAgents.disable?.length) {
      fail(`${label} 档案 sub_agents.enable 与 disable 互斥`)
    }
  }

  const tools = obj(root.tools, `${label} 档案 tools`)
  if (tools) {
    // tools.enable 属运行期（GEBAI_PROFILE）语义：构建期没有工具白名单变量，明确提示而非静默忽略
    checkKeys(tools, ["enable", "disable"], `${label} 档案 tools`)
    if (tools.enable !== undefined) {
      console.warn(`[build-image-plan] ${label}: tools.enable 仅运行期生效（构建期未提供工具白名单），已忽略；如需构建期裁剪请用 tools.disable`)
    }
    plan.tools = { disable: strList(tools.disable, `${label} 档案 tools.disable`) }
  }

  const assets = obj(root.assets, `${label} 档案 assets`)
  if (assets) {
    checkKeys(assets, ASSET_KEYS, `${label} 档案 assets`)
    for (const k of ASSET_KEYS) {
      if (assets[k] !== undefined) plan.assets[k] = boolValue(assets[k], `${label} 档案 assets.${k}`)
    }
  }

  const web = obj(root.web, `${label} 档案 web`)
  if (web) {
    checkKeys(web, ["vendor"], `${label} 档案 web`)
    const vendor = strList(web.vendor, `${label} 档案 web.vendor`)
    if (vendor) {
      const unknown = vendor.filter((g) => !(VENDOR_GROUPS as readonly string[]).includes(g))
      if (unknown.length) fail(`${label} 档案 web.vendor 含未知组: ${unknown.join(", ")}（可用: ${VENDOR_GROUPS.join(", ")}）`)
      plan.vendor = vendor
    }
  }

  const system = obj(root.system, `${label} 档案 system`)
  if (system) {
    checkKeys(system, SYSTEM_GROUPS, `${label} 档案 system`)
    for (const k of SYSTEM_GROUPS) {
      if (system[k] !== undefined) plan.system[k] = boolValue(system[k], `${label} 档案 system.${k}`)
    }
  }
  plan.image = parseImage(root.image, label)
  // 时区联动：设了 image.tz 却把 tzdata 裁掉时，TZ 设了也不生效（“看着配了、实际没用”）——
  // 直接补上并在报告里说明，不静默失效也无需用户再跑一次
  if (plan.image.tz && !plan.system.tzdata) {
    plan.system.tzdata = true
    console.warn(`[build-image-plan] ${label}: 设了 image.tz="${plan.image.tz}" 但 system.tzdata=false —— 时区生效需要 tzdata，已自动启用该项`)
  }
  return plan
}

/**
 * 供给收尾：在**档案 + CLI 覆盖都合并完之后**调用，把依赖其他字段的供给决策定下来。
 * 必须在覆盖之后——否则 `--set system.chromium=1` 这类覆盖看不到效果（auto 会按旧值定型）。
 * 幂等：已定型的值再跑一次不变。
 */
export function finalizeProvisioning(plan: BuildPlan): void {
  // node 供给 auto：跟随浏览器需求——浏览器桥接是 `Bun.spawn(["node", driver])`，没有 node 就没有
  // 浏览器能力，所以「装了浏览器却没装 node」是必须报出来的矛盾
  if (plan.image.nodeSource === "auto") plan.image.nodeSource = plan.system.chromium ? "image" : "off"
  // node=apt 时把 nodejs 并入装包清单（apt 通道无需额外的阶段逻辑）
  if (plan.image.nodeSource === "apt" && !plan.image.extraPackages.includes("nodejs")) {
    plan.image.extraPackages = [...plan.image.extraPackages, "nodejs"]
  }
}

/** CLI 覆盖（`--set 点路径=值`）：字段级覆盖档案，未覆盖处保持档案值。 */
export function applyOverride(plan: BuildPlan, set: string): void {
  const eq = set.indexOf("=")
  if (eq <= 0) fail(`--set 需要 键=值 形式（收到 "${set}"）`)
  const key = set.slice(0, eq).trim()
  const value = set.slice(eq + 1).trim()
  const list = value.split(",").map((s) => s.trim()).filter(Boolean)
  const bool = (v: string, where: string): boolean => {
    const t = v.toLowerCase()
    if (["1", "true", "on", "yes"].includes(t)) return true
    if (["0", "false", "off", "no"].includes(t)) return false
    return fail(`${where} 需要布尔值（1/0、true/false、on/off、yes/no），收到 "${v}"`)
  }
  switch (key) {
    case "sub_agents.enable":
      plan.subAgents.enable = list
      plan.subAgents.disable = undefined
      return
    case "sub_agents.disable":
      plan.subAgents.disable = list
      plan.subAgents.enable = undefined
      return
    case "sub_agents.preload":
      plan.subAgents.preload = list
      return
    case "tools.disable":
      plan.tools.disable = list
      return
    case "web.vendor": {
      const unknown = list.filter((g) => !(VENDOR_GROUPS as readonly string[]).includes(g))
      if (unknown.length) fail(`--set web.vendor 含未知组: ${unknown.join(", ")}（可用: ${VENDOR_GROUPS.join(", ")}）`)
      plan.vendor = list
      return
    }
    case "description":
      plan.description = value || undefined
      return
    default:
      break
  }
  const assetMatch = /^assets\.(\w+)$/.exec(key)
  if (assetMatch) {
    const k = assetMatch[1]!
    if (!(ASSET_KEYS as readonly string[]).includes(k)) fail(`--set 未知资产 "${k}"（可用: ${ASSET_KEYS.join(", ")}）`)
    plan.assets[k as AssetKey] = bool(value, `--set ${key}`)
    return
  }
  const sysMatch = /^system\.(\w+)$/.exec(key)
  if (sysMatch) {
    const k = sysMatch[1]!
    if (!(SYSTEM_GROUPS as readonly string[]).includes(k)) fail(`--set 未知系统组 "${k}"（可用: ${SYSTEM_GROUPS.join(", ")}）`)
    plan.system[k as SystemKey] = bool(value, `--set ${key}`)
    return
  }
  const hcMatch = /^image\.healthcheck\.(\w+)$/.exec(key)
  if (hcMatch) {
    const k = hcMatch[1]!
    if (!(HEALTHCHECK_KEYS as readonly string[]).includes(k)) fail(`--set 未知健康检查字段 "${k}"（可用: ${HEALTHCHECK_KEYS.join(", ")}）`)
    const cur = plan.image.healthcheck === false ? (defaultImage().healthcheck as Exclude<ImageSpec["healthcheck"], false>) : plan.image.healthcheck
    const where = `--set ${key}`
    if (k === "retries") {
      if (!/^\d+$/.test(value)) fail(`${where} 需要整数`)
      plan.image.healthcheck = { ...cur, retries: Number(value) }
      return
    }
    if (k === "path") {
      plan.image.healthcheck = { ...cur, path: value || fail(`${where} 需要非空值`) }
      return
    }
    // interval / timeout / start_period
    const field = k === "start_period" ? "startPeriod" : (k as "interval" | "timeout")
    plan.image.healthcheck = { ...cur, [field]: value || fail(`${where} 需要非空值`) }
    return
  }
  const imgMatch = /^image\.(\w+)$/.exec(key)
  if (imgMatch) {
    const k = imgMatch[1]!
    if (!(IMAGE_KEYS as readonly string[]).includes(k)) fail(`--set 未知镜像字段 "${k}"（可用: ${IMAGE_KEYS.join(", ")}）`)
    applyImageOverride(plan, k, value, list, bool)
    return
  }
  fail(`--set 未知键 "${key}"（可用: sub_agents.enable / sub_agents.disable / sub_agents.preload / tools.disable / assets.<…> / web.vendor / system.<…> / image.<…> / image.healthcheck.<…> / description）`)
}

/** `image.*` 的 CLI 覆盖：与档案同一条校验路径（非法值同样报错，不允许绕过档案校验）。 */
function applyImageOverride(plan: BuildPlan, key: string, value: string, list: string[], bool: (v: string, where: string) => boolean): void {
  const W = `--set image.${key}`
  const num = (min: number, max: number): number => {
    if (!/^\d+$/.test(value)) fail(`${W} 需要整数（收到 "${value}"）`)
    const n = Number(value)
    if (n < min || n > max) fail(`${W} 须在 ${min}~${max} 之间（收到 ${n}）`)
    return n
  }
  switch (key) {
    case "base":
      plan.image.base = value || fail(`${W} 需要非空值`)
      return
    case "bun_image":
      plan.image.bunImage = value || fail(`${W} 需要非空值`)
      return
    case "apt_mirror":
      plan.image.aptMirror = value || undefined
      return
    case "npm_registry":
      plan.image.npmRegistry = value || undefined
      return
    case "proxy":
      plan.image.proxy = value || undefined
      return
    case "tz":
      plan.image.tz = value || undefined
      return
    case "user": {
      const next = value || fail(`${W} 需要非空值`)
      // 家目录跟随：仍为默认（/home/<旧名>）时随之更新，已显式指定过则保持不动
      const follows = plan.image.home === "/home/gebai" || plan.image.home === `/home/${plan.image.user}`
      plan.image.user = next
      if (follows) plan.image.home = `/home/${next}`
      return
    }
    case "home":
      plan.image.home = value || fail(`${W} 需要非空值`)
      return
    case "shell":
      plan.image.shell = value || fail(`${W} 需要非空值`)
      return
    case "data_dir":
      plan.image.dataDir = value || fail(`${W} 需要非空值`)
      return
    case "mode":
      plan.image.mode = value || fail(`${W} 需要非空值`)
      return
    case "host":
      plan.image.host = value || fail(`${W} 需要非空值`)
      return
    case "uid":
      plan.image.uid = num(1, 4294967294)
      return
    case "gid":
      plan.image.gid = num(1, 4294967294)
      return
    case "port":
      plan.image.port = num(1, 65535)
      return
    case "run_as_root":
      plan.image.runAsRoot = bool(value, W)
      return
    case "extra_packages":
      plan.image.extraPackages = list
      return
    case "labels": {
      const labels: Record<string, string> = {}
      for (const item of list) {
        const eq = item.indexOf("=")
        if (eq <= 0) fail(`${W} 需 "键=值" 逗号分隔（收到 "${item}"）`)
        labels[item.slice(0, eq).trim()] = item.slice(eq + 1).trim()
      }
      plan.image.labels = labels
      return
    }
    case "healthcheck":
      plan.image.healthcheck = bool(value, W) ? plan.image.healthcheck === false ? defaultImage().healthcheck : plan.image.healthcheck : false
      return
    case "browser_source": {
      if (!(BROWSER_SOURCES as readonly string[]).includes(value)) fail(`${W} 须为 ${BROWSER_SOURCES.join(" / ")}（收到 "${value}"）`)
      plan.image.browserSource = value as ImageSpec["browserSource"]
      return
    }
    case "browser_dir":
      plan.image.browserDir = value || fail(`${W} 需要非空值`)
      return
    case "browser_download_host":
      plan.image.browserDownloadHost = value || undefined
      return
    case "browser_deps":
      plan.image.browserDeps = value === "" ? "off" : value
      return
    case "bun_source": {
      if (!(BUN_SOURCES as readonly string[]).includes(value)) fail(`${W} 须为 ${BUN_SOURCES.join(" / ")}（收到 "${value}"）`)
      plan.image.bunSource = value as ImageSpec["bunSource"]
      return
    }
    case "bun_dir":
      plan.image.bunDir = value || fail(`${W} 需要非空值`)
      return
    case "node_source": {
      if (!(NODE_SOURCES as readonly string[]).includes(value)) fail(`${W} 须为 ${NODE_SOURCES.join(" / ")}（收到 "${value}"）`)
      plan.image.nodeSource = value as ImageSpec["nodeSource"]
      return
    }
    case "node_image":
      plan.image.nodeImage = value || fail(`${W} 需要非空值`)
      return
    case "node_dir":
      plan.image.nodeDir = value || fail(`${W} 需要非空值`)
      return
    default:
      return fail(`${W} 不支持（可用: base / bun_image / apt_mirror / npm_registry / proxy / user / uid / gid / home / shell / run_as_root / data_dir / mode / host / port / tz / extra_packages / labels / healthcheck / browser_source / browser_dir / browser_download_host / browser_deps / bun_source / bun_dir / node_source / node_image / node_dir）`)
  }
}

/** 计划 → 构建期环境文件内容（Dockerfile `source` 后驱动构建脚本与系统包安装）。 */
export function planToEnv(plan: BuildPlan, source: string): string {
  const flag = (b: boolean): string => (b ? "1" : "0")
  // 运行期包 = 固定基础设施 + 未裁剪的系统组 + 档案声明的额外包（额外包排在最后，便于日志辨认）
  const packages = [
    ...BASE_PACKAGES,
    ...SYSTEM_GROUPS.filter((g) => g !== "chromium" && plan.system[g]).flatMap((g) => SYSTEM_PACKAGES[g as Exclude<SystemKey, "chromium">]),
    ...plan.image.extraPackages,
  ]
  const lines = [
    "# 由 scripts/build-image-plan.ts 生成：镜像构建裁剪计划（勿手改）",
    `# 档案: ${plan.name}${plan.description ? `（${plan.description}）` : ""}${source ? `  来源: ${source}` : ""}`,
    `GEBAI_BUILD_WEB_UI=${flag(plan.assets.web_ui)}`,
    `GEBAI_BUILD_CV=${flag(plan.assets.cv)}`,
    `GEBAI_BUILD_D2=${flag(plan.assets.d2)}`,
    `GEBAI_BUILD_ANALYZER=${flag(plan.assets.analyzer)}`,
    `GEBAI_BUILD_BROWSER=${flag(plan.assets.browser)}`,
    `GEBAI_BUILD_RG=${flag(plan.assets.ripgrep)}`,
    `GEBAI_BUILD_SUBAGENTS=${(plan.subAgents.enable ?? []).join(",")}`,
    `GEBAI_BUILD_EXCLUDE_SUBAGENTS=${(plan.subAgents.disable ?? []).join(",")}`,
    `GEBAI_BUILD_PRELOAD=${(plan.subAgents.preload ?? []).join(",")}`,
    `GEBAI_BUILD_EXCLUDE_TOOLS=${(plan.tools.disable ?? []).join(",")}`,
    `GEBAI_WEB_VENDOR=${plan.vendor.join(",")}`,
    `PLAN_SYSTEM_PACKAGES="${packages.join(" ")}"`,
    `PLAN_WITH_BROWSER=${flag(plan.system.chromium)}`,
    `PLAN_IMAGE_DESCRIPTION=${JSON.stringify(`${plan.name}${plan.description ? " — " + plan.description : ""}`)}`,
    // 镜像本体（构建步骤级使用；指令级由 --emit-args 的 build-arg 驱动）
    `PLAN_DATA_DIR=${shellQuote(plan.image.dataDir)}`,
    `PLAN_USER=${shellQuote(plan.image.user)}`,
    `PLAN_UID=${plan.image.uid}`,
    `PLAN_GID=${plan.image.gid}`,
    `PLAN_USER_HOME=${shellQuote(plan.image.home)}`,
    `PLAN_USER_SHELL=${shellQuote(plan.image.shell)}`,
    `PLAN_RUN_AS_ROOT=${flag(plan.image.runAsRoot)}`,
    `PLAN_APT_MIRROR=${shellQuote(plan.image.aptMirror ?? "")}`,
    `PLAN_NPM_REGISTRY=${shellQuote(plan.image.npmRegistry ?? "")}`,
    `PLAN_IMAGE_BASE=${shellQuote(plan.image.base)}`,
    // 浏览器供给（构建阶段使用）：来源 / 预置目录 / 下载源 / 系统依赖策略
    `PLAN_BROWSER_SOURCE=${plan.image.browserSource}`,
    `PLAN_PLAYWRIGHT_REVISION=${resolvePlaywrightRevision()}`,
    `PLAN_BROWSER_DIR=${shellQuote(plan.image.browserDir)}`,
    `PLAN_BROWSER_DOWNLOAD_HOST=${shellQuote(plan.image.browserDownloadHost ?? "")}`,
    `PLAN_BROWSER_DEPS=${shellQuote(plan.image.browserDeps)}`,
    `PLAN_RUNTIME_BUN_SOURCE=${plan.image.bunSource}`,
    `PLAN_RUNTIME_BUN_DIR=${shellQuote(plan.image.bunDir)}`,
    `PLAN_RUNTIME_NODE_SOURCE=${plan.image.nodeSource}`,
    `PLAN_RUNTIME_NODE_DIR=${shellQuote(plan.image.nodeDir)}`,
    "",
  ]
  return lines.join("\n")
}

/** shell 单引号包裹（计划文件被 `source` 执行，值中可能含空格与引号）。 */
function shellQuote(v: string): string {
  return `'${v.replace(/'/g, `'\\''`)}'`
}

/** 仓库版本（写进镜像的 org.opencontainers.image.version；package.json 缺失/无版本时留空）。 */
function projectVersion(): string {
  try {
    const pkg = JSON.parse(readFileSync(join(ROOT, "package.json"), "utf8")) as { version?: unknown }
    return typeof pkg.version === "string" ? pkg.version : ""
  } catch {
    return ""
  }
}

/**
 * 期望的 chromium revision：从仓库实际安装的 playwright-core/browsers.json 读——与运行期所用模块
 * 同一份真相，不另立会漂移的版本参数（曾经硬编码的 playwright 版本已与实际依赖不符）。
 *
 * 定位兼顾两种依赖布局：hoisted（`node_modules/playwright-core/`）与 bun 的 store 布局
 * （`node_modules/.bun/playwright-core@<版本>/node_modules/playwright-core/`）。找不到返回空串。
 */
export function resolvePlaywrightRevision(root: string = ROOT): string {
  const candidates = [
    join(root, "node_modules", "playwright-core", "browsers.json"),
    join(root, "packages", "server", "node_modules", "playwright-core", "browsers.json"),
  ]
  try {
    const store = join(root, "node_modules", ".bun")
    for (const e of readdirSync(store)) {
      if (e.startsWith("playwright-core@")) candidates.push(join(store, e, "node_modules", "playwright-core", "browsers.json"))
    }
  } catch {
    /* 无 .bun（hoisted 布局） */
  }
  for (const p of candidates) {
    if (!existsSync(p)) continue
    try {
      const b = JSON.parse(readFileSync(p, "utf8")) as { browsers?: Array<{ name?: string; revision?: unknown }> }
      const entry = b.browsers?.find((x) => x.name === "chromium")
      if (entry?.revision != null) return String(entry.revision)
    } catch {
      /* 该候选不可解析，试下一个 */
    }
  }
  return ""
}

/**
 * 指令级 build-arg（供 `docker/build.sh --emit-args` 读取后传给 `docker build`）。
 * Docker 的 FROM/USER/VOLUME/EXPOSE/ENV/LABEL/HEALTHCHECK 无法在构建过程中条件化，所以这几项的值
 * 必须在宿主侧算好。
 */
export function emitBuildArgs(plan: BuildPlan): string[] {
  const hc = plan.image.healthcheck
  return [
    `IMAGE_VERSION=${projectVersion()}`,
    // bun 来源：local 时 FROM 阶段退化为 base 镜像占位（内网拉不到 oven/bun），二进制从预置文件装
    `BUN_IMAGE=${plan.image.bunSource === "local" ? plan.image.base : plan.image.bunImage}`,
    // node 来源：仅 image 通道需要拉 node 镜像；local/apt/off 都退化为 base 占位
    `NODE_IMAGE=${plan.image.nodeSource === "image" ? plan.image.nodeImage : plan.image.base}`,
    `BASE_IMAGE=${plan.image.base}`,
    `BUN_IMAGE=${plan.image.bunImage}`,
    `IMAGE_USER=${plan.image.user}`,
    // 镜像自身版本（写进 org.opencontainers.image.version；缺省取仓库 package.json 版本）
    `IMAGE_UID=${plan.image.uid}`,
    `IMAGE_GID=${plan.image.gid}`,
    `IMAGE_HOME=${plan.image.home}`,
    `IMAGE_SHELL=${plan.image.shell}`,
    `IMAGE_RUN_AS_ROOT=${plan.image.runAsRoot ? "1" : "0"}`,
    // USER 指令无法条件化：显式 root 运行时就以 root 作为运行用户
    `IMAGE_RUNTIME_USER=${plan.image.runAsRoot ? "root" : plan.image.user}`,
    `IMAGE_DATA_DIR=${plan.image.dataDir}`,
    `IMAGE_MODE=${plan.image.mode}`,
    `IMAGE_HOST=${plan.image.host}`,
    `IMAGE_PORT=${plan.image.port}`,
    `IMAGE_TZ=${plan.image.tz ?? ""}`,
    // 标签逐个输出（IMAGE_LABEL=k=v 多行）：Docker 的 LABEL 指令不支持用变量展开追加任意标签
    // （`LABEL ... ${VAR}` 会被当键值对解析而报 "can't find ="），改由宿主侧转成 `docker build --label`
    ...Object.entries(plan.image.labels).map(([k, v]) => `IMAGE_LABEL=${k}=${v}`),
    `HEALTHCHECK_ENABLED=${hc === false ? "0" : "1"}`,
    `HEALTHCHECK_PATH=${hc === false ? "/api/health" : hc.path}`,
    `PLAYWRIGHT_SOURCE=${plan.image.browserSource}`,
    `PLAYWRIGHT_DIR=${plan.image.browserDir}`,
    `PLAYWRIGHT_DOWNLOAD_HOST=${plan.image.browserDownloadHost ?? ""}`,
    `PLAYWRIGHT_DEPS=${plan.image.browserDeps}`,
    `RUNTIME_BUN_SOURCE=${plan.image.bunSource}`,
    `RUNTIME_BUN_DIR=${plan.image.bunDir}`,
    `RUNTIME_NODE_SOURCE=${plan.image.nodeSource}`,
    `RUNTIME_NODE_DIR=${plan.image.nodeDir}`,
    `APT_MIRROR=${plan.image.aptMirror ?? ""}`,
    `NPM_REGISTRY=${plan.image.npmRegistry ?? ""}`,
    `BUILD_PROXY=${plan.image.proxy ?? ""}`,
  ]
}

/** 人读裁剪报告（构建日志与 `--print-plan` 共用）。 */
export function renderReport(plan: BuildPlan, source: string): string {
  const onOff = (b: boolean): string => (b ? "on" : "off")
  const subs = plan.subAgents
  const subText = subs.enable?.length
    ? `包含 [${subs.enable.join(", ")}]（依赖自动连带）`
    : subs.disable?.length
      ? `排除 [${subs.disable.join(", ")}]`
      : "全量"
  const vendorText = plan.vendor.length === VENDOR_GROUPS.length ? "全量" : plan.vendor.length ? plan.vendor.join(", ") : "无（跳过全部前端引擎）"
  const packagesIndividual = BASE_PACKAGES.length + SYSTEM_GROUPS.filter((g) => g !== "chromium" && plan.system[g]).flatMap((g) => SYSTEM_PACKAGES[g as Exclude<SystemKey, "chromium">]).length
  const packages = packagesIndividual + plan.image.extraPackages.length
  return [
    `镜像构建裁剪计划：${plan.name}${plan.description ? `（${plan.description}）` : ""}${source ? `  来源: ${source}` : ""}`,
    `  子Agent      : ${subText}${subs.preload?.length ? `，预加载 [${subs.preload.join(", ")}]` : ""}`,
    `  全局工具      : ${plan.tools.disable?.length ? `排除 [${plan.tools.disable.join(", ")}]` : "全量"}`,
    `  内嵌资产      : web_ui=${onOff(plan.assets.web_ui)} cv=${onOff(plan.assets.cv)} d2=${onOff(plan.assets.d2)} analyzer=${onOff(plan.assets.analyzer)} browser=${onOff(plan.assets.browser)} ripgrep=${onOff(plan.assets.ripgrep)}`,
    `  前端 vendor   : ${vendorText}`,
    `  系统能力      : ${SYSTEM_GROUPS.filter((g) => g !== "chromium").map((g) => `${g}=${onOff(plan.system[g])}`).join(" ")}`,
    `  浏览器 chromium: ${onOff(plan.system.chromium)}`,
    `  运行期 apt 包 : ${packages} 个（含固定项 ${BASE_PACKAGES.join(", ")}）${plan.image.extraPackages.length ? `，额外 [${plan.image.extraPackages.join(", ")}]` : ""}`,
    "  镜像本体      : " +
      `${plan.image.base}｜用户 ${plan.image.runAsRoot ? "root（显式）" : `${plan.image.user}(${plan.image.uid}:${plan.image.gid})`}｜数据根 ${plan.image.dataDir}｜端口 ${plan.image.port}｜时区 ${plan.image.tz ?? "未设"}`,
    "  指令自定义    : " +
      [
        `标签 ${Object.keys(plan.image.labels).length} 个`,
        plan.image.healthcheck === false
          ? "健康检查 off（--target runtime-nohealthcheck）"
          : `健康检查 on（探针 ${plan.image.healthcheck.path}；间隔/超时/重试取 Dockerfile 默认 30s/5s/3，运行期可用 compose healthcheck 或 --health-* 覆盖）`,
        plan.image.aptMirror ? `apt 源 ${plan.image.aptMirror}` : "",
        plan.image.npmRegistry ? `npm 源 ${plan.image.npmRegistry}` : "",
        plan.image.proxy ? `代理 ${plan.image.proxy}` : "",
      ]
        .filter(Boolean)
        .join("｜"),
    "  浏览器        : " +
      (plan.system.chromium
        ? plan.image.browserSource === "local"
          ? `预置目录 ${plan.image.browserDir}（零网络）｜系统依赖 ${plan.image.browserDeps}`
          : plan.image.browserSource === "off"
            ? "不装（system.chromium 已置位但 browser_source=off）"
            : `下载源 ${plan.image.browserDownloadHost ?? "playwright CDN（缺省）"}｜系统依赖 ${plan.image.browserDeps}`
        : "未启用（system.chromium=false）"),
    `  运行时        : bun=${plan.image.bunSource}${plan.image.bunSource === "local" ? `（${plan.image.bunDir}/bun）` : `（${plan.image.bunImage}）`}｜node=${plan.image.nodeSource}${plan.image.nodeSource === "local" ? `（${plan.image.nodeDir}/node）` : plan.image.nodeSource === "image" ? `（${plan.image.nodeImage}）` : ""}`,
  ].join("\n")
}

/**
 * 浏览器供给的落地前置检查：预置目录不存在、开关与来源不一致这类问题若漏到 `docker build` 才报，
 * 错误信息远不如这里清楚（COPY 失败只说「找不到文件」）。`exists` 注入以便单测。
 */
export function validateProvisioning(plan: BuildPlan, exists: (p: string) => boolean): string | null {
  if (plan.system.chromium && plan.image.browserSource !== "off" && plan.image.nodeSource === "off") {
    return "启用了浏览器（system.chromium=1）但 node_source=off —— 浏览器桥接是 `Bun.spawn([\"node\", driver])`，没有 node 时浏览器子Agent 启动即失败。请把 node_source 设为 auto（默认跟随）/ image / local / apt"
  }
  if (plan.image.bunSource === "local" && !exists(`${plan.image.bunDir}/bun`)) {
    return `bun_source=local 但预置文件不存在：${plan.image.bunDir}/bun —— 放入 bun 可执行文件（如 docker/build.sh --export-runtimes <目录> 导出），或改回 bun_source=image`
  }
  if (plan.image.nodeSource === "local" && !exists(`${plan.image.nodeDir}/node`)) {
    return `node_source=local 但预置文件不存在：${plan.image.nodeDir}/node —— 放入 node 可执行文件（docker/build.sh --export-runtimes <目录>），或改用 node_source=image / apt`
  }
  if (!plan.system.chromium) {
    if (plan.image.browserSource === "local") {
      return "浏览器未启用（system.chromium=false）却指定了 browser_source=local —— 两者需一致：启用浏览器请加 --with-browser / --set system.chromium=1，或把 browser_source 改回 download"
    }
    return null
  }
  if (plan.image.browserSource === "local" && !exists(plan.image.browserDir)) {
    return `browser_source=local 但预置目录不存在：${plan.image.browserDir} —— 把 playwright 浏览器目录（如 chromium-<revision>/，见「离线浏览器」）放到该位置，或用 docker/build.sh --export-browsers <目录> 从已构建镜像/宿主缓存导出`
  }
  return null
}

/** 档案引用解析：名字 → `docker/profiles/{名}.json`；含分隔符或 .json 结尾 → 路径（相对 cwd，其次仓库根）。 */
function resolveProfileRef(ref: string): string {
  const asPath = isAbsolute(ref) ? ref : resolve(ref)
  if (isAbsolute(ref) || /[\\/]/.test(ref) || /\.json$/i.test(ref)) {
    if (existsSync(asPath)) return asPath
    const fromRoot = join(ROOT, ref)
    if (existsSync(fromRoot)) return fromRoot
    fail(`档案不存在: ${ref}`)
  }
  const named = join(ROOT, "docker", "profiles", `${ref}.json`)
  if (existsSync(named)) return named
  fail(`档案不存在: ${named}（可用: ${listProfiles().join(", ") || "无预置档案"}）`)
}

/** 预置档案名清单（`docker/profiles/*.json`）。 */
function listProfiles(): string[] {
  const dir = join(ROOT, "docker", "profiles")
  if (!existsSync(dir)) return []
  return readdirSync(dir)
    .filter((f) => f.endsWith(".json"))
    .map((f) => f.slice(0, -5))
    .sort()
}

function run(): void {
  const argv = process.argv.slice(2)
  let profileRef = ""
  let profileB64 = ""
  let out = "/tmp/gebai-plan.env"
  let printOnly = false
  let emitArgs = false
  const overrides: string[] = []
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]!
    if (a === "--profile") profileRef = argv[++i] ?? ""
    else if (a === "--profile-b64") profileB64 = argv[++i] ?? ""
    else if (a === "--set") overrides.push(argv[++i] ?? "")
    else if (a === "--out") out = argv[++i] ?? ""
    else if (a === "--print-plan") printOnly = true
    else if (a === "--emit-args") emitArgs = true
    else fail(`未知参数 "${a}"（可用: --profile / --profile-b64 / --set / --out / --print-plan / --emit-args）`)
  }
  if (profileRef && profileB64) fail("--profile 与 --profile-b64 互斥")

  let raw: unknown
  let source = ""
  if (profileB64) {
    let text: string
    try {
      text = Buffer.from(profileB64, "base64").toString("utf8")
    } catch {
      return fail("--profile-b64 解码失败")
    }
    try {
      raw = JSON.parse(text)
    } catch (err) {
      return fail(`档案 JSON 解析失败（base64 输入）: ${err instanceof Error ? err.message : String(err)}`)
    }
    source = "build-arg"
  } else if (profileRef) {
    const path = resolveProfileRef(profileRef)
    try {
      raw = JSON.parse(readFileSync(path, "utf8"))
    } catch (err) {
      return fail(`档案 JSON 解析失败（${path}）: ${err instanceof Error ? err.message : String(err)}`)
    }
    source = path
  } else {
    raw = { name: "full", description: "全量（未指定档案）" }
  }

  const plan = parseProfile(raw, source || "内联")
  for (const o of overrides) applyOverride(plan, o)
  finalizeProvisioning(plan)
  // 浏览器供给前置检查：早失败，错误信息比 docker build 的 COPY 失败清楚得多
  const browserIssue = validateProvisioning(plan, existsSync)
  if (browserIssue) fail(browserIssue)

  // --emit-args：仅输出指令级 build-arg（KEY=VAL 每行）——宿主侧 build.sh 读入后拼 --build-arg；
  // 报告走 stderr，避免与 build-arg 行混淆
  if (emitArgs) {
    console.error(renderReport(plan, source))
    process.stdout.write(emitBuildArgs(plan).join("\n") + "\n")
    return
  }

  const report = renderReport(plan, source)
  console.log(report)
  if (printOnly) return
  writeFileSync(out, planToEnv(plan, source))
  console.log(`\n[build-image-plan] 计划已写入 ${out}`)
}

/** 命令行入口：错误统一带前缀打印并以非零码退出（内部校验一律抛错，便于测试直接断言）。 */
function main(): void {
  try {
    run()
  } catch (err) {
    console.error(`[build-image-plan] ${err instanceof Error ? err.message : String(err)}`)
    process.exit(1)
  }
}

// 直接执行时跑计划器；被测试 import 时不执行（argv 属测试进程）
if (import.meta.main) main()
