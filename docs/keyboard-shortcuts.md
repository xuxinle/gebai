# 歌白键盘快捷键

这份表由键位表生成口径维护：**唯一来源**是 `packages/web/src/keymap.ts`（机制 + 浏览器冲突判定）+ `keymap-main.ts`（主界面表）+ `sessions.ts` / `files-entry.ts`（主界面运行时注册）+ `files/main.ts`（工作台动作表）+ `files/keymap-wb.ts`（元素级登记）+ `files/term-keys.ts`（终端面板表，含 `validateKeymap` 单测）。页面里的快捷键一览（标题栏轮盘「快捷键」、工作台「更多 → 快捷键」）也由同一张表渲染，不存在第二份手写清单。

## 总则：用常用键，能接管浏览器的就接管

键位取**用户已经在用的那些**——`Ctrl+S` 就是保存、`Ctrl+P` 就是快速打开、`Ctrl+F` 就是过滤、`F5` 就是刷新、`Ctrl+Shift+E/F/G` 切面板、终端 `Ctrl+Shift+C/V` 复制粘贴。早先「全员 `Ctrl+Alt+*`」的时代结束了：那一族整体腾空留作他用，也**不设备用键**（一个功能一个键）。

为什么敢抢浏览器快捷键——因为 Chromium 的按键流转是**页面优先**的（读了源码，不是推测）：

| 环节 | 源码 | 含义 |
|---|---|---|
| 按键先给页面 | `chrome/browser/ui/views/frame/browser_view.cc` → `PreHandleKeyboardEvent()`：注释原文 *"if the accelerator is associated with the browser, and it is a reserved one (e.g. Ctrl+w), process it … if not a reserved one, do nothing"*，非保留命令返回 `NOT_HANDLED_IS_SHORTCUT` | 页面 `preventDefault()` 即接管，浏览器动作不再执行 |
| 少数保留命令 | `chrome/browser/ui/browser_command_controller.cc` → `IsReservedCommandOrKey()` | 浏览器在把按键交给页面**之前**就处理掉，页面收不到事件 |
| 桌面形态豁免 | 同上：`TYPE_APP` / `TYPE_APP_POPUP` 直接 `return false`（"no keys are reserved"） | PWA / WebView2 / `--app` 窗口里**一条都不保留**，`Ctrl+N/T/W` 全归歌白 |

判据写成了可执行形式 `browserConflict(spec)`，返回三档：

- `free`：浏览器与系统都没绑（`Alt+字母`、裸键 `Y`/`N`、`F2`/`F8`/`F9`…）；
- `override`：浏览器有默认行为但**按键先到页面**——表里必须显式声明 `browser: "override"`，且必须真拦截默认行为；
- `reserved`：浏览器自己吞掉（页面拿不到）——**一律不入表**（见下）。

键位只有一套：要在所有形态下都按得动，就不能用页面收不到的键。`keymap.test.ts` 遍历全表断言「无保留键、接管声明齐备」（漏写或用错键直接变红），帮助 UI 会给接管的键位标一个「接管 xx」（如「接管 打印」）。

## 主界面

| 快捷键 | 作用 |
|---|---|
| `Alt+N` | 新建会话（进入草稿页；已在草稿页时无操作，防误触清草稿）· 为什么不用 `Ctrl+N` 见下 |
| `Ctrl+B` | 折叠 / 展开会话列表（窄屏为滑动抽屉）· 接管浏览器「书签（Firefox 侧栏）」 |
| `Ctrl+\` | 开关文件工作台：会话桌面 → **并列**（文件工作台停靠一侧，可在工作台「更多」菜单里换侧）→ 回会话桌面；窗口容不下并列时它是**全屏**开关，全屏态下它关闭文件工作台——连按两次总能回到会话桌面 · 工作台里那一侧的 `Ctrl+\` 是「关闭文件工作台」（并列 / 全屏都适用） |
| `Y` / `N` | 审批：通过 / 拒绝最早等待的卡片（无输入焦点时；有修饰键或长按不触发） |
| `Ctrl+Enter` | 中断插入提交（运行中取消当前循环后立即执行；空闲等同发送） |
| `Enter` / `Shift+Enter` | 输入框内：发送 / 换行 |
| `↑` / `↓` | 输入框内：浏览用户级输入历史（空输入进入，`↓` 回到底部恢复草稿） |
| `←→↑↓` / `Home` / `End` / `Enter` / `Space` | 消息分段条获焦后：移动焦点、跳到首/末分段、跳到选中分段 |
| `Esc` | 关闭最上层浮层（确认框 / 输入框 / 主题面板 / 会话菜单 / 文件预览 / 图表与 HTML 查看器 / 动作轮盘 / 输入建议） |

## 文件工作台（`/files` 页与分屏 iframe）

| 快捷键 | 作用 | 接管/冲突说明 |
|---|---|---|
| `Ctrl+S` | 保存（文件内容 / 合并结果 / 暂存结果，按活动标签分派） | 接管「保存网页」 |
| `Ctrl+P` | 快速打开文件（VSCode 式模糊搜文件名；空查询显示最近打开）——面板内 `↑`/`↓` 选、`Enter` 开预览标签、`Ctrl+Enter` 固定为常驻标签 | 接管「打印」 |
| `Ctrl+Shift+O` | 转到符号（**当前文件内**）：焦点在编辑器外时打开工作台的符号面板（`↑`/`↓` 选、`Enter` 跳；状态栏标出结果来自「语法树」还是「词法规则」），编辑器内由 Monaco 自带的大纲接管 | 接管「书签管理器」——仅焦点不在编辑器内时接管（编辑器内让给 Monaco：TS/JS/JSON/CSS/HTML 由内置语言服务出符号，其余语言仍走面板） |
| `Alt+W` | 关闭当前标签 | 为什么不用 `Ctrl+W` 见下 |
| `Ctrl+E` | 查看 ↔ 编辑模式 | 接管「地址栏搜索」 |
| `Ctrl+F` | 资源管理器：在当前目录过滤 | 接管「页面查找」；焦点在 Monaco 编辑器内时让位（那是编辑器自己的查找） |
| `Ctrl+B` | 显示 / 隐藏左侧栏 | 接管「书签（Firefox 侧栏）」 |
| `Ctrl+Shift+E` / `Ctrl+Shift+F` / `Ctrl+Shift+G` | 显示资源管理器 / 搜索视图 / 左侧变更面板 | 接管 `Ctrl+Shift+E`（Firefox 网络监视器）等 |
| `Ctrl+K` | 「更多」菜单（新建 / 比较 / 重新加载 / 快捷键 / 服务端开关 / 浏览器全屏 / 停靠侧 / 关闭文件工作台） | 接管「地址栏搜索」 |
| `Ctrl+\` | 关闭文件工作台（回到会话工作台；并列 / 全屏都是它）——嵌入态（分屏 iframe）才有的键 | 与宿主主界面的 `Ctrl+\` 同一个键：那边管「开关」，面板里这个管「关闭」 |
| ``Ctrl+` `` | 底部终端工具窗 | — |
| `F5` | 刷新资源管理器与 Git 状态 | 接管「刷新页面」；`Ctrl+R` 仍留给浏览器做整页刷新 |
| `Ctrl+Shift+D` | 比较（任意两个提交 / 提交与工作区） | 接管 `Ctrl+Shift+D`（Firefox 收藏全部标签页） |
| `Alt+G` | 底部 Git 工具窗（G = Git） | — |
| `F7` / `Shift+F7` | 差异视图：下一处 / 上一处差异（同一文件内） | 接管 `F7`（光标浏览）；与 Monaco diff 的 F7 同义，由歌白统管计数 |
| `F8` / `Shift+F8` | 下一处 / 上一处：下一个变更文件（差异审视）或下一处冲突（合并视图） | VSCode「下一个问题」同款；按当前视图分派 |
| `Alt+M` | 合并视图：标记为解决（`git add`；M = 标记） | — |
| `F2` | 重命名选中项（资源管理器） | — |
| `Ctrl+C` | 资源管理器：复制选中项（右键「复制」同义） | 只在资源管理器为活动区且焦点不在编辑器/终端/输入框内时接管；其余场合仍是文本复制 |
| `Ctrl+V` | 资源管理器：粘贴到选中目录（选中文件则贴到它的父目录） | 同上；粘贴不覆盖：同名时落成「xxx - 副本」 |
| `Alt+Z` | 切换自动换行 | VSCode 同款；捕获阶段接管 |
| `Ctrl+Enter` | 提交信息框内：提交 | — |
| `Esc` | 关闭菜单 / 弹窗 / 浮层（只关最上层） | — |

> 接管浏览器默认的绑定一律走**捕获阶段**：Monaco 自己的快捷键服务会先吃掉一部分组合（`Ctrl+K` 是多键组合的前缀、`F7` 是 diffReview），冒泡阶段来不及。

## 终端面板内（焦点在终端时，工作台全局键一律让位）

| 快捷键 | 作用 | 接管/冲突说明 |
|---|---|---|
| `Ctrl+Shift+C` / `Ctrl+Insert` | 复制选区 | 接管「粘贴为纯文本」等；DevTools 打开时 `Ctrl+Shift+C` 会被它的「审查元素」抢走（已知取舍） |
| `Ctrl+Shift+V` / `Shift+Insert` | 粘贴（中键粘贴同效） | 单行与多行都按 shell 的 bracketed paste 语义落入命令行，不自动执行 |
| `Ctrl+F` | 搜索滚动缓冲（`Aa` / `ab` / `.*` 三开关 + 第 n/m 项计数） | 接管「页面查找」 |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | 字号 +1 / −1 / 复位 | 接管「页面缩放」（仅在终端获焦时；别处页面缩放照常） |
| `Ctrl+滚轮` | 字号缩放（可在设置里关） | 不接管的话会落到浏览器的整页缩放上 |
| `Ctrl+Shift+K` | 清屏（连回放缓冲一起清） | 接管「地址栏搜索」的近邻；**不用 `Ctrl+K`**——那个键归 shell（readline 删至行尾），VSCode 在 Windows/Linux 上也不给清屏绑键 |
| `Ctrl+Shift+A` | 全选滚动缓冲 | — |
| `Ctrl+Shift+\`` | 新建终端（直接建，选 shell 走标题栏「＋」） | — |
| `Alt+Shift+W` | 关闭当前终端（有输出在跑时先确认） | 工作台全局的 `Alt+W` 在终端内让位给 shell，故另取一键 |
| `Ctrl+Shift+↑` / `Ctrl+Shift+↓` | 上一个 / 下一个终端标签 | VSCode 用的 `Ctrl+PageUp/PageDown` 是 Chromium 保留组合，页面收不到事件 |
| `Ctrl+Shift+Home` / `Ctrl+Shift+End` | 滚动到顶 / 底 | — |
| `Ctrl+C` | 中断当前命令（有选区时改为复制） | 浏览器不独占它（C 是编辑键），终端的中断语义就建在它上面 |
| `Enter`（进程已结束时） | 重启该终端 | 与 VSCode 一样不用先关再建 |
| `Ctrl+L` | 清屏（仅降级终端，xterm 侧由 PTY 自处理） | 接管「聚焦地址栏」 |
| `Enter` / `↑` `↓` | 执行命令 / 命令历史（降级终端的输入框内） | — |
| `Esc` | 关闭终端搜索框；面板菜单/下拉开着时先关浮层 | 终端里的 `Esc` 归 shell（vim 退出插入模式等），只有浮层遮住面板时才归歌白 |

## 哪些键真的拿不到（所以一个都没用）

只有下面这一小撮——它们是 Chromium 的**保留命令**（`IsReservedCommandOrKey()`），浏览器在把按键交给页面之前就处理掉，`preventDefault` 无效、页面根本收不到事件：

| 键 | 浏览器行为 |
|---|---|
| `Ctrl+N` / `Ctrl+T` / `Ctrl+W` | 新窗口 / 新标签页 / 关闭标签页 |
| `Ctrl+Shift+N` / `Ctrl+Shift+T` / `Ctrl+Shift+W` | 无痕窗口 / 恢复刚关闭的标签页 / 关闭窗口 |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` / `Ctrl+PageUp` / `Ctrl+PageDown` | 切换标签页 |
| `Ctrl+Shift+Q` | 退出浏览器（Linux/ChromeOS） |
| `Alt+F4` / `Ctrl+Alt+Del` / `Ctrl+Shift+Esc` | 系统级（关窗口 / 安全选项 / 任务管理器） |

判定范围同样照抄源码，三点很重要：

1. **这套保留只在普通浏览器窗口里生效**。`TYPE_APP` / PWA / 桌面形态（`gebai-desktop.exe` 的 WebView2 窗口、`--app` 窗口）下 `IsReservedCommandOrKey()` 直接 `return false`（"no keys are reserved"），上面的键全归页面；浏览器全屏时也只有 fullscreen/exit 保留。
2. 但**键位表不据此分叉**：在浏览器窗口里按不动的键等于没这个功能，所以新会话用 `Alt+N`、关标签用 `Alt+W`——同一套键在所有形态下都按得动（PWA 应用窗口只是额外让地址栏与浏览器手势退场）。
3. 浏览器自己的 `Ctrl+W` 仍会关掉整个歌白页面，而页面拦不住它——所以工作台有**离开确认**：有未保存的改动时（文件标签 dirty / 合并或暂存视图未保存），关标签页、刷新、跳转都会先弹「离开此网站？」；没有改动就不打扰（标签与会话状态本来就在 `sessionStorage` 里，重新打开即原样）。

其余浏览器快捷键（`Ctrl+S/P/F/K/E/B/L/D/O/U`、`Ctrl+=/-/0`、`Ctrl+Shift+*`、`F5`、`Ctrl+1..9`…）都是**可接管**的：按键先到页面，歌白的 `preventDefault` 让浏览器动作不再执行。

### macOS 上的 Alt 组合（Option 死键）

macOS 的 Option 是字符组合键：`Option+N` 得到的 `e.key` 是 `ñ`、`Option+Z` 是 `Ω`——只比字符的话 `Alt+N`/`Alt+Z` 在 Mac 上永远命中不了。所以 `matchKey()` 在字符不匹配时回退比**物理键位**（`e.code` 的 `KeyN`/`KeyZ`），仅对带 Alt 的组合启用（其余组合的 `e.key` 本来就可靠，不引入误判面）。`Option+N` / `Option+Z` / `Option+W` 在 Mac 上照常可用。

## 装成应用（PWA）

浏览器里可以把歌白装成**应用窗口**：地址栏、标签栏与浏览器自带手势全部退场，界面只剩歌白自己。

- 入口：**浏览器自带**——Chromium 地址栏的安装图标，或菜单「安装歌白…」（页面不再自建安装按钮，见下）；`manifest.webmanifest` 具备即出现，装出来的同样是应用窗口。
- 图标：就是**原图标本身**——`packages/web/public/icons/icon-192.png`、`icon-512.png`（由 `public/favicon.svg` 栅格化，蓝底脑形），manifest 里按 `any` 声明：不套深色底板、不留 `maskable` 安全区内边距，否则桌面安装出来又会是一圈「边框」感。
- 前提：Chromium 系浏览器要求**安全上下文**——`http://127.0.0.1` / `localhost` 满足，局域网 IP 访问需 `https` 才能安装；iOS Safari 不派发安装事件（其路径是「分享 → 添加到主屏幕」）。
- **键位不因形态而变**：装成应用只是让浏览器 UI 与手势退场，快捷键还是本文这一套（本来就没有用页面拿不到的键）。

## 焦点守卫与分发规则

守卫集中在分发器（`keymap.ts` 的 `createKeymap`）：

- 焦点在**终端面板**内时，工作台全局键一律不接管——shell 的 readline 键（`Ctrl+W` 删词、`Ctrl+P` 上一条、`Ctrl+E` 行尾、`Ctrl+K` 删至行尾、`Ctrl+B` 光标左移）全部回归终端；
- 焦点在**输入框**内时，**接管浏览器默认的全局键照常生效**（否则在提交信息框里按 `Ctrl+S` 弹出的是浏览器的「保存网页」）；只有编辑器查找类（`Ctrl+F`）在输入框里让位；
- **主界面例外**：那里的默认焦点就是聊天输入框（进草稿页/切会话/回答结束都会自动聚焦），所以会话语的带修饰键快捷键（`Alt+N`、`Ctrl+B`、`Ctrl+\`）在输入框内照常生效；不带修饰键的 `Y`/`N` 仍排除输入框（打字 `y`/`n` 不应误批）；
- **输入法组合态**（中文候选）一律不接管，`Enter`/`Esc` 不会被当成发送或关闭；
- **长按重复**默认不触发动作；
- **`Esc` 只关最上层**：弹窗、菜单、查看器、下拉都以「作用域」入栈，`Esc` 命中的是栈顶那一个。

## 旧 → 新对照

| 功能 | 旧 | 新 |
|---|---|---|
| 主界面 · 新会话 | `Ctrl+Alt+N` → `Ctrl+N` | `Alt+N`（`Ctrl+N` 是浏览器保留命令，页面收不到） |
| 主界面 · 会话列表 | `Ctrl+Alt+B` | `Ctrl+B` |
| 主界面 · 文件工作台 | `Ctrl+Alt+E` | `Ctrl+\` |
| 工作台 · 保存 | `Ctrl+Alt+S` | `Ctrl+S` |
| 工作台 · 快速打开 | `Ctrl+Alt+O` | `Ctrl+P`（模糊搜文件名 + 最近打开） |
| 工作台 · 关闭标签 | `Ctrl+Alt+W` → `Ctrl+W` | `Alt+W`（`Ctrl+W` 是浏览器保留命令，页面收不到） |
| 工作台 · 查看 ↔ 编辑 | `Ctrl+Alt+E` | `Ctrl+E` |
| 工作台 · 左栏显隐 | `Ctrl+Alt+B` | `Ctrl+B` |
| 工作台 · 资源管理器 / 搜索 / 变更 | `Ctrl+Alt+1/2/3` | `Ctrl+Shift+E` / `Ctrl+Shift+F` / `Ctrl+Shift+G` |
| 工作台 · 目录过滤 | `Ctrl+Alt+F` | `Ctrl+F` |
| 工作台 · 更多菜单 | `Ctrl+Alt+K` | `Ctrl+K` |
| 工作台 · 底部终端 | `Ctrl+Alt+T` | ``Ctrl+` `` |
| 工作台 · 底部 Git 工具窗 | `Ctrl+Alt+G` | `Alt+G` |
| 工作台 · 比较 | `Ctrl+Alt+D` | `Ctrl+Shift+D` |
| 工作台 · 刷新 | `Ctrl+Alt+R` | `F5` |
| 工作台 · 差异块导航 | `Ctrl+Alt+↓` / `Ctrl+Alt+↑` | `F7` / `Shift+F7` |
| 工作台 · 跨文件 / 冲突导航 | `Ctrl+Alt+→` / `Ctrl+Alt+←`、`F8` / `F9` | `F8` / `Shift+F8` |
| 工作台 · 标记为解决 | `Ctrl+Alt+M` | `Alt+M` |
| 终端 · 复制 / 粘贴 | `Ctrl+Alt+C` / `Ctrl+Alt+V` | `Ctrl+Shift+C` / `Ctrl+Shift+V` |
| 终端 · 搜索 | `Ctrl+Shift+F` | `Ctrl+F` |
| 终端 · 字号 | `Ctrl+Alt+=` / `-` / `0` | `Ctrl+=` / `Ctrl+-` / `Ctrl+0` |
| 终端 · 清屏（降级） | `Ctrl+Alt+L` | `Ctrl+L` |
| 保持不变 | `Y`/`N`、`Enter`、`Shift+Enter`、`Ctrl+Enter`、`↑↓`、`Alt+Z`、`F2`、`Ctrl+C`（终端中断；编辑器/输入框内的文本复制）、`Esc` | |

## 新增或修改一个键位

1. 先查 `browserConflict("你的键")`：
   - `free` → 直接用；
   - `override` → 加 `browser: "override"`，并确认分发器真会拦截默认行为（`intercept` 不能为 `false`）；
   - `reserved` → **不能用**（页面收不到，键位只有一套），另取可达的键（`Alt+W` / `Alt+N` 就是这么来的）。
2. 接管浏览器默认的绑定用 `phase: "capture"`（抢在 Monaco / xterm 之前）；焦点范围按用途选 `FOCUS_ALL_FIELDS`（含输入框）/ `DEFAULT_FOCUS`（普通区域与编辑器）/ `["terminal"]` / `["other"]`。
3. 帮助 UI 与本文档**不需要手改键位数据**（`helpGroups()` 从表生成），但要顺手更新这页的对照表；
4. 提示文案（按钮 `title`/`data-tip`、菜单项 `shortcut`、占位符）仍需手改，保持「动作（快捷键）」两句式；
5. 跑 `bun test`（`keymap.test.ts` 会校验重复、接管声明、**无保留键**、`Alt+N`/`Alt+W` 已就位与「`Ctrl+Alt` 族已腾空」）。
