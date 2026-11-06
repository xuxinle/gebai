#!/usr/bin/env python3
"""逆向验证：窗口圆角抗锯齿判据（回退 → 变红 → 恢复）。

本轮的缺陷是**覆盖率公式口径错**：
    coverage = clamp(1.5 - d, 0, 1)   ← 错：d=0 时给 1.0，弧上只有一档
    coverage = clamp(0.5 - d, 0, 1)   ← 对：d=0 时给 0.5，标准 1px 抗锯齿

对齐目标是 DWM 的实测过渡值（34/80/127/174/221，5 档）。

用法：python tools/window_corner_aa_reverse_verify.py
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
        "name": "覆盖率公式回退成 1.5-d（弧上只有一档）",
        "file": CORE,
        # 单行锚点：只改公式那一行本身（多行 old 会因行尾差异匹配不上）。
        "old": "      const float coverage = std::clamp(0.5f - d, 0.0f, 1.0f);",
        "new": "      const float coverage = std::clamp(1.5f - d, 0.0f, 1.0f);   // 回退：口径错",
        "expect": "window_chrome_corner_ramp_matches_dwm_grade",
    },
    {
        "name": "整条 AA 带关闭",
        "file": CORE,
        "old": "    if (antialias_corner && d > -0.5f && d < 0.5f) {",
        "new": "    if (false && antialias_corner && d > -0.5f && d < 0.5f) {",
        "expect": "window_chrome_ring_can_disable_corner_aa",
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
        original = path.read_bytes().decode("utf-8")

        base = run_test(case["expect"])
        print(f"  基线 {case['expect']}: {base}")
        if base != "green":
            print("  基线不绿，先修好再跑逆向验证")
            return 1

        if case["old"] not in original:
            print(f"[?] 找不到回退点：{case['name']}")
            failures.append(case["name"])
            continue
        patched = original.replace(case["old"], case["new"], 1)
        path.write_bytes(patched.encode("utf-8"))
        try:
            why = run_test(case["expect"])
            ok = why == "red"
            print(f"[{'OK' if ok else 'X'}] 回退「{case['name']}」→ "
                  f"{case['expect']} {'变红' if ok else f'**没变红（{why}）**'}")
            if not ok:
                failures.append(case["name"])
        finally:
            path.write_bytes(original.encode("utf-8"))

    final = run_test("window_chrome")
    print(f"[{'OK' if final == 'green' else 'X'}] 恢复后全绿（{final}）")
    if final != "green":
        failures.append("恢复后全绿")
    print()
    if failures:
        print(f"失败 {len(failures)} 项：{failures}")
        return 1
    print("全部通过：圆角过渡档数与 AA 开关都被判据钉住")
    return 0


if __name__ == "__main__":
    sys.exit(main())
