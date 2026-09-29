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
# `-Ivendor`：自举路径也要能看到 vendored 头（nlohmann/json 等）。
FLAGS=(-std=c++20 -O1 -g -Iinclude -Ivendor -fno-strict-aliasing
       -Wall -Wextra -Wconversion -Wshadow -Wpedantic -Wold-style-cast -Wnon-virtual-dtor)

# C 源用独立标志（同一个编译器二进制 + `-x c`，不必引入第二套工具链）：
# `FLAGS` 里 `-Wnon-virtual-dtor` 等是 C++ 专属，喂给 C 编译会直接报错。
CFLAGS=(-std=gnu11 -O1 -g -Iinclude -Ivendor -x c)

mkdir -p build/bin build/obj/bootstrap
mapfile -t SOURCES < <(find src/core src/ext src/pkg tools/stpm -name '*.cpp' | sort)
# 第三方 C 源（vendor/）：st 自身也要链接脚本引擎，故自举必须一并编译
mapfile -t C_SOURCES < <(find vendor -name '*.c' 2>/dev/null | sort)

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
printf '%s\n' "${SOURCES[@]}" "${C_SOURCES[@]}" | xargs -P "$JOBS" -I% bash -c 'compile_one "$@"' _ %

OBJECTS=()
for src in "${SOURCES[@]}"; do OBJECTS+=("$OBJDIR/$(echo "$src" | tr '/' '_').o"); done
for src in "${C_SOURCES[@]}"; do OBJECTS+=("$OBJDIR/$(echo "$src" | tr '/' '_').o"); done

echo "[bootstrap] 链接 st ..."
"$CXX" "${FLAGS[@]}" "${OBJECTS[@]}" -o build/bin/st -lpthread -ldl -lm
END=$(date +%s%N)
echo "[bootstrap] 完成 → build/bin/st（$(( (END-START)/1000000 )) ms）"
"./build/bin/st" version || true
