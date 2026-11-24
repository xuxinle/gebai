#!/usr/bin/env python3
"""编译成本实测：TU 耗时 + 头文件体重 × 传染面。

## 为什么需要它

`tools/compile_eff.py` 只对「>1500 行」或「含 all.hpp」的 suspect 跑 `-fsyntax-only`，
看不见「52 行却 6.9 s」这类单元——而本仓实测最贵的单元恰恰是小的。

本工具做三件 compile_eff 不做的事：
  1. 对**全部** src+apps TU 跑完整 `-c -O1`（= 真实并行关键路径成本）
  2. 统计每个头**被多少个 TU 直接包含**（= 改这个头的爆炸半径）
  3. 量每个头的**体重**（只 include 它的空 TU 耗时），给出 `体重 × 传染数` 的真实账本

## 用法

    py -3 tools/tu_cost.py scan               # 全库 TU 扫描（~2 分钟，14 路并行）
    py -3 tools/tu_cost.py headers            # 头文件体重 × 传染面（~20 秒）
    py -3 tools/tu_cost.py scan --jobs 8

产物落 `tmp/tu_cost.json` / `tmp/tu_cost.csv` / `tmp/header_weights.json`。

## 判据（2026-11 实测得出，别凭直觉改）

- **行数是症状，不是成本**：6,610 行的 `platform_d3d11.cpp` 8.2 s，
  而 **52 行**的 `dsl_component_imports.cpp` 6.9 s、**66 行**的 `time.hpp`（单头体重）1.98 s。
- **拆 .cpp 一定让总编译量上升**：每个新 TU 重付一遍 include 固定开销，
  且小函数让 `-O1` 慢 20~34%。只有「新 TU 的 include 面确实收窄」才净赚。
  反面教材：`apps/gbcode` 的 page 系列已拆 8 个文件，**每个都 9~10 s**（都 include `page.hpp`）。
- **足迹（体重 × TU 数）大但体重小 ⇒ 无害**（如 `string.hpp`）；足迹大且体重大 ⇒ 才值得减。
- **巨类不是编译问题**：`st/ui/element.hpp` 1,990 行/1,605 行类声明，足迹仅排第 11。
  它是职责设计问题，别用「能提速」给重构找理由。

详见 `CONVENTIONS.md` §11。
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "tmp"

# 传递依赖的包含根（与 `st` 构建驱动一致；生成头与工程根不可漏，
# 否则 apps/* 会整片失败并把「缺头」误读成「耗时短」）。
INCLUDE_ROOTS = [
    "include",
    "third_party",
    "third_party/sqlite",
    "third_party/quickjs",
    "third_party/webview2",
    "third_party/micropython",
    "build/dev/embed/lib/include",
    "build/dev/embed/gallery/include",
    "build/dev/embed/gbcode/include",
    "build/dev/embed/caiyun/include",
    "build/dev/embed/jianmu/include",
    "build/dev/embed/louyue/include",
    "build/dev/embed/gedou/include",
    "build/dev/embed/xiaobai/include",
    "build/dev/embed/xiangqi/include",
    "build/dev/embed/cefu/include",
    "build/dev/embed/gebai_desktop/include",
    "build/dev/embed/st_desktop/include",
    "",  # 工程根：`#include "apps/gbcode/git_service.hpp"` 这种形态
]

INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
SKIP_DIRS = {"build", "out", "third_party", "tmp", "logs", "node_modules", ".git"}


def _flags() -> list[str]:
    out = ["-std=c++23"]
    for root in INCLUDE_ROOTS:
        out += ["-I", str(REPO / root) if root else str(REPO)]
    return out


def _base() -> list[str]:
    return [*_flags(), "-O1", "-fsyntax-only", '-DST_VERSION="0.1.0"', "-DST_ENABLE_LINT=1"]


def collect_sources() -> list[Path]:
    out: list[Path] = []
    for root in ("src", "apps"):
        base = REPO / root
        if not base.exists():
            continue
        for path in base.rglob("*.cpp"):
            if SKIP_DIRS & set(path.parts):
                continue
            out.append(path)
    return sorted(out)


def measure_tu(path: Path) -> dict:
    text = path.read_text(encoding="utf-8", errors="ignore")
    incs = [m.group(1) for m in (INC_RE.match(line) for line in text.splitlines()) if m]
    start = time.perf_counter()
    proc = subprocess.run(["g++", *_base(), str(path)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ms = int((time.perf_counter() - start) * 1000)
    return {
        "path": str(path.relative_to(REPO)).replace("\\", "/"),
        "lines": text.count("\n") + 1,
        "ms": ms,
        "ok": proc.returncode == 0,
        "err": proc.stderr.decode("utf-8", "ignore")[:300] if proc.returncode else "",
        "includes": incs,
    }


def weigh_header(header: str) -> int:
    """只 include 该头的空 TU 耗时（2 轮取最小）。失败返回 -1。"""
    probe = OUT / f"_hw_{abs(hash(header)) % 10**9}.cpp"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text(f'#include "{header}"\nint hw_dummy = 0;\n', encoding="utf-8")
    best = 10**9
    try:
        for _ in range(2):
            t = time.perf_counter()
            proc = subprocess.run(["g++", *_base(), str(probe)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if proc.returncode != 0:
                return -1
            best = min(best, int((time.perf_counter() - t) * 1000))
    finally:
        probe.unlink(missing_ok=True)
    return best


def cmd_scan(jobs: int) -> int:
    sources = collect_sources()
    print(f"共 {len(sources)} 个 TU，{jobs} 路并行，完整 -c -O1 ...", flush=True)
    results: list[dict] = []
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for i, item in enumerate(pool.map(measure_tu, sources), 1):
            results.append(item)
            if i % 50 == 0:
                print(f"  {i}/{len(sources)}  {time.perf_counter() - t0:.0f}s", flush=True)
    elapsed = time.perf_counter() - t0

    ok = [r for r in results if r["ok"]]
    bad = [r for r in results if not r["ok"]]
    (OUT / "tu_cost.json").write_text(json.dumps(
        {"elapsed_s": round(elapsed), "tu": sorted(ok, key=lambda r: -r["ms"]),
         "failed": [r["path"] for r in bad]}, ensure_ascii=False, indent=1), encoding="utf-8")
    with (OUT / "tu_cost.csv").open("w", encoding="utf-8") as f:
        f.write("ms,lines,n_includes,path\n")
        for r in ok:
            f.write(f'{r["ms"]},{r["lines"]},{len(r["includes"])},{r["path"]}\n')

    print(f"\n完成：{len(ok)} 成功 / {len(bad)} 失败，墙钟 {elapsed:.0f}s，串行合计 "
          f"{sum(r['ms'] for r in ok) / 1000:.0f}s\n")
    print("=== 最慢 25 个 TU（全量并行的关键路径由它们决定）===")
    for r in sorted(ok, key=lambda r: -r["ms"])[:25]:
        print(f'{r["ms"]:7d}ms {r["lines"]:5d}行 {r["path"]}')
    if bad:
        print(f"\n⚠ {len(bad)} 个 TU 编译失败（口径污染，统计偏乐观）：")
        for r in bad[:8]:
            print(f'  {r["path"]}')
    return 0


def cmd_headers(top: int) -> int:
    scan_file = OUT / "tu_cost.json"
    if not scan_file.exists():
        print("先跑 `py -3 tools/tu_cost.py scan`", file=sys.stderr)
        return 1
    scan = json.loads(scan_file.read_text(encoding="utf-8"))
    fanout: dict[str, int] = {}
    for r in scan["tu"]:
        for header in set(r["includes"]):
            if header.startswith(("st/", "apps/")):
                fanout[header] = fanout.get(header, 0) + 1

    cands = [h for h, _ in sorted(fanout.items(), key=lambda kv: -kv[1])][:top]
    print(f"测 {len(cands)} 个头（每头 2 轮取最小）...", flush=True)
    weights: dict[str, int] = {}
    with ThreadPoolExecutor(max_workers=6) as pool:
        for header, ms in zip(cands, pool.map(weigh_header, cands)):
            weights[header] = ms

    rows = []
    for header in cands:
        weight = weights.get(header, -1)
        if weight < 0:
            continue
        rows.append((weight * fanout[header] / 1000.0, weight, fanout[header], header))
    rows.sort(reverse=True)

    print(f"\n{'足迹s':>8} {'体重ms':>8} {'TU数':>5}  头文件")
    print("-" * 64)
    for cost, weight, count, header in rows:
        print(f"{cost:8.1f} {weight:8d} {count:5d}  {header}")

    print("\n判据：足迹 = 体重 × TU 数。足迹大且体重大 => 减体重（前向声明/移实现）；"
          "足迹大但体重小 => 无害，别动。")
    (OUT / "header_weights.json").write_text(json.dumps(
        {h: {"weight_ms": weights.get(h), "tu_count": fanout[h]} for h in cands},
        ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="编译成本实测（TU 耗时 / 头文件体重）")
    sub = parser.add_subparsers(dest="cmd", required=True)
    scan = sub.add_parser("scan", help="全库 TU 扫描")
    scan.add_argument("--jobs", type=int, default=14)
    heads = sub.add_parser("headers", help="头文件体重 × 传染面")
    heads.add_argument("--top", type=int, default=34)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.cmd == "scan":
        return cmd_scan(args.jobs)
    return cmd_headers(args.top)


if __name__ == "__main__":
    sys.exit(main())
