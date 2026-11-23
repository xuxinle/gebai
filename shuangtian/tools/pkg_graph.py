#!/usr/bin/env python3
"""包级依赖图分析：为「按需编译」提供决策依据（对应 Go 的 import 图）。

## 为什么需要它

Go 能按需编译，靠的是编译器掌握**精确的 import 图** + **禁止循环依赖**。
霜天没有那层编译器支持，只能自己从 `#include` 关系把图重建出来。

本工具做三件事：
  1. **复核零环**：包图必须是有向无环图（DAG），否则"闭包"无意义；
  2. **算闭包**：每个应用真正需要的包集合（从它源码的 include 起点做传递闭包）；
  3. **报可裁量**：闭包外的框架单元就是可裁的（按需编译的收益上限）。

## 包的定义（与 `st::pkg::graph` 同一套规则，两边必须一致）

头文件 `include/st/<a>/<b>[/<c>...]/<name>.hpp` 归属包 `st/<a>/<b>[/<c>]`：

  - 取到**目录层**，不含文件名：`st/ui/components/button.hpp` → `st/ui/components`
  - 但 `st/ui/ui_root.hpp`（只两层）→ `st/ui`，即**浅层头归到它所在的最近目录**

实现 `src/<a>/<b>[/<c>...]/<name>.cpp` 归属包 `st/<a>/<b>[/<c>]`（把 `src/` 换成 `st/`）。

> ⚠ 这条规则与「组件头名 ≠ 类名」那个坑无关：本工具只用**路径**判归属，
> 不猜类名（`MenuBar` 住在 `menu.hpp` 这种事不影响它）。

## 用法

    python tools/pkg_graph.py --check                 # 零环断言 + 包统计
    python tools/pkg_graph.py --closure gallery       # 单应用闭包与可裁单位
    python tools/pkg_graph.py --all                   # 全部应用的裁剪收益
    python tools/pkg_graph.py --units gallery         # 列出可裁的具体单元
"""
from __future__ import annotations

import argparse
import collections
import fnmatch
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INCLUDE = REPO / "include"
SRC = REPO / "src"

# `#include "st/..."` —— 只认带引号的工程内头；尖括号是系统/第三方，不参与包图
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"(st/[^"]+)"', re.M)


def package_of_st_path(st_path: str) -> str:
    """`st/ui/components/button.hpp` → `st/ui/components`（规则见模块 docstring）。"""
    parts = st_path.split("/")
    # 去掉文件名，剩下目录层
    dirs = parts[:-1]
    return "/".join(dirs)


def package_of_unit(rel_from_repo: str, self_includes: set[str] | None = None,
                    headers_by_stem: dict[str, list[str]] | None = None) -> str:
    """实现单元 → 包名。四条判据与 C++ 的 `st::pkg::package_of_unit` 一致：

    1. **单元自己 include 了哪个同名头**（最权威，2026-11-23 新增）；
    2. 同目录同名头存在 → 那个包；
    3. 全局唯一同名头 → 它的目录；
    4. 都判不了 → 回退目录同名（`platform_*` 这类无公开头的单元）。

    模型必须与 C++ 一致：两边走歧时，本工具会给出“应该能裁”的乐观数字，
    而实际构建可能因误裁报 `undefined symbol`。
    """
    parts = rel_from_repo.split("/")
    dirs = parts[1:-1]
    fallback = "st/" + "/".join(dirs) if dirs else "st"
    stem = os.path.splitext(parts[-1])[0]
    candidates = (headers_by_stem or {}).get(stem)
    if not candidates:
        return fallback
    same_stem = "/" + stem + ".hpp"
    # ① 自己 include 的同名头
    if self_includes:
        matched = set()
        for inc in self_includes:
            if not inc.endswith(same_stem) or inc not in candidates:
                continue
            matched.add(package_of_st_path(inc))
        if len(matched) == 1:
            return matched.pop()
        if len(matched) > 1:
            return fallback
    # ② 同目录同名头
    same_dir = fallback + same_stem
    if same_dir in candidates:
        return package_of_st_path(same_dir)
    # ③ 唯一同名头
    if len(candidates) == 1:
        return package_of_st_path(candidates[0])
    return fallback


def package_of_cpp(rel_from_repo: str) -> str:
    """兼容旧调用：仅目录推断（不含同名头规则）。新代码用 `package_of_unit`。"""
    parts = rel_from_repo.split("/")
    dirs = parts[1:-1]
    return "st/" + "/".join(dirs) if dirs else "st"


def read_includes(path: Path) -> set[str]:
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return set()
    return set(INCLUDE_RE.findall(text))


def collect_headers() -> dict[str, list[Path]]:
    """包 -> 该包的头文件列表。"""
    out: dict[str, list[Path]] = collections.defaultdict(list)
    for f in INCLUDE.glob("st/**/*.hpp"):
        rel = f.relative_to(INCLUDE).as_posix()
        out[package_of_st_path(rel)].append(f)
    return out


def collect_units() -> dict[str, list[str]]:
    """包 -> 该包的实现单元（相对仓库根的 posix 路径）。

    归属用 `package_of_unit`（同名头 + self-include 优先），**不是**按目录——
    `src/geom/` 的 13 个单元里 11 个实现的是 `st/raster/*.hpp`，按目录判会集体错归，
    本工具就会给出“可裁”的乐观数字而实际裁剪会报 undefined symbol。
    """
    headers_by_stem: dict[str, list[str]] = collections.defaultdict(list)
    for f in INCLUDE.glob("st/**/*.hpp"):
        rel = f.relative_to(INCLUDE).as_posix()
        headers_by_stem[os.path.splitext(f.name)[0]].append(rel)
    out: dict[str, list[str]] = collections.defaultdict(list)
    for f in SRC.glob("**/*.cpp"):
        rel = f.relative_to(REPO).as_posix()
        pkg = package_of_unit(rel, read_includes(f), headers_by_stem)
        out[pkg].append(rel)
    return out


def build_graph(strict: bool = True) -> tuple[dict[str, set[str]], dict[str, list[Path]], dict[str, list[str]]]:
    """构造包级依赖图。

    strict=True 时把 **cpp 的 include 也算进来**（严口径）：一个包的实现可能依赖
    另一个包，而它的头文件并不暴露这层（`platform_*.cpp` 那类）。按需编译要保证
    "编了 a 就一定编了 a 运行所需的 b"，所以必须用严口径。

    strict=False 只看头文件（松口径，接近 Go 的"只导出依赖"），仅供对照。
    """
    headers = collect_headers()
    units = collect_units()
    edges: dict[str, set[str]] = collections.defaultdict(set)

    for pkg, files in headers.items():
        for f in files:
            for tgt in read_includes(f):
                them = package_of_st_path(tgt)
                if them != pkg:
                    edges[pkg].add(them)

    if strict:
        for pkg, files in units.items():
            for rel in files:
                for tgt in read_includes(REPO / rel):
                    them = package_of_st_path(tgt)
                    if them != pkg:
                        edges[pkg].add(them)

    # 保证每个包都出现在图里（即使无出边）
    for pkg in list(headers) + list(units):
        edges.setdefault(pkg, set())
    return edges, headers, units


def find_cycles(graph: dict[str, set[str]]) -> list[list[str]]:
    """返回所有环（每条环以起点闭合）。用于 `--check` 的零环断言。"""
    WHITE, GRAY, BLACK = 0, 1, 2
    color: dict[str, int] = collections.defaultdict(int)
    cycles: list[list[str]] = []

    def dfs(node: str, stack: list[str]) -> None:
        color[node] = GRAY
        stack.append(node)
        for nxt in sorted(graph.get(node, ())):
            if color[nxt] == WHITE:
                dfs(nxt, stack)
            elif color[nxt] == GRAY:
                idx = stack.index(nxt)
                cycles.append(stack[idx:] + [nxt])
        stack.pop()
        color[node] = BLACK

    sys.setrecursionlimit(10000)
    for node in sorted(graph):
        if color[node] == WHITE:
            dfs(node, [])
    return cycles


def transitive_closure(graph: dict[str, set[str]], roots: set[str]) -> set[str]:
    """从 roots 出发的可达闭包（含 roots 自身）。"""
    seen: set[str] = set()
    stack = list(roots)
    while stack:
        node = stack.pop()
        if node in seen:
            continue
        seen.add(node)
        stack.extend(graph.get(node, ()))
    return seen


def app_root_packages(app: str) -> set[str]:
    """一个应用的直接依赖包（从它自己源码的 include 起点）。"""
    roots: set[str] = set()
    base = REPO / "apps" / app
    if not base.is_dir():
        return roots
    for f in base.glob("**/*.cpp"):
        for tgt in read_includes(f):
            roots.add(package_of_st_path(tgt))
    for f in base.glob("**/*.hpp"):
        for tgt in read_includes(f):
            roots.add(package_of_st_path(tgt))
    return roots


def app_names() -> list[str]:
    base = REPO / "apps"
    if not base.is_dir():
        return []
    return sorted(d.name for d in base.iterdir() if d.is_dir() and any(d.glob("**/*.cpp")))


def strongly_connected(graph: dict[str, set[str]]) -> list[list[str]]:
    """Tarjan 强连通分量（迭代版，与 C++ 实现同口径）。

    为什么要 SCC 而不是只检环：实测存在 `st/ui` ↔ `st/ui/components`，
    收缩成一个节点后仍是 DAG，闭包与按需编译照常成立。
    """
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    on_stack: set[str] = set()
    stack: list[str] = []
    result: list[list[str]] = []
    counter = 0

    for root in sorted(graph):
        if root in index:
            continue
        frames: list[tuple[str, list[str], int]] = []

        def push(node: str) -> None:
            nonlocal counter
            index[node] = low[node] = counter
            counter += 1
            stack.append(node)
            on_stack.add(node)
            frames.append((node, sorted(graph.get(node, ())), 0))

        push(root)
        while frames:
            node, successors, position = frames[-1]
            if position < len(successors):
                frames[-1] = (node, successors, position + 1)
                nxt = successors[position]
                if nxt not in index:
                    push(nxt)
                elif nxt in on_stack:
                    low[node] = min(low[node], index[nxt])
                continue
            frames.pop()
            if frames:
                parent = frames[-1][0]
                low[parent] = min(low[parent], low[node])
            if low[node] == index[node]:
                component: list[str] = []
                while True:
                    member = stack.pop()
                    on_stack.discard(member)
                    component.append(member)
                    if member == node:
                        break
                result.append(sorted(component))
    result.sort(key=lambda c: c[0])
    return result


def report_check(strict: bool) -> int:
    graph, headers, units = build_graph(strict)
    cycles = find_cycles(graph)
    total_edges = sum(len(v) for v in graph.values())
    total_units = sum(len(v) for v in units.values())
    scope = "严口径（头 + 实现）" if strict else "松口径（只头文件）"
    comps = strongly_connected(graph)
    multi = [c for c in comps if len(c) > 1]
    print(f"包级依赖图 [{scope}]")
    print(f"  包数     : {len(graph)}")
    print(f"  依赖边   : {total_edges}")
    print(f"  框架单元 : {total_units}")
    print(f"  强连通分量: {len(comps)}（含多包的分量 {len(multi)}）")
    if cycles:
        # **不是错误**：按需编译把切强连通分量收缩成一个节点，收缩后必为 DAG。
        # Go 硬性禁循环，霜天不为此重排代码（代价远大于收益，见 CONVENTIONS §10.4 ⑤）。
        print(f"\n  环（将由强连通分量收缩处理，不影响正确性）:")
        for comp in multi:
            print("     " + " ↔ ".join(comp))
        # 真正需要红的是“收缩后仍有环”——那只可能是算法错。
        comp_of = {p: i for i, c in enumerate(comps) for p in c}
        reduced: dict[int, set[int]] = collections.defaultdict(set)
        for v, targets in graph.items():
            for w in targets:
                if comp_of[v] != comp_of[w]:
                    reduced[comp_of[v]].add(comp_of[w])
        residual = find_cycles({str(k): {str(x) for x in v} for k, v in reduced.items()})
        if residual:
            print(f"  ✗ 收缩后仍有 {len(residual)} 个环（算法异常）")
            return 1
        print("  ✓ 收缩后为 DAG")
    else:
        print("  ✓ 零环（DAG 成立）")
    return 0


def report_closure(app: str, strict: bool, list_units: bool) -> int:
    graph, headers, units = build_graph(strict)
    roots = app_root_packages(app)
    if not roots:
        print(f"应用 {app} 没有找到任何 st/ 头引用（路径对不对？）")
        return 1
    need = component_closure(graph, roots)
    all_pkgs = set(graph)
    total_units = sum(len(v) for v in units.values())
    kept = sum(len(units.get(p, ())) for p in need)
    cut_pkgs = sorted(all_pkgs - need)
    dropped = [u for p in cut_pkgs for u in units.get(p, ())]

    print(f"应用 {app}")
    print(f"  直接引用包 : {len(roots)}")
    print(f"  闭包包数   : {len(need)} / {len(all_pkgs)}")
    print(f"  需要单元   : {kept} / {total_units}  ({kept / total_units * 100:.1f}%)")
    print(f"  可裁单元   : {len(dropped)}")
    print(f"  可裁的包   : {', '.join(cut_pkgs) if cut_pkgs else '（无）'}")
    if list_units and dropped:
        print("\n  可裁单元清单：")
        for u in sorted(dropped):
            print(f"    {u}")
    return 0


def component_closure(graph: dict[str, set[str]], roots: set[str]) -> set[str]:
    """按强连通分量求闭包（与 C++ 的 `PackageGraph::closure` 同口径）。

    分量内的包同生共死：任一个可达，整个分量都算可达。
    """
    comps = strongly_connected(graph)
    comp_of = {p: i for i, c in enumerate(comps) for p in c}
    reduced: dict[int, set[int]] = collections.defaultdict(set)
    for v, targets in graph.items():
        for w in targets:
            if comp_of[v] != comp_of[w]:
                reduced[comp_of[v]].add(comp_of[w])
    reached: set[int] = set()
    stack = [comp_of[r] for r in roots if r in comp_of]
    while stack:
        node = stack.pop()
        if node in reached:
            continue
        reached.add(node)
        stack.extend(reduced.get(node, ()))
    return {p for p in graph if comp_of[p] in reached}


class _GraphAdapter(dict):
    """占位：保留类型别名位（当前未使用）。"""


def target_excludes() -> dict[str, list[str]]:
    """读 `st.pkg` 里各目标的 `exclude_sources`（清单已经排掉的单元不必再算进「可裁」）。

    为何要读它：探针不算排除时，会把 `src/pkg/*`（16 个）、`st/test`（2 个）也算成
    「按需编译可裁」——而它们早就在 `st.pkg` 里被每个应用排掉了。
    实测偏差：探针说可裁 33，实际构建只剔了 14（差额正是清单已排的那部分）。
    两个数字口径不一致会直接导致结论失真（“以为能省 9% 实际只省 3.7%”）。
    """
    manifest = REPO / "st.pkg"
    try:
        import json

        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    out: dict[str, list[str]] = {}
    for name, spec in (data.get("targets") or {}).items():
        if not isinstance(spec, dict):
            continue
        patterns = spec.get("exclude_sources") or []
        if patterns:
            out[name] = list(patterns)
    return out


def excluded_units(app: str, units: dict[str, list[str]]) -> set[str]:
    """该应用因清单 `exclude_sources` 而不参与编译的单元集合。"""
    patterns = target_excludes().get(app, [])
    if not patterns:
        return set()
    result: set[str] = set()
    for pkg_units in units.values():
        for unit in pkg_units:
            for pattern in patterns:
                if fnmatch.fnmatch(unit, pattern) or fnmatch.fnmatch(unit, pattern.replace("/*", "/**")):
                    result.add(unit)
                    break
    return result


def report_all(strict: bool, list_units: bool) -> int:
    graph, headers, units = build_graph(strict)
    total_units = sum(len(v) for v in units.values())
    print(f"{'应用':12s} {'闭包包数':>8s} {'清单已排':>8s} {'按需可裁':>8s} {'总参与':>7s}")
    print("-" * 56)
    for app in app_names():
        roots = app_root_packages(app)
        if not roots:
            continue
        need = component_closure(graph, roots)
        kept = sum(len(units.get(p, ())) for p in need)
        already = excluded_units(app, units)
        # 真正能再裁的 = 闭包外 − 清单已经排掉的
        prunable = sum(
            1
            for p in graph
            if p not in need
            for u in units.get(p, ())
            if u not in already
        )
        print(
            f"{app:12s} {len(need):8d} {len(already):8d} {prunable:8d} {kept + prunable:7d}"
        )
    print("\n（列释义：清单已排 = st.pkg 的 exclude_sources；按需可裁 = 包闭包外且清单未排的单元）")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="包级依赖图分析（按需编译的依据）")
    ap.add_argument("--check", action="store_true", help="零环断言 + 包统计")
    ap.add_argument("--closure", metavar="APP", help="单应用闭包与可裁单元")
    ap.add_argument("--all", action="store_true", help="全部应用的裁剪收益")
    ap.add_argument("--units", metavar="APP", help="列出该应用可裁的具体单元")
    ap.add_argument("--loose", action="store_true", help="松口径：只看头文件（默认严口径）")
    args = ap.parse_args()

    strict = not args.loose
    if args.check:
        return report_check(strict)
    if args.closure:
        return report_closure(args.closure, strict, list_units=False)
    if args.units:
        return report_closure(args.units, strict, list_units=True)
    if args.all:
        return report_all(strict, list_units=False)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
