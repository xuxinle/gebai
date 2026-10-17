/**
 * 反向验证的门禁判据：**给定观察集，判这条反向验证是不是真的**。
 *
 * ## 它要解决的那个问题
 *
 * 「加了机制 ≠ 机制生效」是本仓代价最高的一条纪律（四次同型）。它的难处不在立规，
 * 而在**判真假**：worker 交一条 `verify: "bun test x.test.ts"` 时，那条命令可能
 *   · 真的在验反向（退回旧实现它会变红）
 *   · 只是碰巧通过（恒绿：断言写得没判别力）
 *   · 是空壳（`exit 1` / `true` / 一个不存在的文件路径）
 *   · 验的根本不是这一单（命令与 spec 的 rollback 无关）
 * 前三种都能让「正向通过 + 反向通过 + 会红的反证」这套收单口径在**形式上**成立。
 *
 * ## 为什么不能用字符串黑名单
 *
 * 「禁掉 `exit 1`、`true`、`:`」是最先想到的做法，也是最没用的：那是
 * 「把关键词抹掉就通过」的同型失效（本仓 PITFALLS 已记多例）。改写成
 * `sh -c 'exit 7'`、`test -f /nonexistent`、`bun test 空目录` 就绕开了，
 * 而**真正的空壳**往往长得完全正常（一个永远不成立的断言）。
 *
 * ## 判据：**干预**，而不是长相
 *
 * 唯一可靠的判法是让命令在两个**由我们控制**的状态下必须给出相反结果：
 *
 *   ① `original`  —— 交付后的代码（工作树原样）
 *   ② `rolled`    —— 把 spec 声明的 `rollback` 那段替换成 `defect`（= 退回缺陷写法）
 *
 * 阶梯（**从硬到软**，前几档不过就停，后面的观察再好看也不算）：
 *
 *   S0 **命令非空且不是明显恒真**：剥离注释后不能只剩 `true`/`:`/`exit 0`
 *      —— 只挡最廉价的假，**不作为主要判据**（主要判据是干预）。
 *   S1 **original 必须绿**：交上来的东西自己先得是对的。红 ⇒ `broken`。
 *   S2 **rolled 必须红**：这是「会红的反证」的机器化。绿 ⇒ `vacuous`（恒绿）。
 *   S3 **rolled 的红必须来自 rollback**：把 `rollback` 段**原样放回**（第三次运行）
 *      必须恢复绿。否则红是别的原因（环境坏了、命令本来就红）⇒ `wrong-cause`。
 *      —— 这一档挡的是「命令恰好红」：比如 `bun test 拼错文件` 两次都红，
 *      但它与这一单的缺陷毫无关系。
 *   S4 **稳定性**：同一状态下重复观察必须一致（本仓已三次踩到抖动导致的误判）。
 *      不一致 ⇒ `unstable`，**不判通过也不判失败**，交给人。
 *
 * 四档全过 ⇒ `genuine`。
 *
 * ## 一个刻意不做的判定
 *
 * **不判断「rollback 那段是不是真的重要」。** spec 是 worker 自己声明的，它可以挑一段
 * 无关紧要的代码当 `rollback`，让 S2/S3 轻松通过。这不是本判定器能解决的（它需要
 * 读懂代码）。本判定器只保证一件事：**如果 `rollback` 换掉它宣称会变红，且变红确由它引起，
 * 那么这条反向验证在它自己声明的范围内是真的**。范围本身的合理性由 review 的人/模型看——
 * 这是诚实边界，不是遗漏。S5 只能给出**提示**（如 rollback 段过短 / 落在注释里），不判失败。
 */

/** 一次观察：在某个状态下跑 spec 的命令，得到了什么。 */
export interface Observation {
  /** 实际退出码（`null` = 没跑起来：超时 / 命令不存在）。期望值**不在这里**：
   *  每一档期望什么由档位本身决定（S1 要绿、S2 要红…）——调用方再声明一份就会漂成
   *  「它说该红、判据说该绿」两个源头。 */
  code: number | null
  /** 输出尾部（诊断用，不参与判定）。 */
  tail?: string
}

/** 一条反向验证的声明（由工单给出，随 inflight 落盘）。 */
export interface ReverseSpec {
  /** 被验的命令（真公开面入口，如 `bun test path/to/x.test.ts`）。 */
  cmd: string
  /** 会被回退的那段源码所在的文件（仓内相对路径）。 */
  file: string
  /** 修复后的写法（必须**逐字**出现在 file 里，否则无法回退 ⇒ 判 `unsupported`）。 */
  rollback: string
  /** 缺陷的写法（把 rollback 替换成它 = 退回缺陷态）。 */
  defect: string
}

/** 判定档位。 */
export type Verdict =
  | "genuine" // 四档全过：这条反向验证是真的
  | "empty" // S0：命令为空 / 明显恒真
  | "broken" // S1：original 是红的（交上来的东西本身就不对）
  | "vacuous" // S2：退回缺陷后仍然绿（判据没有判别力 / 空壳）
  | "wrong-cause" // S3：红不是 rollback 引起的
  | "unsupported" // rollback 锚点不在文件里（无法干预 ⇒ 无从判定，不是失败也不是通过）
  | "unstable" // S4：两次同态观察不一致
  | "not-run" // 没观察到（缺观察集）

export interface GateResult {
  verdict: Verdict
  /** 一句话结论（进台账/回显）。 */
  reason: string
  /** 只报不改的提示（范围合理性等），不影响 verdict。 */
  hints: string[]
}

/** 剥离 shell 注释（`#` 到行尾）与首尾空白，用于 S0。 */
function stripComment(cmd: string): string {
  return cmd
    .split("\n")
    .map((l) => l.replace(/(^|\s)#.*$/, ""))
    .join("\n")
    .trim()
}

/**
 * S0：这条命令是不是**明显**恒真/空。
 *
 * 刻意保守：只认「剥离注释后整条就是一个恒真动作」这一种形态。**不追长黑名单**——
 * 见文件头「为什么不能用字符串黑名单」。真正的判别力来自 S1~S4 的干预。
 */
export function looksEmpty(cmd: string): boolean {
  const body = stripComment(cmd).replace(/;$/, "").trim()
  if (!body) return true
  // 整条命令就是这些恒真动作之一（允许带 `&&`/`;` 串接的同一类）
  const benign = /^(true|:|exit\s+0|echo(\s+[^;&|]*)?)$/
  const parts = body.split(/&&|;|\|\|/).map((p) => p.trim()).filter(Boolean)
  return parts.length > 0 && parts.every((p) => benign.test(p))
}

/**
 * 四档判定。`obs` 缺项 ⇒ 对应档位得 `not-run`（**不判通过**）。
 *
 * 顺序即优先级：前一档失败，后面观察再好也不看——这正是「从硬到软」的含义。
 * 反过来（先看软再看硬）会让一个恒绿命令靠「original 绿」蒙过去。
 */
export function judge(spec: ReverseSpec, obs: Partial<Record<"original" | "rolled" | "restored" | "original2" | "rolled2", Observation>>): GateResult {
  const hints: string[] = []

  // —— S0 ——
  if (!spec.cmd.trim()) return { verdict: "empty", reason: "反向验证命令为空", hints }
  if (looksEmpty(spec.cmd)) {
    return { verdict: "empty", reason: `反向验证命令是恒真动作（\`${stripComment(spec.cmd)}\`）——它永远不会红`, hints }
  }
  if (!spec.rollback.trim() || !spec.defect.trim()) {
    return { verdict: "empty", reason: "spec 缺 rollback/defect——没有干预就没有判定", hints }
  }
  if (spec.rollback === spec.defect) {
    // 替换前后一样 = 回退是**空操作**。本仓已有真案例：空操作回退 ⇒ 行为没变 ⇒ 0 条红 ⇒ 假绿。
    return { verdict: "empty", reason: "rollback 与 defect 相同——回退是空操作，干预不成立", hints }
  }

  // —— S1：original 必须绿 ——
  const o1 = obs.original
  if (!o1) return { verdict: "not-run", reason: "缺少 original 观察（没跑命令）", hints }
  if (o1.code === null) return { verdict: "not-run", reason: "original 没跑起来（命令不存在/超时）——不等于通过", hints }
  if (o1.code !== 0) {
    return { verdict: "broken", reason: `original 是红的（exit ${o1.code}）——交上来的东西本身就不对，反向验证无从谈起`, hints }
  }

  // —— S2：rolled 必须红 ——
  const o2 = obs.rolled
  if (!o2) return { verdict: "not-run", reason: "缺少 rolled 观察（没做回退干预）", hints }
  if (o2.code === null) return { verdict: "not-run", reason: "rolled 没跑起来——回退版可能编译不过，那是「假绿」的一种", hints }
  if (o2.code === 0) {
    return {
      verdict: "vacuous",
      reason: "退回缺陷写法后命令**仍然绿**——这条反向验证没有判别力（恒绿/空壳）",
      hints,
    }
  }

  // —— S3：红必须来自 rollback（原样放回应恢复绿）——
  const o3 = obs.restored
  if (!o3) return { verdict: "not-run", reason: "缺少 restored 观察（没做第三次对照）", hints }
  if (o3.code === null) return { verdict: "not-run", reason: "restored 没跑起来——无法确认红由 rollback 引起", hints }
  if (o3.code !== 0) {
    return {
      verdict: "wrong-cause",
      reason: `把 rollback 原样放回后仍是红的（exit ${o3.code}）——这个红不是 rollback 引起的（命令本身坏了或环境不对）`,
      hints,
    }
  }

  // —— S4：稳定性 ——
  const pairs: [Observation | undefined, Observation | undefined, string][] = [
    [obs.original, obs.original2, "original"],
    [obs.rolled, obs.rolled2, "rolled"],
  ]
  for (const [a, b, label] of pairs) {
    if (!a || !b) continue // 没做第二次观察 = 不判不稳定（而是少一道保险，记 hint）
    const norm = (code: number | null) => (code === null ? "null" : code === 0 ? "green" : "red")
    if (norm(a.code) !== norm(b.code)) {
      return {
        verdict: "unstable",
        reason: `${label} 两次同态观察不一致（${norm(a.code)} vs ${norm(b.code)}）——判据在抖，结论不可信`,
        hints,
      }
    }
  }
  if (!obs.original2 || !obs.rolled2) hints.push("只做了一轮观察（没有复跑对照）：抖动无法排除")

  // —— S5（只提示，不判失败）——
  if (spec.rollback.trim().length < 12) {
    hints.push(`rollback 段过短（${spec.rollback.trim().length} 字）——可能不是「一处有意义的实现」，它能变红不代表机制真被验到`)
  }
  if (/^\s*(\/\/|#|\*|<!--)/m.test(spec.rollback)) {
    hints.push("rollback 落在注释行——回退它不会改变行为（本仓「锚点只在注释里」同型）")
  }
  if (!/\.(ts|tsx|js|mjs|cpp|hpp|h|c|py|rs|go)$/.test(spec.file)) {
    hints.push(`rollback 目标不是源码文件（${spec.file}）——回退文档改动的「反向验证」通常没有判别力`)
  }

  return { verdict: "genuine", reason: "四档全过：原样绿、退回红、放回又绿、复跑一致", hints }
}

/** 判定是否构成「可以入账」的通过（只有 `genuine` 算）。 */
export function isPassing(r: GateResult): boolean {
  return r.verdict === "genuine"
}

/**
 * 不可判定 ≠ 失败。`not-run` / `unsupported` / `unstable` 三种既不能入账、
 * 也不该被当成「这单是坏的」——它们要求**补证据或人看一眼**。
 *
 * 为什么要区分：把「没跑」报成「失败」会让人去修一个没坏的东西（本仓
 * 「假退化」的教训：「服务活着」与「子 Agent 能用」是两件事）。
 */
export function isUndecidable(r: GateResult): boolean {
  return r.verdict === "not-run" || r.verdict === "unstable" || r.verdict === "unsupported"
}
