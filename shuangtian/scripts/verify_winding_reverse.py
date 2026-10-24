#!/usr/bin/env python3
"""绕序回归测试的逆向验证：把每个生成器的绕序改坏，确认测试变红。

## 为什么必须做这一步

一条「恒绿」的测试比没有测试更糟--它看起来像护栏，实际什么都拦不住。
本轮最大的缺陷（`Mesh::sphere` 绕序反向）正是**绕过所有现有测试**进来的：
1849 条全绿，包括专门测剔除方向那条（它只用平面，而平面的绕序恰好是对的）。

所以新加的绕序测试必须证明：**把绕序改回错误版本，它会变红**。

## 判定链路（本仓库踩过的坑，全部规避）

1. `st test` 的 **returncode 不可用**（无论结果如何都返回 1）→ 解析输出。
2. 单用例模式**没有汇总行** → 匹配 `^\\s*FAIL\\s+<名>`。
3. ANSI 色码会插在中间 → 先去色再匹配。
4. `st build` **不编测试单元** → 必须触发测试构建（删 `st_tests.exe`）。
5. 源文件恢复时不能保留旧 mtime（`copy2`/`git checkout` 都会）→ 用 write 重写。
6. 回退点要能编译过（`-Werror`）→ 不改签名、不留未用变量。
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MESH = os.path.join(ROOT, "src", "raster", "mesh.cpp")
TEST_NAME = "mesh_generators_have_consistent_winding"
ANSI = re.compile(r"\x1b\[[0-9;]*m")

# 正确的绕序（当前实现）
GOOD = "{a, a + 1U, b, a + 1U, b + 1U, b}"
# 错误的绕序（修复前的实现）--这正是被 `CullMode::Back` 暴露的那个缺陷
BAD = "{a, b, a + 1U, a + 1U, b, b + 1U}"


def force_rebuild_tests() -> None:
    """删掉测试二进制，逼它重建（`st build` 不编测试单元）。"""
    exe = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
    if os.path.exists(exe):
        os.remove(exe)


def run_test() -> tuple[bool, str]:
    force_rebuild_tests()
    proc = subprocess.run(
        [os.path.join(ROOT, "build", "bin", "st.exe"), "test", TEST_NAME, "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    out = ANSI.sub("", (proc.stdout or "") + (proc.stderr or ""))
    failed = bool(re.search(r"^\s*FAIL\s+" + re.escape(TEST_NAME), out, re.MULTILINE))
    if "构建失败" in out:
        return False, "BUILD_FAILED"
    return (not failed), out


def patch(old: str, new: str) -> None:
    src = open(MESH, encoding="utf-8").read()
    if old not in src:
        raise SystemExit(f"找不到要替换的片段：{old[:60]}")
    open(MESH, "w", encoding="utf-8").write(src.replace(old, new, 1))


def main() -> int:
    original = open(MESH, encoding="utf-8").read()
    results: list[tuple[str, bool]] = []
    try:
        print("基线（未修改）...", end=" ", flush=True)
        ok, out = run_test()
        if not ok:
            print("红 [X]  -- 基线就不绿，先修测试")
            print(out[:1500])
            return 2
        print("绿 [OK]")

        print("改坏「球体绕序反向」...", end=" ", flush=True)
        patch(GOOD, BAD)
        ok, out = run_test()
        if ok:
            print("仍绿 [X] -- 测试抓不住这个缺陷")
            results.append(("球体绕序反向", False))
        else:
            print("变红 [OK]")
            results.append(("球体绕序反向", True))

    finally:
        # 恢复：整文件重写（保证 mtime 更新，避免「假绿」）
        open(MESH, "w", encoding="utf-8").write(original)
        force_rebuild_tests()

    print("\n恢复后复验...", end=" ", flush=True)
    ok, out = run_test()
    print("绿 [OK]" if ok else "红 [X]")
    results.append(("恢复后基线", ok))

    print()
    bad = [n for n, r in results if not r]
    if bad:
        print(f"[X] 有 {len(bad)} 项未拦住：{', '.join(bad)}")
        return 1
    print("全部回退都被拦住了--测试是真护栏")
    return 0


if __name__ == "__main__":
    sys.exit(main())
