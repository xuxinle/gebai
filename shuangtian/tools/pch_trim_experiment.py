#!/usr/bin/env python3
"""PCH 精简实验：完整 PCH vs 精简 PCH，在**真实 TU** 上的耗时对照。

背景（2026-11 实测）：
  空 TU + 完整 PCH（107.8 MB，40 个标准库头）   = 1,976 ms
  空 TU + 精简 PCH（52.9 MB，11 个常用标准库头）=   390 ms
  ⇒ 每个 TU 多付 ~1.6 s 固定底噪。312 个 TU / 16 路并行 ≈ 31 s，而全量墙钟仅 36 s。

关键问题：精简 PCH 后，那些**真用到** <format>/<filesystem>/<regex> 的 TU
会不会因为要自己解析而变慢？本脚本用真实 TU 对照回答它。
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

REPO = Path(r"D:\code\gebai-pro\shuangtian")

COMMON = [
    "-std=c++23", "-fsyntax-only", "-O1",
    "-I", str(REPO / "include"),
    "-I", str(REPO / "third_party"),
    "-I", str(REPO / "third_party/sqlite"),
    "-I", str(REPO / "third_party/quickjs"),
    "-I", str(REPO / "third_party/webview2"),
    "-I", str(REPO / "third_party/micropython"),
    "-I", str(REPO / "build/dev/embed/lib/include"),
    "-I", str(REPO),
    '-DST_VERSION="0.1.0"', "-DST_ENABLE_LINT=1",
]

PCH_FULL = ["-I", str(REPO / "build/quick/pch"), "-include", "prefix.hpp"]
PCH_SMALL = ["-I", str(REPO / "tmp/pchmin"), "-include", "prefix.hpp"]
# 中度精简：从 40 头移到 33 头（只剔掉 <regex>/<flat_map>/<mdspan>/<stdexcept>/<numbers>/<variant>）
PCH_MID = ["-I", str(REPO / "tmp/pchmid"), "-include", "prefix.hpp"]

TU = [
    # 名字, 文件, 是否重标准库
    ("core/string.cpp（string 大户）", "src/core/string.cpp"),
    ("raster/canvas.cpp（几何+光栅）", "src/raster/canvas.cpp"),
    ("ui/element.cpp（2,400 行，UI 基类）", "src/ui/element.cpp"),
    ("ui/dsl.cpp（1,984 行，模板密集）", "src/ui/dsl.cpp"),
    ("pkg/build.cpp（4,668 行，构建驱动）", "src/pkg/build.cpp"),
    ("core/time.cpp（chrono 使用者）", "src/core/time.cpp"),
    ("core/fs.cpp（filesystem 大户）", "src/core/fs.cpp"),
    ("text/font.cpp（2,568 行）", "src/text/font.cpp"),
    ("raster/platform_d3d11.cpp（6,610 行）", "src/raster/platform_d3d11.cpp"),
    ("control/server.cpp（2,225 行）", "src/control/server.cpp"),
]


def build_pch(header_list: list[str], directory: Path) -> Path | None:
    """用**与消费端完全相同的标志**生成 PCH。

    ⚠ 这是本实验最容易错的地方（实测踩过）：GCC 对 PCH 的匹配是严格的——
    创建与消费的标志集不一致就**静默丢弃**（不报错，只是变慢）。
    第一版实验用手写的 `-I` 集生成 PCH，消费时 `COMMON` 有 8 个 -I——
    于是 PCH 被忽略，结果读出“中度精简反而慢 7.3%”，完全是一个假结论。
    """
    directory.mkdir(parents=True, exist_ok=True)
    wrapper = directory / "prefix.hpp"
    wrapper.write_text("\n".join(f"#include <{h}>" for h in header_list) + "\n", encoding="utf-8")
    gch = wrapper.with_suffix(".hpp.gch")
    # 创建标志 = 消费标志去掉 -fsyntax-only（创建 PCH 不能带它）
    creating = [f for f in COMMON if f != "-fsyntax-only"]
    proc = subprocess.run(
        ["g++", *creating, "-x", "c++-header", str(wrapper), "-o", str(gch)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print(f"PCH 生成失败：{proc.stderr.decode('utf-8', 'ignore')[:300]}")
        return None
    return directory


def run(flags: list[str], source: Path) -> tuple[int, bool]:
    best = 10**9
    ok = True
    for _ in range(2):
        start = time.perf_counter()
        proc = subprocess.run(["g++", *COMMON, *flags, str(source)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        ms = int((time.perf_counter() - start) * 1000)
        ok = ok and proc.returncode == 0
        best = min(best, ms)
    return best, ok


def main() -> int:
    full_headers = [
        "algorithm", "array", "bit", "cassert", "charconv", "cmath", "concepts", "cstddef",
        "cstdint", "cstdio", "cstdlib", "cstring", "exception", "expected", "filesystem",
        "flat_map", "format", "functional", "initializer_list", "limits", "map", "mdspan",
        "memory", "numbers", "optional", "queue", "ranges", "span", "stdexcept", "string_view",
        "string", "tuple", "type_traits", "unordered_map", "unordered_set", "utility",
        "variant", "vector",
    ]
    removed = {"regex", "flat_map", "mdspan", "stdexcept", "numbers", "variant"}
    mid_headers = [h for h in full_headers if h not in removed]

    root = REPO / "tmp" / "pch_exp"
    dir_full = build_pch(full_headers, root / "full")
    dir_mid = build_pch(mid_headers, root / "mid")
    if dir_full is None or dir_mid is None:
        return 1
    for label, d in (("完整", dir_full), ("中度", dir_mid)):
        size = (d / "prefix.hpp.gch").stat().st_size / 1024 / 1024
        print(f"{label} PCH：{len(full_headers if label == '完整' else mid_headers)} 头，{size:.1f} MB")

    flags_full = ["-I", str(dir_full), "-include", "prefix.hpp"]
    flags_mid = ["-I", str(dir_mid), "-include", "prefix.hpp"]

    # 先验空 TU：确认两个 PCH 都真的被采纳（否则后面全是假结论）
    probe = root / "_empty.cpp"
    probe.write_text("int x;\n", encoding="utf-8")
    e_full, _ = run(flags_full, probe)
    e_mid, _ = run(flags_mid, probe)
    print(f"空 TU 基线：完整 {e_full} ms / 中度 {e_mid} ms\n")

    print(f"{'TU':<38} {'完整PCH':>9} {'中度PCH':>9} {'差':>9}")
    print("-" * 70)
    tot_full = tot_mid = 0
    for name, rel in TU:
        path = REPO / rel
        if not path.exists():
            print(f"{name:<38} (缺失)")
            continue
        full, ok1 = run(flags_full, path)
        mid, ok2 = run(flags_mid, path)
        if not (ok1 and ok2):
            print(f"{name:<38} {full:>8} {mid:>8}   ⚠ 编译失败")
            continue
        tot_full += full
        tot_mid += mid
        print(f"{name:<38} {full:>8}ms {mid:>8}ms {mid - full:>+8}ms")
    print("-" * 70)
    print(f"{'合计':<38} {tot_full:>8}ms {tot_mid:>8}ms {tot_mid - tot_full:>+8}ms")
    if tot_full:
        print(f"\n变化：{(tot_mid - tot_full) / tot_full * 100:+.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
