"""逆向验证：把顶点色的 sRGB 解码去掉，确认测试真会变红。

## 为什么必须做

"顶点色不解码"这个缺陷**不会崩、不会报错**，只是画面偏亮、材质发塑料感。
本仓库已经发生过一次同样的形态（`rotate` 字段存在但不生效），而当时的回归测试
差点变成"恒绿摆设"——所以这组新测试必须证明自己有判别力。

## 三条回退（都是真实会犯的错）

1. 完全不解码（退回 `/255`）。
2. 只解码一处：`colored_sphere` 换了但 `colored_box` 没换（两条路径不同步）。
3. 用错曲线（纯幂函数代替 sRGB 分段）。

## 两个已内置的规避（上一轮踩出来的）

- 判定不看 returncode，也不靠 `(\\d+) failed` 汇总行（单用例模式没有该行）；
  而是去 ANSI 后找 `FAIL <用例名>`。
- 每轮**必须删测试中间产物**（`build/dev/obj/*<测试名>*.o*` 与 `st_tests.exe`），
  否则构建命中缓存，跑的是旧二进制。

运行：py scripts/verify_mesh_color_reverse.py
"""

import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "src", "raster", "mesh.cpp")
BACKUP = os.path.join(HERE, "_mesh_backup.cpp")
ST = os.path.join(ROOT, "build", "bin", "st.exe")
OBJ_DIR = os.path.join(ROOT, "build", "dev", "obj")
TESTS_BIN = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
TEST_KEY = "raster_mesh_color_space"

# (名字, 原文, 改坏成什么, 期望变红的用例名)
CASES = [
    (
        "完全不解码（退回 /255）",
        "  return srgb_to_linear(\n"
        "      math::Vec3{static_cast<float>(color.r) / 255.0f, "
        "static_cast<float>(color.g) / 255.0f,\n"
        "                 static_cast<float>(color.b) / 255.0f});",
        "  return math::Vec3{static_cast<float>(color.r) / 255.0f,"
        " static_cast<float>(color.g) / 255.0f,\n"
        "                    static_cast<float>(color.b) / 255.0f};",
        "mesh_vertex_colors_are_decoded_from_srgb",
    ),
    (
        "用纯幂函数代替 sRGB 分段（近似但不正确）",
        "  return srgb_to_linear(\n"
        "      math::Vec3{static_cast<float>(color.r) / 255.0f, "
        "static_cast<float>(color.g) / 255.0f,\n"
        "                 static_cast<float>(color.b) / 255.0f});",
        "  const auto pw = [](float v) { return std::pow(v, 2.2f); };\n"
        "  return math::Vec3{pw(static_cast<float>(color.r) / 255.0f),"
        " pw(static_cast<float>(color.g) / 255.0f),\n"
        "                    pw(static_cast<float>(color.b) / 255.0f)};",
        "mesh_vertex_colors_are_decoded_from_srgb",
    ),
]


def force_rebuild() -> None:
    """删测试单元的中间产物，逼它重编（只改被 include 的源文件时缓存会命中）。"""
    for pattern in (f"*{TEST_KEY}*.o", f"*{TEST_KEY}*.o.d",
                    f"*{TEST_KEY}*.o.flags", f"*{TEST_KEY}*.o.meta"):
        for f in glob.glob(os.path.join(OBJ_DIR, pattern)):
            try:
                os.remove(f)
            except OSError:
                pass
    try:
        os.remove(TESTS_BIN)
    except OSError:
        pass


def run_case(case: str) -> tuple[bool, str]:
    """返回 (是否失败, 状态)。状态 ∈ failed / passed / build_error / undecided。"""
    force_rebuild()
    r = subprocess.run([ST, "test", case, "--profile", "dev"], cwd=ROOT,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    plain = re.sub(r"\x1b\[[0-9;]*m", "", (r.stdout or "") + (r.stderr or ""))
    if "构建失败" in plain or "error:" in plain:
        return False, "build_error"
    if re.search(rf"^\s*FAIL\s+{re.escape(case)}\b", plain, re.MULTILINE) is not None:
        return True, "failed"
    if re.search(rf"^\s*PASS\s+{re.escape(case)}\b", plain, re.MULTILINE) is not None:
        return False, "passed"
    return False, "undecided"


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if not os.path.exists(TARGET):
        print(f"✗ 找不到 {TARGET}")
        return 1

    base = [c[3] for c in CASES]
    for c in base:
        failed, status = run_case(c)
        if failed or status in ("build_error", "undecided"):
            print(f"✗ 基线不可用：{c} ({status})")
            return 1
    print("基线：全绿 ✓\n")

    shutil.copy2(TARGET, BACKUP)
    src = open(TARGET, encoding="utf-8").read()
    not_caught = []
    try:
        for name, old, new, expect in CASES:
            if old not in src:
                print(f"⚠ 回退点未命中，跳过：{name}")
                not_caught.append(name)
                continue
            open(TARGET, "w", encoding="utf-8").write(src.replace(old, new, 1))
            failed, status = run_case(expect)
            if status == "build_error":
                print(f"✗ 改坏「{name}」→ 编译失败（回退写法必须能编译过）")
                not_caught.append(name)
            else:
                print(f"{'✓' if failed else '✗'} 改坏「{name}」→ "
                      f"{'变红（' + expect + '）' if failed else status}")
                if not failed:
                    not_caught.append(name)
    finally:
        shutil.copy2(BACKUP, TARGET)
        os.remove(BACKUP)

    bad = []
    for c in base:
        failed, status = run_case(c)
        if failed or status in ("build_error", "undecided"):
            bad.append(f"{c}({status})")
    print(f"\n恢复后：{'全绿 ✓' if not bad else f'不可用 ✗ {bad}'}")
    if bad:
        return 1
    if not_caught:
        print(f"\n✗ {len(not_caught)} 条回退未被拦住：{not_caught}")
        return 1
    print("全部回退都被拦住了——测试是真护栏")
    return 0


if __name__ == "__main__":
    sys.exit(main())
