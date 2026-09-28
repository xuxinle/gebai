#!/bin/sh
# 容器健康探针（Dockerfile 的 HEALTHCHECK 调用；运行期由容器内 shell 执行）。
#
# 为什么是脚本而不是把 curl 直接写进 HEALTHCHECK 的 CMD：Dockerfile 里 `$$` 在非 RUN 指令中不会转义成
# 字面 `$`（实测 HEALTHCHECK 的 CMD 会原样保留 `$$`，被 shell 展开成 PID，探针 URL 直接无效），而 `${VAR}`
# 又会在构建期被替换成固定值、失去「运行期覆盖端口即跟随」的能力。独立脚本两条都避开。
#
# 端口与路径取运行期环境变量（GEBAI_PORT 由镜像 ENV 提供，GEBAI_HEALTHCHECK_PATH 由构建档案的
# image.healthcheck.path 烘焙），未设置时回落到镜像默认值。/api/health 免鉴权（服务模式下也放行），
# 返回 { ok, boot }。
set -eu
exec curl -fsS "http://127.0.0.1:${GEBAI_PORT:-3000}${GEBAI_HEALTHCHECK_PATH:-/api/health}" >/dev/null
