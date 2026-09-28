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
#   PLAYWRIGHT_VERSION          浏览器版本，须与仓库依赖一致  默认 1.62.1
#   HTTP_PROXY / HTTPS_PROXY / NO_PROXY  Docker 预定义代理 build-arg（按需，仅构建阶段生效）
#
# 构建器：不依赖 BuildKit（未用 `RUN --mount`，普通 `docker build` 也能构建）；有 buildx 时
# 可额外用于 --platform / --push。
#
# 架构：二进制内嵌的 @resvg/resvg-js 是**平台原生模块**，跨架构编译会嵌错平台 —— 本镜像只支持
# 「构建机架构 = 目标架构」（linux/amd64 在 amd64 上构建、linux/arm64 在 arm64 上构建）。

ARG BASE_IMAGE=ubuntu:24.04
ARG BUN_IMAGE=oven/bun:1.4.2

# ── bun 可执行文件来源：只为取出 bun 这一个文件，最终镜像不引入该基础镜像 ──
FROM ${BUN_IMAGE} AS bun-src

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
ARG PLAYWRIGHT_VERSION=1.62.1
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
COPY --from=bun-src /usr/local/bin/bun /usr/local/bin/bun

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
FROM ${BASE_IMAGE} AS runtime
ARG BUILD_PROFILE_NAME=""
ARG APT_MIRROR=""
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
# 构建计划随镜像保留（/etc/gebai/build-plan.env）：镜像里究竟裁了什么、定了什么口径，随时可查
COPY --from=builder /tmp/gebai-plan.env /etc/gebai/build-plan.env
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
