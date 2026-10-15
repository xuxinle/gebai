/**
 * 变异体清单（status-core 判据口径）——**本文件是数据，不是胶水**。
 *
 * 数据与执行分开的理由：变异体是**历史缺陷清单**（下面 A 组 21 条，逐条都是实测踩过的坑：
 * A8「查看态显示只读」、A9「编辑态显示中性编辑」、A12/A13「两颗按钮的禁用态反向要求」……），
 * 它会随时间只增不减地长；而执行胶水（写文件 → 跑测试 → 还原 → 判定）是几十行、几乎不变。
 * 混在一起的结果是：加一条变异体要先读懂整个执行器，于是「发现新缺陷就补一条」这件事被劝退。
 *
 * ## 四条期望（**口径定义在 DESIGN.md「测试策略 → 变异验证」**，此处只复述一句）
 *   - `code`   回归变异：把判据/界面改回旧口径 → **必须被杀**（存活 = 回归失败：这条规则没有任何断言在守）
 *   - `benign` 良性重构：只改写法不改判据 → **必须存活**（被杀 = 误杀，比漏杀更糟：下一个人会去删断言）
 *   - `branch` 类型化新分支：加判据成员而不补矩阵 → **必须被拦**（`tsc` 报缺键，矩阵存在的主要理由）
 *   - `blind`  闭集机关（矩阵的「渲染函数体 text/title 字面量闭集」）**应该**扫出的运行期新分支 → 必须被杀
 *   - `residual` **已知盲区**：新分支的文案与 title 都复用期望表里已有的字面量前缀 → **期望存活**
 *     （如实记录「守不住」这件事，而不是假装守住了；DESIGN.md「已知盲区」一节有它的成因与边界）
 *
 * ## `find` 串的写法（会被执行器强制校验，写错不会静默通过）
 *   - 必须是**源码里的原文逐字**（含缩进、含 `\n`——`find` 里没写 `\n` 就匹配不到跨行片段）；
 *   - 必须在原文件里**恰好出现一次**（`countOccurrences` 会对账；0 次或多次都判为「变异体失效」并计入失败）；
 *   - 改坏的产品文件是 `packages/web/src/files/status-core.ts`（唯一被改的文件，由执行器在**临时副本**上改）。
 *
 * ## 加一条变异体的做法（不需要动执行器）
 *   1. 复制 `status-core.ts` 里的一段原文到 `find`；2. 写出「改坏后」的版本到 `replace`；
 *   3. 选 `kind`：改的是**判据/文案/禁用态**就是 `code`，只是**换写法**就是 `benign`；
 *   4. `bun run mutate:status` —— 新加的 `code` 若存活，就是又发现了一处「测试绿但没守住」。
 */

/** 变异体类别（期望见文件头；`residual` 是唯一「期望存活」的回归类）。 */
export type MutantKind = "code" | "benign" | "branch" | "blind" | "residual"

export interface Mutant {
  /** 编号 + 一句话说明改坏的是什么（它会出现在失败报告里，故要写清「用户会看到什么」）。 */
  name: string
  kind: MutantKind
  /** 原文件里的**原文片段**（逐字，含缩进；必须恰好命中一次）。 */
  find: string
  /** 替换后的片段（`""` = 删掉这段）。 */
  replace: string
}

/** 被改坏的产品文件（相对仓库根）；执行器只碰这一个文件，且只在临时副本上碰。 */
export const TARGET_FILE = "packages/web/src/files/status-core.ts"

/** 判据所在的测试（**只有这一张矩阵**——变异验证问的就是「这张表守不守得住」）。 */
export const MATRIX_TEST = "packages/web/src/files/status-matrix.test.ts"

export const MUTANTS: Mutant[] = [
  /* ================================================================== *
   * A 回归变异（`code`）：把判据 / 界面改回旧口径——**必须被杀**。
   * 逐条都是实测踩过的坑；有存活的，说明那条规则实际**没有任何断言在守**（测试绿是假象）。
   * ================================================================== */
  {
    name: "A1 丢掉「根只读」这一因（saveTab 曾只看根，现在连根都不看）",
    kind: "code",
    find: `  if (o.rootWritable === false) return "readonlyRoot"\n`,
    replace: ``,
  },
  {
    name: "A2 丢掉「截断」这一因（回写会把文件截成已加载的那段 = 真数据损坏）",
    kind: "code",
    find: `  if (o.truncated) return "truncated"\n`,
    replace: ``,
  },
  {
    name: "A3 丢掉「类型/权限不可编辑」这一因",
    kind: "code",
    find: `  if (o.statEditable === false) return "notEditable"\n`,
    replace: ``,
  },
  {
    name: "A4 判序错：截断抢在根只读之前（用户以为换个文件就能写，实际要改服务端开关）",
    kind: "code",
    find: `  if (o.rootWritable === false) return "readonlyRoot"\n  if (o.truncated) return "truncated"\n`,
    replace: `  if (o.truncated) return "truncated"\n  if (o.rootWritable === false) return "readonlyRoot"\n`,
  },
  {
    name: "A5 判序错：类型不可编辑抢在根只读之前",
    kind: "code",
    find: `  if (o.rootWritable === false) return "readonlyRoot"\n  if (o.truncated) return "truncated"\n  if (o.statEditable === false) return "notEditable"\n`,
    replace: `  if (o.statEditable === false) return "notEditable"\n  if (o.truncated) return "truncated"\n  if (o.rootWritable === false) return "readonlyRoot"\n`,
  },
  {
    name: "A6 stat 未到（undefined）被当成不可写（冷启动闪假「不可编辑」）",
    kind: "code",
    find: `if (o.statEditable === false) return "notEditable"`,
    replace: `if (o.statEditable !== true) return "notEditable"`,
  },
  {
    name: "A7 根可写性未知被当成只读（冷启动闪跟文件无关的 warn）",
    kind: "code",
    find: `if (o.rootWritable === false) return "readonlyRoot"`,
    replace: `if (!o.rootWritable) return "readonlyRoot"`,
  },
  {
    name: "A8 查看态文案退回「只读」（本模块存在的理由：宣称文件写不了而事实相反）",
    kind: "code",
    find: `: { text: "查看", cls: "", title: "查看模式：本文件可编辑，Ctrl+E 进入编辑" }`,
    replace: `: { text: "只读", cls: "", title: "查看模式：本文件可编辑，Ctrl+E 进入编辑" }`,
  },
  {
    name: "A9 编辑态 + 不可写退回中性「编辑」（用户以为改动存得下去 = 本单闭合的缺口）",
    kind: "code",
    find: `if (o.mode === "edit") return { text: "不可保存", cls: "warn", title:`,
    replace: `if (o.mode === "edit") return { text: "编辑", cls: "warn", title:`,
  },
  {
    name: "A10 编辑态 + 不可写文案对但退回中性配色（看不见的警告）",
    kind: "code",
    find: `if (o.mode === "edit") return { text: "不可保存", cls: "warn", title:`,
    replace: `if (o.mode === "edit") return { text: "不可保存", cls: "", title:`,
  },
  {
    name: "A11 查看态 + 不可写退回中性配色",
    kind: "code",
    find: `return { text: "不可编辑", cls: "warn", title:`,
    replace: `return { text: "不可编辑", cls: "", title:`,
  },
  {
    name: "A12 编辑按钮在不可写时也禁用（Ctrl+E 不看可写性，此路径真会走到）",
    kind: "code",
    find: `\`, disabled: true, active: false }`,
    replace: `\`, disabled: false, active: false }`,
  },
  {
    name: "A13 编辑态按钮改成禁用（唯一退路只剩 Ctrl+E，界面上却告诉用户这里关不掉）",
    kind: "code",
    find: `      title: o.readOnly ? \`切换为查看（Ctrl+E）——本文件存不回：\${readOnlyWhy(o.readOnly)}\` : "切换为查看（Ctrl+E）",\n      disabled: false,`,
    replace: `      title: o.readOnly ? \`切换为查看（Ctrl+E）——本文件存不回：\${readOnlyWhy(o.readOnly)}\` : "切换为查看（Ctrl+E）",\n      disabled: true,`,
  },
  {
    name: "A14 编辑按钮 title 三因归一（一律「类型不支持」= 用户去扩展名上找原因）",
    kind: "code",
    find: `return { icon: "edit", title: \`无法编辑：\${readOnlyWhy(o.readOnly)}；\${readOnlyNext(o.readOnly)}\`, disabled: true, active: false }`,
    replace: `return { icon: "edit", title: "该文件类型不支持编辑", disabled: true, active: false }`,
  },
  {
    name: "A15 保存项的禁用态不再跟随判据（状态栏说存不回、保存按钮却是亮的）",
    kind: "code",
    find: `return { title: \`保存（Ctrl+S）· 存不回：\${saveRefusalWhy(o.refusal)}\`, disabled: true }`,
    replace: `return { title: \`保存（Ctrl+S）· 存不回：\${saveRefusalWhy(o.refusal)}\`, disabled: false }`,
  },
  {
    name: "A16 保存项 title 只说「存不回」不说是哪一因（同屏两种解释）",
    kind: "code",
    find: `return { title: \`保存（Ctrl+S）· 存不回：\${saveRefusalWhy(o.refusal)}\`, disabled: true }`,
    replace: `return { title: "保存（Ctrl+S）· 存不回", disabled: true }`,
  },
  {
    name: "A17 无改动时也报「存不回」（陈旧/误导提示）",
    kind: "code",
    find: `  if (!o.dirty) return { title: "保存（Ctrl+S）", disabled: true }`,
    replace: `  if (!o.dirty) return { title: \`保存（Ctrl+S）· 存不回：\${o.refusal ? saveRefusalWhy(o.refusal) : "未知"}\`, disabled: true }`,
  },
  {
    name: "A18 拒绝模板把「为什么」与「下一步」调换（句子读起来像把动作顶在原因前）",
    kind: "code",
    find: `: \`无法保存：\${readOnlyWhy(r)}；\${readOnlyNext(r)}\``,
    replace: `: \`无法保存：\${readOnlyNext(r)}；\${readOnlyWhy(r)}\``,
  },
  {
    name: "A19 截断的 why 与「类型不可编辑」归一（历史缺陷：三因说同一句话）",
    kind: "code",
    find: `  truncated: { why: "文件过大、只加载了开头一段，为免保存时把文件截短，写已被禁用", next: "请下载后编辑" },`,
    replace: `  truncated: { why: "该文件类型或权限不支持编辑", next: "请下载后编辑" },`,
  },
  {
    name: "A20 根只读的 why 不再点名 GEBAI_FS_WRITE（用户找不到要改的开关）",
    kind: "code",
    find: `readonlyRoot: { why: "整站只读模式（服务端 GEBAI_FS_WRITE=false），保存会被直接拒绝", next: "需由服务端开启写权限" },`,
    replace: `readonlyRoot: { why: "当前无法写入", next: "需由服务端开启写权限" },`,
  },
  {
    name: "A21 根未就绪不再自成一句（吃掉保存侧唯一的刻意分歧）",
    kind: "code",
    find: `  return r === "rootsPending" ? \`无法保存：\${PENDING}\` : \`无法保存：\${readOnlyWhy(r)}；\${readOnlyNext(r)}\``,
    replace: `  return r === "rootsPending" ? \`无法保存：\${PENDING}；请稍后再试\` : \`无法保存：\${readOnlyWhy(r)}；\${readOnlyNext(r)}\``,
  },

  /* ================================================================== *
   * B 良性重构（`benign`）：只改写法不改判据——**必须存活**。
   * 被误杀的后果更隐蔽：下一个人遇到红测试会去删断言，于是守的东西越改越少。
   * ================================================================== */
  {
    name: "B1 判据条件提成局部变量",
    kind: "benign",
    find: `  if (o.rootWritable === false) return "readonlyRoot"\n  if (o.truncated) return "truncated"\n  if (o.statEditable === false) return "notEditable"\n  return null`,
    replace: `  const rootReadOnly = o.rootWritable === false\n  const isTruncated = Boolean(o.truncated)\n  const noEdit = o.statEditable === false\n  if (rootReadOnly) return "readonlyRoot"\n  if (isTruncated) return "truncated"\n  if (noEdit) return "notEditable"\n  return null`,
  },
  {
    name: "B2 判据改写成等价三元链",
    kind: "benign",
    find: `  if (o.rootWritable === false) return "readonlyRoot"\n  if (o.truncated) return "truncated"\n  if (o.statEditable === false) return "notEditable"\n  return null`,
    replace: `  return o.rootWritable === false ? "readonlyRoot" : o.truncated ? "truncated" : o.statEditable === false ? "notEditable" : null`,
  },
  {
    name: "B3 rootsPending 判据拆成 null / undefined 两半",
    kind: "benign",
    find: `  if (o.rootWritable == null) return "rootsPending"`,
    replace: `  if (o.rootWritable === null || o.rootWritable === undefined) return "rootsPending"`,
  },
  {
    name: "B4 局部名 r → reason（纯改名：声明与**全部**引用同时改）",
    kind: "benign",
    find: `  const r = REASON[o.readOnly]\n  // 编辑态 + 不可写（Ctrl+E 不看 editable，这条路径真会被走到）：必须说清「编辑了也存不回」\n  if (o.mode === "edit") return { text: "不可保存", cls: "warn", title: \`编辑模式，但本文件存不回：\${r.why}；\${r.next}（Ctrl+E 切回查看）\` }\n  return { text: "不可编辑", cls: "warn", title: \`不可编辑：\${r.why}；\${r.next}\` }`,
    replace: `  const reason = REASON[o.readOnly]\n  // 编辑态 + 不可写（Ctrl+E 不看 editable，这条路径真会被走到）：必须说清「编辑了也存不回」\n  if (o.mode === "edit") return { text: "不可保存", cls: "warn", title: \`编辑模式，但本文件存不回：\${reason.why}；\${reason.next}（Ctrl+E 切回查看）\` }\n  return { text: "不可编辑", cls: "warn", title: \`不可编辑：\${reason.why}；\${reason.next}\` }`,
  },
  {
    name: "B5 模式格分支折行重排（prettier 常见形态变化）",
    kind: "benign",
    find: `  if (o.mode === "edit") return { text: "不可保存", cls: "warn", title: \`编辑模式，但本文件存不回：\${r.why}；\${r.next}（Ctrl+E 切回查看）\` }`,
    replace: `  if (o.mode === "edit") {\n    return { text: "不可保存", cls: "warn", title: \`编辑模式，但本文件存不回：\${r.why}；\${r.next}（Ctrl+E 切回查看）\` }\n  }`,
  },
  {
    name: "B6 保存项分支改用显式 !== null（等价）",
    kind: "benign",
    find: `  if (o.refusal) return { title:`,
    replace: `  if (o.refusal !== null) return { title:`,
  },
  {
    name: "B7 取词函数换写法（提取到局部再返回）",
    kind: "benign",
    find: `  return REASON[r].why`,
    replace: `  const entry = REASON[r]\n  return entry.why`,
  },
  {
    name: "B8 徽标 title 模板改成先拼后返回（等价）",
    kind: "benign",
    find: `  return { text: "不可编辑", cls: "warn", title: \`不可编辑：\${r.why}；\${r.next}\` }`,
    replace: `  const title = \`不可编辑：\${r.why}；\${r.next}\`\n  return { text: "不可编辑", cls: "warn", title }`,
  },

  /* ================================================================== *
   * C 类型化新分支（`branch`）：加判据成员而不补矩阵——**必须被拦**。
   * 拦它的是 `tsc`（期望表是 `Record<判据枚举,…>`，缺键即报），不是测试。
   * ================================================================== */
  {
    name: "C1 新增第四因「加密」而不补矩阵（类型层应拦下）",
    kind: "branch",
    find: `export type ReadOnlyReason = "readonlyRoot" | "truncated" | "notEditable"`,
    replace: `export type ReadOnlyReason = "readonlyRoot" | "truncated" | "notEditable" | "encrypted"`,
  },

  /* ================================================================== *
   * D 闭集机关的边界（`blind` 应被扫出 / `residual` 如实存活）
   * 三例都在测同一件事：**`as` 转型绕过类型**造出的运行期新分支，矩阵的
   * 「渲染函数体 text/title 字面量闭集」这一道机关能看见到什么程度。
   * ================================================================== */
  {
    name: "D1 运行期新分支 + 新文案（`as` 转型绕过类型）——闭集机关应扫出未进期望表的「加密」",
    kind: "blind",
    find: `  const r = REASON[o.readOnly]`,
    replace: `  if ((o.readOnly as string) === "encrypted") return { text: "加密", cls: "warn", title: "文件已加密，无法编辑" }\n  const r = REASON[o.readOnly]`,
  },
  {
    name: "D2 运行期新分支但**复用旧文案**（`不可编辑`）而 title 是新的——闭集机关从 title 扫出",
    kind: "blind",
    find: `  const r = REASON[o.readOnly]`,
    replace: `  if ((o.readOnly as string) === "encrypted") return { text: "不可编辑", cls: "warn", title: "文件已加密，无法编辑" }\n  const r = REASON[o.readOnly]`,
  },
  {
    name: "D3 运行期新分支且**文案与 title 都复用旧字面量前缀**——确认为真残余盲区，期望存活",
    kind: "residual",
    find: `  const r = REASON[o.readOnly]`,
    replace: `  if ((o.readOnly as string) === "encrypted") return { text: "不可编辑", cls: "warn", title: "不可编辑：文件已加密" }\n  const r = REASON[o.readOnly]`,
  },
]

/**
 * 每类的**期望**：`true` = 必须被杀，`false` = 必须存活。
 *
 * 写成函数而不是散在汇总处，是为了让「改期望」这个动作显式且有唯一位置——
 * 尤其 `residual`：它期望存活**不是**因为它是良性的，而是因为矩阵确实看不见它
 * （见 DESIGN.md「已知盲区」）。哪天真堵住了，它的期望要改成 `true`，而这一行就是那处改动。
 */
export function expectsKill(kind: MutantKind): boolean {
  return kind === "code" || kind === "branch" || kind === "blind"
}
