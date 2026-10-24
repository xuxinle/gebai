"""逆向验证：把颜色管线的实现逐条改坏，确认测试真会变红。

## 为什么这一步不能省

颜色管线的 bug 表现为"画面偏暗/偏灰"，**没有任何异常或崩溃**。
不做逆向验证，很容易交付一组"看起来在测、其实拦不住"的用例
（本轮已在 rotate 那批上连踩六次，见 `verify_rotate_reverse.py`）。

## 本脚本已内置的两个规避（都是上一轮踩出来的）

1. **判定不经 `st test` 的 returncode**（它恒为 1），也不靠 `(\\d+) failed`
   汇总行（单用例模式没有该行）——而是去掉 ANSI 后找 `FAIL <用例名>`。
2. **每轮必须删测试中间产物**（`build/dev/obj/*<测试名>*.o*` 与
   `build/dev/bin/st_tests.exe`）。只改源文件时构建系统会命中缓存，
   跑的是旧二进制 ⇒ 四条回退全报"仍绿"。

运行：py scripts/verify_color_reverse.py
"""

import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "src", "raster", "color_pipeline.cpp")
BACKUP = os.path.join(HERE, "_color_pipeline_backup.cpp")
ST = os.path.join(ROOT, "build", "bin", "st.exe")
OBJ_DIR = os.path.join(ROOT, "build", "dev", "obj")
TESTS_BIN = os.path.join(ROOT, "build", "dev", "bin", "st_tests.exe")
TEST_KEY = "raster_color_pipeline"

# (名字, 原文, 改坏成什么, 期望变红的用例名)
CASES = [
    (
        "sRGB 编码退化成简单幂函数（丢掉分段线性段）",
        "  if (linear <= kSrgbCutoff) return linear * kSrgbSlope;\n"
        "  return kSrgbAlpha * std::pow(linear, 1.0f / kSrgbGamma) - kSrgbOffset;",
        "  return std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / kSrgbGamma);",
        # 期望用**暗部**那条：连续性测不出纯幂函数（幂函数本身处处连续），
        # 真正区分的是暗部斜率（分段线性段 12.92 vs 幂函数 ~3~5）。
        "color_srgb_dark_end_follows_the_linear_branch",
    ),
    (
        "解码侧分段点抄错（0.04045 → 0.0031308，最常见的混淆）",
        "  if (v <= kSrgbCutoff * kSrgbSlope) return v / kSrgbSlope;",
        "  if (v <= kSrgbCutoff) return v / kSrgbSlope;",
        "color_srgb_roundtrip_is_exact",
    ),
    (
        "量化用截断代替四舍五入（整幅偏暗半级）",
        "  const int value = static_cast<int>(std::lround(encoded * 255.0f));",
        "  const int value = static_cast<int>(encoded * 255.0f);",
        "color_srgb8_rounds_instead_of_truncating",
    ),
    (
        "None 档也做压灰（破坏老场景逐像素不变）",
        "    case ToneMap::None:\n      return std::clamp(x, 0.0f, 1.0f);",
        "    case ToneMap::None:\n      return reinhard(x);",
        "color_tonemap_none_is_identity_clamp",
    ),
    (
        "ACES 系数抄错（2.51 → 2.15）",
        "  constexpr float a = 2.51f;",
        "  constexpr float a = 2.15f;",
        # 期望用**钉具体值**那条："ACES 与 Filmic 差 >0.01" 的阈值比错误量小，拦不住。
        "color_aces_matches_its_reference_values",
    ),
    (
        "曝光在色调映射之后应用（顺序反了）",
        "  const math::Vec3 exposed{hdr_linear.x * gain, hdr_linear.y * gain, hdr_linear.z * gain};\n"
        "  const math::Vec3 mapped = tone_map(exposed, op);",
        # 回退写法必须能编译过：`-Werror` 下多一个未使用的 `exposed` 会直接
        # 打断编译，测试根本没跑，而脚本只看“有没有 FAIL 行”就判“仍绿”
        # （CONVENTIONS §7.3 的假绿形态，实测踩到）。所以把 `exposed` 真正用上。
        "  const math::Vec3 mapped = tone_map(hdr_linear, op);\n"
        "  const math::Vec3 exposed{mapped.x * gain, mapped.y * gain, mapped.z * gain};\n"
        "  (void)exposed;",
        "color_encode_pixel_applies_exposure_before_mapping",
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
    """构建并跑该用例。

    返回 `(是否失败, 状态说明)`。状态有三种，**不能混同**：

    - `failed`：真变红（我们要的）
    - `build_error`：**编译失败**——必须单独报，不能算“仍绿”。
      （CONVENTIONS §7.3：回退写法编译不过时，测试跑的是上一份二进制，
      四个回退全部假绿。本脚本实测踩到过：`-Werror` 拦住了未使用的变量。）
    - `passed`：真跑完且全绿（说明回退没被拦住）
    """
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
    # 既没 PASS 也没 FAIL：分片没跑起来，这也是“未判定”，不能当绿
    return False, "undecided"


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for path in (TARGET,):
        if not os.path.exists(path):
            print(f"✗ 找不到 {path}")
            return 1

    base = [c[3] for c in CASES]
    bad0 = []
    for c in base:
        failed, status = run_case(c)
        if failed or status in ("build_error", "undecided"):
            bad0.append(f"{c}({status})")
    if bad0:
        print(f"✗ 基线不可用：{bad0}")
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
                print(f"✗ 改坏「{name}」→ **编译失败**（回退写法必须能编译过，见 §7.3）")
                not_caught.append(name)
            else:
                print(f"{'✓' if failed else '✗'} 改坏「{name}」→ "
                      f"{'变红（' + expect + '）' if failed else status}")
                if not failed:
                    not_caught.append(name)
    finally:
        shutil.copy2(BACKUP, TARGET)
        os.remove(BACKUP)

    bad1 = []
    for c in base:
        failed, status = run_case(c)
        if failed or status in ("build_error", "undecided"):
            bad1.append(f"{c}({status})")
    print(f"\n恢复后：{'全绿 ✓' if not bad1 else f'不可用 ✗ {bad1}'}")
    if bad1:
        return 1
    if not_caught:
        print(f"\n✗ {len(not_caught)} 条回退未被拦住：{not_caught}")
        return 1
    print("全部回退都被拦住了——测试是真护栏")
    return 0


if __name__ == "__main__":
    sys.exit(main())
