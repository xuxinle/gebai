#!/usr/bin/env bash
# 编译**需要 UI 层**的单文件探针（布局/绘制量尺）。
#
# 与 `build_one_probe.sh` 的分工：那个只取 core/math/codec/raster/text（够画文字），
# 本脚本多带 `ui`（组件/主题/文本端口）——但**剔除 dsl/script_host/declarative_host**
# 那几支（它们拉 quickjs 与 shell，量尺不需要）。
#
# 用法： tools/build_ui_probe.sh <probe 名> [profile]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name="$1"
profile="${2:-dev}"
objdir="$root/build/$profile/obj"
outdir="$root/build/probe"
mkdir -p "$outdir"

if [ ! -d "$objdir" ]; then
  echo "没有 $objdir：先跑 build/bin/st build --profile=$profile" >&2
  exit 1
fi

# 底层对象（core/math/codec/raster/text/md）
mapfile -t objs < <(ls "$objdir"/*.o | grep -E '_shuangtian_src_(core|math|codec|raster|text|md)_' \
  | grep -v -E 'raster_platform_|shell_platform_|platform_gl|_test\.|test_runner|zz_probe')

# UI 层：组件/主题/文本端口/元素/选择器（**排除** dsl / script_* / declarative_host / xml_ui——
# 它们拉 quickjs 与 shell，量尺不需要）。selector 要留着：ui_root 依赖它的符号。
mapfile -t uiobjs < <(ls "$objdir"/*.o | grep -E '_shuangtian_src_ui_' \
  | grep -v -E 'src_ui_(dsl|script_bridge|script_host|declarative_host|xml_ui|state_trace)\.cpp\.o$')
objs+=("${uiobjs[@]}")

# ext/json：theme_io 的 JSON 解析依赖它
mapfile -t extobjs < <(ls "$objdir"/*.o | grep -E '_shuangtian_src_ext_json\.cpp\.o$')
objs+=("${extobjs[@]}")

# app 层：文本端口适配（RendererTextPort）
mapfile -t appobjs < <(ls "$objdir"/*.o | grep -E '_shuangtian_src_app_text_port\.cpp\.o$')
objs+=("${appobjs[@]}")

if [ "${#objs[@]}" -lt 40 ]; then
  echo "框架对象不足（${#objs[@]}）：先跑 build/bin/st build gallery --profile=$profile" >&2
  exit 1
fi

g++ -std=c++23 -O2 -I"$root/include" -I"$root/third_party" -I"$root/third_party/sqlite" \
    "$root/tools/$name.cpp" "${objs[@]}" -o "$outdir/$name" -lpthread -ldl -lm
echo "== OK: $outdir/$name"
