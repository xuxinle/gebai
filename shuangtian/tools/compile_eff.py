#!/usr/bin/env python3
"""编译效率诊断：找出「值得优化的编译单元」并量化收益。

## 为什么需要它

编译慢的时候，最自然的反应是"拆文件"——**实测那是错的**（见
`CONVENTIONS §10.4`：拆小函数让 `-O1` 慢 20~34%，因为函数级 pass
的固定开销每函数走一遍）。真正有效的是**减少 include**（`dsl.cpp`
换精确头后快 30%）。

所以本工具只做两件事：
  1. **找浪费**：哪些单元 include 了远超实际需要的头（如 `components/all.hpp`）
  2. **量天花板**：每个单元的「只 include、无实现」耗时 = 拆分的固定开销

## 用法

    python tools/compile_eff.py --scan            # 扫全库，按"浪费"排序
    python tools/compile_eff.py --unit src/ui/dsl.cpp   # 单单元分解
    python tools/compile_eff.py --scan --limit 40 --jobs 8

## 判据（来自实测，别凭直觉改）

- include 占比 > 40%：该单元值得减 include
- 实现耗时占大头且是单个巨型函数：**不要拆**（拆更慢），要减 include
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INCLUDE = REPO / "include"

EXTRA_INCLUDES = [
    "-I", str(REPO / "third_party"),
    "-I", str(REPO / "third_party/sqlite"),
    "-I", str(REPO / "third_party/quickjs"),
    "-I", str(REPO / "third_party/webview2"),
    "-I", str(REPO / "third_party/micropython"),
]

# 这些头"很大"，被包含时值得警惕
BLOATED_HEADERS = [
    ("st/ui/components/all.hpp", 60, "全部 UI 组件（多数单元只用其中几个）"),
]


def gxx(extra: list[str], source: Path, timeout: int = 300) -> tuple[float, int]:
    """跑一次 g++，返回 (毫秒, 退出码)。"""
    start = time.perf_counter()
    proc = subprocess.run(
        ["g++", "-std=c++23", "-I", str(INCLUDE), *EXTRA_INCLUDES, *extra, str(source)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout,
    )
    return (time.perf_counter() - start) * 1000.0, proc.returncode


def measure_unit(source: Path) -> dict[str, object]:
    """量一个单元的 include 成本与总成本。"""
    includes = [
        line for line in source.read_text(encoding="utf-8", errors="ignore").splitlines()
        if line.startswith("#include")
    ]
    # 只 include 的空 TU（= 拆分的固定开销）
    probe = REPO / "tmp" / "_ce_includes_only.cpp"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text("".join(line + "\n" for line in includes) + "\nint ce_dummy = 0;\n",
                     encoding="utf-8")
    include_ms, _ = gxx(["-O1", "-fsyntax-only"], probe)
    whole_ms, code = gxx(["-O1", "-fsyntax-only"], source)
    return {
        "source": source,
        "includes": len(includes),
        "include_ms": include_ms,
        "whole_ms": whole_ms,
        "ok": code == 0,
    }


def scan(limit: int, jobs: int) -> int:
    sources: list[Path] = []
    for root in ("src", "apps"):
        base = REPO / root
        if not base.exists():
            continue
        for path in base.rglob("*.cpp"):
            if any(part in {"build", "third_party", "tmp"} for part in path.parts):
                continue
            sources.append(path)
    sources.sort()

    # 先做廉价筛选：只对"包含了大头"或"源码行数大"的单元做精确测量
    suspects: list[Path] = []
    for path in sources:
        text = path.read_text(encoding="utf-8", errors="ignore")
        bloated = any(marker in text for marker, _count, _why in BLOATED_HEADERS)
        long_file = text.count("\n") > 1500
        if bloated or long_file:
            suspects.append(path)
    suspects = suspects[:limit] if limit else suspects

    print(f"候选 {len(suspects)} / {len(sources)} 个单元（含大块头或源码 > 1500 行）\n")
    print(f"{'总耗时':>8} {'include':>8} {'占比':>6} {'include 数':>10}  单元")
    print("-" * 78)

    results: list[dict[str, object]] = []
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        for item in pool.map(measure_unit, suspects):
            if item["whole_ms"] > 0:
                results.append(item)

    for item in sorted(results, key=lambda row: -float(row["whole_ms"])):
        share = float(item["include_ms"]) / float(item["whole_ms"]) * 100.0
        flag = " ←减 include" if share > 40 else ""
        print(f"{item['whole_ms']:>7.0f}ms {item['include_ms']:>7.0f}ms {share:>5.0f}% "
              f"{item['includes']:>10}  {item['source']}{flag}")

    print("\n判据：include 占比高 ⇒ 减 include 有效；不要靠拆函数（实测更慢）。")
    return 0


def unit_detail(source: Path) -> int:
    if not source.exists():
        print(f"不存在：{source}", file=sys.stderr)
        return 1
    text = source.read_text(encoding="utf-8", errors="ignore")
    includes = [line for line in text.splitlines() if line.startswith("#include")]

    print(f"{source}  （{text.count(chr(10))} 行，{len(includes)} 个 include）\n")

    probe = REPO / "tmp" / "_ce_detail_includes.cpp"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text("".join(line + "\n" for line in includes) + "\nint ce_dummy = 0;\n",
                     encoding="utf-8")

    include_ms, _ = gxx(["-O1", "-fsyntax-only"], probe)
    syntax_ms, _ = gxx(["-O1", "-fsyntax-only"], source)
    full_ms, code = gxx(["-O1", "-c", "-o", str(REPO / "tmp" / "_ce.o")], source)
    if code != 0:
        print("编译失败，请先修好单元再量")
        return 1

    print(f"  只 include（拆分固定开销）: {include_ms:>8.0f} ms")
    print(f"  解析+语义（含 include）   : {syntax_ms:>8.0f} ms")
    print(f"  完整 -c -O1               : {full_ms:>8.0f} ms")
    print(f"  优化+代码生成             : {full_ms - syntax_ms:>8.0f} ms"
          f"  ({(full_ms - syntax_ms) / full_ms * 100:.0f}%)")
    print()
    print(f"  拆成 2 个 TU 的净成本 ≈ {include_ms:.0f} ms（多解析一遍 include），")
    print("  而拆函数本身会让 -O1 更慢（实测 +20~34%），所以：")
    print("  → 该做的是**减 include**，不是拆文件。")
    for marker, count, why in BLOATED_HEADERS:
        if marker in text:
            print(f"\n  ⚠ 本文件包含 `{marker}`：{why}。看看能不能换成精确头。")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="编译效率诊断")
    parser.add_argument("--scan", action="store_true", help="扫描全库找值得优化的单元")
    parser.add_argument("--unit", type=str, default="", help="单单元分解")
    parser.add_argument("--limit", type=int, default=30, help="scanf 最多测多少个（默认 30）")
    parser.add_argument("--jobs", type=int, default=6, help="并行测量数（默认 6）")
    args = parser.parse_args()

    if args.unit:
        return unit_detail(Path(args.unit))
    if args.scan:
        return scan(args.limit, args.jobs)
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
