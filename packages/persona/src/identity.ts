/**
 * 身份模板：把「扮演的是谁」从代码里挪到环境变量。
 *
 * ## 为什么身份必须是模板而不是写死的字符串
 *
 * 一份人格定义（`小霜.ts`）是**代码**，而「小霜这个角色由谁扮演」是**部署事实**：
 * 同一个角色在不同部署里是不同的人（不同的姓名、工号、飞书账号）。
 * 把身份写死在 ts 里意味着换个人就得改代码 + 重新过审——人格定义于是无法随部署迁移。
 *
 * 所以身份字段支持 `${VAR}` 占位，取值由部署环境的**白名单变量**提供
 * （`PersonaDef.envVars` 声明，与渠道/事件源同口径：不声明的变量读不到）。
 *
 * ## 三条硬约束（都有反向验证）
 *
 * 1. **加载期校验**：模板里出现的每个变量必须已在 `envVars` 声明。漏声明就该在
 *    加载期报错——否则运行期它会渲染成空串，模型看见「你是，工号」（身份残缺），
 *    而日志里没有任何异常。这是"配了但看不出错"的典型形态。
 * 2. **未解析变量保留原样**（`${X}` 原样留着）而不是渲染成空串：空串会静默污染
 *    提示词，而保留原样一眼能看出"这个变量没配"。
 * 3. **不得跨人格读到别人的变量**：解析只走该人格声明过的白名单集合。
 *
 * ## 为什么不支持函数/表达式
 *
 * 模板只做**替换**，不做求值。带逻辑的身份（"如果是 A 就用 B"）应该写两个 driver
 * 或用部署配置解决——一旦允许表达式，人格定义就变成了一门需要审计的小语言。
 */

/** 模板里引用的变量名（去重，保持出现顺序）。 */
export function templateVars(template: string): string[] {
  const out: string[] = []
  const re = /\$\{([A-Za-z_][A-Za-z0-9_]*)\}/g
  for (const m of template.matchAll(re)) {
    const name = m[1]!
    if (!out.includes(name)) out.push(name)
  }
  return out
}

/**
 * 校验模板引用的变量都在白名单内。
 *
 * @param template 模板文本
 * @param declared 已声明的变量名集合（`PersonaDef.envVars`）
 * @param where 出错时的定位文案（如 `drivers「support」的 focus`）
 * @returns 错误文案；合法返回 null
 */
export function validateTemplate(template: string, declared: ReadonlySet<string>, where: string): string | null {
  const missing = templateVars(template).filter((v) => !declared.has(v))
  if (missing.length === 0) return null
  return (
    `${where} 引用了未声明的环境变量 ${missing.map((v) => `\${${v}}`).join("、")}——` +
    `请在人格定义的 envVars 里声明它（否则运行期渲染不出来，身份会残缺且不报错）`
  )
}

/**
 * 渲染模板。
 *
 * 未提供的变量**保留 `${VAR}` 原样**：空串会让提示词悄悄缺一段，
 * 而原样保留一眼能看出「这个变量没配」，且模型读到占位符也知道信息缺失。
 */
export function renderTemplate(template: string, env: (name: string) => string | undefined): string {
  if (!template.includes("${")) return template
  return template.replace(/\$\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (whole, name: string) => {
    const v = env(name)
    return v === undefined || v === "" ? whole : v
  })
}

/**
 * 身份字段的「整组注入」写法：一个 JSON 环境变量装下全部身份字段。
 *
 * 为何除显式模板之外还要这个：真实部署里身份字段是**按部署下发**的
 * （同一个角色在 A 部门是张三、在 B 部门是李四），而部署方往往只有一个
 * 配置项可填。让它在 `fields` 里逐字段写 `${A}/${B}/${C}…` 意味着改一个身份
 * 要动代码（加字段），而“身份字段有哪些”本来就该由部署方定。
 *
 * 用法：
 *
 * ```ts
 * // 人格定义（代码，不含任何具体身份）：
 * actor: { fields: actorFieldsFromEnv("GEBAI_ACTOR") }
 * // 环境（部署方）：
 * // GEBAI_ACTOR={"姓名":"张三","工号":"10086","部门":"基础架构"}
 * ```
 *
 * ## 为什么返回值是 `${VAR}` 模板而不是直接读值
 *
 * 人格定义在**加载期**被解析，而环境变量可能在运行期被热改（与身份相关的其它变量一样）。
 * 返回模板意味着：解析仍走同一条白名单 + 渲染链，加载期校验也能看见它
 * （漏在 `envVars` 声明就会报错）。
 *
 * 解析失败（非 JSON / 不是字符串值对象）**不在这时报错**：这里返回模板，
 * 真正的问题会在 `resolveActorFields` 里如实上报（那时才拿到值）。
 */
export function actorFieldsFromEnv(varName: string): Record<string, string> {
  if (!varName.trim()) return {}
  // 单个变量的整组注入：渲染阶段把 JSON 展开成多个键
  return { "": `\${${varName.trim()}}` }
}

/** `actorFieldsFromEnv` 的哨兵键（扩展时会被 JSON 展开替换掉）。 */
export const ACTOR_FIELDS_JSON_KEY = ""

/**
 * 解析身份字段：展开整组注入的 JSON、渲染模板、剔除空值与未解析占位符。
 *
 * @returns 逐字段结果 + 解析问题（问题归问题时：不阻断人格加载，但要让人看见）
 */
export function resolveActorFields(
  fields: Readonly<Record<string, string>> | undefined,
  env: (name: string) => string | undefined,
): { fields: Record<string, string>; problems: string[] } {
  const out: Record<string, string> = {}
  const problems: string[] = []
  for (const [key, raw] of Object.entries(fields ?? {})) {
    if (typeof raw !== "string") {
      problems.push(`身份字段「${key}」的值必须是字符串（当前 ${typeof raw}）`)
      continue
    }
    // 整组注入：值是单个 `${VAR}` 且键为空 → 展开该变量里的 JSON 对象
    if (key === ACTOR_FIELDS_JSON_KEY) {
      const json = raw.trim().match(/^\$\{([A-Za-z_][A-Za-z0-9_]*)\}$/)
      if (!json) {
        problems.push(`身份字段整组注入写法必须是单个 \${VAR}（当前：${raw}）`)
        continue
      }
      const value = env(json[1]!)
      if (value === undefined || value.trim() === "") continue // 未配置：安静跳过（部署方还没填）
      let parsed: unknown
      try {
        parsed = JSON.parse(value)
      } catch (err) {
        problems.push(`环境变量 ${json[1]} 不是合法 JSON（身份字段无法展开）：${String((err as Error)?.message || err)}`)
        continue
      }
      if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
        problems.push(`环境变量 ${json[1]} 必须是 JSON 对象（如 {"姓名":"张三"}）`)
        continue
      }
      for (const [k, v] of Object.entries(parsed as Record<string, unknown>)) {
        if (v === null || v === undefined) continue
        if (typeof v === "object") {
          problems.push(`身份字段「${k}」不支持嵌套对象（用字符串或扁平键）`)
          continue
        }
        out[k] = String(v)
      }
      continue
    }
    const rendered = renderTemplate(raw, env).trim()
    // 未配置的变量**不出现在提示词里**，而是记入 problems（由调用方写日志）。
    //
    // 为何与自由文本的处理不同（`identity` 里保留 `${VAR}` 原样）：
    // - 自由文本是部署方写的句子，抽掉占位符会变成「你是，值班工程师」——语法残缺且难看出来；
    //   保留占位符一眼就知道是模板没填。
    // - 结构化身份字段（`姓名: ${X}`）是**成对的事实**，值缺失时整个字段跳过最干净；
    //   留着 `${X}` 反而有被模型复述到群里的风险（“我的工号是 ${ACTOR_STAFF}”）。
    if (!rendered) continue
    if (/\$\{[A-Za-z_][A-Za-z0-9_]*\}/.test(rendered)) {
      problems.push(`身份字段「${key}」的变量未配置（渲染为 ${rendered}），已跳过该字段`)
      continue
    }
    out[key] = rendered
  }
  return { fields: out, problems }
}
