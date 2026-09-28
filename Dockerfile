# syntax=docker/dockerfile:1
#
# 歌白（GEBAI Agent）· 服务模式镜像
#
# 形态：多阶段构建。构建阶段装依赖 → 跑完仓库既有的构建链（前端产物、子Agent/工具注册表、
# 各类内嵌产物）→ `bun build --compile` 产出**单文件 Linux 可执行**；运行阶段只有基础系统库
# 加这一个二进制，不带 node_modules、不带 bun（二进制自带运行时）。
#
# 为什么用二进制形态而不是「源码 + node_modules」：仓库工作区实测 node_modules 1.5GB、
# resources 68GB、infer 9.4GB，源码形态镜像必然臃肿；二进制形态与桌面端（packages/desktop）
# 走同一条链路，Web UI / 子Agent / 工具 / wasm / ripgrep / playwright 驱动均已内嵌进二进制，
# 运行期不需源码树（`isBinaryMode()` 判定成立，配置从 `{GEBAI_HOME}/.env` 与环境变量读）。
#
# ── 定制体系（全部来自同一份**裁剪档案**，用法与矩阵见 docker/README.md）──
# 档案（`--build-arg BUILD_PROFILE_B64=<base64(JSON)>`）+ 细粒度覆盖（`--build-arg BUILD_SET="assets.d2=0"`）
# 汇成一份**构建计划**（`scripts/build-image-plan.ts`），并把裁剪报告打进构建日志。档案分四段：
#   sub_agents / tools / assets / web      能力层：子Agent、全局工具、内嵌资产、前端 vendor 引擎组
#   system                                 系统层：运行期 apt 包组（git/python/bubblewrap/fonts/tzdata/procps/chromium）
#   image                                  镜像本体：基础镜像、运行用户与 uid/gid、数据目录、端口、时区、
#                                          标签、额外 apt 包、apt/npm 源、健康检查
# 落地路径两条：
#  ① **构建步骤级**（能力裁剪、系统包组、额外包、apt/npm 源）→ 计划文件 /tmp/gebai-plan.env，
#     构建阶段 source 后驱动各内嵌产物脚本、前端 vendor 拷贝与 apt 安装；计划同时留在镜像内
#     `/etc/gebai/build-plan.env`，随时可核对该镜像究竟裁了什么、定的什么口径。
#  ② **指令级**（基础镜像、USER/VOLUME/EXPOSE/ENV/LABEL/HEALTHCHECK）→ build-arg：Docker 指令无法在
#     构建过程中条件化，故由 `docker/build.sh` 在宿主侧跑计划器后经 `--build-arg` 传入（`build.sh`
#     的 `--base-image` / `--user` / `--uid` / `--data-dir` / `--port` / `--tz` / `--label` 等开关，
#     或统一走 `--set image.<字段>=值`）。手工 `docker build` 时按下方 ARG 名直接传即可。
#  健康检查的**禁用**走阶段选择：`--target runtime-nohealthcheck`（镜像默认阶段带 HEALTHCHECK）。
#
# 构建参数（见 docker/build.sh 的对应开关）：
#   BASE_IMAGE / BUN_IMAGE      构建与运行的基础镜像、bun 来源镜像（缺省 ubuntu:24.04 / oven/bun:1.4.2）
#   IMAGE_USER / IMAGE_UID / IMAGE_GID / IMAGE_HOME / IMAGE_SHELL / IMAGE_RUN_AS_ROOT / IMAGE_RUNTIME_USER
#                               运行用户（缺省非 root 的 uid/gid 1000 `gebai`；run_as_root=1 时以 root 运行）
#   IMAGE_DATA_DIR / IMAGE_PORT / IMAGE_MODE / IMAGE_HOST / IMAGE_TZ
#                               数据根（挂载卷与 GEBAI_HOME）、监听端口、运行模式与地址、容器时区
#   额外镜像标签由 `docker build --label` 传入（不经本文件）。
#   HEALTHCHECK_*               健康检查开关与参数（间隔/超时/启动期/重试/探针路径）
#   APT_MIRROR / NPM_REGISTRY / BUILD_PROXY
#                               内网 apt 源、npm 源与构建期 HTTP(S) 代理
#   BUN_TARGET                  bun 编译目标（跨架构时显式指定，缺省按构建机架构）
#   BUILD_PROFILE_B64 / BUILD_PROFILE_NAME / BUILD_SET
#                               裁剪档案（base64 JSON）、档案名（写进标签与计划）、细粒度覆盖
#   WITH_CV / WITH_BROWSER      兼容开关：=0 等价 assets.cv=0；=1 等价 assets.browser=1 + system.chromium=1
#   CV_MODEL_BASE               CV 模型下载源（内网可换镜像）默认 hf-mirror 的 RapidOCR 托管
#   PLAYWRIGHT_SOURCE / PLAYWRIGHT_DIR / PLAYWRIGHT_DOWNLOAD_HOST / PLAYWRIGHT_DEPS
#                               浏览器供给（见「离线浏览器」）：download（本地 playwright 包 + 可选下载源，
#                               不用 bunx）/ local（预置目录，零网络）/ off；系统依赖 auto|清单路径|off
#   浏览器版本由仓库依赖（node_modules/playwright-core/browsers.json）唯一确定，不另设版本参数。
#   HTTP_PROXY / HTTPS_PROXY / NO_PROXY  Docker 预定义代理 build-arg（按需，仅构建阶段生效）
#
# 构建器：不依赖 BuildKit（未用 `RUN --mount`，普通 `docker build` 也能构建）；有 buildx 时
# 可额外用于 --platform / --push。
#
# 架构：二进制内嵌的 @resvg/resvg-js 是**平台原生模块**，跨架构编译会嵌错平台 —— 本镜像只支持
# 「构建机架构 = 目标架构」（linux/amd64 在 amd64 上构建、linux/arm64 在 arm64 上构建）。

ARG BASE_IMAGE=ubuntu:24.04
ARG BUN_IMAGE=oven/bun:1.4.2
ARG NODE_IMAGE=node:22-slim

# ── bun 来源：在线取 `BUN_IMAGE` 自带的 bun，离线取预置文件（`docker/bun/bun`）──
# 本阶段只负责把 bun 摆到 /opt/gebai-bun/bun（两种来源二选一，由文件是否就位决定）。
# 离线时 build.sh 会把 BUN_IMAGE 指向 BASE_IMAGE（内网可达）作占位——FROM 不能条件化，
# 而内网拉不到 oven/bun；该阶段退化为壳，真正的 bun 由预置文件提供。
FROM ${BUN_IMAGE} AS bun-src
COPY docker/bun/ /tmp/gebai-bun/
RUN set -e; mkdir -p /opt/gebai-bun; \
    if [ -x /tmp/gebai-bun/bun ]; then \
      echo "[docker] bun 来自预置文件（零网络）：docker/bun/bun"; \
      install -m755 /tmp/gebai-bun/bun /opt/gebai-bun/bun; \
    elif [ -x /usr/local/bin/bun ]; then \
      echo "[docker] bun 来自镜像 ${BUN_IMAGE}"; \
      install -m755 /usr/local/bin/bun /opt/gebai-bun/bun; \
    else \
      echo "[docker] 未取到 bun：既无预置文件 docker/bun/bun，镜像 ${BUN_IMAGE} 里也没有 /usr/local/bin/bun"; exit 1; \
    fi \
 && /opt/gebai-bun/bun --version

# ── node 来源：浏览器桥接是 `Bun.spawn(["node", driver])`，没有 node 就没有浏览器能力 ──
# 在线取 `NODE_IMAGE`，离线取预置文件（`docker/node/node`），或用系统包（node_source=apt，走 apt 装）。
# 同样只用本阶段把 node 摆到 /opt/gebai-node/node；node_source 非 image 时 build.sh 把 NODE_IMAGE
# 指向 BASE_IMAGE 作占位（该阶段退化为壳，不从它取 node）。
FROM ${NODE_IMAGE} AS node-src
COPY docker/node/ /tmp/gebai-node/
RUN set -e; mkdir -p /opt/gebai-node; \
    if [ -x /tmp/gebai-node/node ]; then \
      echo "[docker] node 来自预置文件（零网络）：docker/node/node"; \
      install -m755 /tmp/gebai-node/node /opt/gebai-node/node; \
    elif [ -x /usr/local/bin/node ]; then \
      echo "[docker] node 来自镜像 ${NODE_IMAGE}"; \
      install -m755 /usr/local/bin/node /opt/gebai-node/node; \
    else \
      echo "[docker] node 未由本阶段提供（改用 apt 或不需要浏览器时属正常）"; \
    fi

# ══════════════════════════ 构建阶段 ══════════════════════════
FROM ${BASE_IMAGE} AS builder
ARG BASE_IMAGE
ARG BUN_IMAGE
ARG BUN_TARGET
ARG BUILD_PROFILE_B64=""
ARG BUILD_PROFILE_NAME=""
ARG BUILD_SET=""
ARG WITH_CV=1
ARG WITH_BROWSER=0
ARG CV_MODEL_BASE
ARG APT_MIRROR=""
ARG NPM_REGISTRY=""
ARG BUILD_PROXY=""
# Docker 预定义代理 build-arg（客户端默认从宿主环境透传；也可经 --build-arg 显式指定）
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ENV DEBIAN_FRONTEND=noninteractive \
    HTTP_PROXY=${HTTP_PROXY} \
    HTTPS_PROXY=${HTTPS_PROXY} \
    NO_PROXY=${NO_PROXY} \
    BUN_CONFIG_REGISTRY=${NPM_REGISTRY}
COPY --from=bun-src /opt/gebai-bun/bun /usr/local/bin/bun

# apt 源替换（内网/离线）：只换主机名，保留基础镜像自带的发行版/组件行——整份覆盖会丢掉
# updates/backports 等组件。deb822（Ubuntu 24.04+ / Debian 12+）与旧式 sources.list 两种布局都处理。
RUN if [ -n "$APT_MIRROR" ]; then \
      m="$(printf '%s' "$APT_MIRROR" | sed 's/[&#\\]/\\&/g')"; \
      for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.sources /etc/apt/sources.list.d/*.list; do \
        [ -f "$f" ] || continue; \
        sed -i -E "s#https?://(archive\|security)\.ubuntu\.com/ubuntu#${m}#g; s#https?://deb\.debian\.org/debian#${m}#g" "$f"; \
      done; \
      echo "[docker] apt 源已替换为 $APT_MIRROR"; \
    fi \
 && apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git unzip \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# 依赖清单先入层：源码改动不必重装依赖（.dockerignore 已排除 node_modules/dist）；
# bun.lock 未入库（见 .gitignore），存在时用通配路径让它参与构建以固定版本，缺失时按 package.json 解析
COPY package.json bun.lock* tsconfig.base.json turbo.json ./
COPY packages/server/package.json packages/server/
COPY packages/web/package.json packages/web/
COPY packages/agents/package.json packages/agents/
COPY packages/sdk/package.json packages/sdk/
COPY packages/desktop/package.json packages/desktop/
# lockfile 格式随 bun 版本演进：若构建机的 lockfile 比镜像内 bun 新（"Unknown lockfile version"），
# 退回按 package.json 解析并告警（否则构建卡在依赖层，且报错与代码无关）；其它 frozen 失败照旧中断，
# 不掩盖真实依赖冲突。要严格锁版本，让 BUN_IMAGE 与 lockfile 生成版本一致。
RUN if [ -f bun.lock ]; then \
      if ! bun install --frozen-lockfile > /tmp/bun-install.log 2>&1; then \
        if grep -qi "lockfile" /tmp/bun-install.log; then \
          echo "[docker] 警告：bun.lock 不被镜像内 bun（${BUN_IMAGE}）识别，退回按 package.json 解析（无版本锁定）"; \
          bun install; \
        else \
          cat /tmp/bun-install.log; \
          exit 1; \
        fi; \
      fi; \
    else \
      bun install; \
    fi
COPY . .

# ── 构建计划：档案（base64 build-arg）+ 细粒度覆盖 + 兼容开关三者合并，落成 /tmp/gebai-plan.env ──
# 计划器同时把裁剪报告打进构建日志（每一项 开/关 与运行期 apt 包数量）——构建产物带什么能力一目了然。
RUN set -e; \
    args=""; \
    if [ -n "$BUILD_PROFILE_B64" ]; then args="$args --profile-b64 $BUILD_PROFILE_B64"; fi; \
    for s in $BUILD_SET; do args="$args --set $s"; done; \
    if [ "$WITH_CV" = "0" ]; then args="$args --set assets.cv=0"; fi; \
    if [ "$WITH_BROWSER" = "1" ]; then args="$args --set assets.browser=1 --set system.chromium=1"; fi; \
    bun run scripts/build-image-plan.ts $args --out /tmp/gebai-plan.env

# 前端产物（含 build-vendor：把 plantuml/mermaid/echarts/d2/xterm/monaco/tree-sitter 拷进 public/vendor，
# 再按 GEBAI_WEB_VENDOR 裁剪）。裁剪掉 Web UI 时跳过 vite 构建，build-web-bundle 写空内嵌清单
# （文件必须存在：static.ts 静态 require 该模块；空清单 → 无界面构建的说明页）。
RUN set -a && . /tmp/gebai-plan.env && set +a \
 && if [ "$GEBAI_BUILD_WEB_UI" = "1" ]; then \
      bun run --cwd packages/web build; \
    else \
      echo "[docker] 裁剪：跳过 Web UI 构建（本镜像无内嵌界面，服务与 API 不受影响）"; \
    fi \
 && bun run packages/server/scripts/build-web-bundle.ts

# 内嵌产物与注册表：Web UI bundle / 子Agent / 全局工具 / D2.js / tree-sitter wasm /
# playwright 驱动与 pwcore / CV 驱动 / 内置 ripgrep（grep 工具在二进制形态下无系统依赖）。
# 各脚本读同一个计划：被裁剪项写空产物，且空产物在运行期走「本构建未内嵌 X」的如实降级。
RUN set -a && . /tmp/gebai-plan.env && set +a \
 && bun run packages/server/scripts/build-subagents.ts \
 && bun run packages/server/scripts/build-tools.ts \
 && bun run packages/server/scripts/build-d2js.ts \
 && bun run packages/server/scripts/build-analyzer-wasm.ts \
 && bun run packages/server/scripts/build-driver-embed.ts \
 && bun run packages/server/scripts/build-pwcore-embed.ts \
 && bun run packages/server/scripts/build-cvdriver-embed.ts \
 && bun run packages/server/scripts/build-rg-embed.ts \
 && if [ -n "$CV_MODEL_BASE" ]; then export GEBAI_CV_MODEL_BASE="$CV_MODEL_BASE"; fi \
 && bun run packages/server/scripts/build-cv-embed.ts
# 本地 CV（PP-OCR 模型 + onnxruntime-web）：模型缺失时脚本写空清单并告警、构建不失败
# （运行期降级为「本地 OCR 不可用」，并在工具输出里给出 GEBAI_CV_MODELS_DIR 配置指引）。

# 单文件可执行（--external 排除 d2：二进制模式从内嵌产物物化，dev/dist 形态才 import 包）
RUN mkdir -p /out \
 && bun build packages/server/src/index.ts --compile ${BUN_TARGET:+--target=$BUN_TARGET} \
      --outfile=/out/gebai --external @terrastruct/d2 \
 && ls -lh /out/gebai

# 可选：playwright 浏览器（由计划 PLAN_WITH_BROWSER 决定），三条供给通道（见 docker/README.md「离线浏览器」）：
#   local    ——把预置目录（PLAYWRIGHT_DIR，缺省 docker/browsers）拷入（零网络；离线/内网主通道）
#   download ——从 PLAYWRIGHT_DOWNLOAD_HOST（缺省 playwright CDN）拉；**不用 bunx**（bunx 会去 npm 取包，
#              内网无 npm 出口时直接用不了），改用 bun 跑构建阶段 node_modules 里已装的 playwright CLI；
#              期望的浏览器 revision 从 playwright-core/browsers.json 读，不用会漂移的硬编码版本号
#   off      ——不装（system.chromium 已置位也不给，用于明确声明）
# 系统依赖由 PLAYWRIGHT_DEPS 决定：auto（--with-deps）/ 预置清单路径 / off；实际安装的依赖清单写入
# /etc/gebai/playwright-deps.txt 随镜像保留，可从镜像导出给内网复用。
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/ms-playwright
RUN set -e; mkdir -p /opt/ms-playwright; touch /tmp/chromium-deps.txt; \
    set -a; . /tmp/gebai-plan.env; set +a; \
    if [ "$PLAN_WITH_BROWSER" != "1" ] || [ "$PLAN_BROWSER_SOURCE" = "off" ]; then \
      echo "[docker] 计划未启用浏览器（system.chromium=$PLAN_WITH_BROWSER, browser_source=$PLAN_BROWSER_SOURCE）：playwright/reverse_site 等浏览器子Agent 不可用"; \
    else \
      # 期望 revision：由计划器从仓库依赖（playwright-core/browsers.json）算出并写进计划文件——
      # 与运行期所用模块同一份真相，不用会漂移的硬编码版本号
      rev="$PLAN_PLAYWRIGHT_REVISION"; \
      if [ -z "$rev" ]; then echo "[docker] 无法确定期望的 chromium revision（未找到 playwright-core/browsers.json，依赖是否已安装？）"; exit 1; fi; \
      if [ "$PLAN_BROWSER_SOURCE" = "local" ]; then \
        # 可用的预置目录：完整 chromium 优先，其次 headless shell（playwright 1.49+ 的无头模式默认用
        # chromium-headless-shell；driver 以 headless:true 启动，两都可以）。两者都没有则报错。
        have=""; \
        for d in chromium-$rev chromium_headless_shell-$rev; do \
          if [ -d /src/"$PLAN_BROWSER_DIR"/$d ]; then have="$d"; break; fi; \
        done; \
        if [ -z "$have" ]; then \
          echo "[docker] 预置目录 $PLAN_BROWSER_DIR 里没有匹配的浏览器：期望 chromium-$rev 或 chromium_headless_shell-$rev（来自仓库依赖 playwright-core/browsers.json），实际内容："; \
          ls /src/"$PLAN_BROWSER_DIR" 2>&1 | head -20 || true; \
          echo "        请用匹配该版本的目录，或 docker/build.sh --export-browsers <目录> 重新导出"; exit 1; \
        fi; \
        cp -a /src/"$PLAN_BROWSER_DIR"/. /opt/ms-playwright/; \
        echo "[docker] 已从预置目录拷贝浏览器（零网络）：$PLAN_BROWSER_DIR → /opt/ms-playwright（$have）"; \
        if [ "$have" != "chromium-$rev" ]; then \
          echo "[docker] 提示：只预置了 headless shell（无头模式可用，playwright 默认走它）；若需有头/扩展场景请补 chromium-$rev"; \
        fi; \
      else \
        deps_arg=""; if [ "$PLAN_BROWSER_DEPS" = "auto" ]; then deps_arg="--with-deps"; fi; \
        if [ -n "$PLAN_BROWSER_DOWNLOAD_HOST" ]; then export PLAYWRIGHT_DOWNLOAD_HOST="$PLAN_BROWSER_DOWNLOAD_HOST"; fi; \
        dpkg --get-selections | cut -f1 | sort > /tmp/before.txt; \
        # 用 bun 跑本地已装的 playwright CLI：`./node_modules/.bin/playwright` 的 shebang 是
        # `#!/usr/bin/env node`，而构建镜像里只有 bun 没有 node（exit 127）；bunx 则会去 npm
        # 取包（内网无 npm 出口时不可用）——本地 CLI + bun 两者都避开。
        if [ ! -f node_modules/playwright/cli.js ]; then echo "[docker] 未找到 node_modules/playwright/cli.js（依赖是否已安装？）"; exit 1; fi; \
        bun node_modules/playwright/cli.js install $deps_arg chromium; \
        dpkg --get-selections | cut -f1 | sort > /tmp/after.txt; \
        comm -13 /tmp/before.txt /tmp/after.txt > /tmp/chromium-deps.txt; \
        echo "[docker] 浏览器就绪（下载源 ${PLAYWRIGHT_DOWNLOAD_HOST:-playwright CDN}，chromium-$rev，新增系统依赖 $(wc -l < /tmp/chromium-deps.txt) 个）"; \
      fi; \
    fi
# 预置通道的系统依赖：浏览器本体不从网络拉，但它的系统库仍要装。
# **清单必须在目标镜像内算**——在宿主机上算的清单只反映宿主缺什么（宿主已装了一堆库，结果偏少），
# 拿它去装裸镜像必然缺库（实测：宿主算的 39 个包不够，chrome-headless-shell 报 libatk-1.0.so.0 缺失）。
# playwright 自带依赖表，`install-deps --dry-run` 在容器内离线算出的是该镜像真正需要的清单：
#   auto —— 容器内现算（需 apt 可达，内网配 --apt-mirror）
#   路径 —— 用预置清单文件（完全无 apt 出口时，配合预先下好的 .deb / 自建源）
#   off  —— 不装（基础镜像已含依赖）
RUN set -a; . /tmp/gebai-plan.env; set +a; \
    if [ "$PLAN_WITH_BROWSER" = "1" ] && [ "$PLAN_BROWSER_SOURCE" = "local" ]; then \
      case "$PLAN_BROWSER_DEPS" in \
        off) echo "[docker] 预置通道：不装 chromium 系统依赖（browser_deps=off）"; touch /tmp/chromium-deps.txt ;; \
        auto) \
          if [ ! -f node_modules/playwright/cli.js ]; then echo "[docker] 未找到 node_modules/playwright/cli.js（依赖是否已安装？）"; exit 1; fi; \
          # playwright 的依赖解析依赖 apt 的包索引（本阶段前面装完工具后清掉了）——先刷新；\
          # 装这些依赖本身也需要 apt 可达，所以这一步不额外增加前提
          apt-get update >/dev/null 2>&1 || { echo "[docker] apt-get update 失败（无法确定 chromium 系统依赖；内网请配 --apt-mirror，或改用 browser_deps=off）"; exit 1; }; \
          # 注意：无依赖可装时它输出列表并**返回非零**（「还缺这些」的语义），所以不能拿退出码当失败；\
          # 只认「是否拿到清单」
          bun node_modules/playwright/cli.js install-deps --dry-run chromium > /tmp/deps-raw.txt 2>/tmp/deps-err.txt || true; \
          sed -n '/^  /s/^  //p' /tmp/deps-raw.txt | sort -u > /tmp/chromium-deps.txt; \
          if [ ! -s /tmp/chromium-deps.txt ]; then \
            echo "[docker] 本镜像内未算出 chromium 系统依赖清单（playwright 依赖表不可用？）——可改用 browser_deps=<清单路径> 或 off；原输出末尾："; \
            tail -5 /tmp/deps-err.txt /tmp/deps-raw.txt 2>/dev/null; exit 1; \
          fi; \
          echo "[docker] 预置通道：依赖清单在镜像内算出（$(wc -l < /tmp/chromium-deps.txt) 个包，需 apt 可达）"; ;; \
        *) \
          if [ ! -f "/src/$PLAN_BROWSER_DEPS" ]; then echo "[docker] 依赖清单不存在：$PLAN_BROWSER_DEPS（需为构建上下文内文件路径）"; exit 1; fi; \
          cp "/src/$PLAN_BROWSER_DEPS" /tmp/chromium-deps.txt; \
          echo "[docker] 预置通道：按清单装系统依赖 $PLAN_BROWSER_DEPS（$(wc -l < /tmp/chromium-deps.txt) 个）"; ;; \
      esac; \
    fi

# ══════════════════════════ 运行阶段 ══════════════════════════
FROM ${BASE_IMAGE} AS runtime
ARG BUILD_PROFILE_NAME=""
ARG APT_MIRROR=""
ARG NODE_IMAGE=node:22-slim
ARG IMAGE_USER=gebai
ARG IMAGE_UID=1000
ARG IMAGE_GID=1000
ARG IMAGE_HOME=/home/gebai
ARG IMAGE_SHELL=/bin/bash
ARG IMAGE_RUN_AS_ROOT=0
ARG IMAGE_RUNTIME_USER=gebai
ARG IMAGE_DATA_DIR=/data
ARG IMAGE_PORT=3000
ARG IMAGE_MODE=server
ARG IMAGE_HOST=0.0.0.0
ARG IMAGE_TZ=UTC
ARG IMAGE_VERSION=""
ENV DEBIAN_FRONTEND=noninteractive
# 构建计划随镜像保留（/etc/gebai/build-plan.env）：镜像里究竟裁了什么、定了什么口径，随时可查；
# 浏览器系统依赖清单同样保留（可从镜像导出给内网预置构建复用，见 docker/README.md「离线浏览器」）。
COPY --from=builder /tmp/gebai-plan.env /etc/gebai/build-plan.env
COPY --from=builder /tmp/chromium-deps.txt /etc/gebai/playwright-deps.txt
# 运行期系统依赖：按计划装（PLAN_SYSTEM_PACKAGES，含档案声明的额外包）。分组与用途：
#   【固定基础设施，不参与裁剪】ca-certificates 出站 HTTPS（模型接口）/ curl 健康探针 / tini PID 1 收尸
#     ——工具会 spawn 大量子进程（脚本/浏览器/边车），且缺少探针会让容器「看起来能起」却无法被编排
#   系统 git        git 工具与文件工作台的 Git 面板
#   系统 python     python3(+venv/pip)：py 工具、vision_pip 依赖安装
#   系统 bubblewrap **服务模式默认开启的脚本运行根隔离**的文件系统层（不可用时自动降级为环境收敛）
#   系统 fonts      fonts-noto-cjk + fontconfig：PDF 中文字体嵌入、图表 PNG 渲染的中文显示
#   系统 tzdata     定时任务（cron）与容器时区（image.tz）
#   系统 procps     进程查看（脚本/边车排障）
RUN if [ -n "$APT_MIRROR" ]; then \
      m="$(printf '%s' "$APT_MIRROR" | sed 's/[&#\\]/\\&/g')"; \
      for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.sources /etc/apt/sources.list.d/*.list; do \
        [ -f "$f" ] || continue; \
        sed -i -E "s#https?://(archive\|security)\.ubuntu\.com/ubuntu#${m}#g; s#https?://deb\.debian\.org/debian#${m}#g" "$f"; \
      done; \
    fi \
 && . /etc/gebai/build-plan.env \
 && apt-get update \
 && apt-get install -y --no-install-recommends $PLAN_SYSTEM_PACKAGES \
 && rm -rf /var/lib/apt/lists/*

# 浏览器：二进制（与 bwrap 共用「容器允许 user namespace」这一前提）从构建阶段整体拷入，
# 其系统依赖按构建阶段记录的实际清单复现（两处硬编码会漂移，dpkg 差集不会）。
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/ms-playwright
COPY --from=builder /tmp/chromium-deps.txt /tmp/chromium-deps.txt
RUN if [ -s /tmp/chromium-deps.txt ]; then \
      apt-get update \
      && xargs -a /tmp/chromium-deps.txt apt-get install -y --no-install-recommends \
      && rm -rf /var/lib/apt/lists/*; \
    else \
      echo "[docker] 未请求浏览器：跳过 chromium 系统依赖"; \
    fi \
 && rm -f /tmp/chromium-deps.txt
COPY --from=builder /opt/ms-playwright /opt/ms-playwright

COPY --from=builder /out/gebai /usr/local/bin/gebai

# node 供给（浏览器桥接依赖：`Bun.spawn(["node", driver])`）。三条通道由计划决定：
#   local/image —— 从预置文件或 node 镜像取（零网络 / 内网镜像仓库）；
#   apt        —— 由上面的系统包提供（nodejs 已并入 PLAN_SYSTEM_PACKAGES）；
#   off        —— 不装（不用浏览器时的缺省，如实说明能力面）。
# 装完跑一次 `node --version` 自检：node 二进制依赖 libstdc++/glibc，基础镜像缺库时在这里就暴露，
# 而不是等到浏览器子Agent 报一句看不懂的 spawn 失败。
COPY --from=node-src /opt/gebai-node/ /tmp/gebai-node-image/
COPY docker/node/ /tmp/gebai-node-local/
RUN set -e; . /etc/gebai/build-plan.env; \
    case "$PLAN_RUNTIME_NODE_SOURCE" in \
      local) \
        [ -x /tmp/gebai-node-local/node ] || { echo "[docker] node_source=local 但预置文件缺失（docker/node/node）"; exit 1; }; \
        install -m755 /tmp/gebai-node-local/node /usr/local/bin/node; \
        echo "[docker] node 来自预置文件（零网络）"; ;; \
      image) \
        [ -x /tmp/gebai-node-image/node ] || { echo "[docker] node_source=image 但未取到 node（${NODE_IMAGE} 里无 /usr/local/bin/node？）"; exit 1; }; \
        install -m755 /tmp/gebai-node-image/node /usr/local/bin/node; \
        echo "[docker] node 来自镜像 ${NODE_IMAGE}"; ;; \
      apt) \
        command -v node >/dev/null 2>&1 || { echo "[docker] node_source=apt 但系统包未提供 node"; exit 1; }; \
        echo "[docker] node 来自系统包（$(node --version)）"; ;; \
      off) \
        echo "[docker] 未安装 node（node_source=off）：浏览器类子Agent 不可用"; ;; \
      *) echo "[docker] 未知 node 供给方式：$PLAN_RUNTIME_NODE_SOURCE"; exit 1; ;; \
    esac; \
    rm -rf /tmp/gebai-node-image /tmp/gebai-node-local; \
    if [ -x /usr/local/bin/node ]; then \
      if ! /usr/local/bin/node --version; then \
        echo "[docker] node 已就位但无法运行：多为基础镜像缺库（如 libstdc++6）——装齐后再重建，或改用 node_source=apt"; \
        exit 1; \
      fi; \
    fi
# 健康探针脚本（Dockerfile 里 `$$` 在非 RUN 指令中不转义成字面 `$`，而 `${VAR}` 又会被构建期替换成固定
# 值；独立脚本两条都避开，且可在宿主 `sh -n` 校验）。装在这里而非 healthcheck 阶段：那边已是非 root
# 运行用户，改不动 /usr/local/bin 的权限。
COPY docker/healthcheck.sh /usr/local/bin/gebai-healthcheck

LABEL org.opencontainers.image.title="GEBAI Agent" \
      org.opencontainers.image.description="歌白（GEBAI Agent）服务模式镜像（多阶段构建，单文件二进制；裁剪与定制面见镜像内 /etc/gebai/build-plan.env）" \
      org.opencontainers.image.version="${IMAGE_VERSION}" \
      org.opencontainers.image.licenses="MIT" \
      gebai.profile="${BUILD_PROFILE_NAME}"
# 额外镜像标签（档案 image.labels）由宿主侧转成 `docker build --label` 传入：Docker 的 LABEL 指令
# 不支持用变量展开追加任意标签（`LABEL ... ${VAR}` 会被当键值对解析而报错）。

# 运行用户：默认非 root（沙箱与脚本隔离都以「非特权用户 + 会话目录」为前提，root 反而会让部分工具
# 拒绝执行）。基础镜像自带的同 uid 用户（如 Ubuntu 的 uid 1000 `ubuntu`）先让位，保持
# 「容器内固定 uid」这一可预期约定（宿主目录挂载的属主、文档口径都据此）；
# IMAGE_RUN_AS_ROOT=1 时跳过建用户，显式以 root 运行（并如实说明其代价）。
RUN set -e; \
    if [ "$IMAGE_RUN_AS_ROOT" = "1" ]; then \
      echo "[docker] 提示：IMAGE_RUN_AS_ROOT=1 —— 容器以 root 运行；服务模式的沙箱与脚本隔离以非特权用户为前提，部分工具会因此拒绝执行"; \
    else \
      owner="$(getent passwd "$IMAGE_UID" | cut -d: -f1 || true)"; \
      if [ -n "$owner" ] && [ "$owner" != "$IMAGE_USER" ]; then \
        echo "[docker] 基础镜像的 uid $IMAGE_UID 由 $owner 占用，先移除以让位给 $IMAGE_USER"; \
        userdel -r "$owner" 2>/dev/null || userdel "$owner" 2>/dev/null || true; \
      fi; \
      getent group "$IMAGE_GID" >/dev/null || groupadd --gid "$IMAGE_GID" "$IMAGE_USER"; \
      id -u "$IMAGE_USER" >/dev/null 2>&1 || useradd --uid "$IMAGE_UID" --gid "$IMAGE_GID" \
        --home-dir "$IMAGE_HOME" --create-home --shell "$IMAGE_SHELL" "$IMAGE_USER"; \
    fi; \
    mkdir -p "$IMAGE_DATA_DIR"; \
    chown -R "$IMAGE_UID:$IMAGE_GID" "$IMAGE_DATA_DIR" /etc/gebai; \
    chmod +x /usr/local/bin/gebai /usr/local/bin/gebai-healthcheck

# 服务模式默认值：对外监听、数据根（挂卷持久化）。配置注入两选一：
#   ① docker run -e GEBAI_LLM_API_KEY=… 等环境变量；② 写入挂卷内的 $IMAGE_DATA_DIR/.env（二进制模式自动读）
ENV GEBAI_MODE=${IMAGE_MODE} \
    GEBAI_HOST=${IMAGE_HOST} \
    GEBAI_PORT=${IMAGE_PORT} \
    GEBAI_HOME=${IMAGE_DATA_DIR} \
    TZ=${IMAGE_TZ} \
    GEBAI_HEALTHCHECK_PATH=/api/health
WORKDIR ${IMAGE_DATA_DIR}
USER ${IMAGE_RUNTIME_USER}
VOLUME ${IMAGE_DATA_DIR}
EXPOSE ${IMAGE_PORT}
STOPSIGNAL SIGTERM

ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/usr/local/bin/gebai"]

# 阶段选择：默认阶段（最后定义）带 HEALTHCHECK；`--target runtime-nohealthcheck` 得到不带探针的镜像
# （供用自身探针的编排，或 `docker run --no-healthcheck`）。HEALTHCHECK 的 CMD 用运行时变量展开，
# 因此运行期覆盖 GEBAI_PORT 时探针自动跟随。
FROM runtime AS runtime-nohealthcheck

FROM runtime AS runtime-healthcheck
ARG HEALTHCHECK_PATH=/api/health
ENV GEBAI_HEALTHCHECK_PATH=${HEALTHCHECK_PATH}
# 注意：--interval/--timeout/--start-period/--retries 在 Dockerfile 解析阶段就固定（实测不接受
# 变量展开），所以不能由构建参数注入；需要调整请用运行期选项（compose 的 healthcheck 段或
# `docker run --health-interval` 等），档案里若写了这几个值会由计划器明确告警。
# 探针本体在 runtime 阶段已装好（/usr/local/bin/gebai-healthcheck，运行期读 GEBAI_PORT 与
# GEBAI_HEALTHCHECK_PATH，因此跟随端口与探针路径）。
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD ["/usr/local/bin/gebai-healthcheck"]
