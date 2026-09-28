#!/usr/bin/env bash
#
# 歌白（GEBAI Agent）· 服务模式镜像构建脚本（Ubuntu 24.04 基础镜像）
#
# 用法（在仓库任意位置执行均可，脚本自动定位仓库根作为构建上下文）：
#   docker/build.sh                                  # 构建 gebai:<package.json 版本>
#   docker/build.sh -t gebai:dev --smoke             # 指定标签并跑冒烟自检
#   docker/build.sh --with-browser --no-cv           # 装浏览器、不内嵌本地 CV
#   docker/build.sh --profile minimal                # 预置裁剪档案（docker/profiles/*.json）
#   docker/build.sh --profile code --set assets.d2=1 --set web.vendor=monaco   # 档案 + 字段级覆盖（CLI 优先）
#   docker/build.sh --print-plan                     # 只打印裁剪计划与报告，不构建
#   docker/build.sh --base-image registry.internal/ubuntu:24.04 --apt-mirror http://mirror.internal/ubuntu
#   docker/build.sh --user acme --uid 2001 --data-dir /srv/gebai --port 8080 --tz Asia/Shanghai
#   docker/build.sh --label owner=acme --label tier=prod --extra-packages vim,less --no-healthcheck
#   docker/build.sh --cv-model-base https://…/PP-OCRv4   # 内网镜像源
#   docker/build.sh --platform linux/arm64           # 目标架构（须与本机架构一致，见 Dockerfile 说明）
#   docker/build.sh --push -t registry.example.com/gebai:0.1.0
#
# 定制体系（档案字段与可裁/可定制矩阵见 docker/README.md）：一份 JSON 档案描述
#   能力层（子Agent/全局工具/内嵌资产/前端 vendor）、系统层（apt 包组）、镜像本体（基础镜像/用户/
#   数据根/端口/时区/标签/额外包/apt・npm 源/健康检查）。`--set 键=值` 与上述开关做字段级覆盖。
# 档案由 scripts/build-image-plan.ts 合并成计划：宿主侧算一遍（指令级 build-arg），镜像构建阶段再算一遍
# 驱动 RUN 级裁剪，两边同源；构建日志里会打印完整报告。`--print-plan` 只看不构建（需本机有 bun）。
#
# 依赖：docker（不依赖 BuildKit——Dockerfile 未用 `RUN --mount`，普通 docker build 即可；
# 装有 buildx 时另可用 --platform / --push）。首次构建需联网：npm 源、Ubuntu 的 apt 源、
# 以及（WITH_CV=1 时）CV 模型下载源；离线/内网用 --cv-model-base 换镜像源。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

IMAGE_TAG=""
WITH_BROWSER=0
WITH_CV=1
CV_MODEL_BASE=""
BUN_TARGET=""
PLATFORM=""
NO_CACHE=0
OUTPUT=""
SMOKE=0
PROFILE=""
SETS=()
LABEL_SPECS=()
PRINT_PLAN=0

usage() {
  sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

选项：
  -t, --tag <tag>            镜像标签（默认 gebai:<package.json 的 version>）
      --profile <名|路径>    裁剪档案（docker/profiles/<名>.json 或任意 .json 路径，见 docker/README.md）
      --set <键=值>          档案的字段级覆盖（可多次；键如 assets.d2、web.vendor、system.python、image.user；CLI 优先）
      --print-plan           只打印裁剪计划与报告，不构建（需本机有 bun）
      --with-browser         安装 playwright chromium（等价 system.chromium=1 + assets.browser=1）
      --no-browser           不安装浏览器（默认）
      --with-cv              内嵌本地 CV（PP-OCR 模型 + ort 运行时，默认）
      --no-cv                不内嵌本地 CV（等价 assets.cv=0：镜像更小，本地 OCR/视觉定位不可用）
      --cv-model-base <url>  CV 模型下载源（内网镜像；缺省 hf-mirror 的 RapidOCR 托管）
  —— 镜像本体定制（等价于 --set image.<字段>=值，可写进档案由 --profile 一次切换）——
      --base-image <img>     基础镜像（缺省 ubuntu:24.04；内网/私有仓库可换自备镜像）
      --bun-image <img>      bun 来源镜像（缺省 oven/bun:1.4.2）
      --user / --uid / --gid 运行用户与 uid/gid（缺省 gebai / 1000 / 1000）
      --data-dir <path>      数据根（GEBAI_HOME 与挂载卷，缺省 /data）
      --port <n>             监听端口（同时作为 EXPOSE，缺省 3000）
      --mode <server|local>  GEBAI_MODE（缺省 server）
      --host <addr>          GEBAI_HOST 监听地址（缺省 0.0.0.0）
      --tz <zone>            容器时区（如 Asia/Shanghai；设了自动确保 tzdata 安装）
      --label <键=值>        额外镜像标签（可多次）
      --extra-packages <列表> 裁剪组之外额外安装的 apt 包（逗号分隔）
      --apt-mirror <url>     apt 源（内网镜像；只换主机名，保留发行版/组件行）
      --npm-registry <url>   npm 源（bun install；内网镜像）
      --build-proxy <url>    构建期 HTTP(S) 代理（apt/npm/模型下载）
      --run-as-root          显式以 root 运行（服务模式的沙箱与脚本隔离以非特权用户为前提）
      --no-healthcheck       不写入 HEALTHCHECK（改用不带探针的阶段构建）
      --target <bun-target>  bun 编译目标（如 bun-linux-arm64；缺省按构建机架构）
      --platform <plat>      docker 目标平台（linux/amd64、linux/arm64）
      --no-cache             不使用构建缓存
      --push / --load        buildx 输出方式（缺省 --load 载入本机镜像库）
      --smoke                构建后起容器跑冒烟自检（健康检查 + 隔离能力探测）并清理
  -h, --help                 显示本帮助
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -t|--tag) IMAGE_TAG="${2:?--tag 需要值}"; shift 2 ;;
    --profile) PROFILE="${2:?--profile 需要值}"; shift 2 ;;
    --set) SETS+=("${2:?--set 需要值}"); shift 2 ;;
    --print-plan) PRINT_PLAN=1; shift ;;
    --no-healthcheck) SETS+=("image.healthcheck=false"); shift ;;
    --base-image) SETS+=("image.base=${2:?--base-image 需要值}"); shift 2 ;;
    --bun-image) SETS+=("image.bun_image=${2:?--bun-image 需要值}"); shift 2 ;;
    --user) SETS+=("image.user=${2:?--user 需要值}"); shift 2 ;;
    --uid) SETS+=("image.uid=${2:?--uid 需要值}"); shift 2 ;;
    --gid) SETS+=("image.gid=${2:?--gid 需要值}"); shift 2 ;;
    --data-dir) SETS+=("image.data_dir=${2:?--data-dir 需要值}"); shift 2 ;;
    --port) SETS+=("image.port=${2:?--port 需要值}"); shift 2 ;;
    --mode) SETS+=("image.mode=${2:?--mode 需要值}"); shift 2 ;;
    --host) SETS+=("image.host=${2:?--host 需要值}"); shift 2 ;;
    --tz) SETS+=("image.tz=${2:?--tz 需要值}"); shift 2 ;;
    --label) LABEL_SPECS+=("${2:?--label 需要值}"); shift 2 ;;
    --extra-packages) SETS+=("image.extra_packages=${2:?--extra-packages 需要值}"); shift 2 ;;
    --apt-mirror) SETS+=("image.apt_mirror=${2:?--apt-mirror 需要值}"); shift 2 ;;
    --npm-registry) SETS+=("image.npm_registry=${2:?--npm-registry 需要值}"); shift 2 ;;
    --build-proxy) SETS+=("image.proxy=${2:?--build-proxy 需要值}"); shift 2 ;;
    --run-as-root) SETS+=("image.run_as_root=1"); shift ;;
    --with-browser) WITH_BROWSER=1; shift ;;
    --no-browser) WITH_BROWSER=0; shift ;;
    --with-cv) WITH_CV=1; shift ;;
    --no-cv) WITH_CV=0; shift ;;
    --cv-model-base) CV_MODEL_BASE="${2:?--cv-model-base 需要值}"; shift 2 ;;
    --target) BUN_TARGET="${2:?--target 需要值}"; shift 2 ;;
    --platform) PLATFORM="${2:?--platform 需要值}"; shift 2 ;;
    --no-cache) NO_CACHE=1; shift ;;
    --push) OUTPUT="--push"; shift ;;
    --load) OUTPUT="--load"; shift ;;
    --smoke) SMOKE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1（-h 查看用法）" >&2; exit 1 ;;
  esac
done

command -v docker >/dev/null 2>&1 || { echo "错误：未找到 docker 命令" >&2; exit 1; }

# ── 裁剪/定制档案解析：预置名 → docker/profiles/{名}.json；含分隔符或 .json 结尾 → 按路径 ──
PROFILE_FILE=""
PROFILE_NAME=""
if [ -n "${PROFILE}" ]; then
  if [ -f "${PROFILE}" ]; then
    PROFILE_FILE="${PROFILE}"
  elif [ -f "docker/profiles/${PROFILE}.json" ]; then
    PROFILE_FILE="docker/profiles/${PROFILE}.json"
  else
    echo "错误：裁剪档案不存在：${PROFILE}" >&2
    echo "      预置档案：$(ls docker/profiles/*.json 2>/dev/null | xargs -n1 basename 2>/dev/null | sed 's/\.json$//' | tr '\n' ' ')" >&2
    exit 1
  fi
  PROFILE_NAME="$(basename "${PROFILE_FILE}" .json)"
fi

# ── 计划器参数：档案 + 细粒度覆盖 + 兼容开关（宿主侧与镜像内同源同参）──
# 多个 --label 累积成一个 image.labels 项（逗号分隔）——多次 --set 同一字段是整体覆盖，不能逐个追加
[ ${#LABEL_SPECS[@]} -gt 0 ] && SETS+=("image.labels=$(IFS=,; echo "${LABEL_SPECS[*]}")")
PLAN_ARGS=()
[ -n "${PROFILE_FILE}" ] && PLAN_ARGS+=(--profile "${PROFILE_FILE}")
for s in ${SETS[@]+"${SETS[@]}"}; do PLAN_ARGS+=(--set "$s"); done
[ "${WITH_CV}" = "0" ] && PLAN_ARGS+=(--set "assets.cv=0")
[ "${WITH_BROWSER}" = "1" ] && PLAN_ARGS+=(--set "assets.browser=1" --set "system.chromium=1")

# ── --print-plan：只出计划与报告（需 bun 解析档案与合并覆盖），不构建 ──
if [ "${PRINT_PLAN}" = "1" ]; then
  command -v bun >/dev/null 2>&1 || { echo "错误：--print-plan 需要本机有 bun（解析档案与合并覆盖）" >&2; exit 1; }
  bun run scripts/build-image-plan.ts ${PLAN_ARGS[@]+"${PLAN_ARGS[@]}"} --print-plan
  exit 0
fi

# ── 指令级定制：由宿主侧跑计划器算出（Docker 指令无法在构建过程中条件化）──
# 基础镜像/运行用户/数据根/端口/时区/标签/健康检查都要进 FROM・USER・ENV・VOLUME・EXPOSE・LABEL・
# HEALTHCHECK，必须在 docker build 之前定下；RUN 级裁剪（能力/系统包）在镜像构建阶段用同一份档案再算
# 一次，两边同源。缺 bun 时降级：指令级定制不可用（明确告警），RUN 级裁剪照常生效。
BUILD_EXTRA=()
TARGET_STAGE=""
SMOKE_PORT=3000
SMOKE_DATA_DIR=/data
if command -v bun >/dev/null 2>&1; then
  REPORT_FILE="$(mktemp -t gebai-plan-report.XXXXXX)"
  if ! ARG_LINES="$(bun run scripts/build-image-plan.ts ${PLAN_ARGS[@]+"${PLAN_ARGS[@]}"} --emit-args 2>"${REPORT_FILE}")"; then
    cat "${REPORT_FILE}" >&2
    rm -f "${REPORT_FILE}"
    echo "错误：裁剪/定制档案校验失败（详见上方报告）" >&2
    exit 1
  fi
  cat "${REPORT_FILE}" >&2
  rm -f "${REPORT_FILE}"
  while IFS= read -r line; do
    [ -n "${line}" ] || continue
    key="${line%%=*}"
    val="${line#*=}"
    # 空值行不传：让 Dockerfile 的 ARG 默认值生效（如 IMAGE_TZ 缺省 UTC），避免把空串写进 ENV
    [ -n "${val}" ] || continue
    BUILD_EXTRA+=(--build-arg "${line}")
    case "${key}" in
      IMAGE_PORT) SMOKE_PORT="${val}" ;;
      IMAGE_DATA_DIR) SMOKE_DATA_DIR="${val}" ;;
      # 镜像标签：Docker 的 LABEL 指令不支持变量展开追加，改用 docker build --label
      IMAGE_LABEL) [ -n "${val}" ] && BUILD_EXTRA+=(--label "${val}") ;;
      # PLAN_ 前缀是镜像内自查用的元信息，不是 build-arg
      PLAN_*) ;;
      HEALTHCHECK_ENABLED) [ "${val}" = "0" ] && TARGET_STAGE="runtime-nohealthcheck" ;;
      # 计划里的构建代理同时驱动 Docker 预定义代理 build-arg（apt/npm/模型下载）
      BUILD_PROXY) [ -n "${val}" ] && BUILD_EXTRA+=(--build-arg "HTTP_PROXY=${val}" --build-arg "HTTPS_PROXY=${val}") ;;
    esac
  done <<< "${ARG_LINES}"
else
  echo "警告：未找到 bun —— 跳过镜像本体的指令级定制（基础镜像/用户/数据根/端口/时区/标签/健康检查）与档案预校验；" >&2
  echo "      能力与系统层裁剪仍在镜像构建阶段由同一份档案生效。装 bun 后重跑，或直接用 docker build 传对应 ARG。" >&2
fi

# 默认标签取 package.json 版本（不写死，发版即随）
if [ -z "${IMAGE_TAG}" ]; then
  VERSION="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' package.json | head -n 1)"
  IMAGE_TAG="gebai:${VERSION:-latest}"
fi

# buildx 可用则用之（同时支持 --platform/--push 这类输出）；未装 buildx 时退回 docker build
# （Docker 23+ 已废弃经典构建器、Docker 29 已移除，那种情况下得先装 buildx；本镜像的
#  Dockerfile 本身不依赖 BuildKit，只要构建器本身可用）
BUILDER=(docker buildx build)
if ! docker buildx version >/dev/null 2>&1; then
  BUILDER=(docker build)
  OUTPUT=""              # 经典构建器没有 --load/--push
  echo "提示：未检测到 buildx。若本机 Docker 已移除经典构建器，需先安装：Ubuntu: apt-get install -y docker-buildx"
elif [ -z "${OUTPUT}" ]; then
  OUTPUT="--load"        # buildx 默认只构建不入库，本机使用需 --load
fi

BUILD_ARGS=(
  --build-arg "WITH_CV=${WITH_CV}"
  --build-arg "WITH_BROWSER=${WITH_BROWSER}"
)
# 裁剪档案（base64 传入，避免 JSON 引号在 build-arg 里转义）与字段级覆盖
[ -n "${PROFILE_FILE}" ] && BUILD_ARGS+=(
  --build-arg "BUILD_PROFILE_B64=$(base64 < "${PROFILE_FILE}" | tr -d '\n')"
  --build-arg "BUILD_PROFILE_NAME=${PROFILE_NAME}"
)
[ ${#SETS[@]} -gt 0 ] && {
  # BUILD_SET 走空格分隔（镜像内 `for s in $BUILD_SET` 解析），因此**只传 RUN 级裁剪需要的覆盖**：
  # image.labels 的值可能含空格（会破坏分隔）且它只用于指令级（宿主侧已算好），故不转投。
  RUN_SETS=()
  for s in "${SETS[@]}"; do
    case "$s" in image.labels=*) continue ;; esac
    RUN_SETS+=("$s")
  done
  [ ${#RUN_SETS[@]} -gt 0 ] && BUILD_ARGS+=(--build-arg "BUILD_SET=${RUN_SETS[*]}")
}
# 指令级定制（宿主侧计划器算出的 build-arg）与健康检查阶段选择
[ ${#BUILD_EXTRA[@]} -gt 0 ] && BUILD_ARGS+=(${BUILD_EXTRA[@]+"${BUILD_EXTRA[@]}"})
[ -n "${TARGET_STAGE}" ] && BUILD_ARGS+=(--target "${TARGET_STAGE}")
[ -n "${BUN_TARGET}" ] && BUILD_ARGS+=(--build-arg "BUN_TARGET=${BUN_TARGET}")
[ -n "${CV_MODEL_BASE}" ] && BUILD_ARGS+=(--build-arg "CV_MODEL_BASE=${CV_MODEL_BASE}")
[ -n "${PLATFORM}" ] && BUILD_ARGS+=(--platform "${PLATFORM}")
[ "${NO_CACHE}" = "1" ] && BUILD_ARGS+=(--no-cache)
[ -n "${OUTPUT}" ] && BUILD_ARGS+=("${OUTPUT}")

echo "==> 构建镜像 ${IMAGE_TAG}"
echo "    上下文：${REPO_ROOT}（.dockerignore 已排除 node_modules/dist/resources/infer/vendor 等）"
echo "    参数：WITH_CV=${WITH_CV} WITH_BROWSER=${WITH_BROWSER}${BUN_TARGET:+ BUN_TARGET=${BUN_TARGET}}${PLATFORM:+ PLATFORM=${PLATFORM}}"
echo "    裁剪：档案=${PROFILE_FILE:-全量（未指定）}${SETS:+ 覆盖=${SETS[*]}}"
[ -n "${TARGET_STAGE}" ] && echo "    阶段：${TARGET_STAGE}（无 HEALTHCHECK）"

"${BUILDER[@]}" \
  -f Dockerfile \
  -t "${IMAGE_TAG}" \
  ${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"} \
  .

echo "==> 完成：${IMAGE_TAG}"
docker images --filter "reference=${IMAGE_TAG}" --format '    大小：{{.Size}}（{{.Repository}}:{{.Tag}}）'

if [ "${SMOKE}" = "1" ]; then
  NAME="gebai-smoke-$$"
  VOL="gebai-smoke-$$"
  cleanup() {
    docker rm -f "${NAME}" >/dev/null 2>&1 || true
    docker volume rm "${VOL}" >/dev/null 2>&1 || true
  }
  trap cleanup EXIT

  echo "==> 冒烟自检：起容器（卷 ${VOL} → ${SMOKE_DATA_DIR}）"
  docker volume create "${VOL}" >/dev/null
  # GEBAI_SCHEDULER=off：自检容器不跑定时任务，避免与宿主实例争抢同一份任务队列语义
  # 端口与数据根取自定义后的值（IMAGE_PORT/IMAGE_DATA_DIR 由宿主侧计划器算出）
  docker run -d --name "${NAME}" -e GEBAI_SCHEDULER=off -v "${VOL}:${SMOKE_DATA_DIR}" "${IMAGE_TAG}" >/dev/null

  echo -n "    健康检查 /api/health … "
  ok=0
  for _ in $(seq 1 30); do
    if out="$(docker exec "${NAME}" curl -fsS "http://127.0.0.1:${SMOKE_PORT}/api/health" 2>/dev/null)"; then
      echo "OK  ${out}"
      ok=1
      break
    fi
    sleep 1
  done
  if [ "${ok}" != "1" ]; then
    echo "失败"
    echo "----- 容器日志（末尾 60 行）-----"
    docker logs --tail 60 "${NAME}" 2>&1 || true
    echo "-------------------------------"
    exit 1
  fi

  echo -n "    Web UI / … "
  code="$(docker exec "${NAME}" curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${SMOKE_PORT}/" || true)"
  echo "HTTP ${code}"

  echo -n "    容器内自查（用户/数据根/端口）… "
  docker exec "${NAME}" sh -c 'echo "user=$(id -un) uid=$(id -u) gid=$(id -g) home=$HOME data=$GEBAI_HOME port=$GEBAI_PORT tz=$TZ"' 2>/dev/null || true

  echo -n "    脚本文件系统隔离（user namespace）… "
  if docker exec "${NAME}" unshare --user --map-root-user echo ok >/dev/null 2>&1; then
    echo "可用（GEBAI_SCRIPT_ISOLATION 将生效 bwrap 文件系统隔离）"
  else
    echo "不可用（自动降级为环境收敛：HOME/TEMP/XDG 仍在会话目录内，仅缺文件系统层）"
  fi

  echo "==> 冒烟通过；容器与卷将清理（脚本退出即删）"
fi

cat <<EOF

后续步骤：
  docker run -d --name gebai -p ${SMOKE_PORT}:${SMOKE_PORT} -v gebai-data:${SMOKE_DATA_DIR} -e GEBAI_LLM_API_KEY=… ${IMAGE_TAG}
  ① 配置：环境变量注入，或写挂卷内 ${SMOKE_DATA_DIR}/.env（二进制模式自动读取）
  ② 镜像的裁剪面与定制口径：docker exec <容器> cat /etc/gebai/build-plan.env
  ③ 首次登录：默认 GEBAI_SIGNUP_MODE=open 可自助注册；要启用 admin 则设 GEBAI_ADMIN_PASSWORD_HASH
     （生成：bun run --cwd packages/server hash-password，或见 docker/README.md）
  ④ 能力边界与容器安全前提见 docker/README.md
EOF
