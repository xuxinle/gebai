/**
 * 人格工具审批：**默认全部不审批，只有名单里的工具弹窗问人**。
 *
 * ## 为什么不要「档位」
 *
 * 旧实现是四档（`readonly`/`ask`/`auto`/`deny`）：`ask` 会把**所有**需审批工具都推卡，
 * `readonly` 又完全不能按需收紧。而真实需求是「整体不弹卡，但 `sh` / 删文件必须问人」——
 * 这是一个**工具级**结论，档位表达不了。
 *
 * 所以现在只有一条规则：名单里的工具 → 弹窗；名单外 → 直接执行。
 *
 * ## 名单写法
 *
 * - 精确名：`sh`、`py`、`file.delete`
 * - 通配：`code_*`（子 Agent 的工具带前缀）、`*`（**全部**都问）
 * - 逗号/空白分隔：`sh, py, file.delete`
 *
 * 刻意**不支持**正则：一条写错的正则会让「某个工具该弹窗却没弹」（静默少问一次，
 * 是最危险的方向），或者「某个工具每次都弹」（噪音淹没）。通配只需要 `*`，
 * 覆盖了“整个子 Agent 的工具都问”这个真实诉求。
 *
 * ## 为什么名字匹配要大小写敏感
 *
 * 工具名是代码里的标识符（`sh`、`file.delete`），不是人写的配置。大小写不敏感会
 * 让 `SH` 也命中 `sh`，看似友好实则掩盖拼写错误——而“拼错的名字”恰恰是要报出来的东西。
 */

/** 名单条目：精确名或 `前缀*` / `*`。 */
export type AskToolPattern = string

/**
 * 解析名单字符串（`sh, py, code_*`）→ 条目数组。
 *
 * 空串/空白 → 空数组（= 谁都不问，即缺省行为）。
 * 去重保序：配置里重复写同一个工具不该有任何额外含义。
 */
export function parseAskToolList(raw: string | undefined): AskToolPattern[] {
  const text = (raw ?? "").trim()
  if (!text) return []
  const out: string[] = []
  for (const piece of text.split(/[,\s]+/)) {
    const p = piece.trim()
    if (!p) continue
    if (!out.includes(p)) out.push(p)
  }
  return out
}

/**
 * 单个工具是否命中名单。
 *
 * 通配只认**尾部** `*`（`code_*`）：中间/多处通配会让规则难以预测，
 * 而真实需求只有“这个前缀下的都要问”。
 */
export function matchesAskTool(pattern: AskToolPattern, toolName: string): boolean {
  const p = pattern.trim()
  if (!p) return false
  if (p === "*") return true
  if (p.endsWith("*")) {
    const prefix = p.slice(0, -1)
    return prefix.length > 0 && toolName.startsWith(prefix)
  }
  return p === toolName
}

/** 任一命中即要审批（空名单 = 谁都不问）。 */
export function needsApproval(patternList: readonly AskToolPattern[], toolName: string): boolean {
  for (const p of patternList) {
    if (matchesAskTool(p, toolName)) return true
  }
  return false
}

/** env 视图（与 `env-config.ts` 同形）。 */
export type EnvViewLike = Record<string, string | undefined>

/**
 * 全局兜底的审批名单（`GEBAI_PERSONA_APPROVAL_ASK_TOOLS`）。
 *
 * 不填 = 谁都不问（**缺省行为：人格工具全部直接执行**）。
 * 逐分身的 `askTools` 优先于它（见 `askToolsFor`）。
 */
export function askTools(env: EnvViewLike): AskToolPattern[] {
  return parseAskToolList(env.GEBAI_PERSONA_APPROVAL_ASK_TOOLS)
}

/**
 * 按**人格/分身**解析要审批的工具名单。
 *
 * 优先级：分身 `askTools` > 人格级默认 `askTools` > 全局变量 > []。
 *
 * 为何保留全局变量作最后兜底：有些部署确实想“所有分身一律把 sh 拦住”，
 * 逐个人格写一遍容易漏。而逐分身声明优先，所以“全局拦 sh、但盯告警的分身不必拦”
 * 也能表达（在那个分身里写 `"askTools": ""`：显式空 = 什么都不问）。
 */
export function askToolsFor(env: EnvViewLike, personaId: string, driverId?: string): AskToolPattern[] {
  const raw = env[`GEBAI_PERSONA_${personaId.toUpperCase()}`]
  const declared = raw ? readAskToolsFromPersonaJson(raw, driverId) : undefined
  if (declared !== undefined) return declared
  return askTools(env)
}

/**
 * 从 `GEBAI_PERSONA_<ID>` 的 JSON 里取某分身的 `askTools`。
 *
 * 返回 `undefined` = 该分身与人格级都**没声明**（应回落全局）；
 * 返回 `[]` = **显式声明为空**（什么都不问——用于覆盖全局）。
 *
 * 为何不直接用 `parsePersonaEnv`：那是完整校验（会带出一堆与审批无关的错误），
 * 而这里只需只读地抽一个字段。解析失败当“没声明”，不让一条写坏的人格配置
 * 静默变成“全部放行”——真正的报错在人格装配那边会给出。
 */
export function readAskToolsFromPersonaJson(raw: string, driverId?: string): AskToolPattern[] | undefined {
  let parsed: unknown
  try {
    parsed = JSON.parse(raw.trim())
  } catch {
    return undefined
  }
  if (!Array.isArray(parsed)) return undefined
  let fallback: AskToolPattern[] | undefined
  for (const item of parsed) {
    if (!item || typeof item !== "object" || Array.isArray(item)) continue
    const o = item as Record<string, unknown>
    const name = typeof o.name === "string" ? o.name.trim() : ""
    const v = o.askTools
    if (v === undefined) continue
    const list =
      typeof v === "string" ? parseAskToolList(v) : Array.isArray(v) ? parseAskToolList(v.join(",")) : undefined
    if (list === undefined) continue
    // 分身级优先；人格级默认（不带 name 的那项）留着当 fallback
    if (name && name === driverId) return list
    if (!name) fallback = list
  }
  return fallback
}

/**
 * 名单中**永远匹配不到任何工具**的条目（供启动期告警）。
 *
 * 为什么值得单独查：`shell`（实际叫 `sh`）、`delete`（实际叫 `file.delete`）这类
 * 拼写错误会让“我以为它会弹窗问人”变成静默放行——那是最危险的一种配错。
 * 无法判断带通配的条目（`code_*` 可能在子 Agent 未装载时暂时无匹配），所以只查精确名。
 */
export function unknownAskTools(
  patternList: readonly AskToolPattern[],
  knownToolNames: ReadonlySet<string>,
): AskToolPattern[] {
  return patternList.filter((p) => {
    if (p === "*" || p.endsWith("*")) return false
    return !knownToolNames.has(p)
  })
}
