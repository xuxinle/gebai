#!/usr/bin/env python3
"""「自包含」体检：**不消费 PCH**地逐个编译头文件与翻译单元（CONVENTIONS §6.1 / §6.6 / §10.3）。

## 为什么要有这个脚本

`st build` / `st test` 都会自动 `-include st/pch.hpp`，而 PCH 里含整套常用标准库头。
于是两份契约在**日常构建里根本不被检查**：

* §6.1「每个头文件自包含（单独 include 即可编译）」
* §6.6「每个 .cpp 翻译单元自包含，不得依赖传递包含」

它们只在「还没有 PCH 的那一次编译」才炸——也就是 `bootstrap.ps1` / `bootstrap.sh`。
实测代价：`include/st/core/channel.hpp` 用了 `std::uint16_t` / `std::function` 却没包含
`<cstdint>` / `<functional>`，`st build`、`st check`、`st test` 全绿，只有首次自举报
`'uint16_t' in namespace 'std' does not name a type`——而报错指向的是**使用处**，
不是缺失处，排查方向天然被带偏（同类历史缺陷：`src/pkg/stats.cpp` 缺 `<map>`）。

CONVENTIONS §10.3 给了 POSIX 下的 shell 一行命令，但它只覆盖 `src/**`，
且依赖手写包含根（漏一个根就得到一堆假失败）。本脚本把这件事固化：

1. **头文件**逐个做探针（`include/**/*.hpp`，除 PCH 自己）——§6.1；
2. **翻译单元**逐组编译（`src/**` + `tools/stpm/**` + `tests/**` + 每个 `examples/<目标>/**`）——§6.6；
3. 包含根按**组**补齐（测试要工程根、示例要自己的 `battery/embed.hpp`）——避免假失败。

不覆盖：第三方 C 源（另一套语言标准、`-w`）、链接期问题（ODR / 符号缺失）。

## 用法

    py -3 tools/check_selfcontained.py              # 全量（头 + 翻译单元）
    py -3 tools/check_selfcontained.py --tu-only    # 跳过头文件探针（快一半）
    py -3 tools/check_selfcontained.py -j 8         # 限并发

前置：先跑过一次 `st build`（示例组要 `build/dev/embed/<目标>/include/battery/embed.hpp`，
那是构建期生成物，不在仓库里）。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
PCH = "st/pch.hpp"
ERROR_RE = re.compile(r"error:|fatal error:")


def load_manifest() -> dict:
    return json.loads((ROOT / "st.pkg").read_text(encoding="utf-8"))


def compiler() -> str:
    return os.environ.get("ST_CXX") or os.environ.get("CXX") or "g++"


def base_flags(manifest: dict) -> list[str]:
    """与 `st build` 同口径的最小标志集：语言标准 + 工程宏 + 包含根。

    刻意**不带**告警严格集（`-Werror` 那套）：本脚本问的是「编不编得过」，
    告警由 `st check` 负责——把两件事混在一起，一处新告警就会淹掉真正的自包含缺陷。
    """
    flags = ["-std=c++20", "-fsyntax-only", "-fno-strict-aliasing"]
    flags += [f"-D{d}" for d in manifest.get("defines", [])]
    for toolchain in manifest.get("toolchains", {}).values():
        flags += [f"-D{d}" for d in toolchain.get("defines", [])]
    flags += [f"-I{ROOT / d}" for d in manifest.get("include_dirs", ["include"])]
    flags.append(f"-I{ROOT}")   # 探针按「相对工程根的路径」引用待检头
    return flags


def probe_groups(manifest: dict) -> list[tuple[str, Path, list[str]]]:
    """(组名, 扫描目录, 额外包含根)——包含根按组补齐是**假失败的唯一来源**。

    每组都补 `-I<工程根>`：探针用「相对工程根的路径」引用待检头（见 `compile_one`），
    这样 `include/st/**` 与 `src/**` 下的私有头用同一种写法，不依赖各组的头根约定。
    """
    embed = ROOT / "build" / "dev" / "embed"
    groups = [
        ("头文件", ROOT / "include", [embed / "lib" / "include"]),
        ("框架单元", ROOT / "src", [embed / "lib" / "include"]),
        ("stpm", ROOT / "tools" / "stpm", [embed / "lib" / "include"]),
        # 测试按 `tests/support/xxx.hpp` 全路径引用，需要工程根在搜索路径里
        ("测试", ROOT / "tests", [embed / "lib" / "include"]),
    ]
    for example in sorted(p for p in (ROOT / "examples").iterdir() if p.is_dir()):
        inc = embed / example.name / "include"
        if not inc.is_dir():
            print(f"[warn] 示例 {example.name}: 缺 {inc.relative_to(ROOT)}"
                  f"（先跑一次 st build；本组跳过）", file=sys.stderr)
            continue
        # 示例的嵌入头**逐目标生成**：拿 lib 那份会撞 `battery/embed.hpp` 的 static_assert
        groups.append((f"示例 {example.name}", example, [inc]))
    return groups


def collect(group_dir: Path, headers_only: bool, tu_only: bool) -> list[Path]:
    if headers_only:
        found = [p for p in group_dir.rglob("*.hpp") if p.relative_to(ROOT).as_posix() != PCH]
    elif tu_only:
        found = list(group_dir.rglob("*.cpp"))
    else:
        found = list(group_dir.rglob("*.hpp")) + list(group_dir.rglob("*.cpp"))
    return sorted(p for p in found if p.is_file())


def compile_one(unit: Path, flags: list[str], tmp: Path, index: int) -> tuple[Path, int, str]:
    """把待检文件当**主文件**编一次：头文件走探针 TU（头不是 TU 本身）。

    探针落在会话外的临时目录：待检目录里多一个 `.cpp` 会被别的 glob 收进去。
    """
    if unit.suffix == ".hpp":
        probe = tmp / f"probe_{index}.cpp"
        probe.write_text(f'#include "{unit.relative_to(ROOT).as_posix()}"\n', encoding="utf-8")
        target = probe
    else:
        target = unit
    completed = subprocess.run([compiler(), *flags, str(target)],
                               capture_output=True, text=True, encoding="utf-8",
                               errors="replace", cwd=ROOT)
    return unit, completed.returncode, (completed.stdout or "") + (completed.stderr or "")


def main() -> int:
    parser = argparse.ArgumentParser(description="自包含体检（不消费 PCH）")
    parser.add_argument("-j", "--jobs", type=int, default=max(2, (os.cpu_count() or 4) * 2 // 3))
    parser.add_argument("--tu-only", action="store_true", help="跳过头文件探针")
    parser.add_argument("--headers-only", action="store_true", help="只查头文件")
    args = parser.parse_args()

    manifest = load_manifest()
    flags = base_flags(manifest)
    tmp = Path(os.environ.get("TEMP") or os.environ.get("TMPDIR") or "/tmp") / "st-selfcontained"
    tmp.mkdir(parents=True, exist_ok=True)

    print(f"编译器: {compiler()}（不消费 PCH）")
    total = 0
    failures: list[tuple[Path, str]] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for name, group_dir, extra in probe_groups(manifest):
            if not group_dir.is_dir():
                continue
            units = collect(group_dir, args.headers_only, args.tu_only)
            if not units:
                continue
            group_flags = flags + [f"-I{d}" for d in extra]
            total += len(units)
            results = pool.map(lambda item: compile_one(item[1], group_flags, tmp, item[0]),
                               enumerate(units))
            bad = [(unit, text) for unit, code, text in results if code != 0]
            print(f"[{name}] {len(units)} 个 · 失败 {len(bad)}")
            for unit, text in bad:
                lines = [ln for ln in text.splitlines() if ERROR_RE.search(ln)]
                failures.append((unit, "\n".join(lines[:3])))

    for unit, text in failures:
        print(f"\n--- {unit.relative_to(ROOT).as_posix()}\n{text}")
    print(f"\n合计 {total} 个待检文件（无 PCH 口径）· 失败 {len(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
