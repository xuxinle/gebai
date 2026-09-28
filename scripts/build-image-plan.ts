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
 *
 * 档案 schema（缺省全开，`name` 必填；未知键/类型不符直接报错——配错档案却按默认面构建比失败更难排查）：
 *   {
 *     "name": "minimal", "description": "最小 API 服务",
 *     "sub_agents": { "enable": ["code"], "disable": [], "preload": ["code"] },
 *     "tools": { "disable": ["show"] },
 *     "assets": { "web_ui": true, "cv": true, "d2": true, "analyzer": true, "browser": true, "ripgrep": true },
 *     "web": { "vendor": ["monaco", "xterm"] },
 *     "system": { "git": true, "python": true, "bubblewrap": true, "fonts": true, "tzdata": true, "procps": true, "chromium": false },
 *     "prompt": { ... }    // 运行期（GEBAI_PROFILE）段：构建期忽略，使同一份档案从构建到运行贯穿
 *   }
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

/** 全开默认计划。 */
function defaultPlan(): BuildPlan {
  return {
    name: "full",
    subAgents: {},
    tools: {},
    assets: Object.fromEntries(ASSET_KEYS.map((k) => [k, true])) as Record<AssetKey, boolean>,
    vendor: [...VENDOR_GROUPS],
    system: Object.fromEntries(SYSTEM_GROUPS.map((k) => [k, k === "chromium" ? false : true])) as Record<SystemKey, boolean>,
  }
}

/** 档案解析与结构校验（未知键/类型不符直接报错）。 */
export function parseProfile(raw: unknown, label: string): BuildPlan {
  const root = obj(raw, `${label} 档案`)!
  checkKeys(root, ["name", "description", "sub_agents", "tools", "assets", "web", "system", "prompt"], `${label} 档案`)
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
  return plan
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
  fail(`--set 未知键 "${key}"（可用: sub_agents.enable / sub_agents.disable / sub_agents.preload / tools.disable / assets.<${ASSET_KEYS.join("|")}> / web.vendor / system.<${SYSTEM_GROUPS.join("|")}> / description）`)
}

/** 计划 → 构建期环境文件内容（Dockerfile `source` 后驱动构建脚本与系统包安装）。 */
export function planToEnv(plan: BuildPlan, source: string): string {
  const flag = (b: boolean): string => (b ? "1" : "0")
  const packages = [...BASE_PACKAGES, ...SYSTEM_GROUPS.filter((g) => g !== "chromium" && plan.system[g]).flatMap((g) => SYSTEM_PACKAGES[g as Exclude<SystemKey, "chromium">])]
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
    "",
  ]
  return lines.join("\n")
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
  const packages = BASE_PACKAGES.length + SYSTEM_GROUPS.filter((g) => g !== "chromium" && plan.system[g]).flatMap((g) => SYSTEM_PACKAGES[g as Exclude<SystemKey, "chromium">]).length
  return [
    `镜像构建裁剪计划：${plan.name}${plan.description ? `（${plan.description}）` : ""}${source ? `  来源: ${source}` : ""}`,
    `  子Agent      : ${subText}${subs.preload?.length ? `，预加载 [${subs.preload.join(", ")}]` : ""}`,
    `  全局工具      : ${plan.tools.disable?.length ? `排除 [${plan.tools.disable.join(", ")}]` : "全量"}`,
    `  内嵌资产      : web_ui=${onOff(plan.assets.web_ui)} cv=${onOff(plan.assets.cv)} d2=${onOff(plan.assets.d2)} analyzer=${onOff(plan.assets.analyzer)} browser=${onOff(plan.assets.browser)} ripgrep=${onOff(plan.assets.ripgrep)}`,
    `  前端 vendor   : ${vendorText}`,
    `  系统能力      : ${SYSTEM_GROUPS.filter((g) => g !== "chromium").map((g) => `${g}=${onOff(plan.system[g])}`).join(" ")}`,
    `  浏览器 chromium: ${onOff(plan.system.chromium)}`,
    `  运行期 apt 包 : ${packages} 个（含固定项 ${BASE_PACKAGES.join(", ")}）`,
  ].join("\n")
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
  const overrides: string[] = []
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]!
    if (a === "--profile") profileRef = argv[++i] ?? ""
    else if (a === "--profile-b64") profileB64 = argv[++i] ?? ""
    else if (a === "--set") overrides.push(argv[++i] ?? "")
    else if (a === "--out") out = argv[++i] ?? ""
    else if (a === "--print-plan") printOnly = true
    else fail(`未知参数 "${a}"（可用: --profile / --profile-b64 / --set / --out / --print-plan）`)
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
