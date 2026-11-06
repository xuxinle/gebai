#!/usr/bin/env python3
"""逆向验证：黑点与阴影包络（回退 → 变红 → 恢复）。

本轮的缺陷有两个，都在 `build_ring_pixels` 的 `write_pixel` 里：

1. **黑点**：阴影公式 `t = clamp((d-1)/fade, 0, 1)` 在 `d < 1` 时取满强度，
   于是窗口边缘紧邻的一圈是全图最暗 —— 圆弧离开窗口本体处露出孤立黑点。
   修法：改成升-降包络（贴窗口为 0，向外先增后减）。

2. **阴影与圆角二选一**：AA 带（`d ∈ (-0.5,0.5)`）之外直接画纯阴影，
   两者没有按覆盖率合成。修法：窗口色 × cov + 阴影 × (1-cov)。

用法：python tools/window_corner_darkdot_reverse_verify.py
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ST = ROOT / "build/bin/st.exe"
CORE = ROOT / "src/shell/window_chrome.cpp"

CASES = [
    {
        "name": "阴影回退成「贴窗口满强度」（黑点复现）",
        "file": CORE,
        "old": "    const float rise = std::clamp(d / (fade * 0.5f), 0.0f, 1.0f);\n    const float fall = std::clamp(1.0f - (d - fade * 0.5f) / fade, 0.0f, 1.0f);\n    const float falloff = rise * fall * fall;",
        "new": "    const float t_old = std::clamp((d - 1.0f) / fade, 0.0f, 1.0f);\n    const float falloff = (1.0f - t_old) * (1.0f - t_old);",
        "expect": "window_chrome_arc_has_no_isolated_dark_dots",
    },
    {
        "name": "阴影回退后剖面变成「贴窗口最浓」",
        "file": CORE,
        "old": "    const float rise = std::clamp(d / (fade * 0.5f), 0.0f, 1.0f);\n    const float fall = std::clamp(1.0f - (d - fade * 0.5f) / fade, 0.0f, 1.0f);\n    const float falloff = rise * fall * fall;",
        "new": "    const float t_old = std::clamp((d - 1.0f) / fade, 0.0f, 1.0f);\n    const float falloff = (1.0f - t_old) * (1.0f - t_old);",
        "expect": "window_chrome_ring_shadow_profile_is_soft",
    },
]


def run_test(name: str) -> str:
    result = subprocess.run([str(ST), "test", name, "--profile", "debug"],
                            cwd=str(ROOT), capture_output=True)
    out = ((result.stdout or b"") + (result.stderr or b"")).decode("utf-8", "replace")
    if "编译失败" in out or "构建失败" in out:
        return "build-failed"
    for m in re.finditer(r"(\d+)\s+passed,\s*(\d+)\s+failed", out):
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

    final = run_test("window_chrome")
    print(f"[{'OK' if final == 'green' else 'X'}] 恢复后全绿（{final}）")
    if final != "green":
        failures.append("恢复后全绿")
    print()
    if failures:
        print(f"失败 {len(failures)} 项：{failures}")
        return 1
    print("全部通过：黑点与阴影包络都被判据钉住")
    return 0


if __name__ == "__main__":
    sys.exit(main())
