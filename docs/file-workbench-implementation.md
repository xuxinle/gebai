[输出超长，已截断，完整内容见文件: tmp/truncated/read_84f9e89b9e0f97a99929529ce3976f5cf3c22705001dc01993e7f14d094b0738.txt]

# 文件工作台（File Workbench）实现说明

> 姊妹篇：设计稿 `docs/file-workbench-design.md`（需求分析、方案取舍、分阶段路线）。
> 本文是**as-built 记录**：实际落点、关键契约、与设计稿的偏差、验证方式。
>
> 状态：已实现并验证（单元/契约测试 + 真实浏览器冒烟 + 全量回归）。

## 与设计稿的偏差（实现时按代码现状收敛）

| 设计稿 | 实际落地 | 原因 |
|---|---|---|
| 审计在 `core/audit/fs-audit.ts` | `core/fs/audit.ts` | 与 fs 域内聚，避免 core 下散落小目录 |
| 根类型 `sess/proj/user/abs` | 增加 `bind:<agent>` | 会话绑定项目（`{AGENT}_PROJECT`）需要独立根 id |
| 差异视图直接看 patch | 优先 Monaco DiffEditor（两侧真实文本），patch 仅作降级 | CRLF/编码差异下的表现更正确 |
| `routes/git.ts` 用 `path` 定位仓库子目录 | 改用独立 `dir` 参数 | `path` 在 diff/compare/content 里是 pathspec，混用会把文件当目录 |
| — | 新增「目录限定」语义（Git 面板与比较视图默认限定 root 子目录，可切整仓库） | 会话工作区常是仓库子目录，整仓库变更噪音大 |
| — | 新增会话启动遮罩（`gb-splash`）的移除逻辑与端点选择器 Esc/Enter 关闭 | 冒烟测试中发现的真实缺陷 |

## 1. 需求 → 设计映射

| # | 需求 | 落地设计 | 关键实现 |
|---|------|----------|----------|
| 1 | 目录树，可打开文件夹与预置项目 | 「根」抽象 + 懒加载树 + 根选择器 | `core/fs/roots.ts`（`sess:`/`proj:`/`bind:`/`user:`/`abs:`）、`files/explorer.ts` |
| 2 | Monaco（VSCode 同款）查看编辑 + 语法高亮 | Monaco AMD 本地加载，语言按扩展名推断，diff 编辑器同源 | `files/editor.ts`、`files/main.ts:languageOf`、`public/vendor/monaco` |
| 3 | 默认只读，点按钮才进编辑 | 状态机 `view ⇄ edit`，未进编辑态时编辑器 `readOnly: true`、编辑器不产生脏状态 | `files/main.ts`（状态栏「只读/编辑」）、`files/editor.ts` |
| 4 | 图片/视频/PDF/WPS/代码都能看，仅代码可编辑 | 查看器链：文本编辑器 / 图片（缩放平移）/ 视频音频（Range）/ PDF 内嵌 / Office 转换（docx·xlsx·pptx→预览）/ 压缩包列表 / 二进制 hex / 图表；仅 `kind=text\|diagram` 开放编辑 | `files/viewers.ts`、`core/fs/mime.ts`、`routes/fs.ts /office /archive` |
| 5 | 支持下载 | 单文件流式下载（`Content-Disposition`、Range）+ 目录/多选打包 ZIP（UTF-8 文件名）+ 上传（拖拽/粘贴/多选） | `core/fs/service.ts:zipPaths`、`core/fs/archive.ts`、`routes/fs.ts:/download /upload` |
| 6 | 完备 Git 图形操作，界面参考 IDEA | 右侧 VCS 工具窗 + 差异编辑器 + 日志/分支/标签/暂存/远程五视图 + 提交框 + 上下文菜单；写操作全部带备份/冲突提示 | `files/git.ts`（955 行）、`core/git/service.ts`、`routes/git.ts` |
| 7 | 独立页面与路径，入口在主界面轮盘按钮左侧 | `/files` 独立 HTML 入口（Vite 多入口），标题栏轮盘左侧新增「文件」按钮（新标签打开，透传 session/root/project/path/主题） | `packages/web/files.html`、`vite.config.ts`、`files-entry.ts`、`app.ts`（`/files` 静态路由） |
| 8 | 彻底摆脱 VSCode 且更好 | 会话工作区 / 预置项目 / 绑定项目 / 任意本地目录统一为「根」；编辑带乐观锁 + 编码/换行保真；Git 侧支持任意两端对比、逐行暂存、冲突三方查看 | 见下文各节 |

---

## 2. 架构总览

```
┌────────────────────────────── 浏览器标签：/files（独立页面） ──────────────────────────────┐
│  菜单栏（文件/编辑/查看/Git/帮助）  ·  全局搜索（Ctrl+Shift+F）· 命令面板（Ctrl+Shift+P）      │
│ ┌────────────┬────────────────────────────────────────────┬──────────────────────────────┐ │
│ │ 左栏        │ 标签页 + 面包屑 + 工具栏                     │ 右侧 Git 工具窗（可折叠/拖宽）│ │
│ │ · 根选择器  │  · Monaco 编辑器（只读→编辑）                │  · 变更（分组/逐行暂存）      │ │
│ │ · 资源管理器│  · Monaco Diff（并列/内联，任意两端）        │  · 日志（图/过滤/右键操作）   │ │
│ │ · 搜索      │  · 查看器：图片/视频/PDF/Office/压缩包/hex   │  · 分支 / 标签 / 暂存 / 远程  │ │
│ └────────────┴────────────────────────────────────────────┴──────────────────────────────┘ │
│  状态栏：根 · 分支 · 变更计数 · 编码/换行 · 语言 · 只读/编辑 · 光标 · 大小 · 修改时间 · 引擎   │
└──────────────────────────────────────────────────────────────────────────────────────────┘
                                     │ HTTPS（Bearer/Basic/Local）
┌────────────────────────────────────▼─────────────────────────────────────────────────────┐
│ packages/server                                                                          │
│  routes/roots.ts    GET /api/v1/roots[/resolve]        根清单与解析                        │
│  routes/fs.ts       /api/v1/fs/*                       列举/树/读/写/上传/下载/搜索/压缩/回收站 │
│  routes/git.ts      /api/v1/git/*                      状态/差异/对比/日志/分支/提交/网络操作  │
│  core/fs/{roots,service,write,archive,mime,audit}.ts   根解析·列举·解码·写保护·打包·类型·审计 │
│  core/git/service.ts                                   全量 git 命令封装（只读+写，带备份）  │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

### 2.1 安全与权限模型（先定边界，再谈功能）

- **根（root）是权限边界**：`resolveRoot()` 把 `sess:<id>` / `proj:<name>` / `bind:<agent>` / `user:` / `abs:<path>` 解析为绝对路径；`abs:` 在服务模式（沙箱）下一律拒绝，只允许注册的项目根与用户目录。
- **两条写开关**：`GEBAI_FS_WRITE=false` 全局只读；git 另有 `GEBAI_GIT_WRITE` / `GEBAI_GIT_REMOTE`（远程操作单独放行）。

...（省略 141 行）...

- **便捷动作**：消息文件链接、搜索结果、Git 变更一键跳转 `/files?root=…&path=…`；支持复制当前文件的「工作台链接」给他人。

### 5.1 消息流 → 工作台（第二轮补入）

入口不止标题栏那一个：**消息流里的文件产物本身就是入口**。

| 位置 | 按钮 | 说明 |
|---|---|---|
| 文件链接 chip（弹窗查看模式） | 「在文件工作台中打开」+ 下载 | chip 本体仍点开预览弹窗，两个图标按钮 `stopPropagation` 不触发它 |
| 文件卡工具栏（嵌入模式，`code`/`file` 块） | 「在文件工作台中打开」+ 复制 + 原文件 | hover 渐显的工具栏内 |
| 原文件弹窗标题栏 | 「在文件工作台中打开」+ 下载 + 全宽 | 预览后想直接改：一键抳进 IDE |

- **会话归属不漂移**：chip 显式携带**渲染时**的会话 id（`opts.session`），历史消息里的产物不会因当前会话切换而开错工作区。
- **主界面不解析根**：只把原始路径透传（`/files?session=…&path=…`），由工作台 `files/deeplink.ts` 定位所属根。原因：产物路径有两类（会话相对 `tmp/` 逻辑路径 / 项目绝对路径），主界面若自行匹配根，就要复制一份根清单与匹配规则，根的增删/改名都会让它失配。
- **定位规则（三档优先级）**：① 显式 `?root=`/`?project=`；② 绝对路径 → **最长前缀匹配**的根（平局时按类型：proj/bind > sess > user > abs，因为本地模式的 `abs:` 白名单根常与注册项目指向同一目录，不应抢占项目根）；③ 其余 → `?session=` 的会话根（无参数默认项目根 → 绑定根 → 第一个根）。`?line=` 打开后跳行；`tmp/` 前缀自动剥离。
- **可测性**：解析逻辑抽为独立纯函数模块（参数可注入 `search`/`isWin`、无 DOM 依赖）——`main.ts` 顶层会建 DOM 与启动，无法直接对其单测；抽离后得 23 例单测（显式根 / 嵌套根取最精确 / Windows 盘符大小写 / temp 前缀 / 中文空格路径 / 无匹配回退）。
- **实测（Playwright，真实服务）**：

```
① 绝对路径 /workspaces/gebai/packages/server/src/app.ts → 打开 app.ts（41 行，typescript，只读）
② tmp/ 相对路径（带前缀）+ line=3              → 会话工作区，光标落在行 3
③ 仅 ?session=                                 → 本会话工作区（入口按钮形态）
④ 无参数                                       → 项目/绑定根树（15 条目）
控制台/HTTP 错误：无
```


### 5.2 冲突合并（三窗格，第三轮补入）

对齐 IDEA Merge 的可用形态：左「我方（当前分支）」/ 中「合并结果（可编辑 Monaco）」/ 右「对方（合入分支）」，可选展开共同祖先（diff3 `base` 段）。

| 元素 | 行为 |
|---|---|
| 打开入口 | Git 面板冲突组的 `merge` 按钮 / 冲突行双击 / 右键「解决冲突（三窗格合并）」 |
| 冲突导航 | 「上一个 / 下一个」+「冲突 n/m · 我方 x 行 · 对方 y 行」（F8/F9） |
| 采纳 | 当前块：采纳我方 / 采纳对方 / 两者都留；整文件：全部我方 / 全部对方（二次确认） |
| 保存 | Ctrl+S，走 fs 写 + **etag 乐观锁**（外部改动 → 409 提示，不静默覆盖） |
| 标记为解决 | Ctrl+Shift+R，`git add` 后冲突态结束、面板冲突行消失 |
| 窄屏 | <1180px 三窗格纵向堆叠（每格太窄没法看代码） |

- **解析/替换为独立纯函数层**（`files/merge.ts`）：合并会**改写用户文件**，解析漏块或替换错行即静默损坏——因此必须能脱离 DOM/Monaco 单测（16 例）。容错取向：只把**完整闭合**的块算作冲突，畸形片段原样保留不参与解决。
- **实测（Playwright，真实冲突仓库）**：

```
① 变更视图识别冲突行（! 标记）
② 点击「解决冲突（三窗格合并）」→ 合并标签打开
③ 三窗格：我方（当前分支） | 合并结果（可编辑） | 对方（合入分支）；导航「冲突 1/1 · 我方 1 行 · 对方 1 行」
④ 内容正确：左 MAIN-change / 右 FEATURE-change / 中带冲突标记
⑤ 采纳我方 → 标记消除、中间变为 MAIN-change、导航「无冲突标记」
⑥ 保存 → 「已保存，可标记为已解决」
⑦ 标记为解决 → 面板冲突行 0 个
控制台/HTTP 错误：无
```

> ⑦ 曾失败：`onResolved` 未 await 状态刷新就重渲染面板 → 面板用旧 status 画（冲突行不消失）。改为「先拉新状态再刷新」后通过——竞态在 UI 上表现为「操作没生效」，很值得实测抓出来。


### 5.3 界面结构改造（第四轮：三项反馈）

> 注：第四轮把 Git 面板从右栏移到**底部三栏**；第六轮（5.5）进一步把「变更内容」拆到左栏，底部只留「分支 | 日志 | 提交内容」，并移除标题栏。

#### ① 目录树 Git 状态（含一个「永远不显示」的缺陷）

**根因**：树首次渲染发生在 git 状态**异步到达之前**——`gitDecoration()` 读到的 `state.gitStatus` 还是 `null`，所有行都没有徽标；而 `refreshGit()` 拿到状态后只刷新了状态栏，没让树重画。只有手动刷新（F5/保存后）才偶然出现。

**修复**：`explorer.refreshGitDecorations()` —— 遍历既有行、只换装饰元素（徽标 + 状态类），**不重建树**（重建会丢展开态、滚动位置，并闪一下）。`refreshGit()` 成功与失败路径都回填。

**新增 IDEA 风格下划线**：文件名下方彩色线表示状态，与右侧字母徽标并存（颜色一扫便知、字母给精确状态）。用 `text-decoration` 而非伪元素：不占布局、不受省略号影响、选中文字时不错位。

| 状态 | 下划线 | 徽标 |
|---|---|---|
| 修改 | 工具色 `--tool` | `M` |
| 新增 / 未跟踪 / 已暂存 | 成功色 `--success` | `A` / `U` / `S` |
| 冲突 | 危险色 `--danger` | `!` |
| 删除 | 删除线（危险色） | `D` |
| 目录（子项有变更） | 中性色 | `•` |

#### ② 关闭按钮移入面板

原先在标签栏右侧（`.fw-tabbar` 的 actions 区），离面板很远。现改为**工具窗自己的标题栏**内（连同刷新按钮），并顺带把面板身份信息（「源代码管理」+ 当前分支 chip + 是否限定子目录 + 进行中的操作）一起放进标题栏。

#### ③ 面板挪到下方 + 三栏并排

- 右侧 `340px` 面板整体撤掉 → 编辑区下方**底部工具窗**（`.fw-git-dock`）。IDEA 的 Git 工具窗就在下方，横向空间比纵向充裕。
- 内部从「六个标签页」改为**三栏并排**：变更内容 | 日志 | 分支。理由：这三块是提交前后最常互相参照的信息，同屏可见比来回切标签有用；标签/暂存/远程收进「分支」栏内的小切换（同属引用与远程语义、用得不频繁）。
- 高度可拖（`--git-dock-h`，120px ~ 80vh，记忆在 localStorage，双击拖条复位）；拖动/收展后对 Monaco 调 `layout()`——否则编辑器留白或裁切。
- 变更栏计数徽标落在栏标题上（原顶部标签的 badge 功能不丢），hover 给出分项明细。
- 窄屏（<1180px）三栏纵向堆叠，工具窗整体滚动。

**实测（Playwright）**：

```
① 树：徽标 M | U；装饰类 src[git-modified] / untracked-new.md[git-untracked]；
   下划线 computed：修改=工具色、未跟踪=rgb(85,211,150)
② 工具窗：dockY=676 / dockH=300 / viewport=1000（在底部）；关闭按钮在标题栏内且在面板内；旧 .fw-right 不存在
③ 三栏：变更内容 | 日志 | 分支；分支栏内切换 分支/标签/暂存/远程
④ 交互：拖高 300 → 488px；关闭按钮 → 高度 0（collapsed）；rail 重新展开 → 488px（高度记忆）
⑤ 拖高后编辑器 568 → 340px（重排生效，非空白）
⑥ 日志栏点提交 → 提交详情（返回日志 / 与工作区比较 / 变更文件）在日志栏内渲染
⑦ 计数徽标「3」+ hover「已暂存 1 · 未暂存 1 · 未跟踪 1」
控制台/HTTP 错误：无
```



### 5.4 树上字母「看着像 q」的根因：line-height: 1 + overflow: hidden（第五轮）

用户反馈「目录树条目的 g 字母下半部没显示，看起来像 q」。**不是字体问题，是截断**。

```
.fw-tree { font-size: 12.5px; line-height: 1 }      ← 行盒 12.5px
字体度量：ascent 12 + descent 3 = 15px              ← 比行盒大 2.5px
半行距 = (12.5 - 15) / 2 = -1.25px                 ← 负值：墨迹上下都超出行盒
基线以下可用 = -1.25 + 3 = 1.75px
g 的墨迹下降 = 3px                                  ← 需要 3px → 被切 1.25px
.fw-tree-name { overflow: hidden; }                 ← 横向省略号所必需，纵向一并裁
```

两个可见后果（同一个根因）：

1. `keqing` 末尾的 `g` 下钩被切平 → 剩下的形状（碗 + 右侧直干）= **正好是 q 的形状**。字体度量更大的系统（Windows/macOS 的系统字体）切得更多，更明显。
2. **新加的状态下划线整条不可见**：下划线画在基线下方 2px + 1px 粗度 = 需要 3px，而只有 1.75px——**computed style 依然报 `text-decoration-line: underline`，实际一个像素都没画**。

**修复**：`.fw-tree { line-height: 1.55 }`（19.4px）→ 基线以下 ~5.2px，尾巴（3px）与下划线（3px）都完整；行高仍为 22px，视觉密度不变。

**A/B 实测（6x 放大 + 像素测量）**：

```
修复前 line-height=12.5px：基线以下 1.75px，g 尾巴需要 3px → 被切 1.25px
       文字墨迹 y = 60..113
修复后 line-height=19.4px：基线以下 5.19px                        → 完整
       文字墨迹 y = 60..117   ← 被裁的下半部回来了
```

**顺带修正的颜色语义**：下划线原用 `var(--tool)`——那是聊天里「工具消息」的颜色，在 acrylic 主题下是灰色（`#9a9aa6`），语义不符且不显眼。改为与 Git 面板变更字母同一套：修改=`--warning`（琥珀）、新增/未跟踪/已暂存=`--success`（绿）、冲突=`--danger`（红）、删除=红色删除线、「仅子项有变更」的目录=中性灰。

**为什么值得记一笔**：这是「computed style 通过、渲染不通过」的典型陷阱——查 computed style 会得到 `text-decoration-line: underline`，只有量**基线以下可用空间 vs 墨迹下降**或直接看渲染结果才能发现。同类风险：任何 `overflow: hidden` + 省略号的容器，只要 `line-height` 小于字体度量，就会静默裁掉 descender 与文字装饰。已扫描工作台其余同类元素（面包屑/标签标题/变更名/日志标题/分支名/状态栏）——line-height 均在 17.8px 以上，无此问题。



### 5.5 界面结构大改（第六轮：六项反馈）

#### ① 主题与主界面共用

`theme-core.ts`（新）：主题引擎——定义 / 解析（URL → localStorage → 全局注入 → 默认）/ 按需加载 CSS / View Transitions 切换 / 人民币面额配色 / 默认主题黑白变体。**刻意不 import 任何界面模块**，因此主界面与工作台两个独立入口都能用；`theme.ts` 只在其上叠加「🎨 主题面板」并 `export *` 转出引擎（既有调用点零改动）。

工作台的本地实现（`bootstrapTheme` / `applyTheme` / `STYLE_KEY` / `THEMES`）整体删除，改为调 `initTheme()`；rail 上的主题切换按钮删除。

**修掉的真实不一致**：旧工作台只读 `gebai.ui.style` 拿到主题 id，**不应用 `data-cny-scheme` 与 `data-acrylic-lt`** —— 主界面把人民币主题设成 50 元（翠绿）后进工作台，会变成 100 元红。实测修复后：

```
主界面切换到 matrix → localStorage matrix
工作台加载       → data-theme=matrix、CSS 同为 matrix-DSwXvGTC.css   ✓ 跟随
工作台（cny + 50）→ data-theme=cny、data-cny-scheme=50              ✓ 面额跟随
```

#### ② 标题栏移除（IDEA 新 UI 的做法）

`.fw-menubar`（品牌 + 文件/编辑/视图/Git/帮助 + 刷新 + 主界面）整块删除，样式一并清掉。功能去向：

| 原位置 | 现位置 |
|---|---|
| 新建文件·文件夹 / 上传 | rail「更多」菜单 + 树右键菜单 |
| 打开文件夹 / 回收站 | rail 下组按钮（原本就有） |
| 保存 / 下载 / 历史 / 编辑模式 | 工具栏（原本就有） |
| 复制路径 | 标签栏动作区（原本就有） |
| 资源管理器 / 搜索 | rail 上组 |
| Git 面板 / 提交 / 日志 / 分支 | rail + 各自面板 |
| 比较 | Git 工具窗标题栏 + rail「更多」+ 各处右键 |
| 主题 | 删除（与主界面共用，见 ①） |
| 全屏 / 快捷键一览 / 服务端开关 / 返回主界面 / 刷新根清单 | rail「更多」菜单（`Ctrl+K`） |

#### ③ rail 重组

上组 = 视图切换（**变更** / 资源管理器 / 搜索），下组 = Git 工具窗开关 / 打开文件夹 / 回收站 / 更多。点当前视图 = 收起左栏（IDEA 活动栏习惯），`Ctrl+B` 同效。

#### ④ 变更面板移到左栏

原底部「变更内容」栏整体迁出为 `files/changes.ts`（左栏工具窗，与目录树、搜索**互斥**）：分组列表（冲突/已暂存/未暂存/未跟踪）、行内 stage/unstage/丢弃（自动 stash 备份）、底部 sticky 提交框（提交 / 提交并推送 / 修补 / 署名）、多步操作横幅（继续/跳过/中止）、`root` 是仓库子目录时「仅当前目录 ↔ 整仓库」范围芯片。rail「变更」按钮带改动数徽标。

改动行的动作按钮 **hover 才显形**（列表保持干净）；`@media (hover: none)` 下常显——触屏没有 hover，否则按钮永远点不到。

#### ⑤ 底部三栏：分支 | 日志 | 提交内容

从左到右是一条动线：**点分支 → 看它的日志 → 点某条提交 → 右栏看内容**。

- 点分支 = 切日志视图（`git log <branch>`，日志头出现分支芯片可清除）；「检出」移到右键菜单（原来单击是检出，与「看日志」的诉求冲突）。被选中的分支行高亮（`log-active`，与「当前检出分支」的 `current` 区分——前者是视图状态，后者是仓库状态）。
- 点日志行 = 内容渲染到**右栏**，日志栏保持列表（列表不被打断、从右栏回看时滚动位置还在），当前提交在列表里高亮、栏标题右侧显示短哈希。
- 两条分界可拖：左界改分支栏、右界改**提交栏**（拖的是右栏左边缘，所以指针向左 = 变宽）；中间日志栏吃剩余空间；宽度记 localStorage，双击复位。

#### ⑥ 目录跳转同步 URL

`files/url-state.ts`（新）：`?root=&path=[&line=]` 与界面双向对应。

- 写入：目录切换 `pushState`（后退能回上一个目录）；同目录内开文件 `replaceState`（否则连开几个文件要按多次后退）；节流 60ms 合并同期变更，避免一次操作写多条历史。
- 恢复：启动时 `restoreFromUrl()`；`popstate` 走同一函数。
- 与 `deeplink.ts` 分工：那边**解析并落位**（含根推断规则），这边只**写回**与从既有参数恢复，共用同一套参数名。

**实测**：

```
点目录 → ?root=abs%3A%2Ftmp%2Fwb2&path=src
开文件 → ?root=abs%3A%2Ftmp%2Fwb2&path=README.md
刷新   → 仍在 README.md（活动标签=README.md）      ✓ 不回退
后退   → ?root=abs%3A%2Ftmp%2Fwb2（回到根）        ✓ 可回退
```

**六项综合实测（Playwright，控制台零错误）**：

```
⑥ .fw-menubar 存在=false
① data-theme/CSS 跟随主界面；面额配色跟随；rail 无主题按钮
⑤ 活动栏：上组 变更/资源管理器/搜索，下组 Git/打开文件夹/回收站/更多
④ 变更面板：未暂存（1）M src/a.ts + 未跟踪（1）U work.txt + 提交框 + 徽标「2」；
   切到资源管理器后 hasChanges=false / hasTree=true（互斥成立）
③ 三栏 refs|log|commit；分界 2 条；拖分支栏 200→330px、拖提交栏 458→608px；
   点 feature/log → 日志出现分支芯片与高亮；点日志 → 右栏出提交内容，
   日志栏仍是列表（html 长度仅 +7：高亮类）、选中行高亮、栏标题显示 3a43d50b
② 见上（URL 四步）
快捷键：Ctrl+Shift+G 开左栏变更面板；Ctrl+Alt+G 底部工具窗 300→0→300；Ctrl+K 开「更多」
左栏拖宽 286→430px
```

---

## 6. 关键 API 一览

```
GET  /api/v1/roots                      根清单（会话/项目/绑定/用户/白名单，含能力开关）
GET  /api/v1/roots/resolve?id=…         单个根解析（校验可用性）

GET  /api/v1/fs/list?root&path&sort…    目录列举（懒加载）
GET  /api/v1/fs/tree?root&depth         树快照（首屏/整树搜索用）
GET  /api/v1/fs/stat?root&path          元信息 + etag
GET  /api/v1/fs/read?root&path          文本（编码/换行/etag/binary/truncated）
GET  /api/v1/fs/raw?root&path           原始字节（Range/图片/视频/PDF）
GET  /api/v1/fs/office?root&path        Office 预览转换
GET  /api/v1/fs/archive?root&path       压缩包条目列表
GET  /api/v1/fs/search?root&q&mode…     名称/内容搜索（ripgrep 优先，回退内置）
GET  /api/v1/fs/download?root&path      单文件下载（POST 版：多路径打包 ZIP）
GET  /api/v1/fs/trash                   回收站列表
PUT  /api/v1/fs/write                  保存（etag 乐观锁、编码/换行保真）
POST /api/v1/fs/{mkdir,rename,move,copy,delete,upload,archive/extract,trash/restore,trash/purge}

GET  /api/v1/git/status?root            状态（分支/变更分组/计数/仓库根/前缀）
GET  /api/v1/git/diff?root&from&to&path        差异（任意两端 / mergeBase）
GET  /api/v1/git/compare?root&from&to&path     对比（文件清单 + 统计，任意两端）
GET  /api/v1/git/file-diff?root&path&from&to   单文件差异
GET  /api/v1/git/content?root&ref&path         端点内容（WORKTREE/INDEX/rev）
GET  /api/v1/git/show?root&ref&path            指定提交的文件内容
GET  /api/v1/git/log?root&path&author&q&skip   提交日志（可按路径过滤）
GET  /api/v1/git/refs?root&recent              分支/标签/最近提交/HEAD
GET  /api/v1/git/branches|tags|remotes|stash|blame|conflicts|commit-detail…
POST /api/v1/git/{stage,unstage,discard,commit,branch,tag,checkout,merge,rebase,
                 cherry-pick,revert,reset,stash,remote,fetch,pull,push,init}
```

所有写操作返回 `{ ok, …结果, backupRef? }`；错误统一 `{ error, code }`，前端 toast + 可展开详情。

---

## 7. 验证

- **单元/契约测试**（`bun test`）：
  - `packages/server/src/core/fs/service.test.ts`（17 例）：目录列举/自然序、越界与软链逃逸防护、二进制判定与编码回环、Range、ZIP（含系统 `unzip` 交叉验证）、搜索、根解析（沙箱拒绝 `abs:`、只读开关、根去重）。
  - `packages/server/src/core/git/service.test.ts`（14 例）：提交↔提交（含反向）、提交↔工作区、提交↔暂存区、暂存区↔工作区、工作区↔历史提交、分支 mergeBase、`contentAt` 三端点、`fileDiff`、`refs/status/log`、非仓库错误。
- **真实浏览器冒烟**（Playwright，`/files`）已验证：外壳渲染、目录展开、Monaco 打开（行 DOM + 语法 token）、查看↔编辑切换、状态栏、Git 面板六视图、分支列表、比较视图（整仓库 11 个差异文件）、Monaco 并列差异渲染、端点选择器分组、日志 60 条、提交↔提交对比；此后逐轮增补：深层链接 4 场景、冲突合并 7 步（真实冲突仓库）、树 Git 装饰与下划线（含 A/B 像素测量）、工具窗拖高/收展/编辑器重排、三栏分界拖动、左侧变更面板与互斥、URL 四步（进目录/开文件/刷新/后退）、**主题共用双向跟随**、**控制台零错误**。
- **全量回归**：`bun run test`（server 全套 500+ 用例）、`bunx tsc --noEmit`（web/server）、`bun run --cwd packages/web build` 全绿。

---

## 8. 已知边界与后续可选增强

- Office 预览依赖服务端转换，复杂排版（图表、批注）不保证像素级一致 → 提供「下载打开」兜底。
- 超大文件（>10MB）默认只读片段并提示下载；二进制无内置十六进制编辑（定位是查看器而非 hex 编辑器）。
- 视频仅浏览器原生可解码格式（mp4/webm；H.265 视浏览器而定）。
- 合并冲突解决目前是「三方内容查看 + 用编辑态改文件 + 标记已解决」，未做专门的三窗格合并编辑器。
- 未做：多根同时打开（一次一个活动根，多标签跨根可开）、Git 子模块详情、LFS 管理、提交图的无上限虚拟滚动（当前分页加载）。
