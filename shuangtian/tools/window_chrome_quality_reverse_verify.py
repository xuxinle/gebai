#!/usr/bin/env python3
"""逆向验证：圆角 AA 与阴影强度两条新判据（回退→变红→恢复）。

两个回退点各自对应本轮的一个修复：

| # | 回退什么 | 应当变红 |
|---|---|---|
| 1 | `build_ring_pixels` 里去掉“圆角内侧 AA 带”（恢复成 `if (d <= 0) return;`） | `window_chrome_corner_edge_has_antialiasing_ramp` |
| 2 | `shadow_alpha` 改回 0.30（旧参数） | `window_chrome_shadow_is_visible_at_the_edge` |

用法：python tools/window_chrome_quality_reverse_verify.py
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ST = ROOT / "build/bin/st.exe"
CORE = ROOT / "src/shell/window_chrome.cpp"
STYLE = ROOT / "include/st/shell/window_chrome.hpp"

CASES = [
    {
        "name": "去掉弧外侧 AA 带",
        "file": CORE,
        # 回退成"一位式"：AA 带整段不画（只留阴影）。用 `false &&` 保证仍能编译。
        "old": "    if (d > 0.0f && d < 1.5f) {",
        "new": "    if (false && d > 0.0f && d < 1.5f) {",
        "expect": "window_chrome_corner_edge_has_antialiasing_ramp",
    },
    {
        "name": "shadow_alpha 改回 0.30",
        "file": STYLE,
        "old": "  float shadow_alpha{0.42f};",
        "new": "  float shadow_alpha{0.30f};",
        "expect": "window_chrome_shadow_is_visible_at_the_edge",
    },
]


def run_test(name: str) -> tuple[bool, str]:
    result = subprocess.run([str(ST), "test", name, "--profile", "debug"],
                            cwd=str(ROOT), capture_output=True)
    out = ((result.stdout or b"") + (result.stderr or b"")).decode("utf-8", "replace")
    if "编译失败" in out or "构建失败" in out:
        return False, "build-failed"
    for m in re.finditer(r"(\d+)\s+passed,\s+(\d+)\s+failed", out):
        if int(m.group(2)) > 0:
            return False, "red"
    if "passed," not in out:
        return False, "crashed"
    return result.returncode == 0, "green"


def main() -> int:
    failures: list[str] = []
    for case in CASES:
        path: pathlib.Path = case["file"]
        original = path.read_text(encoding="utf-8")
        crlf = "\r\n" in original
        norm = original.replace("\r\n", "\n")

        ok, why = run_test(case["expect"])
        print(f"  基线 {case['expect']}: {why}")
        if ok is False and why != "green":
            print("  基线不绿，先修好")
            return 1

        if case["old"] not in norm:
            print(f"[?] 找不到回退点：{case['name']}")
            failures.append(case["name"])
            continue
        patched = norm.replace(case["old"], case["new"], 1)
        path.write_text(patched.replace("\n", "\r\n") if crlf else patched, encoding="utf-8")
        try:
            red_ok, red_why = run_test(case["expect"])
            turned_red = red_why in ("red", "crashed")
            print(f"[{'OK' if turned_red else 'X'}] 回退「{case['name']}」→ "
                  f"{case['expect']} {'变红' if turned_red else f'**没变红（{red_why}）**'}")
            if not turned_red:
                failures.append(case["name"])
        finally:
            path.write_text(original, encoding="utf-8")

    ok, why = run_test("window_chrome")
    print(f"[{'OK' if ok else 'X'}] 恢复后全绿（{why}）")
    if not ok:
        failures.append("恢复后全绿")
    print()
    if failures:
        print(f"失败 {len(failures)} 项：{failures}")
        return 1
    print("全部通过：圆角 AA 与阴影强度都被判据钉住")
    return 0


if __name__ == "__main__":
    sys.exit(main())
