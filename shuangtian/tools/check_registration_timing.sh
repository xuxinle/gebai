#!/usr/bin/env bash
# 验证组件注册的两条时机性质（各自独立产物）。
#
#   ./tools/check_registration_timing.sh
#
# 判据：
#   AUTO       → 真的 new 了组件、不写 import，它必须已注册（AUTO_OK）
#   ON_DEMAND  → 只 include 不 new，它必须查不到，产物里也不得有它的符号
#
# 为何要独立产物：这两条说的都是「某个组件的 .o 有没有进产物」。在同一个
# 测试二进制里所有组件都被链进来了，区分不出来。
#
# 依赖：先构建库（st build 任意目标，保证 build/dev/obj/_library_units_library.a 存在）。
set -u
cd "$(dirname "$0")/.." || exit 1

LIB=build/dev/obj/_library_units_library.a
if [ ! -f "$LIB" ]; then
  echo "[错误] 找不到 $LIB —— 先跑 st build（任意目标）生成库" >&2
  exit 2
fi

# 产物名带 .exe：编译器是 MinGW（Windows 程序），而脚本跑在 bash 里；
# 不带后缀时 bash 找不到可执行文件（实测：编译成功但 "No such file or directory"）。
EXE=.exe

CXX=${CXX:-}
# 为何要挑：bash 的 PATH 里 `g++` 可能是 WSL 的 GCC 13，它没有
# `<mdspan>` / `<format>`（C++23 库），会在 surface.hpp 上直接报
# "mdspan: No such file or directory"。项目实际用 MinGW GCC 16。
# 所以先按已知安装位置找，再退回 PATH 上的 g++。
if [ -z "$CXX" ]; then
  for cand in \
    "/c/Users/Administrator/program/mingw64/bin/g++.exe" \
    "/mnt/c/Users/Administrator/program/mingw64/bin/g++.exe"; do
    if [ -x "$cand" ]; then CXX="$cand"; break; fi
  done
fi
[ -n "$CXX" ] || CXX=g++
if ! "$CXX" --version >/dev/null 2>&1; then
  echo "[错误] 找不到编译器 $CXX（可用 CXX=... 覆盖）" >&2
  exit 2
fi
# 确认是 C++23 库齐的那个编译器（GCC 13 没有 <mdspan>）
if ! printf '#include <mdspan>\nint main(){}\n' | "$CXX" -std=c++23 -x c++ -fsyntax-only - >/dev/null 2>&1; then
  echo "[错误] $CXX 不支持 <mdspan>（需要 GCC 14+）：$( "$CXX" --version | head -1)" >&2
  echo "        改用 CXX=<mingw g++> ./tools/check_registration_timing.sh" >&2
  exit 2
fi
OUT=build/dev/tmp
mkdir -p "$OUT"
fail=0

echo "=== AUTO：真的 new 了 Terminal，且不写 import ==="
if $CXX -std=c++23 -O1 -Iinclude -Ithird_party -Ithird_party/sqlite \
      -DPROBE_AUTO tools/registration_timing_probe.cpp "$LIB" -o "$OUT/probe_auto$EXE" 2>&1 | tail -5; then
  "$OUT/probe_auto$EXE"
  [ $? -eq 0 ] || { echo "  ✗ AUTO 失败：用到组件却没自动注册" >&2; fail=1; }
else
  echo "  ✗ AUTO 编译失败" >&2
  fail=1
fi

echo
echo "=== ON_DEMAND：只 include terminal.hpp，但只用 Text ==="
if $CXX -std=c++23 -O1 -Iinclude -Ithird_party -Ithird_party/sqlite \
      -DPROBE_ON_DEMAND tools/registration_timing_probe.cpp "$LIB" -o "$OUT/probe_demand$EXE" 2>&1 | tail -5; then
  "$OUT/probe_demand$EXE"
  [ $? -eq 0 ] || { echo "  ✗ ON_DEMAND 失败：没用到却进了产物" >&2; fail=1; }
  # 独立再查一次符号，防止注册表层与产物层不一致
  syms=$(nm -C "$OUT/probe_demand$EXE" 2>/dev/null | grep -c "st::ui::Terminal::" || true)
  echo "  产物中 Terminal 成员符号数: $syms"
  [ "$syms" -eq 0 ] || { echo "  ✗ ON_DEMAND 失败：产物里出现了 Terminal 符号" >&2; fail=1; }
else
  echo "  ✗ ON_DEMAND 编译失败" >&2
  fail=1
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "两条时机性质都成立（与 st/ui/components/README.md 第二节一致）"
else
  echo "有断言不成立 —— 文档与代码已经不一致，请先修代码或改文档" >&2
fi
exit "$fail"
