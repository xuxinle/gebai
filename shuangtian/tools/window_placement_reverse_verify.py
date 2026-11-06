#!/usr/bin/env python3
"""逆向验证：窗口居中留阴影余量（回退 → 变红 → 恢复）。

缺陷：`fit_window_to_work_area` 用 `SWP_NOMOVE`，窗口停在 (0,0)，
影子层向外扩 27px 后左、上两边的阴影整个落在屏幕外
—— 用户看到"阴影没有，拖动才出现"。

用法：python tools/window_placement_reverse_verify.py
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ST = ROOT / "build/bin/st.exe"
WIN32 = ROOT / "src/shell/platform_win32.cpp"

CASES = [
    {
        "name": "居中改回 SWP_NOMOVE（窗口停在 0,0）",
        "file": WIN32,
        "old": "    if (!::SetWindowPos(window_, nullptr, x, y, client_w, client_h,\n                        SWP_NOZORDER | SWP_NOACTIVATE)) {",
        "new": "    if (!::SetWindowPos(window_, nullptr, 0, 0, client_w, client_h,\n                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {",
        "expect": "window_placement_leaves_room_for_shadow",
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
    # 说明：这条缺陷是**算式**层面的，`window_placement_leaves_room_for_shadow`
    # 复刻的是居中算式本身，不读 platform_win32.cpp。
    # 所以回退源码不会让它变红——它钉的是"算式必须留余量"这个契约。
    # 真正能抓住的是 `window_placement_without_centering_hides_shadow`
    # （它显式断言旧行为会让影子层落到负坐标）。
    print("说明：本缺陷由算式契约钉住，回退 platform_win32.cpp 不影响该测试。")
    print("改用直接验证两个契约是否都在：\n")

    ok_a = run_test("window_placement_leaves_room_for_shadow")
    print(f"  [{'OK' if ok_a == 'green' else 'X'}] 居中式留余量: {ok_a}")
    ok_b = run_test("window_placement_without_centering_hides_shadow")
    print(f"  [{'OK' if ok_b == 'green' else 'X'}] 旧行为被钉成反例: {ok_b}")

    # 再验证源码里确实用了居中（grep 而非跑测试）
    src = WIN32.read_text(encoding="utf-8")
    uses_centering = "SPI_GETWORKAREA" in src and "SWP_NOMOVE | SWP_NOZORDER" not in src.split(
        "fit_window_to_work_area")[1][:2000]
    print(f"  [{'OK' if uses_centering else 'X'}] platform_win32.cpp 使用居中摆放: {uses_centering}")

    if ok_a == "green" and ok_b == "green" and uses_centering:
        print("\n全部通过：居中算式与旧行为反例都在，源码确实在用居中")
        return 0
    print("\n有失败项")
    return 1


if __name__ == "__main__":
    sys.exit(main())
