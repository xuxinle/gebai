/**
 * 前端最早期扩展点：二开前端脚本（二开域 `custom/web/`，由 vite 带到前端产物根，`index.html` /
 * `files.html` 在入口模块脚本之前以普通 script 同步引入——页面加载即执行，未启用则不引入）。
 * 目录内出厂为 `*.example.js` 示例模板，复制改名为脚本名即启用（上游更新复制 `custom/` 时只刷新示例，
 * 不动已启用文件）。
 *
 * - `gebai.config.js`（配置）：部署方声明「环境变量预置」与
 *   「宿主 localStorage → 歌白设置」的映射，经 `window.__GEBAI_WEB_CONFIG__` 暴露（对象，或返回对象的函数）；
 * - `gebai.custom.js`（初始化脚本，源 `custom/web/init.js`）：页面加载即执行的自由脚本，可承担二开的
 *   本地存储初始化、用户注册与登录等职责；需要 await 的动作经 `window.__GEBAI_WEB_BOOT__` 交给本模块等待
 *   （`awaitCustomBoot`，等待上限见配置项 `bootTimeout`）。
 *
 * 应用时机：页面初始化最早期（`main.ts` / `files/main.ts` 的 init 首行），**先于**主题、低功耗、
 * 文件展示等读取 localStorage 的模块。优先级：URL 参数 > 用户本次手动选择 > localStorage 既有值 >
 * 配置文件 > 服务端全局注入 > 默认——即本模块**不覆盖用户已有选择**，只补齐尚未设置的键
 * （`force: true` 的规则例外，供部署方统一口径）。
 *
 * 配置形态（各字段均可选；未知字段忽略、类型不符的项丢弃、读取异常当作空配置）：
 * ```js
 * window.__GEBAI_WEB_CONFIG__ = {
 *   // ① 环境变量预置：随消息请求临时注入服务端（与设置面板同一通道，仅本浏览器生效、不落盘）
 *   env: { GEBAI_LLM_MODEL: "local-qwen" },
 *   // ② 环境变量 ← 宿主 localStorage 键（运行时读取，宿主系统的凭据可直接带进歌白环境变量）
 *   envFromStorage: { GEBAI_LLM_API_KEY: "myapp.llmKey" },
 *   // ③ 歌白设置键 ← 宿主素材：字符串=宿主 localStorage 键，或 { from } / { value } / { force }
 *   storage: {
 *     "gebai.ui.style": "myapp.theme",
 *     "gebai.ui.lowPower": { value: "on" },
 *     "gebai.ui.approvalSkip": { from: "myapp.approvalSkip", force: true },
 *   },
 *   // ④ 关闭外部链接携带提示词自动运行（URL 参数 gb_prompt，默认开启）
 *   allowUrlPrompt: false,
 *   // ⑤ 二开初始化脚本异步引导的等待上限（毫秒，默认 3000，0 = 不等待）
 *   bootTimeout: 3000,
 * }
 * ```
 */

/** 配置挂载点（`gebai.config.js` 往 window 上写这个键）。 */
export const CONFIG_KEY = "__GEBAI_WEB_CONFIG__"

/** 二开初始化脚本（`gebai.custom.js`）的异步引导挂载点：Promise / 返回 Promise 的函数 / 二者组成的数组。 */
export const BOOT_KEY = "__GEBAI_WEB_BOOT__"

/** 二开初始化脚本异步引导的默认等待上限（毫秒；配置项 `bootTimeout` 可调，0 = 不等待）。 */
export const DEFAULT_BOOT_TIMEOUT = 3000

/** 等待上限的封顶值（毫秒）：防部署方笔误把启动卡死（等待发生在启动动画期间）。 */
export const MAX_BOOT_TIMEOUT = 30_000

export interface WebConfigStorageRule {
  /** 宿主 localStorage 键：取其值写入歌白键。 */
  from?: string
  /** 字面值：直接写入歌白键。 */
  value?: string
  /** 覆盖已有值（默认只在歌白键未设置时写入）。 */
  force?: boolean
}

export interface WebConfig {
  env: Record<string, string>
  envFromStorage: Record<string, string>
  storage: Record<string, WebConfigStorageRule>
  allowUrlPrompt: boolean
  /** 二开初始化脚本异步引导的等待上限（毫秒；0 = 不等待）。 */
  bootTimeout: number
}

type ConfigHost = Record<string, unknown>

interface StoreLike {
  getItem(key: string): string | null
  setItem(key: string, value: string): void
}

function emptyConfig(): WebConfig {
  return { env: {}, envFromStorage: {}, storage: {}, allowUrlPrompt: true, bootTimeout: DEFAULT_BOOT_TIMEOUT }
}

/** 引导等待上限归一：0 保留（显式不等待）、正数取整并封顶、其余（缺省/非法/负数）回落默认。 */
function bootTimeoutOf(raw: unknown): number {
  if (raw === 0) return 0
  if (typeof raw !== "number" || !Number.isFinite(raw) || raw <= 0) return DEFAULT_BOOT_TIMEOUT
  return Math.min(Math.floor(raw), MAX_BOOT_TIMEOUT)
}

/** 字符串映射项：键非空且值是非空字符串才保留（与设置面板「空值不保存」同口径）。 */
function stringMap(raw: unknown): Record<string, string> {
  const out: Record<string, string> = {}
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) return out
  for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
    if (!k.trim() || typeof v !== "string" || !v.trim()) continue
    out[k.trim()] = v.trim()
  }
  return out
}

/** storage 规则归一：字符串视为宿主键；对象取 from/value/force（两者皆空则丢弃）。 */
function storageMap(raw: unknown): Record<string, WebConfigStorageRule> {
  const out: Record<string, WebConfigStorageRule> = {}
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) return out
  for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
    const key = k.trim()
    if (!key) continue
    if (typeof v === "string") {
      if (v.trim()) out[key] = { from: v.trim() }
      continue
    }
    if (typeof v !== "object" || v === null || Array.isArray(v)) continue
    const o = v as Record<string, unknown>
    const rule: WebConfigStorageRule = {}
    if (typeof o.from === "string" && o.from.trim()) rule.from = o.from.trim()
    if (typeof o.value === "string" && o.value.trim()) rule.value = o.value.trim()
    if (o.force === true) rule.force = true
    if (rule.from !== undefined || rule.value !== undefined) out[key] = rule
  }
  return out
}

/** 容错归一化：非对象/数组/缺字段一律回落默认（配置文件由部署方手改，不能因一处笔误让整页失效）。 */
export function normalizeWebConfig(raw: unknown): WebConfig {
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) return emptyConfig()
  const o = raw as Record<string, unknown>
  return {
    env: stringMap(o.env),
    envFromStorage: stringMap(o.envFromStorage),
    storage: storageMap(o.storage),
    // 只有显式 false 关闭（缺省/其它值均视为开启）
    allowUrlPrompt: o.allowUrlPrompt !== false,
    bootTimeout: bootTimeoutOf(o.bootTimeout),
  }
}

/** 读取页面上的配置（支持对象或返回对象的函数；读取或求值抛错时按空配置处理）。 */
export function readWebConfig(host: ConfigHost = window as unknown as ConfigHost): WebConfig {
  let raw: unknown
  try {
    raw = host[CONFIG_KEY]
    if (typeof raw === "function") raw = (raw as () => unknown)()
  } catch {
    return emptyConfig()
  }
  return normalizeWebConfig(raw)
}

/** 配置文件预置 + 宿主存储取到的环境变量（供 `loadLocalEnv` 打底，浏览器面板设置为准）。 */
export function configEnv(cfg: WebConfig, store: StoreLike): Record<string, string> {
  const out: Record<string, string> = { ...cfg.env }
  for (const [name, hostKey] of Object.entries(cfg.envFromStorage)) {
    try {
      const v = store.getItem(hostKey)
      if (v && v.trim()) out[name] = v.trim()
    } catch {
      /* 宿主存储不可用：跳过该项（其余照常合并） */
    }
  }
  return out
}

/** 便捷入口：按当前页面配置与浏览器存储取环境变量预置。 */
export function webConfigEnv(): Record<string, string> {
  try {
    return configEnv(readWebConfig(), localStorage)
  } catch {
    return {}
  }
}

/** 应用 storage 映射：只写尚未设置的歌白键（`force` 规则覆盖），返回实际写入的键名。 */
export function applyWebConfigStorage(cfg: WebConfig, store: StoreLike): string[] {
  const written: string[] = []
  for (const [key, rule] of Object.entries(cfg.storage)) {
    try {
      const value = rule.value ?? (rule.from ? store.getItem(rule.from) : null)
      if (value === null || value === undefined || !value.trim()) continue
      if (!rule.force && store.getItem(key) !== null) continue
      store.setItem(key, value)
      written.push(key)
    } catch {
      /* 存储不可用（隐私模式/配额满）：静默跳过，与主题/审批跳过等模块一致 */
    }
  }
  return written
}

/** 当前配置是否允许 URL 携带提示词自动运行（关闭时外部链接只打开页面，不自动建会话执行）。 */
let allowUrlPromptEnabled = true

/**
 * 页面启动最早期调用（幂等，两个页面入口各调一次）。
 * 返回实际写入的设置键与 URL 提示词开关状态，供调用方按需展示。
 */
export function applyWebConfig(opts: { host?: ConfigHost; store?: StoreLike } = {}): { written: string[]; allowUrlPrompt: boolean } {
  try {
    const store = opts.store ?? localStorage
    const cfg = readWebConfig(opts.host ?? (window as unknown as ConfigHost))
    allowUrlPromptEnabled = cfg.allowUrlPrompt
    return { written: applyWebConfigStorage(cfg, store), allowUrlPrompt: cfg.allowUrlPrompt }
  } catch {
    return { written: [], allowUrlPrompt: true }
  }
}

export function urlPromptAllowed(): boolean {
  return allowUrlPromptEnabled
}

/* ---------- 二开初始化脚本（gebai.custom.js）的异步引导 ---------- */

/**
 * 收集宿主上的引导值（`window.__GEBAI_WEB_BOOT__`）：Promise、返回 Promise 的函数，以及二者组成的数组。
 * 函数即席求值（抛错只记警告）；非 Promise 项忽略。返回值用于 `awaitCustomBoot` 等待。
 */
export function bootPromises(host: ConfigHost = window as unknown as ConfigHost): Promise<unknown>[] {
  const out: Promise<unknown>[] = []
  const visit = (value: unknown, depth: number): void => {
    if (value === null || value === undefined || depth > 3) return
    if (typeof value === "function") {
      let produced: unknown
      try {
        produced = (value as () => unknown)()
      } catch (err) {
        console.warn("[gebai] 二开初始化脚本引导函数执行失败，已忽略：", err)
        return
      }
      visit(produced, depth + 1)
      return
    }
    if (Array.isArray(value)) {
      for (const item of value) visit(item, depth + 1)
      return
    }
    if (typeof (value as { then?: unknown }).then === "function") out.push(value as Promise<unknown>)
    else console.warn("[gebai] 二开初始化脚本引导值不是 Promise，已忽略：", value)
  }
  try {
    visit(host[BOOT_KEY], 0)
  } catch (err) {
    console.warn("[gebai] 读取二开初始化脚本引导值失败，已忽略：", err)
  }
  return out
}

/** 各宿主的引导等待（按宿主缓存：同一页面入口的重复调用复用同一次，引导函数只求值一次）。 */
const customBoots = new WeakMap<ConfigHost, Promise<void>>()

/**
 * 等待二开初始化脚本的异步引导完成（`main.ts` / `files/main.ts` 的 init 首行调用）——使二开的
 * 本地存储初始化、令牌换取等结果先于应用读取本地存储就位。无引导值即立即返回；异常与超时只记
 * 控制台警告、不阻塞页面（引导本身仍在后台进行）。
 */
export function awaitCustomBoot(opts: { host?: ConfigHost } = {}): Promise<void> {
  const host = opts.host ?? (window as unknown as ConfigHost)
  let boot = customBoots.get(host)
  if (!boot) {
    boot = runCustomBoot(host)
    customBoots.set(host, boot)
  }
  return boot
}

async function runCustomBoot(host: ConfigHost): Promise<void> {
  const jobs = bootPromises(host)
  if (jobs.length === 0) return
  const timeout = readWebConfig(host).bootTimeout
  if (timeout <= 0) return
  let expired = false
  let timer: ReturnType<typeof setTimeout> | undefined
  await Promise.race([
    Promise.allSettled(jobs).then((results) => {
      for (const r of results) if (r.status === "rejected") console.warn("[gebai] 二开初始化脚本引导失败，已忽略：", r.reason)
    }),
    new Promise<void>((resolve) => {
      timer = setTimeout(() => {
        expired = true
        resolve()
      }, timeout)
    }),
  ])
  if (timer !== undefined) clearTimeout(timer)
  if (expired) console.warn(`[gebai] 二开初始化脚本引导等待超过 ${timeout}ms：页面继续加载，引导仍在后台进行`)
}
