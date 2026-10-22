/**
 * 主界面（index.html）的键位表：汇总各模块声明的绑定、安装分发、登记为活跃表。
 *
 * 分工：键位族与守卫规则在 `keymap.ts`，具体动作留在各自模块（会话 / 审批 / 分屏…）——
 * 本文件只做汇总，不让业务动作堆成一个上帝模块。需要闭包状态的绑定（侧栏折叠、草稿页）
 * 由 `sessions.ts` 在自己的初始化函数里 `mainKeymap.addAll` 注册。
 */
import { approvalBindings } from "./approvals"
import { splitBindings } from "./files-entry"
import { createKeymap, setActiveKeymap, type KeyBinding, type KeyTarget } from "./keymap"

/**
 * 元素级键位登记（`owned: false`，不参与分发）：这些按键由各自模块在元素上直接监听——
 * 语义与具体控件绑定（输入框里的发送与历史、段条的焦点移动、列表行的 Enter），
 * 收进 document 分发只会把简单的事做复杂。登记在此是为了让帮助 UI 与文档完整，
 * 每条 note 给出实现位置。
 */
const elementBindings: KeyBinding[] = [
  {
    id: "main.composer.send",
    keys: "Enter",
    label: "发送消息",
    group: "main.composer",
    note: "输入框内（composer.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.composer.newline",
    keys: "Shift+Enter",
    label: "输入换行",
    group: "main.composer",
    note: "输入框内（composer.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.composer.interrupt",
    keys: "Ctrl+Enter",
    label: "中断插入提交（空闲时等同发送）",
    group: "main.composer",
    note: "输入框内（composer.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.composer.historyPrev",
    keys: "↑",
    label: "上一条输入历史",
    group: "main.composer",
    note: "输入框内、空输入时进入（composer.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.composer.historyNext",
    keys: "↓",
    label: "下一条输入历史（回到底部恢复草稿）",
    group: "main.composer",
    note: "输入框内（composer.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.nav.next",
    keys: ["↓", "→"],
    label: "消息段条：下一个分段",
    group: "main.nav",
    note: "段条获焦后（msg-nav.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.nav.prev",
    keys: ["↑", "←"],
    label: "消息段条：上一个分段",
    group: "main.nav",
    note: "段条获焦后（msg-nav.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.nav.first",
    keys: "Home",
    label: "消息段条：跳到首个分段",
    group: "main.nav",
    note: "段条获焦后（msg-nav.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.nav.last",
    keys: "End",
    label: "消息段条：跳到末尾分段",
    group: "main.nav",
    note: "段条获焦后（msg-nav.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.nav.jump",
    keys: ["Enter", "Space"],
    label: "消息段条：跳到选中分段",
    group: "main.nav",
    note: "段条获焦后（msg-nav.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.session.multiselect",
    keys: "Ctrl+Click",
    label: "会话列表：Ctrl+点击 追加选中 / Shift+点击 选中区间；分类行勾选框或 Ctrl+点击分类名 = 选中该分类下全部会话",
    group: "main.session",
    note: "会话行上（sessions.ts；也可用右键「选中」进入多选）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.session.renameSave",
    keys: "Enter",
    label: "会话重命名：保存",
    group: "main.session",
    note: "重命名输入框内（sessions.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.session.renameCancel",
    keys: "Esc",
    label: "会话重命名：取消",
    group: "main.session",
    note: "重命名输入框内（sessions.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.todo.add",
    keys: "Ctrl+Enter",
    label: "待办：新增（Shift+Enter 添加为闲时任务）",
    group: "main.nav",
    note: "待办弹窗输入框内（todo-pop.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.fileLink.open",
    keys: ["Enter", "Space"],
    label: "文件卡片：打开预览",
    group: "main.nav",
    note: "文件链接 chip 获焦后（file-link.ts）",
    owned: false,
    run: () => {},
  },
  {
    id: "main.scroll.unfollow",
    keys: ["↑", "PageUp", "Home"],
    label: "解除消息流「粘底跟随」",
    group: "main.nav",
    note: "页面任意处（sticky-follow.ts；刻意不阻止浏览器原生滚动）",
    owned: false,
    run: () => {},
  },
]

export const mainKeymap = createKeymap([...approvalBindings, ...splitBindings, ...elementBindings])

/** 安装：接管 document 的 keydown，并登记为活跃表（浮层作用域从此可用）。 */
export function installMainKeys(target?: KeyTarget): void {
  setActiveKeymap(mainKeymap)
  mainKeymap.install(target)
}
