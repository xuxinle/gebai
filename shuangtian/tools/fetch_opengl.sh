#!/usr/bin/env sh
# 拉取 OpenGL 加载器源码（glad 单头）到 third_party/opengl/ ——**不进 git 仓库**
#
# 与 tools/fetch_opengl.ps1 同一件事的 POSIX 版本（两个脚本而不是一个：Windows 下
# 没有 pwsh 的机器很常见，而 Linux 服务器上装 PowerShell 更少见）。
#
# 用法：sh tools/fetch_opengl.sh [--force]
set -eu

force=0
[ "${1:-}" = "--force" ] && force=1

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dest="$root/third_party/opengl"
header="$dest/gl.h"

if [ -f "$header" ] && [ "$force" -eq 0 ]; then
  echo "已存在：$header（用 --force 重取）"
  exit 0
fi

mkdir -p "$dest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

cloned=0
for url in "ssh://git@ssh.github.com:443/glfw/glfw.git" "https://github.com/glfw/glfw.git"; do
  echo "尝试克隆 $url"
  rm -rf "$tmp/glfw"; mkdir -p "$tmp/glfw"
  if git clone --depth 1 --quiet "$url" "$tmp/glfw" 2>/dev/null && [ -f "$tmp/glfw/deps/glad/gl.h" ]; then
    cloned=1
    break
  fi
done

if [ "$cloned" -eq 0 ]; then
  echo "克隆失败：两条通道都不通。离线环境请手工把 glad 生成的 gl.h 放到 $dest" >&2
  exit 1
fi

cp "$tmp/glfw/deps/glad/gl.h" "$header"
echo "已就位：$header（$(wc -c <"$header") 字节）"
echo "注意：此目录已在 .gitignore 中——OpenGL 源码不进仓库。"
