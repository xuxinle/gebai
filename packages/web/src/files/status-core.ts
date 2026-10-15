/**
 * 文件工作台 · 状态栏「模式 / 可写性」格的纯逻辑：输入(模式 + 可写性) → 输出(文案 + 配色类 + 悬浮原因)。
 *
 * 为什么单独成模块：这块曾经只有一行三元表达式——
 *   `item(t.mode === 'edit' ? '' : 'warn', { text: t.mode === 'edit' ? '编辑' : '只读' })`
 * 于是**查看态**（mode === 'view'，用户按 Ctrl+E 随时能进编辑）与**真正不可写**
 * （只读根源 / 超限截断 / 文件类型不可编辑）渲染成同一个橙色的「只读」徽标。
 * 后果是实测最频繁的误判来源：打开一个普通源码文件，状态栏立刻亮出橙色「只读」，
 * 用户以为文件写不了——而那只是「当前不在编辑态」。危险方向相反且更严重：
 * 真不可写时若也只给中性文案，用户会白按几次 Ctrl+E 才意识到要改 GEBAI_FS_WRITE 或下载后编辑。
 *
 * 判据口径**必须与标签栏的编辑按钮、以及 saveTab 的拒绝口径一致**（main.ts `renderTabActions`
 * 里那颗按钮的 disabled），否则会出现「徽标说能编辑、按钮却是灰的」这种自相矛盾。四处输入：
 *   ① `roots.writable === false`：整站只读根源模式（`GEBAI_FS_WRITE=false`，`saveTab` 直接拒）；
 *   ② `truncated`：只加载了前 N MB，为防误保存服务端/编辑器都禁写（见 loadTab 的 warn 横幅）；
 *   ③ `stat.editable === false`：服务端按文件类型/权限判定不可写（`FileStat.editable`）；
 *   ④ 根可写性未知（`rootWritable === null`，根清单还没到）→ 与按钮同口径**先按可写算**，
 *      否则每次冷启动都会闪一个「不可编辑」——那同样是不实提示。
 *
 * **编辑态 + 不可写是真会发生的**（本单闭合的缺口）：标签栏那颗按钮在不可写时是 disabled，
 * 但 `Ctrl+E`（`toggleActiveMode`）不看 `editable` 直接翻模式——截断的大文件按 Ctrl+E
 * 会进到「编辑态」，而编辑器本身仍是只读的（`setReadOnly(mode !== 'edit' || truncated)`），
 * 保存也会被 `saveTab` 的根只读判断或服务端拒绝。此时若仍显示中性的「编辑」，
 * 用户会一直以为改动存得下去。故编辑态也分两支：可保存 → 中性「编辑」；
 * 不可保存 → warn 的「不可保存」（与查看态的「不可编辑」对称：一个说存不下、一个说改不了）。
 *
 * 纯函数、不碰 DOM：文案与配色是验收点（中性 vs warn 可区分），单测钉在这里。
 */

/** 模式格的视觉档位：`""` 中性 / `"warn"` 警示（CSS 里 warn 才是橙色的 `--warning`）。 */
export type StatusTone = "" | "warn"

export interface ModeSlot {
  /** 显示文案 */
  text: string
  /** 配色类（拼到 `.fw-status-item` 上） */
  cls: StatusTone
  /** 悬浮说明（`title`）：必须说清**为什么**，中性态还要给出下一步动作 */
  title: string
}

/** 不可写原因（枚举顺序即展示优先级：先报阻碍最大的那个）。 */
export type ReadOnlyReason = "readonlyRoot" | "truncated" | "notEditable"

/** 逐因文案与下一步动作（按 enum 穷举，避免漏分支时静默给空 title）。 */
const REASON: Record<ReadOnlyReason, { why: string; next: string }> = {
  readonlyRoot: { why: "整站只读模式（服务端 GEBAI_FS_WRITE=false），保存会被直接拒绝", next: "需由服务端开启写权限" },
  truncated: { why: "文件过大、只加载了开头一段，为免保存时把文件截短，写已被禁用", next: "请下载后编辑" },
  notEditable: { why: "该文件类型或权限不支持编辑", next: "可下载后用本地编辑器修改" },
}

/**
 * 判定**真正不可写**的原因；可写返回 null。
 *
 * 优先级 = **先报全局事实，再报文件级事实**：根只读对每一个文件都成立，若让它被
 * `statEditable === false` 抢先，用户会以为「换一个文件就能写」，而实际要改的是服务端开关；
 * 截断与类型不可编辑同为文件级，截断在前（它连内容都不完整）。
 * @param statEditable 文件自身的可写性（`stat.editable`；stat 未到时不传）
 * @param truncated    内容是否被截断（只加载了前一段）
 * @param rootWritable 当前根是否可写：true/false 为已知；**null = 未知（根清单未到）**，按可写算
 */
export function readOnlyReason(o: { statEditable?: boolean; truncated?: boolean; rootWritable?: boolean | null }): ReadOnlyReason | null {
  if (o.rootWritable === false) return "readonlyRoot"
  if (o.truncated) return "truncated"
  if (o.statEditable === false) return "notEditable"
  return null
}

/**
 * 模式格渲染规格。
 *
 * 中性文案取 **「查看」而非「只读」**：即便配色已变中性，`只读` 这个词本身仍在宣称
 * 「文件写不了」，而它与事实相反（Ctrl+E 即可编辑）；「查看/编辑」是一对互斥的状态名，
 * 说的是「现在处于哪个模式」，不夹带任何可写性判断——这才是该格的本义。
 * （代价是同一行里 `查看` 与 `只读模式` 两格并存时略拗口；但两者语义层次不同：
 *   前者是当前模式、后者是站点策略，且后者带 warn 与原因，不会混为一谈。）
 *
 * 不可写时的文案分两个方向、互相对称：**查看态说「不可编辑」（改不了），
 * 编辑态说「不可保存」（存不下）**。后者不能省成「编辑」——那正是本单闭合的缺口（见文件头）。
 */
export function modeSlot(o: { mode: "view" | "edit"; readOnly: ReadOnlyReason | null }): ModeSlot {
  if (!o.readOnly) {
    return o.mode === "edit"
      ? { text: "编辑", cls: "", title: "编辑模式：Ctrl+E 切回查看（本文件可保存）" }
      : { text: "查看", cls: "", title: "查看模式：本文件可编辑，Ctrl+E 进入编辑" }
  }
  const r = REASON[o.readOnly]
  // 编辑态 + 不可写（Ctrl+E 不看 editable，这条路径真会被走到）：必须说清「编辑了也存不回」
  if (o.mode === "edit") return { text: "不可保存", cls: "warn", title: `编辑模式，但本文件存不回：${r.why}；${r.next}（Ctrl+E 切回查看）` }
  return { text: "不可编辑", cls: "warn", title: `不可编辑：${r.why}；${r.next}` }
}

/** 不可写原因的「为什么」（`why`）——按钮 title、状态栏徽标、保存拒绝提示共用同一份措辞。 */
export function readOnlyWhy(r: ReadOnlyReason): string {
  return REASON[r].why
}

/** 不可写原因的「下一步」（`next`）。 */
export function readOnlyNext(r: ReadOnlyReason): string {
  return REASON[r].next
}

/** 标签栏编辑按钮的渲染规格（文案 / 禁用态 / 按下态一次给全）。 */
export interface EditButton {
  /** 图标名：查看态给 `edit`（去编辑），编辑态给 `eye`（回查看） */
  icon: "edit" | "eye"
  /** 悬浮说明：可写时说下一步动作，不可写时说清原因（与状态栏徽标同一份 REASON） */
  title: string
  disabled: boolean
  active: boolean
}

/**
 * 标签栏那颗编辑按钮 ← **与状态栏「查看 / 编辑」格同源同口径**。
 *
 * 收进来的两个实际缺陷（都不报错，只让人白找原因 / 卡住）：
 * ① 原先不可写时 title 一律写「该文件类型不支持编辑」，而不可写有三个来源（根只读 / 截断 / 类型），
 *    根只读（`GEBAI_FS_WRITE=false`）与超大文件截断都会被说成“类型不支持”——用户会去扩展名上找原因，
 *    而真正要做的是改服务端开关或下载后编辑。按钮与徽标说得不一样时，两个都不可信。
 * ② 编辑态**不能**跟着禁用：`Ctrl+E`（`toggleActiveMode`）不看可写性就能进编辑态，按钮若在不可写时
 *    恒 disabled，用户进了编辑态就**退不出来**（唯一退路只剩再按一次 Ctrl+E——而界面上的按钮是灰的，
 *    等于告诉用户“这里关不掉”）。故编辑态一律可点。
 */
export function editButton(o: { mode: "view" | "edit"; readOnly: ReadOnlyReason | null }): EditButton {
  if (o.mode === "edit") {
    return {
      icon: "eye",
      // 不可写时先把「存不回」说在前面，免得用户以为切回查看就等于问题解决了
      title: o.readOnly ? `切换为查看（Ctrl+E）——本文件存不回：${readOnlyWhy(o.readOnly)}` : "切换为查看（Ctrl+E）",
      disabled: false,
      active: true,
    }
  }
  if (!o.readOnly) return { icon: "edit", title: "编辑（Ctrl+E）", disabled: false, active: false }
  return { icon: "edit", title: `无法编辑：${readOnlyWhy(o.readOnly)}；${readOnlyNext(o.readOnly)}`, disabled: true, active: false }
}
