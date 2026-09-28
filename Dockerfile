# syntax=docker/dockerfile:1
#
# 歌白（GEBAI Agent）· 服务模式镜像（Ubuntu 24.04 基础镜像）
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
# ── 裁剪体系（能力层 + 前端资源层 + 系统层，用法与矩阵见 docker/README.md「裁剪」）──
# 两个入口，最终都汇成一份**构建计划**（`scripts/build-image-plan.ts` → /tmp/gebai-plan.env）：
#   ① 裁剪档案 `--build-arg BUILD_PROFILE_B64=<base64(档案 JSON)>`（docker/build.sh --profile 生成）；
#   ② 细粒度覆盖 `--build-arg BUILD_SET="assets.d2=0 web.vendor=monaco"`（空格分隔的 键=值，优先于档案）。
# 档案 schema：sub_agents（enable/disable/preload）、tools.disable、
#   assets（web_ui/cv/d2/analyzer/browser/ripgrep）、web.vendor（前端引擎组）、
#   system（git/python/bubblewrap/fonts/tzdata/procps/chromium）。
# 计划驱动三件事：各内嵌产物脚本按 `GEBAI_BUILD_*` 写**空产物**（跳过而非不生成——产物被源码
# 静态 import，缺文件会让整个构建失败）、前端 vendor 按 `GEBAI_WEB_VENDOR` 裁剪、运行阶段按
# `PLAN_SYSTEM_PACKAGES` 装包。计划同时留在镜像内 `/etc/gebai/build-plan.env`，可随时核对该镜像
# 究竟裁了什么。
#
# 构建参数（见 docker/build.sh 的对应开关）：
#   UBUNTU_VERSION   基础镜像版本            默认 24.04
#   BUN_VERSION      bun 版本（仅取可执行文件）默认 1.4.2
#   BUN_TARGET       bun 编译目标（跨架构时显式指定，缺省按构建机架构）
#   BUILD_PROFILE_B64  裁剪档案（base64 JSON；缺省 = 全量）
#   BUILD_PROFILE_NAME 档案名（仅写进镜像标签与计划记录）
#   BUILD_SET        细粒度裁剪覆盖（空格分隔 键=值；CLI 优先于档案）
#   WITH_CV          兼容开关：=0 等价于 assets.cv=0                     默认 1
#   WITH_BROWSER     兼容开关：=1 等价于 assets.browser=1 + system.chromium=1  默认 0
#   CV_MODEL_BASE    模型下载源（内网可换镜像）默认 hf-mirror 的 RapidOCR 托管
#   PLAYWRIGHT_VERSION 浏览器版本，须与仓库依赖一致  默认 1.62.1
#
# 构建器：不依赖 BuildKit（未用 `RUN --mount`，普通 `docker build` 也能构建）；有 buildx 时
# 可额外用于 --platform / --push。
#
# 架构：二进制内嵌的 @resvg/resvg-js 是**平台原生模块**，跨架构编译会嵌错平台 —— 本镜像只支持
# 「构建机架构 = 目标架构」（linux/amd64 在 amd64 上构建、linux/arm64 在 arm64 上构建）。

ARG UBUNTU_VERSION=24.04
ARG BUN_VERSION=1.4.2

# ── bun 可执行文件来源：只为取出 bun 这一个文件，最终镜像不引入该基础镜像 ──
FROM oven/bun:${BUN_VERSION} AS bun-src

# ══════════════════════════ 构建阶段 ══════════════════════════
FROM ubuntu:${UBUNTU_VERSION} AS builder
ARG BUN_VERSION
ARG BUN_TARGET
ARG BUILD_PROFILE_B64=""
ARG BUILD_SET=""
ARG WITH_CV=1
ARG WITH_BROWSER=0
ARG PLAYWRIGHT_VERSION=1.62.1
ARG CV_MODEL_BASE
ENV DEBIAN_FRONTEND=noninteractive
COPY --from=bun-src /usr/local/bin/bun /usr/local/bin/bun
RUN apt-get update \
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
# 依赖清单先入层：源码改动不必重装依赖（.dockerignore 已排除 node_modules/dist）；
# bun.lock 未入库（见 .gitignore），存在时用通配路径让它参与构建以固定版本，缺失时按 package.json 解析。
# lockfile 格式随 bun 版本演进：若构建机的 lockfile 比镜像内 bun 新（"Unknown lockfile version"），
# 退回按 package.json 解析并告警（否则构建卡在依赖层，且报错与代码无关）；其它 frozen 失败照旧中断，
# 不掩盖真实依赖冲突。要严格锁版本，把 BUN_VERSION 提到与 lockfile 生成版本一致。
RUN if [ -f bun.lock ]; then \
      if ! bun install --frozen-lockfile > /tmp/bun-install.log 2>&1; then \
        if grep -qi "lockfile" /tmp/bun-install.log; then \
          echo "[docker] 警告：bun.lock 不被镜像内 bun（${BUN_VERSION}）识别，退回按 package.json 解析（无版本锁定）"; \
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

# 可选：playwright 浏览器（由计划 PLAN_WITH_BROWSER 决定）。在构建阶段装（这里有 bun；bun 不进
# 最终镜像），并**记录 install-deps 新装的系统包**供运行阶段按同一份清单复现——避免把依赖表在
# 两处硬编码而漂移。Ubuntu 24.04 的 chromium 包是指向 snap 的过渡包，容器内不可用，故走
# playwright 官方下载。
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/ms-playwright
RUN mkdir -p /opt/ms-playwright \
 && touch /tmp/chromium-deps.txt \
 && set -a && . /tmp/gebai-plan.env && set +a \
 && if [ "$PLAN_WITH_BROWSER" = "1" ]; then \
      dpkg-query -W -f='$${Package}\n' | sort > /tmp/before.txt; \
      bunx --yes playwright@${PLAYWRIGHT_VERSION} install --with-deps chromium; \
      dpkg-query -W -f='$${Package}\n' | sort > /tmp/after.txt; \
      comm -13 /tmp/before.txt /tmp/after.txt > /tmp/chromium-deps.txt; \
      echo "[docker] 浏览器就绪（新增系统依赖 $(wc -l < /tmp/chromium-deps.txt) 个，见 /tmp/chromium-deps.txt）"; \
    else \
      echo "[docker] 计划未启用浏览器（system.chromium=false）：playwright/reverse_site 等浏览器子Agent 不可用"; \
    fi

# ══════════════════════════ 运行阶段 ══════════════════════════
FROM ubuntu:${UBUNTU_VERSION} AS runtime
ARG BUILD_PROFILE_NAME=""
ENV DEBIAN_FRONTEND=noninteractive
# 构建计划随镜像保留（/etc/gebai/build-plan.env）：镜像里究竟裁了什么，随时可查，不靠记忆与文档
COPY --from=builder /tmp/gebai-plan.env /etc/gebai/build-plan.env
# 运行期系统依赖：按计划装（PLAN_SYSTEM_PACKAGES）。分组与用途：
#   【固定基础设施，不参与裁剪】ca-certificates 出站 HTTPS（模型接口）/ curl 健康探针 / tini PID 1 收尸
#     ——工具会 spawn 大量子进程（脚本/浏览器/边车），且缺少探针会让容器「看起来能起」却无法被编排
#   系统 git        git 工具与文件工作台的 Git 面板
#   系统 python     python3(+venv/pip)：py 工具、vision_pip 依赖安装
#   系统 bubblewrap **服务模式默认开启的脚本运行根隔离**的文件系统层（不可用时自动降级为环境收敛）
#   系统 fonts      fonts-noto-cjk + fontconfig：PDF 中文字体嵌入、图表 PNG 渲染的中文显示
#   系统 tzdata     定时任务（cron）按容器时区计算
#   系统 procps     进程查看（脚本/边车排障）
RUN . /etc/gebai/build-plan.env \
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

LABEL org.opencontainers.image.title="GEBAI Agent" \
      org.opencontainers.image.description="歌白（GEBAI Agent）服务模式镜像（多阶段构建，单文件二进制；裁剪面见镜像内 /etc/gebai/build-plan.env）" \
      gebai.profile="${BUILD_PROFILE_NAME}"

# 非 root 运行（沙箱与脚本隔离都以「非特权用户 + 会话目录」为前提，root 反而会让部分工具拒绝执行）。
# Ubuntu 官方基础镜像自带 uid 1000 的 `ubuntu` 用户：先删掉它再建同号的 gebai，保持
# 「容器内固定 uid 1000」这一可预期约定（宿主目录挂载的属主、文档口径都据此）。
RUN if [ "$(id -u ubuntu 2>/dev/null)" = "1000" ]; then userdel -r ubuntu; fi \
 && useradd --create-home --uid 1000 --shell /bin/bash gebai \
 && mkdir -p /data \
 && chown -R gebai:gebai /data /etc/gebai \
 && chmod +x /usr/local/bin/gebai

# 服务模式默认值：对外监听、数据根 /data（挂卷持久化）。配置注入两选一：
#   ① docker run -e GEBAI_LLM_API_KEY=… 等环境变量；② 写入挂卷内的 /data/.env（二进制模式自动读）
ENV GEBAI_MODE=server \
    GEBAI_HOST=0.0.0.0 \
    GEBAI_PORT=3000 \
    GEBAI_HOME=/data
WORKDIR /data
USER gebai
VOLUME ["/data"]
EXPOSE 3000
STOPSIGNAL SIGTERM
# /api/health 免鉴权（服务模式下也放行），返回 { ok, boot }——作为容器健康探针
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:3000/api/health >/dev/null || exit 1

ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/usr/local/bin/gebai"]
