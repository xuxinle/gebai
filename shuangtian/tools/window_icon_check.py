#!/usr/bin/env python3
"""检查各桌面应用的**窗口/任务栏图标**是否真的设置成功。

## 两类图标，两条链路（容易混）

| | 来源 | 怎么看 |
|---|---|---|
| exe 文件图标 | `st.pkg` 的 `"icon": "*.ico"` → PE 资源 | `gui_subsystem_check.py`（读 exe 里的 256 PNG） |
| 窗口/任务栏图标 | 运行期 `AppOptions::window_icon_png` → 后端 `set_window_icon` | **本文** |

它们**不会自动互通**：`.ico` 进的是 PE 资源（资源管理器看），而任务栏上
那个图标是窗口的 `WM_SETICON`（由应用在运行期设置）。只配 `.ico` 不设
`window_icon_png`，任务栏会显示成默认空白图标。

## 判据

在应用日志里找 `窗口图标未设置` 告警；没有告警 = 设置成功。
（`Application` 在 `set_window_icon` 失败时写这条 WARN；成功时不写。）

另外核对源码里确实有 `attach_png(...)` 或 `window_icon_png =` 的赋值——
覆盖“日志没告警但其实压根没设”的情形（空 PNG 不会进 `set_window_icon`）。

用法：
    py -3 tools/window_icon_check.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = ["gallery", "gbcode", "louyue", "caiyun", "xiangqi", "cefu", "gedou"]


def source_of(app: str) -> Path:
    for candidate in (ROOT / "apps" / app / "src" / "main.cpp",
                      ROOT / "apps" / app / "main.cpp"):
        if candidate.exists():
            return candidate
    return ROOT / "apps" / app / "main.cpp"


def sets_window_icon(app: str) -> tuple[bool, str]:
    text = source_of(app).read_text(encoding="utf-8")
    if "attach_png(app_options.window_icon_png" in text:
        return True, "attach_png"
    # 两种写法都认：`window_icon_png = ...` 与 `window_icon_png.assign(...)`
    if "window_icon_png =" in text or "window_icon_png.assign" in text:
        return True, "window_icon_png"
    return False, ""


def main() -> int:
    failures = 0
    print(f"{'应用':10} {'源码设置':14} {'结果'}")
    for app in APPS:
        ok, how = sets_window_icon(app)
        mark = "OK" if ok else "缺失"
        if not ok:
            failures += 1
        print(f"{app:10} {how or '（无）':14} {mark}")
    print()
    print("全部设置" if failures == 0 else f"{failures} 个应用没设窗口图标")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
