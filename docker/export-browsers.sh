#!/usr/bin/env bash
#
# 歌白（GEBAI Agent）· 浏览器预置目录导出（离线/内网部署用）
#
# 把 playwright 的浏览器目录导出成可随源码一起带进内网构建的自包含目录。内网侧构建时不再需要
# playwright CDN（也不经 bunx 取包），见 docker/README.md「离线浏览器」。
#
# 用法（在仓库任意位置执行均可）：
#   docker/export-browsers.sh docker/browsers                      # 从宿主 playwright 缓存导出
#   docker/export-browsers.sh docker/browsers --from-image gebai:0.1.0   # 从已构建镜像导出（最准）
#   docker/export-browsers.sh /tmp/b --force                       # 覆盖已存在的输出目录
#   docker/export-browsers.sh /tmp/b --revision 1243               # 无 bun/node 时显式指定 revision
#
# 导出来源（二选一）：
#   --from-image <镜像>  从镜像里拷 /opt/ms-playwright（该镜像应是用 --with-browser 构建过的）
#                        ——除了浏览器本体，还带上构建时实测的系统依赖清单，内网侧可直接复用。
#   缺省                 从宿主 playwright 缓存（PLAYWRIGHT_BROWSERS_PATH 或各平台默认位置）拷贝：
#                        Linux $XDG_CACHE_HOME|~/.cache/ms-playwright、macOS ~/Library/Caches/ms-playwright、
#                        Windows %LOCALAPPDATA%/ms-playwright（WSL 下可传 PLAYWRIGHT_BROWSERS_PATH 指过去）。
#
# 只导出与**当前仓库依赖**匹配的 revision（从 node_modules/playwright-core/browsers.json 读）——版本不匹配的
# 浏览器在运行期根本启动不了，带上只会让内网构建报错；镜像侧另有同一份校验兜底。
#
# 依赖：bun 或 node（读期望 revision；都没有时用 --revision 显式指定）、docker（仅 --from-image 时需要）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

OUT_DIR=""
FROM_IMAGE=""
FORCE=0
REVISION=""

usage() {
  sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --from-image) FROM_IMAGE="${2:?--from-image 需要值}"; shift 2 ;;
    --revision) REVISION="${2:?--revision 需要值}"; shift 2 ;;
    --force) FORCE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "未知参数：$1（-h 查看用法）" >&2; exit 1 ;;
    *) [ -z "${OUT_DIR}" ] || { echo "只接受一个输出目录（已有：${OUT_DIR}）" >&2; exit 1; }; OUT_DIR="$1"; shift ;;
  esac
done
[ -n "${OUT_DIR}" ] || { usage; echo; echo "错误：缺少输出目录" >&2; exit 1; }

# ── 期望 revision：与构建侧同源（读 playwright-core/browsers.json 的 chromium.revision）──
# 兼顾 hoisted 与 bun store 两种依赖布局。
if [ -z "${REVISION}" ]; then
  for p in node_modules/playwright-core/browsers.json \
           node_modules/.bun/playwright-core@*/node_modules/playwright-core/browsers.json \
           packages/server/node_modules/playwright-core/browsers.json; do
    [ -f "$p" ] && { BROWSERS_JSON="$p"; break; }
  done
  [ -n "${BROWSERS_JSON:-}" ] || {
    echo "错误：找不到 playwright-core/browsers.json（依赖未安装？）——可先 bun install，或用 --revision 显式指定" >&2
    exit 1
  }
  runner=""
  command -v bun >/dev/null 2>&1 && runner="bun"
  [ -z "${runner}" ] && command -v node >/dev/null 2>&1 && runner="node"
  [ -n "${runner}" ] || { echo "错误：需要 bun 或 node 读取期望 revision（或用 --revision 显式指定）" >&2; exit 1; }
  REVISION="$("${runner}" -e "const b=require('/${PWD}/${BROWSERS_JSON}');console.log((b.browsers.find(x=>x.name==='chromium')||{}).revision||'')")"
  [ -n "${REVISION}" ] || { echo "错误：${BROWSERS_JSON} 里没有 chromium 条目" >&2; exit 1; }
fi

# ── 输出目录 ──
if [ -d "${OUT_DIR}" ] && [ -n "$(ls -A "${OUT_DIR}" 2>/dev/null || true)" ]; then
  [ "${FORCE}" = "1" ] || { echo "错误：输出目录已存在且非空：${OUT_DIR}（加 --force 覆盖）" >&2; exit 1; }
  rm -rf "${OUT_DIR}"
fi
mkdir -p "${OUT_DIR}"

WANT=("chromium-${REVISION}" "chromium_headless_shell-${REVISION}")

if [ -n "${FROM_IMAGE}" ]; then
  # ── 来源：已构建镜像（最准：浏览器本体已实测可用，并带构建期的依赖清单）──
  command -v docker >/dev/null 2>&1 || { echo "错误：--from-image 需要 docker 命令" >&2; exit 1; }
  docker image inspect "${FROM_IMAGE}" >/dev/null 2>&1 || { echo "错误：本地无镜像 ${FROM_IMAGE}" >&2; exit 1; }
  cid="$(docker create "${FROM_IMAGE}")"
  trap 'docker rm -f "${cid}" >/dev/null 2>&1 || true' EXIT
  echo "==> 从镜像 ${FROM_IMAGE} 导出浏览器（期望 chromium-${REVISION}）"
  found=0
  for d in "${WANT[@]}"; do
    if docker cp "${cid}:/opt/ms-playwright/${d}" "${OUT_DIR}/${d}" >/dev/null 2>&1; then
      echo "    已导出 ${d}"
      found=1
    fi
  done
  [ "${found}" = "1" ] || {
    echo "错误：该镜像的 /opt/ms-playwright 里没有 chromium-${REVISION}（是否用 --with-browser 构建过？）" >&2
    exit 1
  }
  if docker cp "${cid}:/etc/gebai/playwright-deps.txt" "${OUT_DIR}/playwright-deps.txt" >/dev/null 2>&1; then
    n="$(grep -c . "${OUT_DIR}/playwright-deps.txt" 2>/dev/null || echo 0)"
    [ "${n}" -gt 0 ] && echo "    已导出系统依赖清单 playwright-deps.txt（${n} 个包）" \
      || echo "    依赖清单为空（该镜像构建时未装 chromium 系统依赖）"
  fi
  docker rm -f "${cid}" >/dev/null 2>&1 || true
  trap - EXIT
else
  # ── 来源：宿主 playwright 缓存 ──
  CACHE="${PLAYWRIGHT_BROWSERS_PATH:-}"
  if [ -z "${CACHE}" ]; then
    for d in "${XDG_CACHE_HOME:-$HOME/.cache}/ms-playwright" "$HOME/Library/Caches/ms-playwright" "${LOCALAPPDATA:-}/ms-playwright"; do
      [ -n "${d}" ] && [ -d "${d}" ] && { CACHE="${d}"; break; }
    done
  fi
  [ -n "${CACHE}" ] && [ -d "${CACHE}" ] || {
    echo "错误：找不到宿主 playwright 缓存（可用 PLAYWRIGHT_BROWSERS_PATH 指定，或改用 --from-image 从已构建镜像导出）" >&2
    exit 1
  }
  echo "==> 从宿主缓存 ${CACHE} 导出浏览器（期望 chromium-${REVISION}）"
  found=0
  for d in "${WANT[@]}"; do
    if [ -d "${CACHE}/${d}" ]; then
      cp -a "${CACHE}/${d}" "${OUT_DIR}/${d}"
      echo "    已导出 ${d}（$(du -sh "${OUT_DIR}/${d}" | cut -f1)）"
      found=1
    fi
  done
  [ "${found}" = "1" ] || {
    echo "错误：宿主缓存里没有 chromium-${REVISION}（缓存内的版本：$(ls "${CACHE}" 2>/dev/null | tr '\n' ' ')）" >&2
    echo "      可在任意联网机用 --with-browser 构建一次镜像，再用 --from-image 从该镜像导出" >&2
    exit 1
  }
  echo "    提示：宿主缓存不含系统依赖清单；内网构建首次可用 --browser-deps <清单> 或 --no-browser-deps（基础镜像已含依赖）"
fi

echo
echo "==> 完成：${OUT_DIR}"
echo "    内网构建（把该目录放在仓库内，如 docker/browsers，然后）："
echo "      docker/build.sh --with-browser --browser-dir ${OUT_DIR}"
echo "    或已就位在 docker/browsers 时："
echo "      docker/build.sh --with-browser --browser-source local"
