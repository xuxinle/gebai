#!/usr/bin/env python3
"""逆向验证：圆角四条判据（回退 → 变红 → 恢复）。

| # | 回退什么 | 应当变红 |
|---|---|---|
| 1 | `add_rounded_rect` 的 kappa 改错（弧被画扁） | `rounded_corner_follows_a_true_arc` |
| 2 | 一位式渲染（覆盖率量化到 0/255 两值） | `rounded_corner_coverage_is_continuous` |
| 3 | 半径不参与（弧不随半径变） | `rounded_corner_levels_grow_with_radius` |

用法：python tools/corner_quality_reverse_verify.py
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ST = ROOT / "build/bin/st.exe"
PATH_CPP = ROOT / "src/math/path.cpp"
RASTER = ROOT / "src/raster/rasterizer.cpp"

CASES = [
    {
        "name": "左上角弧被画成 2 倍半径（形状失真）",
        "file": PATH_CPP,
        # 半径按 2 倍用：弧被画得比真圆更"鼓"，逐行边界必然偏离真圆弧 > 1px。
        # 用 `* 2.0f` 而不是 `= 0`：后者会让 `tl` 恒为零、触发 -Wunused 类告警把编译打断。
        "old": "  const float tl = clamp_radius(top_left);",
        "new": "  const float tl = clamp_radius(top_left) * 2.0f;   // 回退：弧被画鼓",
        "expect": "rounded_corner_follows_a_true_arc",
    },
    {
        "name": "覆盖率量化成两值（一位式）",
        "file": RASTER,
        "old": "    run.weight = sign * kSubsampleWeight;",
        "new": "    run.weight = sign * (kSubsampleWeight > 0.5f ? 1.0f : 0.0f);   // 回退：一位式",
        "expect": "rounded_corner_coverage_is_continuous",
    },
    {
        "name": "半径不参与弧长",
        "file": PATH_CPP,
        # 控制点缩到 1/10：弧退化成"几乎直角"，弧长变短 => 级数随之减少。
        "old": "  const float ktl = tl * kCircleKappa;",
        "new": "  const float ktl = tl * 0.05f;   // 回退：控制点极短 => 弧退化成直角",
        "expect": "rounded_corner_levels_grow_with_radius",
    },
]


def run_test(name: str) -> str:
    result = subprocess.run([str(ST), "test", name, "--profile", "debug"],
                            cwd=str(ROOT), capture_output=True)
    out = ((result.stdout or b"") + (result.stderr or b"")).decode("utf-8", "replace")
    if "编译失败" in out or "构建失败" in out:
        return "build-failed"
    for m in re.finditer(r"(\d+)\s+passed,\s+(\d+)\s+failed", out):
        if int(m.group(2)) > 0:
            return "red"
    if "passed," not in out:
        return "crashed"
    return "green"


def main() -> int:
    failures: list[str] = []
    for case in CASES:
        path: pathlib.Path = case["file"]
        original = path.read_text(encoding="utf-8")
        crlf = "\r\n" in original
        norm = original.replace("\r\n", "\n")

        base = run_test(case["expect"])
        print(f"  基线 {case['expect']}: {base}")
        if base != "green":
            print("  基线不绿，先修好再跑逆向验证")
            return 1

        if case["old"] not in norm:
            print(f"[?] 找不到回退点：{case['name']}")
            failures.append(case["name"])
            continue
        patched = norm.replace(case["old"], case["new"], 1)
        path.write_text(patched.replace("\n", "\r\n") if crlf else patched, encoding="utf-8")
        try:
            why = run_test(case["expect"])
            ok = why == "red"
            print(f"[{'OK' if ok else 'X'}] 回退「{case['name']}」→ "
                  f"{case['expect']} {'变红' if ok else f'**没变红（{why}）**'}")
            if not ok:
                failures.append(case["name"])
        finally:
            path.write_text(original, encoding="utf-8")

    final = run_test("rounded_corner")
    print(f"[{'OK' if final == 'green' else 'X'}] 恢复后全绿（{final}）")
    if final != "green":
        failures.append("恢复后全绿")
    print()
    if failures:
        print(f"失败 {len(failures)} 项：{failures}")
        return 1
    print("全部通过：圆角几何精度、四角对称、覆盖率连续、级数随半径增长都被钉住")
    return 0


if __name__ == "__main__":
    sys.exit(main())
