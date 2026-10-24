#!/usr/bin/env python3
"""场景描述层的逆向验证：把两个真实缺陷各自放回去，确认测试变红。

## 缺陷 1：渲染顺序（先设阴影再画地面）

`render_scene` 原本先 `set_planar_shadow` 再画所有对象。地面因此被当成
「实体」打上遮罩，阴影一个像素都画不出来——而图看着**完全正常**，
只是没有影子（实测：开关阴影，画面均值 115.40 → 115.40）。

## 缺陷 2：`ground` 字段被忽略

即使顺序对了，若解析时不读 `ground`，两遍循环都不知道谁是地面，
阴影同样出不来。

两条都只能靠 `scene_script_shadow_takes_effect` 抓到——
「能渲出图」这条判据（`renders_something_substantial`）在两种情况下都是绿的。
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "raster", "scene3d_script.cpp")
TEST = "scene_script_shadow_takes_effect"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def force_rebuild() -> None:
    exe = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
    if os.path.exists(exe):
        os.remove(exe)


def run_test() -> tuple[bool, str]:
    force_rebuild()
    subprocess.run(
        [os.path.join(ROOT, "build", "bin", "st.exe"), "test", TEST, "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    exe = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
    if not os.path.exists(exe):
        return False, "NO_EXE"
    env = dict(os.environ)
    env["ST_SCENE3D_BACKEND"] = "software"
    proc = subprocess.run([exe, TEST], cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", env=env)
    out = ANSI.sub("", (proc.stdout or "") + (proc.stderr or ""))
    failed = bool(re.search(r"^\s*FAIL\s+" + re.escape(TEST), out, re.MULTILINE))
    if not failed and "  PASS " not in out:
        return False, "NO_RESULT\n" + out[:600]
    return (not failed), out


# 缺陷 1：把 `set_planar_shadow` 挪回「画所有对象之前」
GOOD_ORDER = """  for (const SceneObjectSpec& obj : spec.objects) {
    if (!obj.ground) continue;
    const auto status = draw_one(obj);
    if (!status) return st::forward_error(status.error());
  }
  (*scene)->set_planar_shadow(spec.shadow);"""
BAD_ORDER = """  (*scene)->set_planar_shadow(spec.shadow);
  for (const SceneObjectSpec& obj : spec.objects) {
    if (!obj.ground) continue;
    const auto status = draw_one(obj);
    if (!status) return st::forward_error(status.error());
  }"""

# 缺陷 2：解析时不读 `ground`
GOOD_GROUND = 'obj.ground = json_get_bool(node, "ground", false);'
BAD_GROUND = 'obj.ground = false;  // 回退：忽略 ground 字段'


def main() -> int:
    original = open(SRC, encoding="utf-8").read()
    results: list[tuple[str, bool]] = []
    try:
        print("基线（未修改）...", end=" ", flush=True)
        ok, out = run_test()
        if not ok:
            print("红 [X]  -- 基线就不绿，先修测试")
            print(out[:1000])
            return 2
        print("绿 [OK]")

        for label, good, bad in [("渲染顺序（先设阴影再画地面）", GOOD_ORDER, BAD_ORDER),
                                 ("ground 字段被忽略", GOOD_GROUND, BAD_GROUND)]:
            print(f"改坏「{label}」...", end=" ", flush=True)
            if good not in original:
                print("跳过（找不到回退点）")
                results.append((label, False))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(good, bad, 1))
            ok, _ = run_test()
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
