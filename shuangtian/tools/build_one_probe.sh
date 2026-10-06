#!/usr/bin/env bash
# 编译单个文字/绘制量尺（仅验证用；不进 st.pkg）。
#
# 复用 build/<profile>/obj 的框架对象，剔除 platform_*（窗口/GPU 后端，量尺不需要
# 且会引入额外系统库）。与 tools/build_probes.ps1 同口径，供 Linux 侧使用。
# 用法： tools/build_one_probe.sh <probe 名> [profile]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name="$1"
profile="${2:-dev}"
objdir="$root/build/$profile/obj"
outdir="$root/build/probe"
mkdir -p "$outdir"

mapfile -t objs < <(ls "$objdir"/*.o | grep -E '_shuangtian_src_(core|math|codec|raster|text)_' \
  | grep -v -E 'raster_platform_|shell_platform_|platform_gl|_test\.|test_runner|zz_probe')

if [ "${#objs[@]}" -lt 10 ]; then
  echo "框架对象不足（${#objs[@]}）：先跑 build/bin/st build gallery --profile=$profile" >&2
  exit 1
fi

g++ -std=c++20 -O1 -I"$root/include" -I"$root/tools" -I"$root/third_party" \
    -I"$root/third_party/sqlite" \
    "$root/tools/$name.cpp" "${objs[@]}" -o "$outdir/$name" -lpthread -ldl -lm
echo "== OK: $outdir/$name"
