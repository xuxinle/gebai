#!/usr/bin/env bash
# smoke-source-build.sh —— 内网源码编译冒烟（POSIX 包装：真正的实现是 smoke-source-build.ts）
#
# 用法:
#   bash infer/scripts/smoke-source-build.sh                 # 发现源码 → 编译 → 起服务 → 结构化推理 → 批量 → 停止
#   bash infer/scripts/smoke-source-build.sh --skip-build    # 只验证下游链路（用已安装引擎，几十秒）
#   bash infer/scripts/smoke-source-build.sh --device cuda --items 4
#   bash infer/scripts/smoke-source-build.sh --keep --port 19181
#
# 为什么包装成脚本而不是直接写 shell：编译流程与产品共用同一套实现（local_infer 子Agent 的工具），
# 脚本里重复一遍 cmake 调用必然与产品漂移。本包装只负责找到 bun 并把参数原样透传。
#
# 退出码：0 全通过 / 1 有失败项（可接 CI）。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ts="$here/smoke-source-build.ts"

if ! command -v bun >/dev/null 2>&1; then
  echo "找不到 bun —— 本脚本用 bun 运行 TypeScript 实现（$ts）。" >&2
  echo "安装 bun: https://bun.sh  （或改用同目录的 smoke-source-build.ps1，同样需要 bun）" >&2
  exit 127
fi

exec bun run "$ts" "$@"
