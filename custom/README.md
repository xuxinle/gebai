# custom/ — 二次开发子代理与依赖（独立迁移域）

本目录是**歌白二次开发的专属目录**：自研子代理与依赖组件放这里，与上游 `packages/` 完全隔离——
**上游版本更新时，整个 `custom/` 文件夹复制到新版本仓库根即完成迁移**（上游不触碰本目录，无合并冲突面）。

```
custom/
├── agents/          # 二开子代理定义（自动扫描域——同 packages/agents/src/agents/ 布局）
│   └── my_agent/                 # ← 自建：{name}/{name}.ts（export const def: SubAgentDef = {...}）
│       ├── my_agent.ts           #    系统提示词可拆 {name}.md；纯 {name}.md 亦可（零 TS 简化定义）
│       └── my_agent.md
├── core/            # 二开依赖组件（自动可 import——同 packages/agents/src/core/ 布局）
│   └── my_lib/                   # ← 自建：{lib}/index.ts，子代理内相对引用 ../../core/{lib}
│       └── index.ts
├── auth/            # 二开凭证来源（自动扫描域——鉴权链的可插拔入口，详见下节）
│   └── my_source.ts              # ← 自建：export default 单个 CredentialSource 或来源数组
├── web/             # 二开前端脚本（复制示例改名即启用；构建/开发时带到 Web 产物根，在入口脚本之前引入）
│   ├── gebai.config.example.js   #    配置示例 → 复制为 gebai.config.js 生效
│   └── init.example.js           #    初始化脚本示例 → 复制为 init.js 生效（产物名 gebai.custom.js）
└── tsconfig.json    # 已配 paths：@gebai/sdk / @gebai/sdk/node / @gebai/agents 指向上游包
```

（以上 `my_agent/`、`my_lib/` 仅为结构示意——本目录出厂为空骨架，`agents/` 与 `core/` 内的 `.gitkeep`
仅用于占位保留目录，无功能语义，复制迁移时可一并覆盖，不影响任何行为。）

## 自动发现（零注册）

- **dev 运行时**：`custom/agents/` 与内置域**双域扫描自动合并**，新增/修改/删除文件即热加载生效（与内置子代理同机制）——放文件即注册，无需改任何清单或入口
- **构建打包**：`build-subagents.ts` 同样双域扫描，二开子代理自动打进 bundle 注册表
- **同名覆盖**：`custom/agents/{name}` 与内置子代理同名时，**custom 版本胜出**（覆盖内置定义）——可用于改写内置行为而不动上游代码
- **失败隔离**：单个二开子代理 import 抛错只记 loadErrors（模型侧可见根因），不影响内置与其他二开子代理

## 编码约定（与内置子代理一致）

- 子代理名 `[a-z0-9_]+`；入口 `{name}/{name}.ts` 或 `{name}/index.ts`（前者优先）；纯 `{name}/{name}.md` 即零 TS 简化定义
- 契约类型从 `@gebai/sdk` 导入；node 工具值导入走 `@gebai/sdk/node`
- 依赖组件写 `custom/core/{lib}`，子代理内相对引用 `../../core/{lib}`
- typecheck：`bun run typecheck:custom`（根 `bun run typecheck` 已自动包含）

## 迁移

新版本歌白发布后：`cp -r custom/ <新仓库根>/`（目录内文件覆盖同名，`.gitkeep` 无碍），重启即生效。
二开资产与上游升级互不干扰。

## 前端脚本（`custom/web/`）

Web UI 浏览器端的二开入口，**页面加载即执行、先于歌白应用初始化**（普通 script 同步执行，而入口模块
脚本为 deferred）——承担本地存储初始化、用户注册与登录这类必须先于应用初始化的动作：

- `gebai.config.js`（配置）：经 `window.__GEBAI_WEB_CONFIG__` 预置浏览器环境变量、把宿主 localStorage
  映射为歌白设置、关闭 URL 携带提示词自动运行、调整二开引导的等待上限（`bootTimeout`）。
- `init.js`（初始化脚本，产物根名 `gebai.custom.js`）：可执行任意初始化逻辑——直接读写 localStorage、
  调 `/api/v1/auth/*` 完成注册/登录（令牌写 `gebai.auth.token`，**同时写同名 cookie**——图片/视频/下载等
  原生资源请求只能靠 cookie 带凭证，示例里的 `gebaiSyncToken` 即此）、或写入宿主登录态供「外部身份兑换」
  自动换令牌；需要 await 的动作赋给 `window.__GEBAI_WEB_BOOT__`（Promise / 返回 Promise 的函数 /
  二者组成的数组），歌白会在应用初始化最早期等待其完成（超时与异常只记控制台警告、不阻塞页面）。
  另可给 `window.__GEBAI_AUTH__` 赋值整体接管「令牌读/写/清 + 请求头构造」（见示例例 4），
  典型场景：同一个 IP 上并排多套歌白——cookie 按 host 共享会互相覆盖，改 sessionStorage/自定义头即完全隔离。

**启用方式：复制示例改名**——目录内的 `gebai.config.example.js` 与 `init.example.js` 是带注释的模板，
**不参与接入**；把它们复制为 `gebai.config.js` / `init.js` 即生效。之所以用示例名：本目录属二开域，
上游版本更新时会把 `custom/` 整体复制到新仓库根——生效文件不会被覆盖，示例随上游刷新供对照参考。

生效后构建（或 `vite dev`）时由 vite 插件带到前端产物根，`index.html` / `files.html` 自动在入口模块
脚本之前引入**已启用的那些脚本**（按产物根文件名自动接入、无需清单；`init.js` 以并列命名
`gebai.custom.js` 输出；未启用的脚本不产出也不注入）。纯前端文件，改完刷新页面即生效（不需要重启服务）。

## 凭证来源（`custom/auth/`）

服务端鉴权链的可插拔入口：REST 请求「从哪里认出用户」默认是 Bearer → Basic → cookie
`gebai.auth.token`（见 DESIGN「凭证来源扩展点」），本目录可增删来源。目录里每个 `*.ts`/`*.js`
（非 `*.test.*`、非 `_` 前缀）**默认导出**一个凭证来源或来源数组即注册，无需清单：

```ts
// custom/auth/gateway_header.ts —— 企业网关注入的身份头
import type { CredentialSource } from "@gebai/server/credential-sources"
const source: CredentialSource = {
  name: "gateway-header",
  safeMethodsOnly: false, // 该头由网关剥离外部伪造，故可全方法生效
  resolve: (c) => c.userByName(c.header("x-gateway-user") ?? ""),
}
export default source
```

- 与数据级 `GEBAI_CREDENTIAL_SOURCES`（`bearer`/`basic`/`cookie:<名>`/`header:<名>`）可同时使用：
  环境变量**显式设置**即以其为准、代码级来源前置（便于改写内置行为）；未设置则内置缺省链在前、
  代码级追加在后。
- **安全边界（代码级来源不例外）**：来源只负责取出身份，校验一律下沉（令牌型交 `c.authorize()`
  验签+TTL+disabled；用户名型经 `c.userByName()` 只能命中**已启用**用户）；`cookie` 类默认仅 GET/HEAD。
- **失败隔离**：单个文件 import/导出形态出错只记启动 warning，不影响其余来源与框架可用性。
- **仅源码形态生效**：二进制/镜像部署无源码树，该配置改用 `GEBAI_CREDENTIAL_SOURCES`。
