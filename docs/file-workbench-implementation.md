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
- **路径防护三层**：`resolveInRoot` 词法校验（拒绝 `..`、绝对路径）→ `assertNoSymlinkEscape`（realpath 校验，拒绝软链逃逸）→ `isInside` 前缀校验（含兄弟目录边界，`/root2` 不算 `/root` 内）。
- **审计**：`core/fs/audit.ts` 记录每次写操作（谁、哪个根、什么动作、成功与否、可选 IP），写操作与 git 写操作均留痕。
- **删除即回收站**：删除不直接 unlink，落 `trash` 清单 + 备份，支持恢复/彻底清除。

### 2.2 只读优先的默认姿态（需求 3）

编辑器状态机：

```
打开文件 ──► 查看态（Monaco readOnly，无脏状态，快捷键不拦截）
              │  点击工具栏「编辑」或 Ctrl+E / 双击状态栏
              ▼
            编辑态（可写；标题出现 ● 脏标记；Ctrl+S 保存）
              │  保存成功 → 回到查看态可选；未保存关闭 → 二次确认
```

「默认查看」不只是 UI 习惯，也是**安全默认**：Agent 与用户共用同一套文件，误触键盘不应改文件。

---

## 3. 文件能力（需求 1/2/4/5）

### 3.1 目录树与根

- **懒加载**：只列举展开的目录（`GET /api/v1/fs/list`），大目录单层上限 5000 条并标记 `truncated`。
- **排序**：目录优先 + 自然序（`file2` 在 `file10` 前）；支持按名称/修改时间/大小/类型排序（前端本地重排，零请求）。
- **条目元数据**：`kind`（text/image/video/audio/pdf/office/archive/binary/diagram）、`language`（Monaco 语言 id）、`editable`——前端据此决定「用什么查看器」「要不要显示编辑按钮」，服务端是唯一裁决者。
- **装饰（IDEA 风格）**：Git 状态色（M/A/D/R/U/!）+ 目录含变更时的小圆点，随 Git 面板刷新同步。
- **文件操作**：新建文件/文件夹、重命名、移动、复制、删除（回收站）、上传（拖拽到树节点=指定目录，拖到空白=当前目录）、下载、路径复制、在搜索中定位。
- **隐藏文件**：默认不显示（`.git` 等），一键切换。

### 3.2 查看器矩阵

| 类型 | 查看方式 | 关键点 |
|------|----------|--------|
| 文本/代码/配置 | Monaco（VSCode 同款） | 语法高亮、括号配对着色、缩进引导、minimap 可选、大文件降级（> 阈值只读前 N MB 并提示） |
| 图片（png/jpg/gif/webp/svg/bmp/ico） | 内嵌查看器 | 适应窗口/1:1/缩放平移、SVG 走 DOM 渲染 |
| 视频/音频 | `<video>/<audio>` + HTTP Range | `parseRange` 支持 `bytes=a-b`、开放式、后缀式、越界收敛与 416 |
| PDF | 内嵌 `<iframe>` 浏览器原生渲染 | 走 `/fs/raw`，支持 Range |
| WPS（docx/xlsx/pptx） | 服务端转换后预览（表格/文本/文档结构） | `routes/fs.ts:/office`；转换失败降级为「下载打开」提示 |
| 压缩包（zip/jar…） | 零依赖中央目录解析 → 条目列表 | `core/fs/archive.ts`（`listZip`/`readZipEntry`/`safeEntryName`），可直接解压到工作区 |
| 二进制/未知 | hex 预览 + 元信息 | 不误当文本（`looksBinary`：NUL 字节判定） |
| 图表源文件（.mmd/.puml/.d2/.echarts） | 文本 + 可切图 | 复用会话内图表渲染约定 |

### 3.3 编码 / 换行保真（编辑不毁文件）

- 读取时探测：UTF-8（BOM）、UTF-16LE/BE（BOM 或 NUL 分布启发）、**GBK/GB18030 回退**、latin1 兜底；换行探测 lf/crlf/cr/mixed。
- 编辑器状态栏显示并可切换编码与换行；保存时以原编码 `iconv-lite` 编码回写、按目标换行重排。
- 单测钉死「GBK 解码 → 编码回写字节完全一致」，避免中文项目被静默转码。

### 3.4 并发与一致性（乐观锁）

- 每个文件读返回 `etag`（`W/"mtime-size-hash"`）。
- 保存携带 `etag`：服务端比对，若磁盘已变（Agent 刚改过、另一个标签改过）→ **409 冲突**并返回当前磁盘内容，前端给出「覆盖 / 另存 / 放弃」三选一，绝不静默覆盖。
- 外部变更监听：轮询 `/fs/stat`（可配 2s）比对 etag，自动重载干净标签、脏标签只提示。

### 3.5 下载 / 上传

- 下载：单文件流式（带 `Content-Disposition` 与文件名 RFC5987 编码）；目录或多选 → ZIP 打包（`zipPaths`，逐文件累计限额，超限 413 并提示分批）。
- 上传：`multipart`，路径经 `resolveInRoot` + 覆盖确认；同名冲突可「覆盖/跳过/重命名」；上传后自动刷新树与 Git 面板。

---

## 4. Git 图形化（需求 6 + 「任意两端对比」）

### 4.1 端点模型（本设计的关键抽象）

差异与对比统一抽象为**端点（endpoint）**：

```
WORKTREE       工作树（磁盘现状）
INDEX          暂存区（:0:）
""  / 省略     与 WORKTREE 搭配时表示「暂存区」（即未暂存改动）
<rev>          任意提交 / 分支 / 标签 / HEAD~n / 任意 SHA
```

- `compare(from,to)`：`git diff [--find-renames] <from> <to>`；`from=WORKTREE` 时用 `git diff <to>`（工作树↔暂存区语义），`to=WORKTREE` 时用 `git diff <from>`（提交↔工作树语义）。
- `mergeBase=true` 时切换为 `git diff --merge-base <a> <b>`（三点 `...` 语义）——「我和目标分支各自改了什么」。
- `contentAt(ref, path)`：`ref` 为 `WORKTREE` 读磁盘、为 `INDEX` 用 `git show :0:<path>`、否则 `git show <rev>:<path>`；端点不存在返回 `missing: true`（前端显示「新增/删除」而非报错）。
- 前端「比较」标签页：左右两端各有**端点选择器**（工作副本 / 引用 / 本地分支 / 远程分支 / 最近提交 / 手输 rev），中间「⇄」可一键交换，`...` 开关切换共同祖先语义，`path` 输入框做路径过滤。

**能力清单（任意组合）**

| 场景 | 端点组合 |
|------|----------|
| 工作区改动 vs 上次提交 | `HEAD → WORKTREE` |
| 已暂存内容 vs 上次提交 | `HEAD → INDEX` |
| 未暂存改动 | `INDEX → WORKTREE` |
| 任意两个提交 | `sha1 → sha2`（含「最近提交」下拉与前/后快捷） |
| 提交 vs 当前工作区 | `sha → WORKTREE` |
| 当前工作区 vs 历史提交 | `WORKTREE → sha` |
| 分支 vs 分支（各自引入） | `main → feature` + `...` |
| 分支 vs 分支（全量差异） | `main → feature` |
| 标签 / 远程分支 | 同 rev 语义 |
| 与父提交 | 日志右键「与父提交比较」（`sha^ → sha`） |
| 日志里任意两条 | 日志多选（Ctrl 点选）→「比较所选两提交」 |

### 4.2 差异编辑器

- 用 **Monaco DiffEditor** 而不是文本 patch：并列/内联切换、语法高亮、折叠未变区域、词级差异、忽略空白开关。
- 数据来自 `contentAt` 两侧真实文本（不是解析 patch），保证 CRLF/编码差异表现正确；二进制或超大文件降级为 `fileDiff` patch 文本视图。
- 差异页签支持「跳到工作区对应行」「还原此文件」「暂存此文件」。
- 冲突文件：`git/conflicts` 提供 base/ours/theirs/current 四方内容，按需三方查看。

### 4.3 工具窗（IDEA 布局对照）

| IDEA | 本工作台 | 说明 |
|------|----------|------|
| Local Changes | 变更 | 分组（冲突/已暂存/未暂存/未跟踪）、逐行级暂存、丢弃（自动 stash 备份）、全部暂存/取消 |
| Commit 面板 | 提交框 | 提交信息（Ctrl+Enter）、修补上次提交、署名、提交并推送 |
| Log | 日志 | 列表 + 图形化（分支/合并线）、按路径过滤、按作者/关键字搜索、分页加载、右键操作菜单 |
| Branches / Tags | 分支 / 标签 | 本地/远程分组、检出、新建/删除/重命名、合并（no-ff/squash）、变基、推送/拉取、删除远程分支、创建/删除标签（含附注） |
| Git Toolbar | 远程 | fetch/pull/push（force-with-lease）/remote add·set-url·remove、网络操作独立开关 |
| Diff | 比较标签页 | 见 4.1 |
| Stash | 暂存 | 列表/push（含未跟踪、指定路径）/pop/apply/drop/show/clear |
| History for file | 文件历史 | `log --follow` + 单文件历史差异 |
| Annotate | 追溯 | `git blame` 逐行作者/时间，点击跳转到对应提交 |

写操作统一走「guard（开关）→ 执行 → 审计」，破坏性操作（丢弃/重置 hard/删除分支）**默认先备份**（临时 stash 分支或备份分支），并在返回里给出可恢复引用。

### 4.4 会话工作区 vs 仓库根（子目录仓库的边界处理）

`root` 可能只是仓库的一个子目录（典型：会话工作区 `…/sessions/ab/cd/<id>/tmp` 位于 `gebai` 仓库内）。
此时：

- 服务端 `status` 返回 `rootPath`（仓库根）与 `prefix`（root 在仓库内的相对路径），前端据此计算 `repoPrefix`。
- Git 面板与比较视图**默认限定在该子目录**（IDEA 也只显示项目根范围内变更），并提供 chip 一键切「整仓库」；比较视图在限定范围内无差异时会提示「整仓库有 N 处差异」。
- 路径语义两套并存且明确：**git 侧路径是仓库相对**（`contentAt`/`git` 命令使用），**fs 侧路径是根相对**（编辑器/树使用）；`toRootPath()` 负责换算（打开文件时）；差异取数保持仓库相对不变。
- `routes/git.ts` 的仓库定位用独立 `dir` 参数（不复用 `path`）——`path` 在这些端点里是 pathspec，混用会把文件当目录（已修复并有测试覆盖）。

---

## 5. 入口与集成（需求 7）

- **页面**：`packages/web/files.html` + `src/files/main.ts`（Vite 多入口），生产构建产出 `dist/files.html` 与 `dist/assets/files-*.js`；服务端静态路由按 `readPage("files.html")` 是否存在决定注册（dev-reload 首轮构建窗口期返回 503 占位页）。
- **入口按钮**：`files-entry.ts` 在标题栏轮盘按钮**左侧**注入「文件」按钮（hover 高亮、图标与主题一致），点击新标签打开 `/files`，并透传：
  - `session`：当前会话 id → `sess:` 根指向本会话工作区（Agent 刚写的文件就地可查）；
  - `project` / `root`：会话绑定项目或已选根（直达项目树）；
  - `path` / `line`：从消息里的文件链接直达文件与行；
  - `gb_style`：沿用当前 UI 主题，两页观感一致。
- **为什么是新标签而不是内嵌路由**：IDE 式工作台资源重（Monaco + Git 面板 + 大量请求），与聊天并行使用是常态（一边让 Agent 改、一边自己核差异）；独立页面同时带来故障隔离。
- **便捷动作**：消息文件链接、搜索结果、Git 变更一键跳转 `/files?root=…&path=…`；支持复制当前文件的「工作台链接」给他人。

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
- **真实浏览器冒烟**（Playwright，`/files`）已验证：外壳渲染、目录展开、Monaco 打开（行 DOM + 语法 token）、查看↔编辑切换、状态栏、Git 面板六视图、分支列表、比较视图（整仓库 11 个差异文件）、Monaco 并列差异渲染、端点选择器分组、日志 60 条、提交↔提交对比、**控制台零错误**。
- **全量回归**：`bun run test`（server 全套 500+ 用例）、`bunx tsc --noEmit`（web/server）、`bun run --cwd packages/web build` 全绿。

---

## 8. 已知边界与后续可选增强

- Office 预览依赖服务端转换，复杂排版（图表、批注）不保证像素级一致 → 提供「下载打开」兜底。
- 超大文件（>10MB）默认只读片段并提示下载；二进制无内置十六进制编辑（定位是查看器而非 hex 编辑器）。
- 视频仅浏览器原生可解码格式（mp4/webm；H.265 视浏览器而定）。
- 合并冲突解决目前是「三方内容查看 + 用编辑态改文件 + 标记已解决」，未做专门的三窗格合并编辑器。
- 未做：多根同时打开（一次一个活动根，多标签跨根可开）、Git 子模块详情、LFS 管理、提交图的无上限虚拟滚动（当前分页加载）。
