/**
 * 文件工作台 · **编辑位置历史**（后退 / 前进）：VSCode `workbench.action.navigateBack/Forward` 同款语义。
 *
 * 为什么是**一条线性栈 + 指针**（不是每文件一条栈）：用户心智里「后退」是沿**时间**回溯
 * 「我到过的地方」——A.ts:100 → B.ts:5（转定义）→ 后退要回 A.ts:100，而不是 B.ts 内部
 * 光标怎么挪过。VSCode / IDEA 的编辑位置历史都是全局线性序，这里对齐。
 *
 * 记录点（哪些动作算「一次导航」——由 main.ts 判定后调 `jump`，本模块只管栈）：
 * - 打开/切换文件（树单击、快速打开、LSP 转定义、深链接、popstate 恢复）；
 * - 同文件**显式**跳行（转到符号、带 line 的再次打开）。
 * 普通光标移动**不**产生记录（那会把栈灌成「光标轨迹」），但会经 `updateTop` 修正栈顶——
 * 「后退回到离开时最后看的位置」正是靠它（见下）。
 *
 * 不跨刷新持久化（sessionStorage 不存栈）：栈是「本次浏览的回溯线」，刷新即重开一条；
 * 刷新回到原处由既有的状态记忆（session-state.ts 的 line）承担，职责不重叠。
 *
 * 为什么单独一个文件：纯数据结构 + 去重/截断规则，零 DOM 零依赖（对照 session-state.ts 的先例），
 * 抽出来就能直接测（见 nav-history.test.ts）。
 */

/** 一个编辑位置：根 id + 根内相对路径 + 行/列（1 起）。 */
export interface NavEntry {
  root: string
  path: string
  /** 1 起始行（与工作台其余口径一致：FwTabState.line / revealLine 的 opts.line） */
  line: number
  /** 1 起始列（0 视为「未给」→ 归一到 1） */
  column: number
}

/** 栈上限：与 VSCode 50 同值。历史是回溯线不是流水账，再长也只是翻得更远。 */
export const NAV_LIMIT = 50;

/** 相邻两次导航的最小行距（同文件内）：小于它的跳过不记——「跳到 102 行」不值得为 2 行单开一条。 */
const NEAR_LINE = 5;

export interface NavHistory {
  /** 当前指针能否后退（标签栏按钮的可用态）。 */
  canBack(): boolean
  /** 当前指针能否前进。 */
  canForward(): boolean
  /** 记一次导航：截断前进段、去重、压栈。恢复/回放期间调用方应自己抑制（见 main.ts 的 navGuard）。 */
  jump(e: NavEntry): void
  /**
   * 用**当前光标位置**修正栈顶：普通移动不产生记录，但「离开这个文件时的最后位置」
   * 应该是后退回来的落点（离开那一刻栈顶定格在 jump 时的行，此后在同一文件里又读了
   * 300 行再切走——按 jump 记的行回，用户看到的是「回到我来过但早就不看的那一行」）。
   * 仅当 e 与栈顶**同文件**时生效（切走之后的不算）。
   */
  updateTop(e: { line: number; column: number; root: string; path: string }): void
  /** 后退一步并返回目标位置（栈顶留在原地——它是「现在」，指针只是移走）；无路可退返回 null。 */
  back(): NavEntry | null
  /** 前进一步并返回目标位置；无路可进返回 null。 */
  forward(): NavEntry | null
  /** 只读视图（测试与调试用）。 */
  peek(): { stack: NavEntry[]; index: number }
}

/**
 * 建位置历史。
 *
 * 指针语义（与浏览器 history 同构，`index` 指向「当前位置」在栈中的槽位）：
 * - `jump`：指针不在栈顶时先**截断**前进段（后退之后再导航，旧的前进分支作废——
 *   与浏览器地址栏行为一致，也避免栈里留着永远到不了的分支）；
 * - `back`/`forward` 只移指针，**不动栈**——前进永远还能回到刚才离开的位置。
 */
export function createNavHistory(): NavHistory {
  let stack: NavEntry[] = [];
  let index = -1;

  function jump(e: NavEntry): void {
    const top = stack[index];
    if (top) {
      // 同一位置（同文件同行）不重复记：标签栏点击、激活已有标签都会「打开」，重复入栈
      // 会让第一次后退退回原地。
      if (top.root === e.root && top.path === e.path && top.line === e.line) {
        top.column = e.column || 1;
        return;
      }
      // 同文件近距离挪动不算导航（符号面板相邻符号、差几行的定位）——合并进栈顶。
      if (top.root === e.root && top.path === e.path && Math.abs(top.line - e.line) < NEAR_LINE) {
        top.line = e.line;
        top.column = e.column || 1;
        return;
      }
    }
    // 截断前进段（见函数注释），再压栈
    stack = stack.slice(0, index + 1);
    stack.push({ ...e, line: Math.max(1, e.line), column: Math.max(1, e.column || 1) });
    if (stack.length > NAV_LIMIT) stack.splice(0, stack.length - NAV_LIMIT);
    index = stack.length - 1;
  }

  function updateTop(pos: { line: number; column: number; root: string; path: string }): void {
    const top = stack[index];
    if (!top || top.root !== pos.root || top.path !== pos.path) return
    if (pos.line >= 1) top.line = pos.line
    top.column = Math.max(1, pos.column || 1)
  }

  function back(): NavEntry | null {
    if (index <= 0) return null
    index -= 1
    return { ...stack[index]! }
  }

  function forward(): NavEntry | null {
    if (index >= stack.length - 1) return null
    index += 1
    return { ...stack[index]! }
  }

  return {
    canBack: () => index > 0,
    canForward: () => index < stack.length - 1,
    jump,
    updateTop,
    back,
    forward,
    peek: () => ({ stack: stack.map((e) => ({ ...e })), index }),
  }
}
