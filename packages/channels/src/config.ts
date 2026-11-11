/**
 * 通道实例的声明式配置：`GEBAI_CHANNEL_<TYPE>` = JSON 数组，**每项一个具名实例**。
 *
 * ## 为什么从「按类型拼前缀」改成「每类型一个 JSON 数组」
 *
 * 旧形态把类型名焊进变量名（`GEBAI_CHANNELS_FEISHU_APP_ID`），带来三个死结：
 *
 * 1. **同类不能配两份**：想同时接「主群机器人」和「运维群机器人」，两个都要
 *    `GEBAI_CHANNELS_FEISHU_APP_ID` —— 变量名只有一个，第二份无处可写。
 * 2. **键名散在几十个变量里**：一个渠道要配 4 项，就要 4 个变量、4 行注释，
 *    彼此的关系（这 4 个是同一个实例的）只能靠命名前缀暗示，读不出来。
 * 3. **实例名被文件名绑死**：`feishu.ts` 只能叫 `feishu`，所以「哪个机器人」
 *    在配置面无法表达，只能靠「改代码/换目录」。
 *
 * 改成声明式之后：
 *
 * ```dotenv
 * GEBAI_CHANNEL_IN_FEISHU=[
 *   {"name":"feishu_in","appId":"cli_a","appSecret":"sa","chats":["oc_x"]}
 * ]
 * GEBAI_CHANNEL_OUT_FEISHU=[
 *   {"name":"feishu_out","appId":"cli_a","appSecret":"sa","chats":["oc_y"]}
 * ]
 * ```
 *
 * - 一份配置 = 一个实例 = 一条独立长连接（同类多份从此成立）
 * - `name` 是**引用键**：人格的 `channels:["feishu_main"]` 指的是它
 * - 键集合由**类型自己声明**（`ChannelDef.config`），校验时按 type 派发，
 *   所以「feishu_ops 的 appId 缺失」能精确报到实例级
 *
 * ## 白名单方向反了：从「拒绝未声明变量」到「拒绝未声明键」
 *
 * 旧实现是「渠道读一个环境变量，必须在 `envVars` 里声明过，否则拒绝」。那条规则
 * 防的是「读告警的渠道顺手拿到模型密钥」。新形态下密钥就在它自己的 JSON 项里，
 * 防护目标变成：**JSON 里出现类型不认识的键 → 报错**（拼错的 `appID` 若不报错，
 * 就是「配了但没生效」的经典形态）。
 */

import type { ChannelDef, ConfigKeySpec } from "./channel"

/** 渠道方向：入站（收）与出站（发/读）。 */
export type ChannelDir = "in" | "out"

/** 一个已解析的通道实例：`name` 供人格引用，`settings` 是该实例自己的配置项。 */
export interface ChannelInstance {
  name: string
  /** 该实例负责的方向（来自它写在哪个变量里）。 */
  dir: ChannelDir
  settings: Record<string, unknown>
}

export interface ParseInstancesResult {
  instances: ChannelInstance[]
  /** 配置解析错误（配错不该静默：一条拼错的键等于实例带着默认值空跑）。 */
  errors: string[]
}

const NAME_RE = /^[a-z0-9_]+$/

/**
 * 解析 `GEBAI_CHANNEL_<DIR>_<TYPE>` 的 JSON。
 *
 * `dir` 决定变量名（`IN`/`OUT`）并写进每个实例里。
 * 返回空数组且无错误 = **该类型的该方向未配置**（由调用方决定是报错还是用缺省实例）。
 *
 * 为何按方向分组而不是在项里写 `"dir":"in"`：同一个应用常常要**收一个群、发另一个群**，
 * 分组后两组配置彼此独立（可以不同账号、不同群），而且“这组是收还是发”一目了然；
 * 写在项里则要逐项读才能判断，也更容易写成矛盾（同一实例既是 in 又是 out）。
 */
export function parseChannelInstances(
  typeName: string,
  raw: string | undefined,
  schema: Record<string, ConfigKeySpec> | undefined,
  dir: ChannelDir = "out",
): ParseInstancesResult {
  const errors: string[] = []
  const varName = `GEBAI_CHANNEL_${dir.toUpperCase()}_${typeName.toUpperCase()}`
  const text = (raw ?? "").trim()
  if (!text) return { instances: [], errors }

  let parsed: unknown
  try {
    parsed = JSON.parse(text)
  } catch (err) {
    errors.push(`${varName} 不是合法 JSON：${(err as Error).message}`)
    return { instances: [], errors }
  }
  if (!Array.isArray(parsed)) {
    errors.push(`${varName} 必须是 JSON 数组（每项一个实例，如 [{"name":"feishu_main","appId":"cli_x"}]）`)
    return { instances: [], errors }
  }

  const instances: ChannelInstance[] = []
  const seen = new Set<string>()
  parsed.forEach((item, i) => {
    const at = `${varName}[${i}]`
    if (!item || typeof item !== "object" || Array.isArray(item)) {
      errors.push(`${at} 必须是对象`)
      return
    }
    const o = item as Record<string, unknown>

    // name：实例的引用键（人格的 channels 指向它），也是 target 前缀
    const name = typeof o.name === "string" ? o.name.trim() : ""
    if (!name) {
      errors.push(`${at} 缺少 name（实例名，供人格 channels 引用，如 "feishu_main"）`)
      return
    }
    if (!NAME_RE.test(name)) {
      errors.push(`${at} 的 name「${name}」非法（须 [a-z0-9_]+——它是 target 前缀，如 ${name}:oc_xxx）`)
      return
    }
    if (seen.has(name)) {
      errors.push(`${at} 的 name「${name}」重复（同一类型下实例名必须唯一）`)
      return
    }
    seen.add(name)

    // 键校验：类型不认识的键直接报错（拼错即静默失效，是最难查的一类）
    const settings: Record<string, unknown> = {}
    const known = schema ?? {}
    for (const [k, v] of Object.entries(o)) {
      if (k === "name") continue
      const spec = known[k]
      if (!spec) {
        const allowed = Object.keys(known)
        errors.push(
          `${at}（${name}）含类型「${typeName}」不认识的键「${k}」` +
            (allowed.length ? `（可用：${allowed.join(" / ")}）` : "（该类型不接受任何配置项）"),
        )
        continue
      }
      const checked = checkValue(v, spec, `${at}（${name}）的 ${k}`)
      if (checked.error) {
        errors.push(checked.error)
        continue
      }
      settings[k] = checked.value
    }

    // 必填项
    for (const [k, spec] of Object.entries(known)) {
      if (spec.required && settings[k] === undefined) {
        errors.push(`${at}（${name}）缺少必填项「${k}」（${spec.description}）`)
      }
    }

    instances.push({ name, dir, settings })
  })

  return { instances, errors }
}

/** 按声明做类型检查与规整（不做隐式转换：`"3"` 当 number 用会掩盖配置错误）。 */
function checkValue(
  raw: unknown,
  spec: ConfigKeySpec,
  where: string,
): { value?: unknown; error?: string } {
  switch (spec.type) {
    case "string": {
      if (typeof raw !== "string") return { error: `${where} 必须是字符串` }
      if (!raw.trim() && spec.required) return { error: `${where} 不能为空` }
      return { value: raw }
    }
    case "number": {
      if (typeof raw !== "number" || !Number.isFinite(raw)) return { error: `${where} 必须是数字` }
      return { value: raw }
    }
    case "boolean": {
      if (typeof raw !== "boolean") return { error: `${where} 必须是布尔值（true/false）` }
      return { value: raw }
    }
    case "string[]": {
      if (!Array.isArray(raw)) return { error: `${where} 必须是字符串数组，如 ["oc_xxx"]` }
      for (const x of raw) {
        if (typeof x !== "string" || !x.trim()) return { error: `${where} 每项必须是非空字符串` }
      }
      return { value: raw.map((x) => (x as string).trim()) }
    }
    default:
      return { error: `${where} 的类型声明非法` }
  }
}

/**
 * 把一个类型定义 + 一份实例配置合成为**可运行的渠道实例**。
 *
 * 规则：
 * - 实例名覆盖 `def.name`（类型名只用于找配置与报错）
 * - **按方向裁剪能力面**：`in` 实例只留 `startListen`（收），
 *   `out` 实例只留 `act`/`observe`（发/读）。
 *
 * 为何要裁剪而不只是“启动时跳过”：能力面就是模型看到的工具面（`channels_list`）。
 * 一个纯输出实例如果还挂着 `listens` 声明，模型会以为它能收——
 * 而它永远收不到，因为 `startListen` 根本不会被调用。
 * 裁剪之后“这个实例能干什么”与模型看到的一致。
 */
export function materializeInstance(def: ChannelDef, inst: ChannelInstance): ChannelDef {
  const base: ChannelDef = { ...def, name: inst.name }
  if (inst.dir === "in") {
    // 入站实例：只保留收的一面
    return {
      ...base,
      verbs: undefined,
      observes: undefined,
      act: undefined,
      observe: undefined,
      targets: undefined,
    }
  }
  // 出站实例：只保留发/读的一面（不建长连接）
  return {
    ...base,
    listens: undefined,
    startListen: undefined,
    stopListen: undefined,
    defaultKind: undefined,
  }
}
