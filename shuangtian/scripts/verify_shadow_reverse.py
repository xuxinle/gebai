#!/usr/bin/env python3
"""平面阴影回归测试的逆向验证：把两个真实缺陷各自放回去，确认测试变红。

## 为什么必须做（本轮的两次静默失败）

阴影实现过程中连续两次「完全不出现，且没有任何报错」：

1. **方向判反**：`light_from` 是「指向光源」，光从上方来时 `dir.y > 0`。
   写成 `dir.y >= -1e-4f → return` 就一个三角形都不收集。
2. **深度竞争**：阴影与地面共面，`shadow_z > ground_z`，`z >= depth` 恒成立，
   三角形收集了、绘制也执行了，但零像素被改。

两次的表现都是「画布零差异」，而代码逻辑看起来都对。
所以必须证明：**把缺陷放回去，测试真的会红**。

## 判定链路（规避本仓库踩过的坑）

- `st test` returncode 恒为 1 → 解析 `^\\s*FAIL\\s+<名>`；
- 单用例模式无汇总行；
- ANSI 色码 → 先去色；
- `st build` 不编测试单元 → 删 `st_tests.exe` 强重建；
- 恢复时整文件重写（保证 mtime 更新，避免陈旧 .o 造成假绿）；
- 回退点要能编译过（`-Werror`）。
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "raster", "software_scene.cpp")
TEST_NAME = "planar_shadow_darkens_the_ground"
ANSI = re.compile(r"\x1b\[[0-9;]*m")

# 缺陷 1：方向判反（要求光从下方来 → 默认布光下永不出影子）
GOOD_DIR = "if (dir.y <= 1e-4f) return;   // 光不从上往下照 → 不会在地面留影"
BAD_DIR = "if (dir.y >= -1e-4f) return;   // 光不从上往下照 → 不会在地面留影"

# 缺陷 2：阴影被实体遮罩以外的东西挡住 → 用「恒跳过」模拟"一个像素也画不出来"
GOOD_MASK = """        const std::size_t offset = static_cast<std::size_t>(y) *
                                       static_cast<std::size_t>(bake_width) +
                                   static_cast<std::size_t>(x);
        if (entity_mask_[offset] != 0) continue;   // 被实体占据 → 不画阴影"""
BAD_MASK = """        const std::size_t offset = static_cast<std::size_t>(y) *
                                       static_cast<std::size_t>(bake_width) +
                                   static_cast<std::size_t>(x);
        if (entity_mask_[offset] != 0) continue;   // 被实体占据 → 不画阴影
        if (offset < depth_.size()) continue;      // 回退：模拟"深度竞争"导致的零像素"""


def force_rebuild() -> None:
    exe = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
    if os.path.exists(exe):
        os.remove(exe)


def run_test() -> tuple[bool, str]:
    # 先让 `st test` 把测试二进制编出来（它自己会编），再直接跑单用例。
    # 直接调 `st_tests.exe` 能避开并行分片过滤（16 片并行时单用例会被分到某一片，
    # 有时因夹具没启动而「0 passed, 0 failed」——这不是失败，但会让判定不可靠）。
    force_rebuild()
    build = subprocess.run(
        [os.path.join(ROOT, "build", "bin", "st.exe"), "test", "planar_shadow_darkens_the_ground",
         "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    build_out = ANSI.sub("", (build.stdout or "") + (build.stderr or ""))
    if "构建失败" in build_out:
        return False, "BUILD_FAILED"

    exe = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
    if not os.path.exists(exe):
        return False, "NO_EXE"
    env = dict(os.environ)
    env["ST_SCENE3D_BACKEND"] = "software"
    proc = subprocess.run([exe, TEST_NAME], cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", env=env)
    out = ANSI.sub("", (proc.stdout or "") + (proc.stderr or ""))
    failed = bool(re.search(r"^\s*FAIL\s+" + re.escape(TEST_NAME), out, re.MULTILINE))
    if not failed and "  PASS " not in out:
        return False, "NO_RESULT\n" + out[:800]
    return (not failed), out


def main() -> int:
    original = open(SRC, encoding="utf-8").read()
    results: list[tuple[str, bool]] = []
    try:
        print("基线（未修改）...", end=" ", flush=True)
        ok, out = run_test()
        if not ok:
            print("红 [X]  -- 基线就不绿，先修测试")
            print(out[:1200])
            return 2
        print("绿 [OK]")

        for label, good, bad in [("方向判反", GOOD_DIR, BAD_DIR), ("阴影被深度挡住", GOOD_MASK, BAD_MASK)]:
            print(f"改坏「{label}」...", end=" ", flush=True)
            if good not in original:
                print("跳过（找不到回退点，源码已变）")
                results.append((label, False))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(good, bad, 1))
            ok, out = run_test()
            if ok:
                print("仍绿 [X] -- 测试抓不住这个缺陷")
                results.append((label, False))
            else:
                print("变红 [OK]")
                results.append((label, True))
            open(SRC, "w", encoding="utf-8").write(original)
    finally:
        open(SRC, "w", encoding="utf-8").write(original)
        force_rebuild()

    print("\n恢复后复验...", end=" ", flush=True)
    ok, _ = run_test()
    print("绿 [OK]" if ok else "红 [X]")
    results.append(("恢复后基线", ok))

    bad = [n for n, r in results if not r]
    print()
    if bad:
        print(f"[X] 有 {len(bad)} 项未拦住：{', '.join(bad)}")
        return 1
    print("全部回退都被拦住了 -- 测试是真护栏")
    return 0


if __name__ == "__main__":
    sys.exit(main())
