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

# 清单是**唯一权威**：版本号以外的宏、包含路径、第三方 C 源、C 源专用标志、
# 以及**单元集**（sources − targets.st.exclude_sources + targets.st.sources）全部从 st.pkg 读。
# 历史教训：这些值曾在本脚本里第二遍手写，「清单改了脚本没改」当场把自举打崩
# （SQLite 内置那次：`-include st_sqlite3_config.h` 与 `-Ithird_party/sqlite` 只进了清单，
#  自举仍按硬编码的 `-Iinclude -Ithird_party` 全量编译 `third_party/**/*.c`，
#  sqlite3.c 找不到 sqlite3.h 直接 fatal error）。
# 单元集也一样：曾按目录写死（core/ext/pkg/stpm），新增 `src/ext/video.cpp` 引
# `codec/raster` 后链接期一串 `undefined reference to st::...`——报错指向源码，
# 真因是"该编的单元没编"。口径同 `src/pkg/build.cpp`。
PKG="$ROOT/st.pkg"
json_emit() {  # $1=python 表达式 → 逐行输出
  python3 - "$PKG" "$1" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
value = eval(sys.argv[2], {"data": data})
for item in value or []:
    print(item)
PY
}

# 单元集：glob 展开（`*` 不跨 `/`，`**` 跨；与 `fs::match_glob` 同口径）
mapfile -t UNITS < <(python3 - "$ROOT" <<'PY'
import glob, json, os, sys
root = sys.argv[1]
with open(os.path.join(root, "st.pkg"), encoding="utf-8") as handle:
    data = json.load(handle)
target = (data.get("targets") or {}).get("st") or {}

def expand(pattern):
    return sorted(os.path.relpath(p, root).replace(os.sep, "/")
                  for p in glob.glob(os.path.join(root, pattern), recursive=True)
                  if os.path.isfile(p))

def match(pattern, path):
    """逐段匹配：`**` 跨段，`*`/`?` 不跨 `/`（与 fs::match_glob 同口径）。"""
    pat = [s for s in pattern.split("/") if s]
    seg = [s for s in path.split("/") if s and s != "."]
    def seg_match(p, s):
        if p == "*":
            return True
        out, i = "", 0
        while i < len(p):
            c = p[i]
            if c == "*":
                out += "[^/]*"
                while i + 1 < len(p) and p[i + 1] == "*":
                    i += 1
            elif c == "?":
                out += "[^/]"
            else:
                out += "\\" + c if c in ".^$+{}()|[]\\" else c
            i += 1
        import re
        return re.fullmatch(out, s) is not None
    def walk(pi, si):
        if pi == len(pat):
            return si == len(seg)
        if pat[pi] == "**":
            return any(walk(pi + 1, skip) for skip in range(si, len(seg) + 1))
        if si == len(seg):
            return False
        return seg_match(pat[pi], seg[si]) and walk(pi + 1, si + 1)
    return walk(0, 0)

units = []
for pattern in data.get("sources", []):
    units += expand(pattern)
# 目标段缺失就**显式失败**：静默退化成"编全库"会在编译期以
# `battery/embed.hpp: No such file or directory` 爆掉——那看上去像路径问题，不是清单问题。
if "st" not in (data.get("targets") or {}):
    sys.exit("错误: st.pkg 里没有 targets.st（自举无法推导单元集）")
# **排除先于并入目标源**：所以"在被排除目录里、但工具确实需要"的单元
# （如 src/ui/window_control.cpp——shell 后端的窗口边缘命中契约）写在 targets.st.sources 即可。
# **白名单优先**（与 `src/pkg/build.cpp`、`bootstrap.ps1` 同口径）：`include_sources` 非空时，
# 库源只保留与它匹配的那部分，`exclude_sources` 不再参与。
# 只改构建驱动、不改自举脚本的症状是**旧 st 无法重建自己**（它会按黑名单口径编全库，
# 包括需要构建期生成头的 `src/ui`/`src/app`），而唯一能修它的办法就是自举 ⇒ 死锁。
include_patterns = target.get("include_sources", [])
if include_patterns:
    before = len(units)
    units = [u for u in units if any(match(p, u) for p in include_patterns)]
    print(f"[bootstrap] 白名单: 库源 {before} → {len(units)} 个（include_sources {len(include_patterns)} 条）", file=sys.stderr)
else:
    for pattern in target.get("exclude_sources", []):
        before = len(units)
        units = [u for u in units if not match(pattern, u)]
        if len(units) == before:
            print(f"[bootstrap] 排除模式未命中任何源文件: {pattern}", file=sys.stderr)
for pattern in target.get("sources", []):
    units += expand(pattern)
for unit in sorted(set(units)):
    print(unit)
PY
)
if [ "${#UNITS[@]}" -eq 0 ]; then
  echo "错误: 单元集为空（st.pkg 的 sources / targets.st 解析异常）" >&2
  exit 1
fi

mapfile -t PKG_INCLUDES < <(json_emit 'data.get("include_dirs") or []')
if [ "${#PKG_INCLUDES[@]}" -eq 0 ]; then PKG_INCLUDES=(include third_party); fi
mapfile -t PKG_DEFINES < <(json_emit 'data.get("defines") or []')
mapfile -t PKG_CFLAGS < <(json_emit 'data.get("c_flags") or []')
if [ "${#PKG_CFLAGS[@]}" -eq 0 ]; then PKG_CFLAGS=(-std=gnu11); fi
mapfile -t PKG_THIRD_PARTY < <(json_emit 'data.get("third_party_sources") or []')
# 语言标准：**单一真源在清单**（`cxx_standard`），与 `src/pkg/build.cpp` 同口径。
# `json_emit` 是逐行输出（标量会被逐字符拆开），所以标量项用专门的一行读法。
PKG_STANDARD=$(python3 - "$PKG" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
print(data.get("cxx_standard") or 23)
PY
)
if [ -z "$PKG_STANDARD" ]; then PKG_STANDARD=23; fi

INCLUDES=()
for dir in "${PKG_INCLUDES[@]}"; do INCLUDES+=("-I$dir"); done
DEFINES=()
for macro in "${PKG_DEFINES[@]}"; do DEFINES+=("-D$macro"); done

FLAGS=("-std=c++$PKG_STANDARD" -O1 -g "${INCLUDES[@]}" "${DEFINES[@]}" -fno-strict-aliasing
       -Wall -Wextra -Wconversion -Wshadow -Wpedantic -Wold-style-cast -Wnon-virtual-dtor)

# C 源用清单里的 `c_flags`（含 SQLite 的 `-include`）+ 宏 + 包含路径：
# `-include st_sqlite3_config.h` 能被找到，靠的正是清单里 `third_party/sqlite` 那条 `-I`。
CFLAGS=("${PKG_CFLAGS[@]}" -O1 -g "${INCLUDES[@]}" "${DEFINES[@]}" -x c)

mkdir -p build/bin build/obj/bootstrap

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

echo "[bootstrap] 编译器: $CXX · 并行 $JOBS · 源文件 ${#UNITS[@]} 个（另 C 源 ${#C_SOURCES[@]} 个）"
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
printf '%s\n' "${UNITS[@]}" "${C_SOURCES[@]}" | xargs -P "$JOBS" -I% bash -c 'compile_one "$@"' _ % || true

OBJECTS=()
MISSING=()
for src in "${UNITS[@]}"; do
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

# 链接库：读仓库根的 link-libs.txt（单一数据源；bootstrap.ps1/st.pkg/build.cpp 共用同一份）。
# 历史教训：清单曾在多处各写一份，漏项的报错全部指向源码（undefined reference），
# 排查方向被带偏——收敛到一份文件后改一处即全局生效。
LINK_LIBS_FILE="$ROOT/link-libs.txt"
if [ ! -f "$LINK_LIBS_FILE" ]; then
  echo "[bootstrap] 缺 $LINK_LIBS_FILE（链接库单一数据源）" >&2
  exit 1
fi
LINK_LIBS=()
in_posix=0
while IFS= read -r line || [ -n "$line" ]; do
  trimmed="${line#"${line%%[![:space:]]*}"}"   # 去左空白
  case "$trimmed" in
    ''|'#'*) continue ;;
    posix:*) in_posix=1 ;;
    windows:*) in_posix=0 ;;
    *) [ "$in_posix" -eq 1 ] && LINK_LIBS+=("-l$trimmed") ;;
  esac
done < "$LINK_LIBS_FILE"

echo "[bootstrap] 链接 st ...（系统库：${LINK_LIBS[*]}）"
"$CXX" "${FLAGS[@]}" "${OBJECTS[@]}" -o build/bin/st "${LINK_LIBS[@]}"

# **链接后分离调试信息**（与 `st build` 的 `separate_debug_info` 同口径）：
# 自举产物与 st 构建的产物应当**尺寸量级一致**（否则“自举出来的 st”与
# “st 自己重建的 st”看起来像两个东西）。dev 档下 `.debug_*` 占 89%——
# 实测 st 70.1MB → 4.59MB，而 gdb/addr2line 仍能用（它自己认 `.gnu_debuglink`）。
#
# ⚠ 失败**不阻断自举**（`|| true` 不是偷懒）：产物已经能跑，只是胖了点；
# 为“有个文件多 65MB”把自举打回去，代价与收益不成比例。
if command -v objcopy >/dev/null 2>&1 && [ "${ST_NO_STRIP:-0}" != "1" ]; then
  if objcopy --only-keep-debug build/bin/st build/bin/st.debug \
     && objcopy --strip-debug build/bin/st \
     && objcopy --add-gnu-debuglink=build/bin/st.debug build/bin/st; then
    echo "[bootstrap] 已分离调试信息 → build/bin/st（$(du -m build/bin/st | cut -f1) MB）+ st.debug（$(du -m build/bin/st.debug | cut -f1) MB）"
  else
    echo "[bootstrap] 警告：分离调试信息失败（产物仍可用，仅体积偏大）" >&2
  fi
fi
END=$(date +%s%N)
echo "[bootstrap] 完成 → build/bin/st（$(( (END-START)/1000000 )) ms）"
"./build/bin/st" version || true
