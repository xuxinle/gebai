#!/usr/bin/env python3
"""按需链接的**产物级判据**：不按名造组件的应用，产物里不该有重型组件。

为什么必须是产物级
------------------
这是**链接期**属性。单测进程（`st_tests`）直传所有用例源、必然链接全部组件，
所以在单测里量不到它 —— 实测：把修复回退之后单测依然全绿，那是一条
"恒绿的测试"（`CONVENTIONS.md` §7 的经典形态）。

判据有两条，缺一条都会被"改坏了功能"骗过：

===========  ==========================================  =======================
应用         形态                                        期望
===========  ==========================================  =======================
``gedou``    直 ``new`` 组件，无 XML / 无脚本 / 不按名建  八个重型组件符号全为 0
``xiangqi``  XML 界面（按名字造组件）                     重型组件**必须存在**
===========  ==========================================  =======================

第二条是第一条的必要配对：只测"没进来"会奖励一个把功能改坏的实现 —— 实测
把导入挪走的那一版 ``gedou`` 体积降了，而 xiangqi 装树失败
（``认不出的标签 <Scroll>``）。

用法
----
    python tools/check_component_pruning.py [--bin-dir build/release/bin]
    python tools/check_component_pruning.py --skip-build   # 只跑判据

退出码 0 = 两条都成立；1 = 有断言红（会逐条打印）。
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

# Windows 控制台默认 GBK：判据输出保持 ASCII，避免 UnicodeEncodeError。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

#: 重型组件：它们的存在价值就是"只有少数应用用到"，而不是全体都背。
#: 实测（2026-11-21）`gedou` 一个都不用，却因为 `dsl.cpp` 无条件全量导入白背 656 KB。
HEAVY = [
    "CodeEditor",
    "Terminal",
    "MarkdownView",
    "DiffView",
    "HtmlView",
    "Table",
    "Tree",
    "FileDialog",
]

#: 不该有重型组件的应用（它们一个都不按名字建）。
PRUNED = ["gedou"]

#: 必须有重型组件的应用（XML / 脚本 / 协议按名字建 —— 省了就是功能坏了）。
KEPT = ["xiangqi"]

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_nm() -> str | None:
    """定位 `nm`（Windows 上 mingw 的那份，带 `.exe`）。"""
    for candidate in ("nm", "nm.exe"):
        found = shutil.which(candidate)
        if found:
            return found
    # 本仓开发机常见位置（与构建链一致）
    for base in (r"C:\Users\Administrator\program\mingw64\bin",):
        path = os.path.join(base, "nm.exe")
        if os.path.exists(path):
            return path
    return None


def defined_symbols(nm: str, debug_binary: str) -> str:
    """取产物 debug 符号表里的**已定义**符号文本（一次 nm，供多次查找）。"""
    completed = subprocess.run(
        [nm, "-C", "--defined-only", debug_binary],
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    return completed.stdout


def check_app(nm: str, bin_dir: str, app: str, expect_present: bool) -> bool:
    debug_binary = os.path.join(bin_dir, app + ".exe.debug")
    if not os.path.exists(debug_binary):
        print(f"  ! missing symbols:：{debug_binary}（先构建 {app}）")
        return False
    symbols = defined_symbols(nm, debug_binary)
    size_mb = os.path.getsize(os.path.join(bin_dir, app + ".exe")) / (1024 * 1024)
    print(f"{app}  {size_mb:.2f} MB  期望重型组件 {'存在' if expect_present else '缺席'}")
    ok = True
    for name in HEAVY:
        present = f"st::ui::{name}" in symbols
        good = present == expect_present
        mark = "OK " if good else "BAD"
        print(f"  {mark} {name:14s} present={int(present)}")
        ok = ok and good
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bin-dir",
        default=os.path.join(REPO_ROOT, "build", "release", "bin"),
        help="产物目录（默认 build/release/bin）",
    )
    args = parser.parse_args()

    nm = find_nm()
    if nm is None:
        print("找不到 nm（需要 mingw 的 binutils）", file=sys.stderr)
        return 2

    ok = True
    for app in PRUNED:
        ok = check_app(nm, args.bin_dir, app, expect_present=False) and ok
    for app in KEPT:
        ok = check_app(nm, args.bin_dir, app, expect_present=True) and ok

    print("RESULT=" + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
