#!/usr/bin/env bash
# 霜天自举脚本（唯一非 st 构建入口）：用编译器直接编译 stpm 自身。
# 之后一切构建/测试/依赖管理都走 `st`。
# 并行策略：每个翻译单元单独编译（-j），再统一链接——与 st 构建驱动同样的思路。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

CXX="${ST_CXX:-${CXX:-g++}}"
if ! command -v "$CXX" >/dev/null 2>&1; then
  if command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
  else
    echo "错误: 未找到 C++ 编译器（g++/clang++），请安装后重试" >&2
    exit 1
  fi
fi

JOBS="${ST_JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4 )}"
# 清单是**唯一权威**：版本号以外的宏、包含路径、第三方 C 源、C 源专用标志全部从 st.pkg 读。
# 历史教训：这些值曾在本脚本里第二遍手写，「清单改了脚本没改」当场把自举打崩
# （SQLite 内置那次：`-include st_sqlite3_config.h` 与 `-Ithird_party/sqlite` 只进了清单，
#  自举仍按硬编码的 `-Iinclude -Ithird_party` 全量编译 `third_party/**/*.c`，
#  sqlite3.c 找不到 sqlite3.h 直接 fatal error）。口径同 `src/pkg/build.cpp`。
PKG="$ROOT/st.pkg"
json_list() {  # $1=字段名 → 每行一项（缺字段时输出空）
  python3 - "$PKG" "$1" <<'PY' 2>/dev/null || true
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
for item in data.get(sys.argv[2]) or []:
    print(item)
PY
}
mapfile -t PKG_INCLUDES < <(json_list include_dirs)
if [ "${#PKG_INCLUDES[@]}" -eq 0 ]; then PKG_INCLUDES=(include third_party); fi
mapfile -t PKG_DEFINES < <(json_list defines)
mapfile -t PKG_CFLAGS < <(json_list c_flags)
if [ "${#PKG_CFLAGS[@]}" -eq 0 ]; then PKG_CFLAGS=(-std=gnu11); fi
mapfile -t PKG_THIRD_PARTY < <(json_list third_party_sources)

INCLUDES=()
for dir in "${PKG_INCLUDES[@]}"; do INCLUDES+=("-I$dir"); done
DEFINES=()
for macro in "${PKG_DEFINES[@]}"; do DEFINES+=("-D$macro"); done

FLAGS=(-std=c++20 -O1 -g "${INCLUDES[@]}" "${DEFINES[@]}" -fno-strict-aliasing
       -Wall -Wextra -Wconversion -Wshadow -Wpedantic -Wold-style-cast -Wnon-virtual-dtor)

# C 源用清单里的 `c_flags`（含 SQLite 的 `-include`）+ 宏 + 包含路径：
# `-include st_sqlite3_config.h` 能被找到，靠的正是清单里 `third_party/sqlite` 那条 `-I`。
CFLAGS=("${PKG_CFLAGS[@]}" -O1 -g "${INCLUDES[@]}" "${DEFINES[@]}" -x c)

mkdir -p build/bin build/obj/bootstrap
mapfile -t SOURCES < <(find src/core src/ext src/pkg tools/stpm -name '*.cpp' | sort)
# 第三方 C 源（third_party/）：st 自身也要链接脚本引擎，故自举必须一并编译；
# 但只编 `third_party_sources` 点名的（不是全量扫描）。
C_SOURCES=()
if [ "${#PKG_THIRD_PARTY[@]}" -gt 0 ]; then
  for pattern in "${PKG_THIRD_PARTY[@]}"; do
    while IFS= read -r file; do
      [ -n "$file" ] && C_SOURCES+=("$file")
    done < <(compgen -G "$pattern" || true)
  done
else
  mapfile -t C_SOURCES < <(find third_party -name '*.c' 2>/dev/null | sort)
fi

echo "[bootstrap] 编译器: $CXX · 并行 $JOBS · 源文件 ${#SOURCES[@]} 个（另 C 源 ${#C_SOURCES[@]} 个）"
START=$(date +%s%N)

# 逐文件并行编译（首次自举无法使用 PCH：st 尚未生成）
export CXX FLAGS_STR="${FLAGS[*]}" CFLAGS_STR="${CFLAGS[*]}" OBJDIR="$ROOT/build/obj/bootstrap" SRCDIR="$ROOT"
compile_one() {
  local src="$1"
  local obj="$OBJDIR/$(echo "$src" | tr '/' '_').o"
  # shellcheck disable=SC2086
  case "$src" in
    *.c) "$CXX" ${CFLAGS_STR} -MMD -MF "$obj.d" -c "$SRCDIR/$src" -o "$obj" ;;
    *)   "$CXX" ${FLAGS_STR} -MMD -MF "$obj.d" -c "$SRCDIR/$src" -o "$obj" ;;
  esac
}
export -f compile_one
# ⚠ **xargs 会吞掉编译失败**（它只在"命令找不到/不可执行"时返回非零，编译错误不算）——
# 于是 `set -e` 形同虚设：源码编不过时脚本照样往下走、链接旧的 .o、
# 最后打印"完成"，留下一份**看起来成功但其实是旧版**的二进制。
# 实测代价：改 lint 规则后 bootstrap 报"完成"，实际新规则根本没进去，排查绕了几轮
#（与"崩溃只有信号名"是同一类问题：失败必须显式可见）。
# **先清掉本轮的 .o**：否则"文件存在"里混着上一次成功编译的残留，
# 下面的"缺哪些 .o"检查就永远通过（实测：故意造语法错误，bootstrap 仍报完成，
# 因为旧的 lint.cpp.o 还在）。
rm -f "$OBJDIR"/*.o
# `|| true`：让 xargs 的退出码**不**直接终止脚本（`set -e`），
# 好让下面那段"缺哪些 .o"的诊断跑完——它给出的信息比 xargs 的裸退出码有用得多
#（哪些文件失败、旧产物已删、下一步怎么办）。
printf '%s\n' "${SOURCES[@]}" "${C_SOURCES[@]}" | xargs -P "$JOBS" -I% bash -c 'compile_one "$@"' _ % || true

OBJECTS=()
MISSING=()
for src in "${SOURCES[@]}"; do
  obj="$OBJDIR/$(echo "$src" | tr '/' '_').o"
  OBJECTS+=("$obj")
  [ -f "$obj" ] || MISSING+=("$src")
done
for src in "${C_SOURCES[@]}"; do
  obj="$OBJDIR/$(echo "$src" | tr '/' '_').o"
  OBJECTS+=("$obj")
  [ -f "$obj" ] || MISSING+=("$src")
done
# 产物的存在性就是编译成功的判据（失败的文件没有 .o）。
if [ "${#MISSING[@]}" -gt 0 ]; then
  {
    echo ""
    echo "[bootstrap] 失败：${#MISSING[@]} 个源文件没有产出目标文件（上面应已打印编译错误）"
    printf '  - %s\n' "${MISSING[@]:0:10}"
    [ "${#MISSING[@]}" -gt 10 ] && echo "  …（共 ${#MISSING[@]} 个）"
    echo "[bootstrap] 提示：删除旧产物以免被误当新版本——"
    echo "  rm -f build/bin/st"
  } >&2
  # 删掉旧产物：留着它会让后续 `st build` 用到旧版工具链，问题以别的方式再现
  #（实测：旧 st 里的 lint 规则是上一版，改完看不到效果）。宁可没有，也不要错的。
  rm -f build/bin/st
  exit 1
fi

echo "[bootstrap] 链接 st ..."
"$CXX" "${FLAGS[@]}" "${OBJECTS[@]}" -o build/bin/st -lpthread -ldl -lm
END=$(date +%s%N)
echo "[bootstrap] 完成 → build/bin/st（$(( (END-START)/1000000 )) ms）"
"./build/bin/st" version || true
