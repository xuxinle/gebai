# 歌白（GEBAI Agent）· 容器部署

服务模式镜像：Ubuntu 24.04 基础镜像 → 构建阶段跑完仓库既有构建链 → `bun build --compile`
产出**单文件 Linux 可执行** → 运行阶段只有系统库加这一个二进制（不带 node_modules、不带 bun）。

- `../Dockerfile`：镜像定义（多阶段、构建参数、运行期依赖逐项注明用途）
- `build.sh` / `build.ps1`：构建脚本（Linux/macOS 与 Windows 同参数）
- `compose.yaml`：最小可跑的 compose 示例

## 快速开始

```bash
# 1) 构建（默认标签 gebai:<package.json 版本>）
docker/build.sh                       # Windows: pwsh -File docker/build.ps1
docker/build.sh --smoke               # 构建后自动起容器自检（健康检查 + 隔离探测）再清理

# 2) 运行
docker run -d --name gebai -p 3000:3000 \
  -v gebai-data:/data \
  -e GEBAI_LLM_API_KEY=sk-… \
  -e GEBAI_LLM_MODEL=gpt-4o-mini \
  gebai:0.1.0

# 3) 打开 http://localhost:3000
```

## 构建

| 参数（脚本开关） | 默认 | 说明 |
|---|---|---|
| `--profile <名\|路径>`（`BUILD_PROFILE_B64`） | 空（全量） | 裁剪与定制档案（见「裁剪与镜像本体定制」）；预置档案在 `docker/profiles/` |
| `--set <键=值>`（`BUILD_SET`） | 空 | 档案的字段级覆盖（可多次；CLI 优先于档案） |
| `--print-plan` | — | 只打印计划与报告，不构建（需本机有 bun） |
| `--base-image` / `--bun-image` | `ubuntu:24.04` / `oven/bun:<BUN_VERSION>` | 基础镜像与 bun 来源镜像（等价 `image.base` / `image.bun_image`） |
| `--user` / `--uid` / `--gid` | `gebai` / `1000` / `1000` | 运行用户与 uid/gid（非 root） |
| `--data-dir` / `--port` / `--mode` / `--host` / `--tz` | `/data` / `3000` / `server` / `0.0.0.0` / `UTC` | 数据根与挂载卷、监听端口、运行模式与地址、容器时区 |
| `--label` / `--extra-packages` | 空 | 额外镜像标签（可多次）、裁剪组之外的额外 apt 包（逗号分隔） |
| `--apt-mirror` / `--npm-registry` / `--build-proxy` | 空 | 内网 apt 源、npm 源、构建期 HTTP(S) 代理 |
| `--run-as-root` / `--no-healthcheck` | off | 显式以 root 运行；不写 HEALTHCHECK（改选 `runtime-nohealthcheck` 阶段） |
| `UBUNTU_VERSION` | `24.04` | 基础镜像版本（兼容开关；等价于 `--base-image ubuntu:<版本>`） |
| `BUN_VERSION` | `1.4.2` | bun 版本（兼容开关；等价于 `--bun-image oven/bun:<版本>`）。**最终镜像不含 bun**；须能解析仓库的 `bun.lock`（不兼容时自动退回无锁定安装并告警） |
| `WITH_CV`（`--no-cv`） | `1` | 内嵌本地 CV（PP-OCR 模型 + onnxruntime-web 运行时）；`=0` 等价 `assets.cv=0`，本地 OCR/视觉定位不可用 |
| `CV_MODEL_BASE`（`--cv-model-base`） | hf-mirror 的 RapidOCR 托管 | 内网/离线改自备镜像 |
| `WITH_BROWSER`（`--with-browser`） | `0` | 安装 playwright chromium（浏览器类子Agent 用）；`=1` 等价 `assets.browser=1 system.chromium=1`，镜像显著增大 |
| `PLAYWRIGHT_VERSION` | `1.62.1` | 须与仓库依赖一致，否则运行时版本不匹配 |
| `BUN_TARGET`（`--target`） | 空（按构建机架构） | bun 编译目标（如 `bun-linux-arm64`；与 `--target` 阶段选择不同义） |
| `HTTP_PROXY` / `HTTPS_PROXY` / `NO_PROXY` | 空 | Docker 预定义代理 build-arg（客户端默认从宿主环境透传；仅构建阶段生效） |

**架构限制**：二进制内嵌 `@resvg/resvg-js`（平台原生模块），跨架构编译会嵌错平台 —— 本镜像只支持
「构建机架构 = 目标架构」。`linux/arm64` 请在 arm64 机器上构建（或在该架构的 CI runner 上）。

**构建上下文**：`.dockerignore` 排除了工作区的运行期数据与大体积资产（实测 `resources` 68GB、
`infer` 9.4GB、`vendor` 0.7GB、`node_modules` 1.5GB，以及 `users/`、`tmp/`、`.env`、桌面端产物
`*.exe`/`*.bun-build` 等），只传源码；改前端或后端源码不必重装依赖（依赖层只依赖各 `package.json`）。

**构建期网络前置**（全部在构建阶段，与运行期无关）：

| 用途 | 目标 | 不可达时的处理 |
|---|---|---|
| 拉基础镜像 | Docker Hub（`ubuntu:24.04`、`oven/bun`） | 配镜像加速器（`/etc/docker/daemon.json` 的 `registry-mirrors`，或 Docker Desktop 同名设置）；本机实测 `docker.m.daocloud.io`、`docker.1ms.run`、`docker.xuanyuan.me` 可拉到这两个镜像 |
| 装依赖 | npm registry | `--npm-registry` 指向内网源（`bun install` 走 `BUN_CONFIG_REGISTRY`） |
| 装系统包 | Ubuntu apt（`archive.ubuntu.com`） | 用 `--apt-mirror` 指向内网源（只换主机名，保留组件行） |
| CV 模型（`WITH_CV=1`） | `hf-mirror.com` | `--cv-model-base` 指向自备镜像；也可先 `bun run resources:download` 后把模型放进构建上下文（脚本优先用本地已有模型） |
| 浏览器（`--with-browser`） | playwright CDN | 内网无出口时不要该开关（浏览器类子Agent 不可用） |

上述任何一项需经代理时，用 `--build-proxy http://proxy:3128`（或宿主 `HTTP_PROXY`/`HTTPS_PROXY` 环境变量，Docker 客户端默认透传）。

## 裁剪与镜像本体定制

镜像支持从全量裁到只留所需能力，也支持把**镜像本体**改成自部署需要的口径：**能力层**（子Agent /
全局工具 / 内嵌资产）、**前端资源层**（vendor 引擎组）、**系统层**（运行期 apt 包组）、**镜像本体**
（基础镜像 / 运行用户 / 数据根 / 端口 / 时区 / 标签 / 额外包 / apt・npm 源 / 健康检查）。两者由同一份
`--profile` 档案 + `--set` 覆盖声明；计划由 `scripts/build-image-plan.ts` 合并生成，**构建日志会打印
完整报告**，同一份计划也留在镜像内 `/etc/gebai/build-plan.env`——镜像里到底裁了什么、定成什么口径，随时可查。

### 裁剪档案

档案是 JSON（预置在 `docker/profiles/`，也可给任意路径）。字段全部可选，**缺省即全开/默认**：

| 段 | 字段 | 声明什么 | 裁掉后（口径如实） |
|---|---|---|---|
| `sub_agents` | `enable[]` / `disable[]`（互斥）/ `preload[]` | 只打包 / 排除指定子Agent；`preload` 烘焙「启动即装载」 | 子Agent 不在产物内（`agent_load` 报未知名）。`enable` 自动连带依赖；`disable` 若使保留者依赖残缺，**构建直接失败**并要求把依赖方一并排除 |
| `tools` | `disable[]` | 全局工具不注册、不暴露 | 工具 schema 不可见、调用报未知工具（实现仍打包，属能力裁剪而非体积裁剪） |
| `assets` | `web_ui` `cv` `d2` `analyzer` `browser` `ripgrep` | 是否内嵌 Web UI / 本地 CV 模型 / 后端 D2.js / tree-sitter 语法 / playwright 驱动与 pwcore / 内置 ripgrep | 逐项降级且提示明确：无界面（`/` 返回说明页，API 与 WS 照常）、本地 OCR 不可用（给出 `GEBAI_CV_MODELS_DIR` 指引）、后端 D2 不可用、语法分析不可用（`read` 分段阅读兜底）、浏览器类子Agent 不可用、grep 回退内置遍历引擎（只降速） |
| `web.vendor` | 组清单 | 前端 vendor 引擎：`monaco` `plantuml` `mermaid` `echarts` `d2js` `xterm` `tree_sitter` | 对应前端渲染/编辑器能力降级（引擎懒加载失败即降级，不破主界面；Monaco 缺失退回轻量编辑器） |
| `system` | `git` `python` `bubblewrap` `fonts` `tzdata` `procps` `chromium` | 运行期 apt 包组 | git/python 类工具不可用、脚本隔离退回环境收敛（有告警）、PDF/图表中文缺字、时区/进程工具缺失、浏览器类子Agent 不可用 |

`ca-certificates`、`curl`（健康探针）、`tini`（PID 1 收尸）是固定基础设施，**不参与裁剪**——缺了它们
容器会「看起来能起、实际不可用」。

档案还可携带运行期段（`prompt` / `tools` / `sub_agents`）：**同一份档案既能做镜像构建裁剪与定制、又能
作为运行期 `GEBAI_PROFILE`**（`assets`/`web`/`system`/`image` 四个构建段运行期不解释也不报错），构建与
运行的能力面因此不会漂移。

### 镜像本体定制（`image` 段）

| 字段 | 缺省 | 说明 |
|---|---|---|
| `base` | `ubuntu:24.04` | 基础镜像（构建阶段与运行阶段同源）；内网/私有仓库指向自备镜像 |
| `bun_image` | `oven/bun:1.4.2` | 仅取 bun 可执行文件的来源镜像；**须能解析仓库的 `bun.lock`**（不兼容时构建退回无锁定安装并告警） |
| `apt_mirror` | 空 | apt 源（内网镜像）；只换主机名，保留基础镜像自带的发行版/组件行（deb822 与旧式 sources.list 两种布局都处理） |
| `npm_registry` | 空 | npm 源（`bun install` 走 `BUN_CONFIG_REGISTRY`） |
| `proxy` | 空 | 构建期 HTTP(S) 代理（apt/npm/模型下载）；不写进最终镜像的运行环境 |
| `user` / `uid` / `gid` | `gebai` / `1000` / `1000` | 容器内运行用户；基础镜像里占用同 uid 的用户（如 Ubuntu 的 `ubuntu`）会先让位。改 `user` 时家目录自动跟随（`/home/<用户>`） |
| `home` / `shell` | `/home/gebai` / `/bin/bash` | 运行用户家目录与登录 shell |
| `run_as_root` | `false` | 显式以 root 运行；服务模式的沙箱与脚本隔离以非特权用户为前提，root 下部分工具会拒绝执行（构建时会提示） |
| `data_dir` | `/data` | 数据根（`GEBAI_HOME` + 挂载卷 + WORKDIR）。运行期挂载请一致：`-v <卷>:<data_dir>` |
| `mode` / `host` / `port` | `server` / `0.0.0.0` / `3000` | `GEBAI_MODE` / `GEBAI_HOST` / `GEBAI_PORT`（端口同时作为 EXPOSE） |
| `tz` | `UTC` | 容器时区（写 `TZ`）；设了会自动确保 `tzdata` 被安装（否则时区静默失效） |
| `extra_packages` | `[]` | 裁剪组之外额外安装的 apt 包（并入同一次 apt 安装） |
| `labels` | `{}` | 额外镜像标签（键 → 值）；经 `docker build --label` 传入 |
| `healthcheck` | `{ path: "/api/health" }` | `false` = 不写 HEALTHCHECK（改选 `--target runtime-nohealthcheck` 阶段）；`path` 可改（探针端口跟随 `port`）。**间隔/超时/启动期/重试写在 Dockerfile 指令里**（实测 Docker 解析阶段不做变量展开，构建参数注入不了），需调整请用运行期选项 |

对应的命令行开关（等价于 `--set image.<字段>=值`）：`--base-image` `--bun-image` `--user` `--uid` `--gid`
`--data-dir` `--port` `--mode` `--host` `--tz` `--label`（可多次）`--extra-packages` `--apt-mirror`
`--npm-registry` `--build-proxy` `--run-as-root` `--no-healthcheck`。

```bash
# 内网/私有部署：自备镜像与源，中文时区，三个标签留痕
docker/build.sh --profile intranet -t registry.internal.example.com/gebai:0.1.0 --push

# 只改镜像本体（能力面取全量）
docker/build.sh --base-image registry.internal/ubuntu:24.04 \
  --user acme --uid 2001 --data-dir /srv/gebai --port 8080 --tz Asia/Shanghai \
  --label owner=acme --extra-packages vim,less --no-healthcheck -t gebai:acme
```

**两处口径需要知道**（都是 Docker 本身的限制，不是本镜像的取舍）：

- **健康检查的间隔/超时/启动期/重试**：`HEALTHCHECK` 的选项在 Dockerfile 解析阶段就固定，不能由构建参数
  注入（写 `${VAR}` 会报 `invalid duration`）。镜像默认 `30s/5s/20s/3`；探针路径与端口可配，且 CMD 用运行期
  变量展开（覆盖 `GEBAI_PORT` 时会跟随）。要改间隔等请在运行期给：

  ```yaml
  healthcheck:
    interval: 10s
    retries: 5
  ```

- **额外镜像标签**用 `docker build --label` 传入，而非 Dockerfile 的 `LABEL` 指令：后者不支持用变量展开
  追加任意数量的标签（`LABEL ... ${VAR}` 会被当键值对解析而报错）。

### 预置档案

| 档案 | 面向 | 要点 |
|---|---|---|
| `full` | 等价于不指定 | 全量 + 镜像本体默认口径（也可当作 `image` 段的可复制模板） |
| `code` | 编码场景 | `code`/`explore` 子Agent（预加载 `code`）+ 编辑器/终端/符号提取 vendor + 语法分析 + 内置 ripgrep；无本地识别、无后端 D2、无浏览器 |
| `minimal` | 纯 API 服务 | 无 Web UI、无内嵌资产、无前端引擎；系统层只留时区与基础设施（实测 338MB） |
| `intranet` | 内网/离线部署 | 自备基础镜像与 apt/npm 源、构建期代理、中文时区、额外包与标签留痕；能力面取全量 |

```bash
docker/build.sh --profile code -t gebai:code          # 预置档案
docker/build.sh --profile ./my-profile.json           # 自定义档案（任意路径）
docker/build.sh --profile code --set assets.cv=1      # 档案 + 字段级覆盖（CLI 优先）
docker/build.sh --print-plan --profile minimal        # 只看计划与报告，不构建
```

`--set` 的键：`assets.<资产>`、`web.vendor`、`system.<组>`、`sub_agents.enable|disable|preload`、
`tools.disable`、`image.<字段>`、`image.healthcheck.<字段>`、`description`。既有开关继续可用：`--no-cv`
等价 `assets.cv=0`，`--with-browser` 等价 `assets.browser=1 system.chromium=1`。

### 裁剪与体积

体积大头按顺序是：Web UI 内嵌产物（含前端 vendor 引擎：monaco 约 24MB、d2js 约 8MB、plantuml+viz 约
8.3MB、mermaid 约 3.5MB、echarts 约 1.1MB）、本地 CV（PP-OCR 模型 + ort 运行时，约 24MB）、D2.js 后端
产物（约 10MB）、tree-sitter 语法集（约 3.7MB）、playwright-core（约 3.7MB）、内置 ripgrep（约 3.2MB），
另有子Agent 代码（含 playwright / feishu / wps 等重模块）。

实测（linux/amd64，同一构建机；全量为 `DESIGN.md` 记录的构建值，裁剪值为本次实测）：

| 构建 | 镜像 | 镜像内二进制 |
|---|---|---|
| 全量（缺省） | 867MB | 244MB |
| `--profile code` | **656MB** | **159.9MB** |
| `--profile minimal` | **338MB** | **124.7MB** |

`code` 裁掉本地 CV、后端 D2、浏览器驱动与 plantuml/mermaid/echarts/d2js 四个前端引擎（保留 monaco/
xterm/tree_sitter：编辑器、终端与符号提取），仅打包 `code`/`explore` 两个子Agent；`minimal` 再关掉
Web UI 内嵌产物、语法集、内置 ripgrep 与 git/python/bubblewrap/fonts/procps 系统包组。两者冒烟自检
均通过（`/api/health` 正常），容器内也可核实：

```bash
docker exec <容器> cat /etc/gebai/build-plan.env          # 该镜像的完整裁剪面与定制口径
docker exec <容器> sh -c 'command -v python3 || echo 已裁'  # 系统层：minimal 下 python3/git/bwrap 均不存在
# 前端 vendor：被裁引擎的请求回退 SPA 入口（返回 index.html 字节），保留的返回真实资源字节
docker exec <容器> curl -s localhost:3000/vendor/monaco/vs/loader.js | wc -c   # 真实资源（数万字节）
docker exec <容器> curl -s localhost:3000/vendor/mermaid.js | head -c 15        # <!doctype html> → 已裁
# 镜像本体定制：用户/数据根/端口/时区、标签、挂载卷、额外包
docker inspect <镜像> --format 'user={{.Config.User}} volumes={{json .Config.Volumes}}'
docker inspect <镜像> --format '{{json .Config.Labels}}'
docker run --rm --entrypoint sh <镜像> -c 'command -v vim'   # 档案声明的额外包
```

裁剪项与定制项在构建日志与计划文件里逐项可见，不需要靠记忆推断镜像能力面；服务端启动日志、工具输出与
前端降级提示同样按「本构建未内嵌 X」如实说明。

## 运行

### 配置注入（两种，可混用）

1. **环境变量**（`-e` / compose 的 `environment`）——适合密钥与容器编排；
2. **`{GEBAI_HOME}/.env`**，即挂卷里的 `/data/.env`——二进制模式启动时自动读取，适合把一整套配置
   随数据卷走。真实环境变量优先于该文件。

模型配置（`GEBAI_LLM_*`/`GEBAI_VISION_*`）等完整清单见仓库根 `.env.example`。

### 首次登录

- **默认**（未设 `GEBAI_ADMIN_PASSWORD_HASH`）：admin 用户禁用，任意访客可在登录页**自助注册**
  （普通角色）；`GEBAI_SIGNUP_MODE=approval` 时注册需 admin 审批——此时须先有 admin。
- **启用 admin**：设置 `GEBAI_ADMIN_PASSWORD_HASH`（格式 `salt:hash`）。生成方式（在源码检出里）：

  ```bash
  bun run --cwd packages/server hash-password        # 交互输入后输出 salt:hash
  ```

### 数据持久化

`<data_dir>`（缺省 `/data`，即 `GEBAI_HOME`）承载全部持久状态：用户、会话、任务与待办、Webhook 配置，
以及二进制模式运行时物化的目录（`vendor/`：playwright 驱动与 pwcore、d2js、ripgrep；`resources/`：内嵌
CV 模型）。数据根可经 `image.data_dir` 改动，挂载须与之一致：`-v gebai-data:/srv/gebai`。

- **用命名卷**（如 `-v gebai-data:/data`）——首启会继承镜像内数据根的属主（缺省 uid 1000）。
- **用宿主目录挂载**时注意属主：容器以非 root 用户运行（缺省 `gebai`，uid 1000），宿主目录需该 uid
  可写，否则会出现「会话目录不可写」类错误。确有需要可按宿主 uid 重建镜像（`image.uid`）或调整目录属主。

## 容器内的隔离与安全

镜像默认 `GEBAI_MODE=server`，因此仓库既有的两条启动期保证直接生效（启动日志会打印
`auth=server, sandbox=true`）：

- **路径沙箱强制开启**（`GEBAI_SANDBOX=off` 与服务模式互斥，启动即拒）；
- **会话目录脚本隔离强制开启**（`GEBAI_SCRIPT_ISOLATION=off` 与服务模式互斥）：脚本 `cwd` 绑定会话目录，
  `HOME`/`USERPROFILE`/`TEMP`/`TMP`/`TMPDIR`/`XDG_*` 全部指向 `{会话}/script-env/`。

在此之上还有一层**文件系统隔离**（bubblewrap）：它需要容器允许创建 user namespace，而**这一步受容器运行时
限制**——不是镜像里装上 `bubblewrap` 就够了。实测（Ubuntu 24.04 容器，Docker 29，非 root 用户）：

| 运行方式 | user namespace | 脚本实际生效档位 |
|---|---|---|
| `docker run`（默认 seccomp） | ✗ `unshare: Operation not permitted` | 环境收敛（HOME/TEMP/XDG 在会话目录内） |
| `--cap-add SYS_ADMIN` | ✓ 能创建，但 bwrap 仍失败：`pivot_root: Operation not permitted` | 环境收敛 |
| `--security-opt seccomp=unconfined` | ✓ | **环境收敛 + bwrap 文件系统隔离**（系统只读、仅会话目录可写、宿主家目录不可见） |

所以要拿满隔离，用 `--security-opt seccomp=unconfined`（compose 里把 `security_opt` 注释打开）。容器内自查：

```bash
docker exec <容器> unshare --user --map-root-user echo ok   # 输出 ok = bwrap 层可用
```

降级是**如实**且安全的：环境收敛始终生效，不会出现「以为隔离了其实没隔离」；显式配置
`GEBAI_SCRIPT_ISOLATION=bwrap` 而不可用时，首次脚本执行会输出一条带失败原因的告警。
`docker/build.sh --smoke` 也会直接打印该容器里 user namespace 是否可用。

## 能力边界（容器内的如实口径）

| 能力 | 状态 | 对应裁剪开关（缺省全开） | 说明 |
|---|---|---|---|
| 会话/任务/工具/子Agent/文件工作台（前端） | 可用 | `assets.web_ui` / `assets.sub_agents` / `tools.disable` | 按档案可裁到「无界面纯 API」（`/` 返回说明页）；前端 vendor 引擎另由 `web.vendor` 裁 |
| 图表渲染（Mermaid/PlantUML/D2/ECharts） | 可用 | `web.vendor`（前端）/ `assets.d2`（后端 D2） | 裁掉后对应引擎走前端懒加载失败降级，D2 后端渲染报「本构建未内嵌」 |
| 本地 OCR / 视觉定位 | 默认可用 | `assets.cv`（`--no-cv`） | 裁掉后工具给出 `GEBAI_CV_MODELS_DIR` 配置指引 |
| 语法分析 / 符号搜索（`analyze` / `search_symbols`） | 可用 | `assets.analyzer` | 裁掉后报「语法分析不可用」并提示改用 `read` 分段阅读 |
| `grep`（内置 ripgrep） | 可用 | `assets.ripgrep` | 裁掉后回退内置遍历引擎：功能不降级、只降速 |
| 浏览器类子Agent（playwright / reverse_site） | 需 `--with-browser` | `assets.browser` + `system.chromium` | 未装浏览器时报「不可用」并给出指引；且同需 user namespace |
| 脚本文件系统隔离（bubblewrap） | 可用（受容器限制） | `system.bubblewrap` | 裁掉或容器未授予 user namespace 时降级为环境收敛（有告警，见「容器内的隔离与安全」） |
| PDF / 图表中文 | 可用 | `system.fonts` | 裁掉后字体缺失，中文会缺字（构建后如需可挂载字体） |
| `py` 工具 / `vision_pip` | 可用 | `system.python` | 裁掉后 Python 解释器不存在，相关工具不可用 |
| `desktop`（截屏/窗口/键鼠） | 不可用 | — | 服务模式下整体拒绝（宿主桌面操控不对远程用户开放） |
| `tts_speak`（文本转语音） | 不可用 | — | 仅 Windows 内置离线语音引擎；工具会如实说明「不做联网合成」。音效/效果/混音为纯计算，可用 |
| 客卿（多语言）子代理 | 不可用 | — | 服务模式整体禁用（无会话隔离的原生进程） |
| `reel`（产品视频制作） | 不可直接使用 | — | 需 Node 运行时 + ffmpeg + Chrome（镜像未装）；要此能力请派生镜像补装 |
| `restart_server` 工具 | 不适用 | — | 容器内请用 `docker restart gebai`（该工具面向宿主机进程拉起的部署，本镜像未验证其在 PID 1 = tini 下的行为） |

## 运维

```bash
docker logs -f gebai                     # 日志（stdout）
docker exec gebai curl -s localhost:3000/api/health   # 健康检查（免鉴权，返回 { ok, boot }）
docker restart gebai                     # 重启
docker compose -f docker/compose.yaml up -d
```

- **升级**：重新构建镜像并重建容器，`/data` 卷原样保留（数据与配置都不动）。
- **反向代理**：Web UI 的 REST/WS/静态资源一律按页面 URL 相对解析，子路径挂载（如 `/gebai/`）
  无需额外配置；代理需转发 WebSocket。
- **多实例**：同一 `/data` 只应有一个实例（调度主实例锁在同一数据根上互斥）；多副本请各自独立卷。

## 故障排查

| 现象 | 处理 |
|---|---|
| 打不开页面 | `docker logs gebai` 看监听地址；确认 `-p 3000:3000` 且容器健康 |
| 登录页无 admin 账号 | 未设 `GEBAI_ADMIN_PASSWORD_HASH`：改用自助注册，或按上文生成哈希后重建容器 |
| 提示会话目录不可写 | 宿主目录挂载的属主问题（容器 uid 1000）——改用命名卷或调整属主 |
| 日志出现 bwrap 不可用告警 | 容器未授予 user namespace：按上文加 `--security-opt seccomp=unconfined`，或接受环境收敛档 |
| 本地 OCR 报模型缺失 | 构建时用了 `--no-cv`（或 `assets.cv=false`），或模型下载失败（看构建日志）；可重建并指定 `--cv-model-base` |
| 浏览器子Agent 报无浏览器 | 用 `--with-browser` 重建镜像（等价 `system.chromium=1`） |
| 某能力报「本构建未内嵌 X / 不可用」 | 属裁剪预期：`docker exec <容器> cat /etc/gebai/build-plan.env` 查看该镜像的裁剪面，对照本文「裁剪」逐项确认；需要该能力则用对应开关重建 |
| 访问 `/` 返回「本构建未内嵌 Web UI」说明页 | 镜像按 `assets.web_ui=false`（如 `minimal` 档案）构建：服务、REST API 与 WebSocket 正常，仅无界面；需界面请用完整构建重建 |
