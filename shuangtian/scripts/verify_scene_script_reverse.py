#!/usr/bin/env python3
"""场景描述层的逆向验证：把三个真实缺陷各自放回去，确认测试变红。

## 缺陷清单（全部是「代码看着对、画面就是不对」）

1. **渲染顺序**：`render_scene` 原本先 `set_planar_shadow` 再画所有对象，
   地面因此被当成「实体」打上遮罩，阴影一个像素都画不出来
   （实测：开关阴影，画面均值 115.40 → 115.40）。图看着完全正常，只是没影子。

2. **`ground` 字段被忽略**：即使顺序对了，不读这个字段也不知道谁是地面，
   阴影同样出不来。

3. **`render_scene_into` 不复用实例**：如果它内部偷偷 `Scene3D::create`（旧行为），
   宿主（裁云）在环境变量上的保存/恢复就会被破坏——实测导出卡在 33/180 帧、
   控制通道完全超时。

这两条只能靠 `scene_script_shadow_takes_effect` 抓到；
「是否复用实例」由测试自己用**标记法**判定（见测试文件），无需回退源码。
「能渲出图」这条判据（`renders_something_substantial`）在三种情况下都是绿的。
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "raster", "scene3d_script.cpp")
ANSI = re.compile(r"\x1b\[[0-9;]*m")

TEST_SHADOW = "scene_script_shadow_takes_effect"
TEST_INTO = "scene_script_render_into_reuses_the_given_scene"


def force_rebuild() -> None:
    exe = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
    if os.path.exists(exe):
        os.remove(exe)


def run_test(name: str) -> tuple[bool, str]:
    """返回 (是否绿, 输出)。"""
    force_rebuild()
    subprocess.run(
        [os.path.join(ROOT, "build", "bin", "st.exe"), "test", name, "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    exe = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
    if not os.path.exists(exe):
        return False, "NO_EXE"
    env = dict(os.environ)
    env["ST_SCENE3D_BACKEND"] = "software"
    proc = subprocess.run([exe, name], cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", env=env)
    out = ANSI.sub("", (proc.stdout or "") + (proc.stderr or ""))
    failed = bool(re.search(r"^\s*FAIL\s+" + re.escape(name), out, re.MULTILINE))
    if not failed and "  PASS " not in out:
        return False, "NO_RESULT\n" + out[:600]
    return (not failed), out


# ── 缺陷 1：把 `set_planar_shadow` 挪回「画所有对象之前」 ──
GOOD_ORDER = """  for (const SceneObjectSpec& obj : spec.objects) {
    if (!obj.ground) continue;
    const auto status = draw_one(obj);
    if (!status) return st::forward_error(status.error());
  }
  scene.set_planar_shadow(spec.shadow);"""
BAD_ORDER = """  scene.set_planar_shadow(spec.shadow);
  for (const SceneObjectSpec& obj : spec.objects) {
    if (!obj.ground) continue;
    const auto status = draw_one(obj);
    if (!status) return st::forward_error(status.error());
  }"""

# ── 缺陷 2：解析时不读 `ground` ──
GOOD_GROUND = 'obj.ground = json_get_bool(node, "ground", false);'
BAD_GROUND = 'obj.ground = false;  // 回退：忽略 ground 字段'

# ── 缺陷 3 交给测试本身的「标记法」判据（见测试文件注释），
# 这里不靠改源码结构来验证——因为回退要能编译、且不能递归。
def main() -> int:
    original = open(SRC, encoding="utf-8").read()
    results: list[tuple[str, bool]] = []

    cases = [
        ("渲染顺序（先设阴影再画地面）", GOOD_ORDER, BAD_ORDER, TEST_SHADOW),
        ("ground 字段被忽略", GOOD_GROUND, BAD_GROUND, TEST_SHADOW),
    ]

    try:
        print("基线（未修改）...", end=" ", flush=True)
        baseline_ok = True
        for _, _, _, test in cases:
            ok, out = run_test(test)
            if not ok:
                print(f"红 [X] ({test})")
                print(out[:900])
                baseline_ok = False
                break
        if not baseline_ok:
            print("基线就不绿，先修测试")
            return 2
        print("绿 [OK]")

        for label, good, bad, test in cases:
            print(f"改坏「{label}」...", end=" ", flush=True)
            if good not in original:
                print("跳过（找不到回退点，源码已变）")
                results.append((label, False))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(good, bad, 1))
            ok, _ = run_test(test)
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
    ok = all(run_test(t)[0] for _, _, _, t in cases)
    print("绿 [OK]" if ok else "红 [X]")
    results.append(("恢复后基线", ok))

    bad_names = [n for n, r in results if not r]
    print()
    if bad_names:
        print(f"[X] 有 {len(bad_names)} 项未拦住：{', '.join(bad_names)}")
        return 1
    print("全部回退都被拦住了 -- 测试是真护栏")
    return 0


if __name__ == "__main__":
    sys.exit(main())
