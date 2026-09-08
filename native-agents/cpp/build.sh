#!/bin/sh
# C++ 语言目录构建脚本：用 g++（或 clang++ 兜底）编译子代理项目 main.cpp 为 driver。
# 使用：./build.sh [项目目录名]（缺省遍历全部子代理项目）
LANGDIR="$(cd "$(dirname "$0")" && pwd)"

if command -v g++ >/dev/null 2>&1; then
  CXX=g++
elif command -v clang++ >/dev/null 2>&1; then
  CXX=clang++
else
  echo "[build.sh] 未找到 g++/clang++——请安装 GNU 编译器" >&2
  exit 1
fi

if [ -n "$1" ]; then
  TARGETS="$1"
else
  TARGETS=""
  for d in "$LANGDIR"/*/; do
    [ -f "$d/main.cpp" ] && TARGETS="$TARGETS $(basename "$d")"
  done
fi

FAIL=0
for p in $TARGETS; do
  if [ ! -f "$LANGDIR/$p/main.cpp" ]; then
    echo "[build.sh] 跳过 $p（无 main.cpp）" >&2
    continue
  fi
  echo "[build.sh] 编译 $p ..."
  ( cd "$LANGDIR/$p" && "$CXX" -std=c++17 -O2 -I"$LANGDIR/stb" -o driver main.cpp ) || {
    FAIL=1
    echo "[build.sh] $p 编译失败" >&2
    continue
  }
  echo "[build.sh] $p -> driver"
done
exit $FAIL
