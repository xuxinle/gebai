# 文件工作台（File Workbench）设计方案

> 目标：为歌白内建一套「查看 + 编辑 + Git」一体的文件工作台，**彻底摆脱对 VSCode 的依赖**，操作界面参考 IDEA 系列 IDE（Project 工具窗 + 编辑器区 + Git 工具窗 + 变更列表 + 提交面板 + 日志图）。
>
> 状态：**已实施**。实现说明与验证记录见 `docs/file-workbench-implementation.md`（含与设计稿的偏差说明）。
>
> 本文档为**设计稿**。实施落地后须按 `AGENTS.md` 约定把结论回写 `DESIGN.md`（新增章节「文件工作台」+ 通信协议表新增 fs/git 端点 + 软件包结构补充新目录）。
>
> 文档版本：v1（2026-02）

---

## 0. 一页速览

| 维度 | 结论 |
|---|---|
| 形态 | **独立页面 + 独立路径** `/files`（vite 多入口 `files.html`），服务端注册 `/files` 静态回退；主界面标题栏 `#wheel-btn` **左侧**新增 `#files-btn` 入口按钮 |
| 编辑器 | **Monaco Editor**（VSCode 同款内核）——默认 `readOnly`，点「编辑」解锁；差异视图用 `createDiffEditor`；vendor 静态伺服（沿用 `build-vendor.ts` 惯例），不经 vite 打包 |
| 只读预览 | 代码/文本(Monaco 只读) · Markdown(渲染+源码) · HTML(沙箱 iframe) · 图片(缩放/旋转/棋盘) · 视频/音频(**需服务端 Range**) · PDF(原生阅读器) · Office(docx/xlsx/xlsm/pptx/csv/tsv，复用现有 `render=office`) · 压缩包(列表+解压) · 二进制(hex) · 图表源文件(复用 `diagram.ts` 渲染管线) |
| Git | 后端新增 `GitService`（**直接调用宿主 git CLI**，只读解析 + 写操作串行化），REST 暴露 20+ 端点；前端 IDEA 风格 Git 工具窗 + 日志 Graph + 冲突解决器 + blame |
| 路径安全 | 引入 **Root 抽象**（`sess:<id>` / `proj:<name>` / `user:` / `abs:`），所有 fs/git 接口只接受 `(root, 相对路径)`，服务端统一 `resolveFsRoot()` 做边界与符号链接逃逸校验；服务模式默认仅 `users/{user}/` + 会话 tmp + 白名单项目 |
| 体积代价 | Monaco `min/vs` 约 5.7MB（磁盘态）；**二进制内嵌 web bundle 不内嵌 Monaco**（base64 膨胀 33%），运行时从 `{GEBAI_HOME}/web/vendor/monaco` 释放，缺失则降级 highlight.js 只读视图 |
| 分阶段 | P0 骨架/树/只读 → P1 预览矩阵 → P2 编辑保存 → P3 Git 只读 → P4 Git 写 + 冲突 → P5 高级（搜索替换/终端/性能打磨） |
| 与 Agent 的关系 | 页面上的写操作是**用户本人直操**（不走工具审批，但写审计日志）；「让 Agent 去做」仍走工具审批链路；工具卡 / 文件 chip 增加「在文件工作台打开」入口 |

---

## 1. 现状盘点（为什么要做，能复用多少）

### 1.1 现在缺什么

| 缺口 | 现状 | 影响 |
|---|---|---|
| 文件只有「会话 tmp 视图」 | `GET /api/v1/sessions/:id/files` 只暴露会话 `tmp/` 子树（`routes/session-files.ts`） | 项目文件、用户目录、任意路径**在 UI 里完全不可见**，只能靠 VSCode 打开 |
| 无任何文件写接口 | 文件域只有 list/content/download/preview 四个只读端点 | UI 无法新建/重命名/删除/保存，改文件只能靠 Agent 或 VSCode |
| 无 Git UI | `git` 是 code/explore 子Agent 的**只读工具**（`packages/agents/src/core/code-tools.ts`：status/diff/log/show/branch/ls-files/grep） | 提交/分支/合并/冲突解决全靠命令行或 VSCode |
| 前端零框架、单入口 | `packages/web/index.html` + `src/main.ts` 单 SPA，手写 DOM（`el()`） | 大界面（树/多标签/差异/Graph）手写 DOM 会让代码量与 bug 量爆炸 |
| 预览是「卡片碎片」 | `file-card.ts` 有 image/pdf/html/md/text/office/binary 分派，但只在消息流小卡片里 | 没有全屏工作区：无缩放、无多标签、无多文件对比 |
| 无 Range 支持 | `new Response(Bun.file(safe))` 直接整体返回 | **视频无法拖动进度、PDF 无法跳页、大文件下载无法续传** |

### 1.2 可直接复用的资产（避免重复造轮子）

| 资产 | 位置 | 复用方式 |
|---|---|---|
| 会话文件路径解析 + 符号链接逃逸检查 | `core/session/store.ts:resolveSessionTmpFile / resolvePreviewFile` + `assertNoSymlinkEscape` | 抽为通用 `resolveFsRoot()` 的一部分，保持既有语义不变 |
| 沙箱语义 | `core/security/sandbox.ts:enforcedFor / resolveInSandbox` | fs 路由统一走 `sandbox.enforcedFor(user)` 决定「放开绝对路径」还是「限 user 数据目录」 |
| 预置项目注册表 | 会话 env `CODE_PROJECTS`（JSON：`{name, path, description}`）+ `core/tools/projects.ts` | 文件工作台直接把它渲染成 `proj:<name>` 根；解析规则抽公共函数，**保证「模型看到的项目」与「UI 看到的项目」是同一份真相** |
| Office 阅读视图 | `files/preview?render=office` → `@gebai/agents.renderOfficeReadingView` | 文件工作台预览 Office 直接复用同一渲染器（单一真相源） |
| 图表渲染管线 | `web/src/diagram.ts`（mermaid/plantuml/d2/echarts + 导出 PNG） | `.puml/.mmd/.d2/.echarts` 文件在工作台内渲染 |
| HTML 沙箱预览 + 主题注入 | `web/src/html-view.ts`（CSP 注入、`allow-scripts` 无 `allow-same-origin`、`postMessage` 主题广播） | HTML 文件预览直接复用（含域隔离与主题跟随） |
| ZIP 打包 | `server/src/zip.ts:buildZip` | 多选下载 / 目录整包下载复用 |
| 主题系统 | `web/src/theme.ts` + 9 种 UI 风格 CSS 变量 | 工作台跟随主题；Monaco 用 `defineTheme` 从 CSS 变量生成 |
| 后台任务与日志文件 | `sh async:true` + `bg_task` + `tmp/sh-tasks/*.log` | P5 的「终端/输出面板」可基于它做只读日志 + 命令输入 |
| WS 事件总线 | `event.tool.call/tool.result`，`web/src/events.ts` 已在消费 | 「Agent 正在改哪个文件」实时感知，触发树/编辑器刷新 |

### 1.3 技术前提已确认

- 宿主 git 可用：`git version 2.53.0`（本机实测）——直接调 CLI 可行。
- 前端构建：vite 6，`rollupOptions.treeshake:false`、`chunkSizeWarningLimit:7000`，已有 `public/vendor/*` 稳定文件名静态伺服惯例（`build-vendor.ts`）。
- 二进制形态：`server/scripts/build-web-bundle.ts` 把 `web/dist` **全量 base64 内嵌**——**体积是本设计最硬的约束**。
- 后端是 Hono + 域路由注册（`register{Domain}Routes(rc)`，装配顺序在 `app.ts` 有语义）。

---

## 2. 总体架构

### 2.1 分层

```
┌─────────────────────────── 浏览器 / WebView ───────────────────────────┐
│  /files 页面（独立入口 files.html + src/files/*）                       │
│  ├─ 左：Project 工具窗（文件树 / 打开的编辑器 / Git 变更 / 收藏）        │
│  ├─ 中：多标签编辑器区（Monaco 只读→编辑 · Diff · 各类 Viewer）          │
│  ├─ 右：辅助（大纲 / Blame / 属性）                                     │
│  ├─ 底：Git 日志图 / 搜索结果 / 输出                                   │
│  └─ 状态栏：路径 · 编码 · 行尾 · 分支 · 光标 · 大小                     │
│  数据层：@gebai/sdk（复用同源 token / basePath）+ 轻量 store            │
└───────────────┬─────────────────────────────────┬──────────────────────┘
                │ REST /api/v1/fs/*               │ REST /api/v1/git/*
                │ REST /api/v1/roots              │ WS event.fs.changed
┌───────────────▼─────────────────────────────────▼──────────────────────┐
│ Hono 服务端                                                             │
│  routes/roots.ts  根清单（会话/预置项目/用户目录/盘符）                  │
│  routes/fs.ts     树/读/写/改名/删除/上传/下载/搜索/归档                 │
│  routes/git.ts    status/diff/stage/commit/log/graph/branch/remote/...   │
│  core/fs/roots.ts       Root 解析 + 边界校验 + 审计                      │
│  core/fs/service.ts     文件操作（编码探测、Range、大文件保护、zip）      │
│  core/git/service.ts    GitService（spawn git、porcelain 解析、仓库锁）   │
│  core/audit/fs-audit.ts 写操作审计（{GEBAI_HOME}/audit-fs.jsonl）        │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 关键设计取舍（先讲清楚，后面展开）

1. **Root 抽象**：所有文件/Git 接口不以裸路径为参数，而是 `root`（命名空间化标识）+ `path`（相对 root 的 POSIX 风格路径）。好处：多用户隔离点单点收敛；前端树形展示天然多根；会话 tmp 与项目文件用同一套 UI；审计日志可读。
2. **git 用 CLI 不用纯 JS 库**：`isomorphic-git` 对 rebase/cherry-pick/worktree/LFS/submodule/credential helper 覆盖不全，写操作极易踩坑。CLI 是 git 本身，功能 100% 对齐；风险（注入、并发 index.lock、编码）用「参数数组化 spawn（不经 shell）+ 每仓库串行队列 + `-c core.quotepath=false` + `--no-pager`」解决。
3. **Monaco 走 vendor 静态伺服，不进 vite 打包**：与既有 `plantuml/mermaid/d2` 同策略——稳定文件名（避免 hash 404）、构建零耗时；代价是二进制形态需要「运行时释放」策略。
4. **页面独立于主 SPA**：主聊天页动它风险大、收益小；独立入口可单独懒加载 Monaco（首屏 0 成本），且天然满足「单独的页面和路径」。
5. **写操作不弹审批**：审批是「约束模型」的机制，不是「约束用户」的。用户自己点保存却要审批是反体验。改为：鉴权 + 边界校验 + 审计日志 + 破坏性操作二次确认（确认对话框，前端）。

---

## 3. 后端设计

### 3.1 Root 抽象与安全边界（`core/fs/roots.ts`）

**Root 标识形态**

| root 值 | 含义 | 解析 | 可写性 |
|---|---|---|---|
| `sess:<sessionId>` | 会话工作区（`users/{u}/sessions/.../tmp`） | `store.resolveSessionTmpFile` 同规则 | 可写（本人会话） |
| `proj:<name>` | 预置项目（`CODE_PROJECTS` 注册表） | 复用 `resolveProjectRoot(name)` 语义 | 本地默认可写；服务模式按 `GEBAI_FS_PROJECT_WRITE`（默认 on） |
| `bind:<name>` | 会话绑定项目（`CODE_PROJECT` / `SELF_OPTIMIZE_PROJECT`） | 绑定根 | 同上 |
| `user:` | 当前用户数据目录（`users/{u}/`） | 直接 | 可写（仅本人） |
| `abs:<urlencoded-abs-path>` | 绝对路径 | 仅本地模式（`!sandbox.enforcedFor(user)`）允许；否则 403 | 可写（本地模式=操作者本人） |

**统一校验流程**（每个 fs/git 请求必经）：

```
root 解析 → 目标绝对路径（root/join(path)）
  → 路径规范化（resolve，Windows 反斜杠统一为 /）
  → 符号链接逃逸检查（复用 assertNoSymlinkEscape：realpath 必须仍在 root 内）
  → 边界判定（sandbox.enforcedFor(user)：服务模式必须落在 users/{user}/ 内）
  → 写操作：检查只读白名单（.git/objects 等）、大小上限、可选审计
  → 失败统一 4xx（403 越界 / 404 不存在 / 409 冲突 / 413 过大 / 422 非法编码）
```

**新增环境变量**

| 变量 | 说明 | 默认 |
|---|---|---|
| `GEBAI_FS_ENABLED` | 文件工作台总开关（关闭时 `/files` 返回 404 且 fs/git 端点 404） | `true` |
| `GEBAI_FS_WRITE` | 写操作开关（`off` = 只读工作台；服务模式可置 off 作为纯检视环境） | `true` |
| `GEBAI_FS_ROOTS` | 额外白名单根（JSON 数组，服务模式授予指定目录只读/读写） | 空 |
| `GEBAI_FS_MAX_READ` | 单次读取上限（字节，超出走截断/流式） | `10485760`（10MB） |
| `GEBAI_FS_MAX_WRITE` | 单次写入上限 | `10485760` |
| `GEBAI_FS_MAX_UPLOAD` | 上传单文件上限 | `104857600`（100MB） |
| `GEBAI_FS_HIDDEN` | 默认显示隐藏文件（`.env`/`.git`） | `false`（前端可切换） |
| `GEBAI_GIT_WRITE` | Git 写操作开关（服务模式可置 off） | `true` |
| `GEBAI_GIT_REMOTE` | 远程操作（fetch/pull/push）开关 | `true` |
| `GEBAI_FS_AUDIT` | 写操作审计日志（`{GEBAI_HOME}/audit-fs.jsonl`） | `true`（本地）/`true`（服务） |

### 3.2 FS REST API（`routes/fs.ts`）

统一前缀 `/api/v1/fs`，全部要求登录（服务模式）；`root` + `path` 走 query 或 JSON body。

| 端点 | 方法 | 说明 |
|---|---|---|
| `/api/v1/roots` | GET | **根清单**：会话 tmp、预置项目（名称/描述/路径/是否 git 仓库/当前分支）、绑定项目、用户目录、本地模式盘符与常用目录（`~`、home、cwd、`/workspaces` 等）+ 每根的 `writable`/`vcs` 标记 |
| `/api/v1/fs/list` | GET | 单层目录列表：`?root=&path=&showHidden=&sort=`，返回 `{name, path, type: file\|dir\|symlink, size, mtime, mode, isGitIgnored, ext}`；目录优先 + 自然排序；单层上限 5000 条（超出 `truncated`） |
| `/api/v1/fs/tree` | GET | 递归树（`?depth=1..3`）供首屏展开；深度受控（默认 1，前端懒加载） |
| `/api/v1/fs/stat` | GET | 单/多路径元信息（批量 `paths[]`）：类型、大小、mtime、编码探测、行数、是否二进制、是否大文件、mime |
| `/api/v1/fs/read` | GET | 文本内容：`?root=&path=&maxBytes=&encoding=auto`；返回 `{content, encoding, eol, size, mtime, etag, truncated, language}`。**编码探测**：BOM → UTF-8 严格校验 → 回退 GBK/UTF-16LE（复用 `file` 工具既有探测逻辑思路）；**etag = `mtimeMs-size-hash8`** 供乐观锁 |
| `/api/v1/fs/raw` | GET | **二进制/原样流**（图片、音视频、PDF、字体、附件）：`Content-Type` 按扩展名，**支持 `Range`（206 + `Content-Range` + `Accept-Ranges: bytes`）**——视频拖动/PDF 跳页/断点续传的前提；`?inline=1` 内联展示 |
| `/api/v1/fs/download` | GET | 附件下载（`Content-Disposition`，支持 Range）；目录自动打包 zip |
| `/api/v1/fs/download` | POST | 多选/多目录打包 zip（body `{root, paths[]}`，复用 `buildZip`；上限 `GEBAI_FS_MAX_ZIP`，默认 500MB，超限 413 并给出「分批」提示） |
| `/api/v1/fs/write` | PUT | **保存文本**：`{root, path, content, encoding, eol, expectedEtag?, createIfMissing?}`；`expectedEtag` 不匹配 → **409 + 服务端当前内容**（前端弹「磁盘已变更」三选：覆盖 / 对比合并 / 放弃）；写入原子（临时文件 + rename），保留原文件权限位 |
| `/api/v1/fs/mkdir` | POST | 新建目录（`parents: true` 递归） |
| `/api/v1/fs/rename` | POST | 重命名（同目录） |
| `/api/v1/fs/move` | POST | 移动/跨目录改名（含批量 `items[]`、目标同名冲突策略 `overwrite\|skip\|rename`） |
| `/api/v1/fs/copy` | POST | 复制（文件/目录递归） |
| `/api/v1/fs/delete` | POST | 删除（批量；默认**软删除**：移到会话/用户级 `.gebai-trash/`，`hard: true` 才物理删除；破坏性操作前端二次确认） |
| `/api/v1/fs/upload` | POST | multipart 上传（多文件 + 相对路径保留目录结构；拖拽/粘贴上传；单文件上限 `GEBAI_FS_MAX_UPLOAD`；支持 `overwrite/skip/rename`） |
| `/api/v1/fs/search` | GET | 搜索：`?root=&query=&mode=name\|content&glob=&ignoreCase=&maxResults=`；content 模式优先 `ripgrep`（`rg --json`），缺失回退 `walkDirFiles` + 正则逐行；忽略 `node_modules/.git/dist` 等重目录（复用既有跳过清单）；返回文件+行号+片段（前端跳转高亮） |
| `/api/v1/fs/watch` | POST/GET | 可选：为某根开启/关闭变更推送（服务端 `fs.watch` 递归，经 WS 推 `event.fs.changed`）；默认关闭，前端用「可见轮询 + 打开文件 etag 校验」兜底 |
| `/api/v1/fs/archive` | GET | 读压缩包（`.zip`）：条目列表 `{name, size, isDir, mtime}`；`?entry=` 时返回该条目流（**不解压到磁盘**） |
| `/api/v1/fs/archive/extract` | POST | 解压到目标目录（`entries?` 可选子集；路径穿越防护：条目名 `../` 拒绝） |

**WS 事件（新增）**

| 事件 | 触发 | 用途 |
|---|---|---|
| `event.fs.changed` | 服务端文件监听（可选开启）或 Agent 文件工具执行后 | 树刷新、打开文件「磁盘已变更」提示 |
| `event.git.changed` | Git 写操作完成 / 检测到 HEAD 或 index 变化 | 变更列表、日志图刷新 |

**SDK 扩展**（`packages/sdk/src/client.ts`，与既有 `listSessionFiles` 同风格）：`listRoots()` / `fsList()` / `fsRead()` / `fsWrite()` / `fsSearch()` / `gitStatus()` / `gitDiff()` / `gitLog()` …；大流（`fs/raw`）不走 SDK 包装，前端直接用 `fetch` + `Blob`/`URL.createObjectURL` 以保留浏览器原生流式行为。

### 3.3 Git 服务与 API（`core/git/service.ts` + `routes/git.ts`）

**GitService 关键实现约束**

- `spawn("git", [...args], { cwd: repoRoot, env: { ...process.env, GIT_OPTIONAL_LOCKS: "0" }, windowsHide: true })`——**数组参数、不经 shell**（从根上消除注入），`-c core.quotepath=false -c i18n.logOutputEncoding=UTF-8` 统一中文路径与编码，`--no-pager` 防卡住。
- **写操作每仓库串行队列**（Map<repoRoot, Promise 链>）：避免并发触发 `index.lock` 冲突。
- **仓库探测与缓存**：`rev-parse --show-toplevel` 结果缓存（5s TTL，`event.git.changed` 失效）；根清单标记 `isRepo/branch/ahead/behind`。
- **日志图**：`git log --graph --date-order --pretty=format:%H%x01%P%x01%an%x01%ae%x01%at%x01%cn%x01%ct%x01%D%x01%s` 自行解析（**不解析 `--graph` 的 ASCII 线**，前端用 parent 列自行布局泳道——IDEA 风格可控、可分页增量）。
- **大仓库性能**：`log` 分页（`--max-count` + `--skip` 或游标 `hash^`）、`--no-renames` 默认关（可选开）、路径过滤 `-- <path>`、`rev-list --count` 用于统计、尽量走 `--porcelain=v2`/`-z` NUL 分隔避免歧义。
- **远程凭据**：不落盘。优先级——宿主 `credential.helper`（本机已配置的 git 凭据）/ SSH agent / 前端弹窗填入的 HTTPS 账号令牌（**仅本次操作**经临时 `GIT_ASKPASS` 脚本注入，用后即删；服务端日志脱敏）。服务模式可 `GEBAI_GIT_REMOTE=false` 整体禁用。

**REST 端点（`/api/v1/git`，全部带 `?root=`）**

| 端点 | 方法 | 说明 |
|---|---|---|
| `/status` | GET | porcelain=v2 解析：当前分支/upstream/ahead/behind/detached、文件变更（分组：`staged` / `unstaged` / `untracked` / `conflicted` / `renamed` / `deleted`）、stash 数量、rebase/merge/cherry-pick 进行中状态（`.git/MERGE_HEAD` 等） |
| `/diff` | GET | `?path=&staged=&from=&to=&context=3&ignoreWhitespace=`；返回**结构化 hunks**（旧/新行号、行类型），前端交 Monaco diff 渲染；二进制文件返回 `{binary:true, oldSize, newSize}`（前端双栏图片对比） |
| `/stage` · `/unstage` | POST | 变更加入/移出暂存区（`paths[]`；支持 `patch` 行级暂存 P5） |
| `/discard` | POST | 丢弃工作区改动（二次确认 + 可选先自动 stash 备份） |
| `/commit` | POST | `{message, amend?, signoff?, author?, paths?[], push?}`；返回新 commit hash 与摘要；空消息/无变更 422 |
| `/log` | GET | `?limit=50&skip=&path=&ref=&all=&since=&author=&search=&graph=1` → `{commits:[{hash,shortHash,parents,author,email,date,committer,refs,subject,body?}], hasMore}` |
| `/commit/:hash` | GET | 单提交详情（元信息 + 变更文件清单 + 统计 `--shortstat`） |
| `/commit/:hash/diff` | GET | 单提交对某文件的差异（结构化 hunks，同 `/diff`） |
| `/branches` | GET | 本地/远程分支 + 跟踪 + ahead/behind + 最近提交 |
| `/branch` | POST | `create` / `checkout` / `checkout -b` / `delete`（含强制、含远程删除）/ `rename` / `track` / `upstream` |
| `/merge` · `/rebase` · `/cherry-pick` · `/revert` | POST | 写操作；冲突时返回 `409 + {conflicts: [...]}`，前端进冲突解决模式 |
| `/reset` | POST | `--soft/--mixed/--hard <ref>`（hard 二次确认；可选自动备份分支 `gebai/backup-<ts>`） |
| `/tags` | GET/POST | 标签列表 / 创建 / 删除 / 推送标签 |
| `/remotes` | GET/POST | 远程列表 / 增删改 |
| `/fetch` · `/pull` · `/push` | POST | 远程操作（**流式进度可选**：`text/event-stream` 或 WS 事件推 `event.git.progress`）；`push --force-with-lease` 需显式 `forceWithLease: true` |
| `/stash` | GET/POST | 列表 / `push` / `pop` / `apply` / `drop` / `show`（含 stash diff 预览） |
| `/blame` | GET | `?path=&ref=` → 逐行 `{line, hash, author, date, summary}`（前端左侧 blame 栏 + 悬浮提交信息 + 点击跳提交） |
| `/conflicts` | GET | 三方内容：`{base, ours, theirs, merged}`（`git show :1:/:2:/:3:` + 当前工作区内容），供冲突解决器 |
| `/conflicts/resolve` | POST | 提交解决结果（写文件 + `git add`）或标记 `resolved` |
| `/ignore` | POST | 追加 `.gitignore` 条目（从文件树右键「忽略此文件/类型」） |
| `/repos` | GET | 根清单中所有 git 仓库汇总（多仓工作区：每个仓库一张「Changes」卡） |
| `/init` · `/clone` | POST | 目录 `git init` / `clone <url> <dir>`（P5，clone 需网络与凭据） |

**写操作安全策略**

- 破坏性操作（`reset --hard`、`push --force*`、删除分支/标签、`clean -fd`、`discard`）：前端二次确认 + 可选「先建备份分支/自动 stash」+ 审计日志。
- 所有 git 写操作记录：`{ts, user, root, repo, action, args(脱敏), result}`。

### 3.4 终端 API（`core/exec/term-session.ts` + `routes/terminal.ts`）

**TerminalService 关键实现约束**

- **持久 shell 会话**：每条会话一个常驻 shell 子进程（Windows 默认 `cmd.exe`，POSIX 默认 `bash`，`GEBAI_TERMINAL_SHELL` 可指定），stdin 保持打开——`cd`/`set`/`export` 在会话内生效（这是「终端」与「一次性命令」的分界）。
- **不引入 PTY**（Windows 需 ConPTY / 原生依赖）：命令回显与提示符由前端渲染，命令边界用**哨兵行**判定——写完命令立即写入一行哨兵命令，其输出形如 `{TOKEN}{退出码}|{cwd}`：
  - `cmd.exe`：`echo {TOKEN}%errorlevel%^|%CD%`
  - `bash`：`echo "{TOKEN}$?|$PWD"`
  - PowerShell：`Write-Output ("{TOKEN}" + $(if ($?) {0} else {1}) + "|" + (Get-Location).Path)`

  服务端识别行内 `{TOKEN}` 前缀的行（TOKEN 每次随机），从输出中剥离并产出退出码与 cwd。
- **输出与编码**：stdout/stderr 合并进有界环形缓冲，`read(id, since)` 按游标返回增量；Windows 下 shell 启动先 `chcp 65001`，输出按 `TextDecoder(stream: true)` 增量解码，出现 U+FFFD 时按 GBK 回退（与 `sandbox.ts` 的 `decodeOutput` 同口径）。
- **中断语义**：`interrupt` ＝按进程树终止当前 shell（Windows `taskkill /T /F`，POSIX 进程组 SIGKILL）并以**原 cwd 重建** shell（保留会话 id、滚动缓冲、cwd）——`Ctrl+C` 不是「整个会话消失」。
- **资源与回收**：并发会话上限 8、空闲 30 分钟回收；会话为进程内状态，服务重启即消失。

**REST 端点（`/api/v1/terminal`）**

| 端点 | 方法 | 说明 |
|---|---|---|
| `/info` | GET | 能力探测：`{enabled, reason?, sandboxed, writable, shells[], defaultShell, maxSessions, idleMs}`（只列本机实际存在的 shell） |
| `/create` | POST | `{root, cwd?, shell?, session?, env?}` → `{id, shell, shellName, cwd, root, cursor, output, startedAt}` |
| `/input` | POST | `{id, data, exec?}`：`exec:true`（默认）写入 `data + "\n"` 并追加哨兵行；`exec:false` 原样写入（交互输入 / 控制字符） |
| `/read` | GET | `?id=&since=` → `{cursor, text, exits:[{token,code,cwd}], alive}`（增量输出，哨兵行已剥离） |
| `/interrupt` | POST | 终止当前命令并以原 cwd 重建 shell |
| `/close` | POST | 关闭会话（幂等） |
| `/list` | GET | 当前会话清单 |

**安全策略**

- 沙箱启用且用户非豁免 → 全部端点 403（终端等同于任意命令执行，多用户部署不开放）；`GEBAI_FS_WRITE=false` 拒绝创建 / 执行；`GEBAI_TERMINAL=false` 时 `info` 返回 `enabled:false`，其余端点 404。
- cwd 由 `(root, 相对路径)` 经 `resolveInRoot` 解析（越界 403）；每条命令写审计（`action=term.exec`，含命令首行与 cwd）。

### 3.5 审计（`core/audit/fs-audit.ts`）

- 写入 `{GEBAI_HOME}/audit-fs.jsonl`（JSONL 追加，10MB 轮转保留 5 份）。
- 记录：时间、用户、来源（`web`/`agent`/`api`）、root+path、动作（write/mkdir/rename/move/delete/upload/git.*）、大小、结果（ok/error 原因）、客户端 IP（信任代理时取 `X-Forwarded-For`，复用既有代理头约定）。
- 只读操作默认不审计（避免噪声）；`GEBAI_FS_AUDIT=verbose` 可全量。
- 与模型侧「工具审批」解耦：审计是**事后可查**，审批是**事前拦截**——两者拼起来覆盖「用户直操」与「Agent 代操」两种路径。

---

## 4. 前端设计

### 4.1 页面与路径

**vite 多入口**（`packages/web/vite.config.ts`）：

```ts
build: {
  rollupOptions: {
    treeshake: false,
    input: { main: "index.html", files: "files.html" }, // 新增第二入口
    output: { entryFileNames: "assets/[name]-[hash].js", chunkFileNames: "assets/[name]-[hash].js" },
  },
}
```

**服务端**（`routes/static.ts`）：新增

```ts
const filesHtml = () => readFileSync(join(d.config.webDist, "files.html"))   // 同样注入 __GEBAI_UI_STYLE__ / dev-reload 热刷新脚本
app.get(`${base}/files`, handler)          // 精确路由
app.get(`${base}/files/*`, handler)        // 深链（如 /files?root=proj:gebai&path=src/a.ts）由前端 query 承载，不做服务端路由
```

- **路径形式**：`/files`（配合 `GEBAI_BASE_PATH` 自动带上前缀，前端用 `import.meta.env.BASE_URL` 拼接）。
- **深链参数**（query 而非 path 段，避免与真实目录名冲突）：
  `/files?root=proj:gebai&path=src/main.ts&line=42&mode=view|edit|diff&git=log`
- **跨页跳转**：主 SPA 的文件 chip / 工具卡 / show 块增加「在工作台打开」按钮 → `location.assign(filesUrl)`；工作台左上角「返回对话」→ 回 `/`（保留来源会话，`?from=sess:<id>`）。
- **桌面端**（`packages/desktop`）：WebView 单窗口下用**同页导航**（`location.assign`）+ 工作台内「返回」按钮；若用户开启新标签（浏览器形态）则 `target="_blank"`。工作台不依赖主 SPA 的 DOM 与内存状态（独立入口、独立 store），只共享 `localStorage` 的 token/env/主题。

### 4.2 布局（IDEA 风格线框）

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ← 返回对话   [根选择器 ▾ proj:gebai]  路径面包屑  🔍搜索  ⬆上传  ⬇下载  ⌥设置 │  ← 顶栏
├──────────────┬───────────────────────────────────────────────┬───────────────┤
│ Project 工具窗│  [a.ts ×] [b.css ×] [diff: c.ts ×] [+]        │  大纲 / Blame │  ← 编辑器区
│ ▾ gebai       │ ┌───────────────────────────────────────────┐ │  属性          │
│  ▾ src        │ │  Monaco（只读 → 点右上「编辑」解锁）        │ │              │
│    main.ts    │ │  行号 / 折叠 / 查找 / 迷你地图 / 语法高亮   │ │              │
│    wheel.ts   │ │                                           │ │              │
│   ▸ css       │ └───────────────────────────────────────────┘ │              │
│ ▾ Changes(7)  │  状态栏：proj:gebai/src/main.ts · UTF-8 · LF  │              │
│  M src/a.ts   │          · main ↑1↓2 · Ln 42, Col 7 · 12.4KB  │              │
├──────────────┴───────────────────────────────────────────────┴───────────────┤
│ ▾ Git（分支 | 日志 | 提交内容） · ▾ 终端（Ctrl+Alt+T）                          │  ← 底部工具窗（可折叠）
│   Git 与终端同槽互斥：同一停靠位 / 高度 / 拖拽条，切换只切显隐（实例都保留）      │
└──────────────────────────────────────────────────────────────────────────────┘
```

- 左右面板与底部面板**可拖拽调宽**（`localStorage` 持久化），面板可见性 + 尺寸一键复位。
- **窄屏（<900px）**：左面板变抽屉、右面板隐藏、底部面板默认收起；编辑器仍可用（只读优先）。
- **空态**：无标签时展示欢迎页（最近打开、根列表、常用操作、快捷键速查）——参考 IDEA 的 Welcome/无编辑器态。
- 侧栏四视图（对齐 IDEA 左侧工具窗）：**Project（文件树）/ Open Editors（打开的编辑器）/ Changes（Git 变更）/ Favorites（收藏 + 最近）**。

### 4.3 状态管理

- **引入 `preact` + `@preact/signals`（约 5KB gz）仅用于 `/files` 页面**（主聊天页保持零框架不动）。理由：文件树、多标签、Git 图、diff 面板的状态交互复杂度远超手写 DOM 的合理范围；Preact 体积极小、JSX 无运行时魔法、可按需懒加载，不污染主页面。
  - 备选：继续手写 DOM（+`state.ts` 的 `el()`）+ 自研微型 store —— 若坚持零依赖则走这条，但预期代码量增加 40%+ 且易漏状态同步。
- **状态分层**：
  - 服务端状态（根/树/文件/变更/日志）：内存 store + `signal`，按「(root, path) 键」缓存，TTL 与失效规则（写操作后局部失效，git 操作后全量失效）。
  - UI 状态（打开标签、分组宽度、折叠状态、面板可见性、最近打开）：`localStorage`（键前缀 `gebai.files.*`），跨标签 `storage` 事件同步（沿用项目既有做法）。
  - URL 状态（当前 root/path/行号/模式）：可分享深链；`popstate` 支持浏览器前进后退（工作台内的轻量路由：栈式 `pushState`）。
- **并发与刷新**：
  - 打开文件时记录 `etag`；前端轮询（页面可见时 3s，不可见时停）校验 `etag`+目录 `mtime` → 变更则刷新树/提示「磁盘已变更」（IDEA 的 "File changed on disk"）。
  - 订阅 WS：`event.tool.call/tool.result` 中涉及文件工具（read/write/edit/patch/show）→ 立即失效相关路径缓存并提示；`event.git.changed` → 刷 Changes 与日志图。
  - 编辑中若磁盘被外部改动：**不自动覆盖编辑器内容**，弹三选（载入磁盘版 / 保留我的并差异化 / 手动合并）。

### 4.4 文件树（Project 工具窗）

| 能力 | 设计 |
|---|---|
| 懒加载 | 首屏 `depth=1`；展开时 `fs/list` 单层拉取；展开状态按 (root,path) 缓存 |
| 虚拟滚动 | 目录 >2000 条时启用窗口化渲染（只渲染视口 ± 缓冲） |
| 多根 | 根清单分组：会话 tmp / 预置项目 / 绑定项目 / 用户目录 / 本地盘符；每根可折叠、可「在新标签打开」 |
| 排序 | 目录优先 + 自然排序（`a2 < a10`）；可切换「按名称/按修改时间/按大小」 |
| 过滤 | 顶部即时过滤（子串/`*`/`**` glob/正则开关）+「仅显示变更文件」 |
| 状态标识 | 扩展名图标/颜色点、Git 状态色（M 蓝 / A 绿 / D 红 / ?? 灰 / U 冲突红）、忽略文件淡显、符号链接箭头、只读锁标 |
| 右键菜单 | 打开 / 打开方式（文本/预览/系统默认） / 在新标签打开 / 复制路径 / 复制相对路径 / 重命名（F2） / 删除（Del） / 新建文件·目录 / 复制·移动·粘贴 / 下载 / 打包下载 / 加入收藏 / 发送到会话（复制进会话 tmp 供 Agent 读写） / 在 Git 中查看历史 / 忽略此文件·此扩展名 / 在文件管理器中显示（本地模式） |
| 拖拽 | 拖拽移动/复制（`Alt` = 复制）；从系统拖入 = 上传；拖到编辑器区 = 打开 |
| 键盘 | ↑↓ 导航、←→ 折叠展开、Enter 打开、F2 重命名、Del 删除、Ctrl+C/X/V 复制剪切粘贴（沿用系统剪贴板语义 + 内部剪切板） |
| 创建 | 内联输入（`Ctrl+Alt+N` 新文件 / `Ctrl+Alt+Shift+N` 新目录），占位输入框直接在树中展开 |
| 大目录保护 | `node_modules`/`.git`/`dist` 等默认折叠并淡显；`.git` 内部对象目录默认隐藏（可开） |

### 4.5 编辑器（Monaco 集成）

**加载方案（推荐 B）**

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| A. vite 打包 ESM | `import * as monaco from "monaco-editor"` + `?worker` worker | 类型友好、按需 tree-shake | 本项目 `treeshake:false`，产物大且构建慢；worker 名带 hash（本项目已被同类问题坑过：见 `build-vendor.ts` 注释） |
| **B. vendor 静态伺服（推荐）** | `build-vendor.ts` 增加：拷贝 `node_modules/monaco-editor/min/vs` → `public/vendor/monaco/`；页面用 `loader.js`（AMD）按需 `require(["vs/editor/editor.main"], ...)`；`MonacoEnvironment.getWorkerUrl` 指向 `vendor/monaco/base/worker/workerMain.js` | 稳定文件名、构建零耗时、与现有 vendor 惯例一致、语言包可按需加载 | AMD 与 ESM 混用（仅工作台内部）；类型靠 `monaco-editor` 包的 `d.ts`（devDependency 即可） |

**核心配置**

```ts
monaco.editor.create(el, {
  value, language,               // 语言由扩展名映射 + 首行 shebang/模式嗅探
  readOnly: !editable,           // 默认 true —— 满足「默认查看」
  theme: gebaiThemeName,         // 由 CSS 变量 defineTheme 生成
  automaticLayout: true,
  minimap: { enabled: size > 100_000 },   // 小文件默认关
  lineNumbers: "on", renderWhitespace: "selection", tabSize: 2,
  wordWrap: "off", fontFamily: "var(--font-mono)", fontSize, fontLigatures: true,
  scrollBeyondLastLine: false, folding: true, bracketPairColorization: { enabled: true },
  guides: { indentation: true }, stickyScroll: { enabled: true },
  largeFileOptimizations: true, maxTokenizationLineLength: 20000,
})
```

**「默认查看 → 点击编辑」的具体交互（对应需求 3）**

1. 打开任何文本/代码文件：右上角固定「✏️ 编辑」+「⬇ 下载」+「⋯」按钮条；编辑器 `readOnly: true`（光标可选中/复制、查找可用、不可输入）。
2. 点「编辑」：按钮变「💾 保存 / ↩ 放弃 / ✕ 退出编辑」；`readOnly=false`；自动聚焦；侧栏标签显示「● 未保存」点。
3. `Ctrl+S` 保存 → `fs/write`（带 `expectedEtag`）→ 成功后更新 etag、清除脏标记、状态栏闪「已保存」；失败 409 走冲突三选。
4. 「放弃」恢复服务端内容；`Esc` 两次退出编辑态（未保存时确认）。
5. **自动保存可选**（设置开关，默认关；开启后失焦/停笔 2s 保存）。
6. **只读来源标记**：`.git` 内部、只读根（`GEBAI_FS_WRITE=off`）、超限大文件 → 「编辑」按钮禁用并给出原因 tooltip。

**其他编辑器能力**

| 能力 | 说明 |
|---|---|
| 语法高亮 | Monaco 内置语言（70+）；项目特有扩展名映射补充（`.puml→plantuml`、`.mmd→mermaid`、`.d2`、`.echarts`、`.jsonl`、`.env`、`.tsx/.vue`、`.wgsl` …） |
| 多标签 | 拖拽排序、`Ctrl+W` 关闭、`Ctrl+Tab` 切换、右键「关闭其他/右侧全部」、固定标签、标签溢出滚动；状态图标（脏点 / 只读锁 / 冲突） |
| 分屏 | 左右/上下两栏（各栏独立标签栈）+「与已打开文件对比」 |
| 查找替换 | Monaco 内置（`Ctrl+F/H`）；全局搜索替换放 P5（需服务端多文件写） |
| 编码与行尾 | 状态栏可切（UTF-8 / UTF-8 BOM / GBK / UTF-16LE；LF / CRLF）；保存时按所选写回（GBK 走服务端 `iconv` 编码，前端只传 `encoding` 标识） |
| 大文件保护 | >2MB 或 >8 万行 → 强制只读 + 关闭高亮/迷你地图 + 提示「文件过大，已切换为高性能只读模式」；>10MB 引导下载查看 |
| 二进制 | 不发 Monaco，走专用 Viewer（图片/hex/媒体） |
| Monaco diff | `createDiffEditor`：并排/内联切换、`ignoreTrimWhitespace`、忽略空白、行级导航（`F7/F8`）、整文件「接受左/右」 |
| 保存后动作 | 可选：① 自动 `git stage`（设置）；② 通知会话（把「已修改 path」注入下一条对话上下文，让 Agent 知道现状） |
| 编辑历史 | P5：会话级快照（每次保存落 `{GEBAI_HOME}/users/{u}/file-history/{hash}`，保留 N 版），支持「与上次保存对比/回滚」 |
| 快捷键 | VSCode 习惯：`Ctrl+S` 保存、`Ctrl+P` 快速打开、`Ctrl+Shift+P` 命令面板、`Ctrl+B` 侧栏、`Ctrl+\`` 底部面板、`Ctrl+Shift+F` 全窗搜索、`Alt+←/→` 导航历史、`F2` 重命名（树）、`Ctrl+K Ctrl+C` 注释（Monaco 内置） |

### 4.6 预览矩阵（「绝大多数文件格式都要能看」）

| 类别 | 扩展名 | 渲染方式 | 备注 |
|---|---|---|---|
| 代码/文本 | 全部 Monaco 支持语言 + `.txt/.log/.env/.ini/.conf/.jsonl/.gitignore/...` | Monaco 只读（可切编辑） | 编码/行尾检测；>10MB 提示 |
| Markdown | `.md/.markdown/.mdx` | **三态切换**：渲染预览 / 源码 / 左右分栏（同步滚动） | 复用 `markdown.ts`（markdown-it + highlight.js + DOMPurify 已就位）；支持本地相对图片（经 `fs/raw` 解析） |
| HTML | `.html/.htm` | 沙箱 iframe 预览（复用 `html-view.ts` 的 CSP 注入 + 域隔离 + 主题广播）+ 源码 | 默认预览，切源码走 Monaco |
| 图片 | `.png .jpg .jpeg .gif .webp .bmp .ico .avif .svg` | Viewer：缩放（滚轮/双击）、适应窗口/1:1、旋转、透明棋盘、尺寸/大小信息；SVG 可切源码 | 多图目录可「幻灯片」浏览 |
| 视频 | `.mp4 .webm .ogv .mov* .mkv* .m4v` | `<video controls>` 内嵌（**HTTP Range 必需**） | `*` 浏览器解码能力有限时给「下载/系统播放」提示；字幕 `.vtt/.srt` 可加载 |
| 音频 | `.mp3 .wav .ogg .flac .m4a .aac .opus` | `<audio controls>` + 时长/码率 | P5 波形（Canvas 解码绘制） |
| PDF | `.pdf` | 浏览器原生 PDF 阅读器（iframe + Range）为默认；**离线/受限环境回退 PDF.js 自绘**（页码/缩放/文本搜索/双页） | 二进制内嵌场景建议带 PDF.js（P2 决策） |
| Office | `.docx .xlsx .xlsm .pptx`（现有）、`.csv .tsv`、`.ods/.xls`(可选) | 复用 `files/preview?render=office`（结构 HTML，沙箱 iframe）；csv/tsv 走表格视图（列宽/冻结首行/大表虚拟滚动） | 老格式 `.doc/.xls/.ppt` 明确提示不支持（与 wps 子Agent 一致） |
| 压缩包 | `.zip`（现有 `buildZip` 的读侧用 `fs/archive`）、`.tar/.gz/.tgz`(P5) | 条目列表 + 单条目预览（不解压落盘）+ 解压到目录（路径穿越防护） | |
| 图表源文件 | `.puml .plantuml .mmd .mermaid .d2 .echarts` | 复用 `diagram.ts` 渲染（mermaid/plantuml/d2/echarts）+ 导出 PNG/SVG + 源码 | 与消息流图表同款体验 |
| 二进制 | 其他 | **Hex Viewer**（偏移 + 16 进制 + ASCII 列，分页 4KB，跳转偏移）+ 「下载」 | 附带魔数识别（复用 `file` 工具探测） |
| 数据库 | `.sqlite .db`(P5) | 表列表 + 查询（只读） | 需新依赖，列入可选 |
| Notebook | `.ipynb` | JSON → 单元格渲染（代码 + 输出） | P5 可选 |
| 其他 | 字体 `.ttf/.otf/.woff2`（字形预览，P5）、`.diff/.patch`（走 Monaco diff 视图） | | |

**统一下载与「发送到会话」**：所有 Viewer 工具栏一致（下载 / 在新标签打开 / 复制路径 / 发送到会话）。

### 4.7 Git 图形界面（IDEA 风格）

**A. Changes 视图（左侧工具窗）**

- 分组：`Default Changelist`（IDEA 语义）→ 展开为 **Unversioned / Modified / Staged / Conflicted**；按目录树折叠（IDEA 的目录聚合显示），可切「扁平列表」。
- 每行：状态字母 + 文件名 + 路径；hover 显示 diff 摘要（+N/-M）。
- 工具条：刷新 / 全部暂存 / 全部撤销 / 展开全部 / 折叠全部 / 「提交…」/「提交并推送…」/ stash 当前改动 / 忽略（.gitignore）/ 查看差异。
- 右键：显示差异 / 暂存 / 撤销 / 回滚（`git checkout -- <file>`）/ 删除 / 添加到 .gitignore / 显示历史 / blame / 复制路径。
- 多选批量操作；行级暂存（P5：Monaco 差异视图左侧勾选 hunk → `git apply --cached`）。

**B. 提交面板（IDEA 的 Commit 对话框 + 右下提交区）**

- 变更文件勾选（默认全选）、提交信息多行输入（历史消息下拉、`Ctrl+Enter` 提交、模板/最近模板）、作者（可选覆盖）、`Amend`、`Sign-off`、`Commit and Push`。
- 提交前检查：未跟踪/未暂存提示、`.gitignore` 建议（匹配到疑似产物目录时）、分支保护提示（在 main/master 上给警示色）。
- 提交后：Changes 清空、日志图插入节点（不刷新整页）、可选触发通知。

**C. 日志图（底部面板 · IDEA Log 风格）**

- 泳道图：左侧分支线（parent 关系自绘，颜色按分支/合并语义分配），节点 + refs 标签（HEAD/branch/tag/remote）胶囊；右侧提交信息（作者头像/首字母、时间、short hash、subject）。
- 交互：点击 → 右侧详情（提交元信息 + 变更文件列表 + 统计）；双击文件 → 打开该提交版本（只读标签，标注 `@commit`）；右键 → `Checkout`/`Cherry-Pick`/`Revert`/`Reset(soft/mixed/hard)`/`Create Branch`/`Create Tag`/`Copy hash`/`Compare with Local`/`Show in History`。
- 过滤：分支（当前/全部/指定）、作者、日期区间、路径（`-- <path>`，也可从浏览器「Show History」带过来）、消息关键字；分页加载（滚动到底增量取 50 条）。
- 顶部工具条：分支切换下拉、Fetch/Pull/Push（含进度）、刷新、显示全部分支开关、图布局密度。

**D. 分支/远程管理（对话框）**

- 分支列表：本地/远程、跟踪关系、ahead/behind、最后提交；操作：新建（可基于某提交/标签）、检出、合并到当前、Rebase onto、重命名、删除（本地/远程，二次确认）、设为 upstream、比较两分支差异（直接开 diff 标签）。
- 远程：列表、添加/编辑 URL、Fetch 全部、Fetch 单个、Pull（`--ff-only` / `--rebase` / `merge` 三选）、Push（说明：`--force-with-lease` 需勾选）、凭据弹窗（仅本次）。

**E. 冲突解决器（三方合并）**

- 触发：merge/rebase/cherry-pick 返回 409 或 status 出现 `U` 文件 → 顶部横幅「存在 N 个冲突」+ 逐个跳转。
- 视图：三栏（左 ours / 中 结果（可编辑）/ 右 theirs）+ 区块工具条（接受左/右/两者）、行级选择；底部「应用并暂存 / 跳过 / 中止操作（`--abort`）」。
- 全解决后：一键「继续」(`git rebase --continue` / `merge --continue`)。

**F. Blame 与文件历史**

- Blame：编辑器左侧 blame 列（作者缩写 + 日期 + hash 色块，hover 显示完整提交信息与 subject，点击 → 跳到日志图该提交）；可切「Blame 此版本之前」。
- 文件历史：单文件提交时间线（列表 + 每次变更 diff），支持「Compare with Previous/Current」、「Revert this commit 对文件的影响」。

**G. 状态栏与全局**

- 状态栏右侧：当前分支（点击 → 分支弹窗）、ahead/behind（点击 → 同步操作）、仓库状态点（干净/脏/冲突）、stash 数、进行中的操作（`merging/rebasing` 带「中止/继续」）。
- 顶栏 Git 图标：Changes 数徽标、一键「提交…」、一键「更新项目（pull）」、一键「推送」。
- 与 Agent 联动（亮点）：Changes 视图工具条「✨ 生成提交信息」（把 diff 交给 Agent 生成 conventional commit 文案，走正常模型调用）、「🛠 让 Agent 提交」（把变更打包成一次 prompt，走工具审批链路，事后审计）。

**H. 已落地的细节约定（Git 面板）**

- **日志刷新**：每次工具窗刷新都从最新一页重取（提交后 / F5 都能看到新历史），但首页与现有列表一致时不重建 DOM——翻了几页的位置与滚动不会被拽回顶部；滚动到底自动续页（监听真正的滚动容器）。
- **竞态防护**：日志加载带代际号 + 根比对，切根后旧根的响应被丢弃（避免「面板停在上一个根的数据上」）。
- **状态失败与「不是仓库」分开**：`/git/status` 读失败时面板与状态栏显示可重试的故障态，不再摆出「初始化仓库」这类误导入口。
- **网络操作在途反馈**：fetch/pull/push 期间标题栏显示「进行中：抓取远程…」，远程动作按钮同时禁用。
- **栏内状态**：分支 / 标签 / 暂存 / 远程四栏都有加载中与读取失败（含重试）状态，空态区分「没有数据」与「过滤后无结果」。
- **日志过滤**：按文件（资源管理器 / 变更面板 / 提交内容右键「在 Git 日志中筛选」）、按作者（日志行右键）、按提交信息关键字；过滤条常驻不重建，输入与焦点不会被打断。
- **可达性**：列表行 `tabindex` + `role=button`（Enter/Space 等同点击），三栏分界条可聚焦并用 ←/→ 调宽（Pointer Capture + rAF 合并，窗口缩放后重新夹宽度）。
- **入口补全**：blame（文件标签工具条开关，服务端 `/git/blame` 与编辑器行装饰早已就位）、远程分支删除（`git push <remote> --delete`）、`push --tags`、覆盖已有标签显式 `force`。

### 4.8 命令面板与快捷键

- `Ctrl+Shift+P` 命令面板：统一注册所有动作（打开文件、切换根、新建/删除、暂存/提交、切换分支、搜索、导出…），命令可带参数（IDEA Find Action 风格）；`Ctrl+P` 快速打开（文件名模糊搜索 + `@` 符号 + `:` 行号，VSCode 语义）。
- 快捷键集中定义在一处 `files/keymap.ts`，支持后续用户自定义（P5）；与浏览器冲突的键（`Ctrl+W` 等）在工作台内 `preventDefault` 接管，并在首次进入时给出「快捷键说明」提示。

### 4.9 主题与样式

- 复用 `theme.ts` 的 9 种 UI 风格（acrylic/aether/cyberpunk/aurora/synthwave/matrix/tokyo-night/ink/cny）：工作台 CSS 只用现有变量（`--bg/--bg-inset/--text/--border/--accent/--radius-*/--font-mono/--tool/--warning/--danger`）。
- Monaco 主题：读取 `getComputedStyle` 的变量 → `monaco.editor.defineTheme("gebai-<style>", {...})`（暗/亮按变量亮度自动选 base：`vs-dark`/`vs`）→ 主题切换时 `setTheme` + 树/状态栏同步；`storage` 事件跨标签同步（沿用现有做法）。
- 视觉基调对齐 IDEA：左侧窄工具窗按钮条（图标 + 竖排）、卡片式分隔、细边框、紧凑行高（28px）、状态栏深色细条；避免与歌白整体风格冲突（沿用歌白圆角与配色变量，不硬编码颜色）。

### 4.10 与主界面的集成点

| 集成点 | 位置 | 行为 |
|---|---|---|
| 轮盘左侧入口按钮 | `index.html` header `.header-right` 内、`#wheel-btn` **之前**新增 `#files-btn`（图标：文件夹/文件） | 点击 → `/files`（保留来源 `?from=sess:<id>`）；hover tooltip「文件工作台」；新增 `Ctrl+Shift+E` 快捷键 |
| 工具卡 / 文件 chip / show 块 | `file-card.ts`（`fileToolbar`）、`tool-cards.ts` | 新增「在工作台打开」图标 → 携带 `root/path` 深链（root 推导：会话路径→`sess:<id>`；项目绝对路径→匹配预置项目名否则 `abs:`） |
| 会话 tmp 文件面板 | 现有临时文件入口 | 保留（轻量查看场景），增加「在工作台中浏览」；工作台把会话 tmp 作为一等根 |
| 预置项目管理 | 设置面板 env（`CODE_PROJECTS`） | 工作台顶栏根选择器内提供「+ 添加项目」（写回浏览器本地 env，与现有注入机制一致），并提示「模型侧 project 参数同源生效」 |
| 会话上下文 | 工作台「发送到会话」 | 把选中文件复制进会话 `tmp/shown/`（复用 show 的复制逻辑）并在输入框插入引用路径 |
| 反馈回路 | 状态栏「🐞 反馈」 | 打开现有反馈通道，自动带上当前 root/path 上下文（便于问题定位） |

---

## 5. 平台与兼容性

| 议题 | 设计 |
|---|---|
| Windows | 路径统一内部用 POSIX 风格（`/`）表示、落盘经 `node:path` 转换；处理盘符（`C:\`）、UNC（`\\server\share`）、保留名（`CON/PRN/AUX/NUL/COM1..9/LPT1..9`）拒绝创建、尾随点/空格修剪、`MAX_PATH` 长路径（`\\?\` 前缀，本地模式可选开启）；大小写不敏感 → 树中同路径去重、Git 状态匹配也用大小写不敏感比较（Windows 上） |
| 编码 | 读：BOM → UTF-8 严格 → GBK/UTF-16 探测（`file info` 同思路）；写：UTF-8 默认，GBK/UTF-16 经服务端转码（`TextEncoder`/`iconv-lite`）；**文件名**同样按 UTF-8 处理，Windows 下 `core.quotepath=false` 保证 Git 输出中文可读 |
| 中文/Unicode 文件名 | 前端 URL 一律 `encodeURIComponent`；后端 `decodeURIComponent` 后按路径解析；NUL/控制字符拒绝 |
| `GEBAI_BASE_PATH` | 所有新页面与 API 路径经同一前缀拼接（后端 `d.config.basePath`，前端 `import.meta.env.BASE_URL`）；`fs/raw` 的 Range 响应头不受影响 |
| 二进制模式 | **Monaco 不进 `web.bundle.generated.ts`**（`build-web-bundle.ts` 增加排除清单 `EXCLUDE_PREFIXES = ["/vendor/monaco/"]`）：① 磁盘态（源码/dev/dist）直接伺服 `dist/vendor/monaco`；② 二进制态启动时若 `{GEBAI_HOME}/web/vendor/monaco` 缺失 → 从内嵌资源释放「精简包」（`editor.main` + 常用语言，~2.5MB）或直接降级为 `highlight.js` 只读视图并顶部提示「编辑能力需安装 Monaco 资源（一键释放）」；③ 提供 `GEBAI_WEB_EMBED_MONACO=full|slim|none` 构建/启动开关 |
| 桌面端 WebView | 单窗口导航 + 「返回对话」；WebView2/WKWebView 对 Monaco 兼容良好（避免 `Ctrl+W` 等被宿主拦截的键）；文件下载在 WebView 内可能受限于宿主 → 提供「复制路径」与「发送到会话」兜底 |
| 移动端/窄屏 | 只读优先、树为抽屉、编辑器只读、Git 只读视图可用，隐藏重型面板 |
| 离线 | 全功能离线（无 CDN，Monaco/语言包/PDF.js 全部本地化） |
| 反向代理 | `fs/raw` 的 Range 需代理透传（Nginx 默认透传；若启用 `proxy_buffering` 大量视频可能受限，设计上给「小片段分块」兜底） |
| 多用户隔离 | 服务模式：根清单只暴露该用户可达根；`abs:` 根一律 403；`proj:` 根的可见性 = 该会话注入的 `CODE_PROJECTS`（会话/浏览器本地维度）；审计按用户落盘 |

---

## 6. 性能预算与保护阈值

| 场景 | 保护 |
|---|---|
| 超大目录（`node_modules` 5 万条） | 单层上限 5000 条 + `truncated` 标记 + 虚拟滚动；重目录默认折叠 |
| 打开大文件 | 读上限 10MB（超出返回截断 + `truncated`）；Monaco `largeFileOptimizations`；>2MB 强制只读无高亮；>10MB 只给下载 |
| 搜索 | `rg` 优先（含 `--max-count`、`--max-filesize`、忽略清单）；无 `rg` 回退 `walkDirFiles` 流式逐行；结果上限 2000 条 |
| Git 大仓库 | `log` 分页增量；`status/diff` 用 porcelain/NUL；仓库探测缓存；图渲染只画视口内节点 |
| 视频/PDF | Range 分块（默认 4MB chunk）；不预加载全文件 |
| 首屏 | `/files` 入口与 Monaco 均**懒加载**：首屏仅 shell + 树 + 欢迎页（<150KB gz），Monaco 在首次打开文本文件时加载 |
| 内存 | 打开标签上限（默认 20，超出 LRU 卸载 Monaco model）；diff/log 面板离开即释放 |
| 并发写 | 每仓库串行（Git）；文件写原子 rename；`fs/write` 乐观锁防覆盖 |

---

## 7. 实施路线（分阶段，每阶段可独立验收）

### P0 · 骨架与只读浏览（基础设施）
- 后端：`core/fs/roots.ts`（Root 解析 + 边界校验）、`routes/roots.ts`、`routes/fs.ts`（list/tree/stat/read/raw 只读部分 + Range）、`routes/git.ts`（仅 `status`/`log` 只读）、审计模块空实现。
- 前端：vite 多入口 + `files.html` + 服务端 `/files` 静态回退；顶栏 + 工具栏 + 左树（懒加载/虚拟滚动/多根）+ 只读 Monaco + 状态栏 + 主题跟随；`#files-btn` 入口按钮；SDK 增加根/列表/读取方法；`DESIGN.md` 与 `AGENTS.md` 同步。
- **验收**：能打开 `/files`，切根、展开树、点开代码文件看到高亮只读内容；预置项目与会话 tmp 均可见；服务模式下非白名单路径 403。

### P1 · 预览矩阵 + 下载上传
- 后端：`fs/download`（单/多/目录 zip + Range）、`fs/upload`、`fs/archive`、`fs/search`。
- 前端：Viewer 矩阵全部落地（图片/音视频/PDF/Office/HTML/Markdown/图表源文件/压缩包/hex）；Viewer 工具栏（下载/发送到会话/新标签）；多选集与批量下载；拖拽上传；搜索面板。
- **验收**：视频可拖动进度；PDF 可跳页；docx/xlsx/pptx 阅读视图与主界面一致；图片缩放旋转；zip 可浏览条目；上传下载可用。

### P2 · 编辑与保存
- 后端：`fs/write`（etag 乐观锁 + 原子写 + 编码/行尾）、`mkdir/rename/move/copy/delete`（软删除）、`fs/watch` 可选。
- 前端：只读→编辑切换全链路、脏标记与离开确认、编码/行尾切换、冲突三选、树右键完整菜单（新建/重命名/删除/移动/复制/粘贴/收藏）、快捷键与命令面板、URL 深链与浏览器前进后退、磁盘变更检测与提示、标签持久化。
- **验收**：新建文件→编辑→保存→树刷新闭环；Agent 同时改同一文件时能检出冲突并合理处理；`Ctrl+S`/`Ctrl+P`/`Ctrl+Shift+P` 生效。

### P3 · Git 只读可视化
- 后端：`GitService`（探测/缓存/解析）、`git/status|diff|log|commit/:hash|branches|tags|remotes|blame|stash(读)`。
- 前端：Changes 视图（分组/状态色/目录聚合）、点击文件开 diff 标签（Monaco diff，并排/内联）、日志图（泳道自绘 + refs 胶囊 + 详情 + 提交版文件打开）、blame 栏、文件历史、状态栏分支与 ahead/behind。
- **验收**：Changes 与 `git status` 完全一致；diff 与命令行一致（含重命名/二进制）；日志图拓扑正确且增量可滚动。

### P4 · Git 写操作与冲突
- 后端：`stage/unstage/discard/commit/merge/rebase/cherry-pick/revert/reset/stash(push|pop|apply|drop)/branch 写/remote 写/fetch|pull|push`、`conflicts` 与 `conflicts/resolve`、写操作串行队列、凭据临时注入、破坏性操作备份（自动分支/stash）、审计。
- 前端：提交面板（Amend/Sign-off/Commit&Push/提交前检查）、分支与远程对话框、同步按钮与进度、冲突解决器、破坏性确认、Git 与 Agent 联动（生成提交信息 / 让 Agent 提交）。
- **验收**：完整走通「改文件 → 暂存 → 提交 → 建分支 → 合并产生冲突 → 解决 → 推送」全流程；`reset --hard` / force push 有二次确认与备份。

### P5 · 高级能力（增补，按需排期）
- 全局搜索替换、行级暂存/行级撤销、编辑历史快照与回滚、mini 终端（基于 `sh` 后台任务 + `bg_task` 日志流）、输出/问题面板、Notebook/SQLite/字体预览、音频波形、多文件重命名（正则）、图片标注、diff 三方与「与剪贴板/另一文件对比」、自定义快捷键、工作区保存（roots+tabs 布局还原）。

---

## 8. 测试策略

| 层 | 内容 |
|---|---|
| 服务端单测（bun test，随包目录） | `roots.test.ts`：各 root 解析、越界/符号链接逃逸/Windows 路径/编码探测；`fs.test.ts`：读写原子性、etag 冲突 409、Range 切片正确性、zip 打包、软删除；`git.test.ts`：**临时仓库夹具**（`mkdtemp` + `git init` + 造提交/分支/冲突），断言 porcelain 解析、diff hunks、log graph parents、conflicts 三份内容、串行队列不产生 `index.lock` 错误 |
| 集成测试 | `app-ui.test.ts` 同风格：`createApp` 全路由装配后跑端到端 REST（含 401/403/404/409/413/422 各分支） |
| 前端测试 | `bun test`（现有 `*.test.ts` 风格）覆盖纯逻辑：根/路径拼接、语言映射、diff hunk 渲染模型、图泳道布局算法、快捷键分派、编码/行尾 UI 状态机；DOM 交互用最小 happy-dom/jsdom 桩（沿用现有做法） |
| 手工验收脚本 | 每个阶段的「验收」条目写成 checklist（含 Windows/macOS/Linux + 本地/服务模式 + 桌面 WebView） |
| 回归保护 | 现有会话文件面板、文件卡预览、show 块、工具卡全部走既有测试；`/files` 不影响主 SPA（独立入口，互不加载） |

---

## 9. 查漏补缺清单（用户需求之外我补的点）

1. **HTTP Range**——没有它，视频拖动、PDF 跳页、大文件续传全废（现有接口完全不支持）。
2. **编码与行尾**——中文环境刚需：GBK 读写、UTF-16、CRLF↔LF 切换、BOM 处理；否则「能看不能存」。
3. **写冲突与并发**——Agent 与用户同时改同一文件（歌白特有场景！）；etag 乐观锁 + 三选对话框 + 审计。
4. **磁盘变更检测**——打开的文件被 Agent/外部工具改动时的提示与重载（IDEA 的 "changed on disk"）。
5. **软删除/回收站**——删除是最高频事故源，默认进 `.gebai-trash/`。
6. **体积与二进制形态**——Monaco 5.7MB 与 base64 内嵌的冲突，必须显式决策（见 §10 决策点 3）。
7. **服务模式权限模型**——「本地模式全盘自由」与「服务模式严格沙箱」的双语义，前端需可视（不可写根显示锁）。
8. **Git 凭据与进度反馈**——push/pull 在服务端执行，凭据如何注入、进度如何回传（大仓 fetch 分钟级）必须先设计。
9. **Git 并发与 `index.lock`**——每仓库串行队列，否则 UI 连点即坏仓库。
10. **破坏性操作兜底**——reset --hard / force push / clean / discard 前自动备份分支或 stash。
11. **大目录与大文件降级**——`node_modules`、10MB 日志、5 万行文件的表现必须预设。
12. **多标签与内存**——Monaco model LRU 卸载，避免开 50 个文件后卡死。
13. **快捷键冲突**——浏览器 `Ctrl+W`/`Ctrl+T`/`Ctrl+N` 与页面级接管策略。
14. **深链与可分享性**——`/files?root=&path=&line=`，便于从聊天/工具卡直达（这也是「摆脱 VSCode」的关键体验）。
15. **无障碍与键盘可达**——树/标签/对话框全键盘可操作（IDEA 用户习惯）。
16. **审计与合规**——谁在什么时候改了服务器上的哪个文件；服务模式必备。
17. **与 Agent 的双向通道**——「在工作台打开 Agent 刚写的文件」+「把工作台里的文件/变更交给 Agent」，让工作台成为 Agent 的可视化操作台，而非孤立工具（这是相对 VSCode 的**超越点**）。
18. **编辑后上下文同步**——用户手改文件后，会话上下文里的旧内容会误导模型；保存后可选注入「路径 + 变更摘要」提示（或自动触发一次 `read`）。
19. **i18n/术语一致性**——界面中文与现有 UI 保持一致（「变更 / 提交 / 检出 / 变基 / 暂存（stash）」采用 IDEA 中文版术语）。
20. **迁移与兼容**——现有 `sessions/:id/files*` 接口保留（其它消费者/测试依赖），文件工作台只是新增视图层，不破坏既有契约。

---

## 10. 待你确认的决策点

| # | 决策 | 选项 | 我的推荐 |
|---|---|---|---|
| 1 | 页面形态 | A. 独立入口 `/files`（vite 多入口） · B. 主 SPA 内全屏覆盖层（hash 路由 `#/files`） | **A**（真路径、独立加载、Monaco 懒加载、可深链可分享；代价：多一个入口文件） |
| 2 | 文件页 UI 框架 | A. Preact + signals（~5KB gz，仅 `/files` 用） · B. 继续手写 DOM（零依赖） | **A**（树/标签/diff/Graph 的状态与交互量级已超出安全手写范围） |
| 3 | Monaco 与二进制体积 | A. vendor 静态伺服 + 二进制形态运行时释放（推荐） · B. 内嵌进 web bundle（+约 7.6MB base64 膨胀） · C. 不进二进制，二进制形态降级 highlight.js 只读 | **A**（可配置：`GEBAI_WEB_EMBED_MONACO=full\|slim\|none`） |
| 4 | Git 实现 | A. 宿主 git CLI（全功能） · B. isomorphic-git（纯 JS，无需 git） | **A**（宿主 git 已存在；B 功能缺口大、写操作风险高） |
| 5 | 服务模式写权限 | A. 默认允许用户改自己目录 + 白名单项目 · B. 默认只读，需 `GEBAI_FS_WRITE=true` 显式开启 | **A**（本地优先的产品，服务端可一键只读） |
| 6 | 删除语义 | A. 默认软删除到 `.gebai-trash/` · B. 直接物理删除 + 确认 | **A** |
| 7 | 是否要 mini 终端（P5） | A. 要（基于 `sh` 后台任务 + 日志流） · B. 不要（避免与 Agent 执行混淆） | **A（可选）**——但须与 Agent 的命令执行明确区分（不同面板、明确标识「你本人执行」） |
| 8 | 是否允许工作台内「让 Agent 提交/重构」 | A. 允许（走标准 prompt + 工具审批） · B. 不允许（工作台保持纯人工） | **A**（这是相对 VSCode 的差异优势） |

---

## 11. 交付物与代码落点（预估）

**新增（服务端）**

| 文件 | 职责 |
|---|---|
| `packages/server/src/core/fs/roots.ts` | 根清单构建 + root 解析 + 边界校验 |
| `packages/server/src/core/fs/service.ts` | 文件操作（读/写/列表/搜索/归档/上传/下载/Range/编码探测） |
| `packages/server/src/core/fs/audit.ts` | 写操作审计落盘 |
| `packages/server/src/core/git/service.ts` | GitService（spawn/解析/锁/缓存/凭据） |
| `packages/server/src/core/git/parse.ts` | porcelain=v2 / diff hunks / log graph / blame 解析 |
| `packages/server/src/routes/roots.ts` `routes/fs.ts` `routes/git.ts` | 三个域路由 + 在 `app.ts` 装配 |
| 测试 | `core/fs/*.test.ts`、`core/git/*.test.ts`（临时仓库夹具）、`routes/fs.test.ts`、`routes/git.test.ts` |

**新增（前端）**

| 文件 | 职责 |
|---|---|
| `packages/web/files.html` | 工作台入口（含 splash/主题注入占位） |
| `packages/web/src/files/main.tsx` | 挂载、路由（history/query）、快捷键、命令面板注册 |
| `packages/web/src/files/store.ts` | 状态（roots/tree/tabs/git/搜索）+ 缓存与失效 |
| `packages/web/src/files/tree.tsx` `tabs.tsx` `editor.ts` | 树 / 标签 / Monaco 封装（model 管理、只读切换、diff、主题） |
| `packages/web/src/files/viewers/*` | image / media / pdf / office / markdown / html / archive / hex / diagram / binary |
| `packages/web/src/files/git/*` | changes.tsx / commit-panel.tsx / log-graph.tsx / branches.tsx / conflict.tsx / blame.ts |
| `packages/web/src/files/api.ts` | fs/git REST 封装（含 Range、上传进度、错误码映射） |
| `packages/web/src/files/fs.css`（或并入 `css/`） | 工作台样式（仅用主题变量） |
| 测试 | `files/*.test.ts`（逻辑：语言映射/路径拼接/泳道布局/快捷键/冲突状态机） |

**修改**

| 文件 | 改动 |
|---|---|
| `packages/web/vite.config.ts` | 多入口 |
| `packages/web/index.html` | 新增 `#files-btn`（`#wheel-btn` 左侧） |
| `packages/web/scripts/build-vendor.ts` | 拷贝 monaco `min/vs` → `public/vendor/monaco` |
| `packages/server/src/routes/static.ts` | `/files` 页面路由 + dev-reload 热刷新同款处理 |
| `packages/server/src/app.ts` | 装配三个新域路由（顺序：roots/fs/git 在 static 之前） |
| `packages/server/src/core/base/config.ts` | 新增 §3.1 环境变量 |
| `packages/server/scripts/build-web-bundle.ts` | 排除 `vendor/monaco`（按 `GEBAI_WEB_EMBED_MONACO`） |
| `packages/sdk/src/client.ts` | fs/git 方法（含类型定义） |
| `packages/web/src/file-card.ts` `tool-cards.ts` | 「在工作台打开」入口 |
| `DESIGN.md` / `AGENTS.md` | 新增章节与协议表（实施时同步） |

**预估规模**：后端 ~3500 行 + 测试 ~1500 行；前端 ~6000 行（含 CSS）+ 测试 ~600 行；分 P0–P4 四批交付。

---

## 12. 「摆脱 VSCode」能力对照自检

| 用户诉求 | 本方案对应 | 状态 |
|---|---|---|
| 1. 目录树，可打开文件夹与预置项目 | §4.4 树 + §3.1 Root（会话 tmp / 预置项目 / 绑定项目 / 用户目录 / 本地盘符） | ✅ |
| 2. VSCode 同款查看编辑组件 + 语法高亮 | §4.5 Monaco（VSCode 内核）+ 语言映射 + diff 视图 | ✅ |
| 3. 默认查看，点按钮进入编辑 | §4.5「默认 readOnly → 编辑/保存/放弃」交互 | ✅ |
| 4. 绝大多数格式能看，只有代码可编辑 | §4.6 预览矩阵（图片/音视频/PDF/Office/压缩包/hex/图表源文件…）；编辑仅对文本/代码开放，媒体类无编辑入口 | ✅ |
| 5. 支持下载 | §3.2 单文件/多选/目录 zip + Range 续传 + 上传（反向） | ✅ |
| 6. 全面 Git 图形化 + IDEA 风格 | §3.3 + §4.7（Changes / Diff / Commit / Log Graph / Branch / Remote / Stash / Conflict / Blame / Revert / Cherry-pick / Reset） | ✅ |
| 7. 单独页面与路径 + 轮盘左按钮入口 | §4.1 `/files` + §4.10 `#files-btn`（`#wheel-btn` 左侧） | ✅ |
| 8. 彻底摆脱 VSCode 甚至更好 | 深链直达、与 Agent 双向联动（看上/交给 Agent）、多根统一视图、审计、软删除、主题无缝、内网离线可用 | ✅（差异优势） |
