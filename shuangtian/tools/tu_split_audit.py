#!/usr/bin/env python3
"""超大 TU 的成本分解报告：把「拆了会变快还是变慢」算清楚再决定。

## 为什么需要它

`tools/compile_eff.py --unit` 给出三档（只 include / 解析语义 / 完整 -O1），
但对**该不该拆**只给一句通用结论。本脚本补上缺的那几列：

  1. **include 面明细**：每个 include 单独要多少秒（找出"拖后腿的那一行"）
  2. **顶层定义聚类**：按命名空间/类前缀把顶层函数归类，看**职责边界**在哪
  3. **拆分盈亏平衡点**：拆成 N 份后净成本 = N × include 固定开销 + 实现体 × 加速比，
     与现状对比给出「拆 N 份是否划算」

## 用法

    py -3 tools/tu_split_audit.py src/raster/platform_d3d11.cpp
    py -3 tools/tu_split_audit.py src/pkg/build.cpp --samples 8

## 判据（来自实测，别凭直觉改）

- 实现体占 >50% 且是**单个巨型函数** ⇒ 拆更慢（函数级 pass 每函数走一遍，-O1 +20~34%）
- include 固定开销占 >60% ⇒ 拆必亏（每个新 TU 重付一遍）
- 只有「拆出的 TU include 面确实收窄」才净赚——这需要看第 1 列明细
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

INCLUDE_DIRS = [
    "include", "third_party", "third_party/sqlite", "third_party/quickjs",
    "third_party/webview2", "third_party/micropython",
    # 生成头：battery/embed.hpp 等由构建驱动产出，缺了 apps/* 与部分 src 会失败
    "build/dev/embed/lib/include",
    "build/dev/embed/gallery/include",
    "include/st/ui/components",
    "",  # 工程根
]
# 同目录私有头（如 `rasterize_internal.hpp`）：只 include 探针必须能解析到它，
# 否则“只 include”这一项永远失败、口径直接作废（实测踩过）。
# 由 `audit` 把源文件所在目录补进来。
EXTRA_DIRS: list[Path] = []
BASE = ["-std=c++23", "-O1", "-fsyntax-only", '-DST_VERSION="0.1.0"', "-DST_ENABLE_LINT=1"]
INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')


def flags() -> list[str]:
    out: list[str] = []
    for d in INCLUDE_DIRS:
        out += ["-I", str(REPO / d) if d else str(REPO)]
    for extra in EXTRA_DIRS:
        out += ["-I", str(extra)]
    return out


def timed(cmd: list[str], rounds: int = 2) -> tuple[int, bool]:
    best = 10**9
    ok = True
    last_err = ""
    for _ in range(rounds):
        start = time.perf_counter()
        proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        best = min(best, int((time.perf_counter() - start) * 1000))
        if proc.returncode != 0:
            ok = False
            last_err = proc.stderr.decode("utf-8", "ignore")
    if not ok:
        print(f"⚠ 编译失败（取一次错误输出）: {last_err[:300]}", file=sys.stderr)
    return best, ok


def probe_includes(incs: list[str], tag: str) -> tuple[int, bool]:
    probe = REPO / "tmp" / f"_audit_{tag}.cpp"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text("".join(f"#include <{i}>\n" if not i.startswith(("st/", "apps/"))
                            else f'#include "{i}"\n' for i in incs) + "int a = 0;\n",
                     encoding="utf-8")
    return timed(["g++", *BASE, *flags(), str(probe)])


def audit(path: Path, samples: int) -> int:
    text = path.read_text(encoding="utf-8", errors="ignore")
    lines = text.split("\n")
    incs = [m.group(1) for m in (INC_RE.match(line) for line in lines) if m]
    EXTRA_DIRS.append(path.parent)

    print(f"\n{'=' * 74}\n{path.relative_to(REPO)}  （{len(lines):,} 行，{len(incs)} 个 include）\n{'=' * 74}\n")

    base_ms, _ = probe_includes([], "empty")
    all_ms, ok_all = probe_includes(incs, "all")
    full_ms, ok_full = timed(["g++", *BASE, *flags(), str(path)], rounds=1)
    print(f"空 TU 基线（仅标志）      : {base_ms:>7} ms")
    print(f"只 include 全部            : {all_ms:>7} ms  ⇒ 固定开销 {all_ms - base_ms} ms")
    print(f"完整 -c -O1                : {full_ms:>7} ms")
    if not (ok_all and ok_full):
        print("⚠ 编译失败，口径不可信")
        return 1
    impl = full_ms - all_ms
    print(f"实现体（解析+优化+代码生成）: {impl:>7} ms  ({impl / full_ms * 100:.0f}%)\n")

    # 1) 逐个 include 的边际成本
    print("── 每个 include 的单独成本（找出拖后腿的那一行）──")
    rows = []
    for inc in incs:
        ms, ok = probe_includes([inc], "one")
        if ok:
            rows.append((ms - base_ms, inc))
    rows.sort(reverse=True)
    for cost, inc in rows[:samples]:
        print(f"  {cost:>6} ms  {inc}")

    # 2) 顶层定义聚类
    print("\n── 顶层定义聚类（职责边界在哪）──")
    groups: dict[str, list[int]] = defaultdict(list)
    depth = 0
    for i, line in enumerate(lines):
        stripped = line.strip()
        if depth == 0 and stripped and not stripped.startswith(("//", "#", "}", "/*", "*")):
            m = re.match(r"^(?:\[\[nodiscard\]\]\s*)?(?:static\s+)?(?:inline\s+)?"
                         r"(?:auto|void|bool|int|struct|class|enum|namespace|template)\b[\w:<>&*,\s]*?"
                         r"\b(\w+)::", stripped)
            if m:
                groups[m.group(1)].append(i + 1)
            elif stripped.startswith(("struct ", "class ", "enum ")):
                groups["<type>:" + stripped.split()[1].split("::")[0]].append(i + 1)
            elif stripped.startswith("namespace"):
                groups["<namespace>"].append(i + 1)
        depth += line.count("{") - line.count("}")
        if depth < 0:
            depth = 0
    for name, spots in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:12]:
        print(f"  {len(spots):>4} 处  {name}")

    # 3) 盈亏平衡
    print("\n── 拆分盈亏平衡（净成本 = N×固定开销 + 实现体/N + 拆函数惩罚）──")
    print(f"{'份数':>4} {'固定开销':>9} {'实现体':>9} {'惩罚+20%':>9} {'合计':>9}  {'vs 现状':>9}")
    for n in range(2, 6):
        fixed = all_ms * n
        body = impl / n * 1.2      # 拆小后 -O1 慢 20%（实测区间 20~34% 取下沿）
        total = fixed + body
        delta = total - full_ms
        mark = "  ← 亏" if delta > 0 else "  ← 赚"
        print(f"{n:>4} {fixed:>8}ms {body:>8.0f}ms {body * 0.2:>8.0f}ms {total:>8.0f}ms"
              f" {delta:>+8.0f}ms{mark}")

    print(f"\n判据：固定开销 {all_ms - base_ms} ms 是**每个新 TU 都要重付**的。")
    print("若上表全是「亏」，那正确的动作是**减 include 面**或**抽独立协作类**，不是拆文件。")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="超大 TU 拆分成本审计")
    parser.add_argument("source", help="源文件路径（相对工程根）")
    parser.add_argument("--samples", type=int, default=8, help="展示前 N 个最贵 include")
    args = parser.parse_args()
    path = REPO / args.source
    if not path.exists():
        print(f"不存在：{path}", file=sys.stderr)
        return 1
    return audit(path, args.samples)


if __name__ == "__main__":
    sys.exit(main())
