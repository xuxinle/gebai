"""`Connector` 回归测试的**逆向验证**：逐条回退修复 → 期望对应断言变红 → 恢复。

## 为什么必须跑它

新写的回归测试很容易是"恒绿的护栏"：它看起来在守，实际什么都没拦住。
实测踩过的两种形态在 `CONVENTIONS.md` §7.1/§7.2/§7.3：
桩把差异抹平、回退文本编译不过（于是跑的是上一份二进制）。

## 本线要验的四条

| # | 回退点 | 期望变红的用例 |
|---|---|---|
| 1 | `route_points` 里的"零法线给默认出线方向"那段整块删掉 | `connector_curve_keeps_horizontal_tangent_at_endpoints` |
| 2 | 元素端点从"绝对 → 本地"的换算改回直接用 `bounds()` | `connector_element_endpoints_follow_component_origin` |
| 3 | `hit_test` 改成整块 `bounds().contains(point)` | `connector_hit_is_a_narrow_band_along_the_line` |
| 4 | 虚线分支改用整条路径描边（不切段） | `connector_dash_reduces_ink_but_keeps_endpoints` |

每条回退都必须**能编译过**（保留 `(void)var;` 一类写法），否则测的是上一份二进制。

用法： python tools/connector_reverse_verify.py [--profile dev]

## 逆向验证自身的两个坑（都踩过）

- **ANSI 颜色码**：`st test` 的 PASS/FAIL 是带着色的（`FAIL\x1b[0m`），
  直接搜 `"FAIL <名>"` 会全部落空 → 四条回退全被误报成“没变红”。必须先剥码。
- **只弄坏一半**：`Curve` 的两端默认法线是**两段**代码。只回退 from 那一端时，
  to 那一端会把方向补回来，曲线仍然是弯的（断言不变红）。需要两段一起失效。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Windows 控制台默认 GBK，输出里的中文/符号会抛 UnicodeEncodeError（实测）。
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ui" / "components" / "connector.cpp"

# (名字, [(原名片段, 回退片段), ...], 期望变红的用例)
CASES: list[tuple[str, list[tuple[str, str]], str]] = [
    (
        "curve-default-normal",
        [
            # 两个端点都没有法线时**必须**补默认出线方向；两段一起失效才能得到
            # “控制点落在端点上 ⇒ 三次贝塞尔退化成直线”（只弄坏一段时另一段会把
            # 方向补回去，曲线仍然是弯的——这就是第一次回退没变红的原因）。
            (
                "  if (length_of(from_dir) < kEpsilon) {",
                "  if (false) {",
            ),
            (
                "  if (length_of(to_dir) < kEpsilon) {",
                "  if (false) {",
            ),
        ],
        "connector_curve_keeps_horizontal_tangent_at_endpoints",
    ),
    (
        "element-endpoint-coordinate-space",
        [
            (
                """    const math::Rect absolute = endpoint.node->bounds();
    const math::Rect rect{absolute.x - bounds_.x, absolute.y - bounds_.y, absolute.width,
                          absolute.height};""",
                """    const math::Rect absolute = endpoint.node->bounds();
    const math::Rect rect{absolute.x, absolute.y, absolute.width, absolute.height};""",
            )
        ],
        "connector_element_endpoints_follow_component_origin",
    ),
    (
        "hit-test-narrow-band",
        [
            (
                "    return distance_to_line(point) <= std::max(hit_tolerance_, width_ * 0.5f + 1.0f);",
                "    return bounds_.contains(point);",
            )
        ],
        "connector_hit_is_a_narrow_band_along_the_line",
    ),
    (
        "dash-splits-path",
        [
            (
                """    for (const math::Polyline& polyline : path.flatten(0.25f)) {
      append_dashed(canvas, polyline.points, dash_, paint, width_, stroke_style);
    }""",
                """    (void)append_dashed;
    canvas.stroke_path(path, paint, width_, stroke_style);""",
            )
        ],
        "connector_dash_reduces_ink_but_keeps_endpoints",
    ),
]


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# 框架级回退（改的是 element.cpp，与组件回退分开验）
FRAMEWORK_CASES: list[tuple[str, list[tuple[str, str]], str, Path]] = [
    (
        "absolute-measure-skip",
        [
            (
                """    if (child->style().absolute) {
      // 约束与流内子元素**完全一致**（只是不把结果计入本元素的尺寸）：
      // 绝对定位改变的是“占不占位”，不是“能多大”。约束不同的话，同一个子元素
      // 在两种摆法下会量出不同尺寸——那不是这个字段该管的事。
      Constraints own;
      own.max_width = row ? kUnbounded : available_width;
      own.max_height = row ? available_height : kUnbounded;
      own.available_width = available_width;
      own.available_height = available_height;
      child->measure(context, own);
      continue;
    }""",
                """    if (false) {
      continue;
    }""",
            )
        ],
        "layout_absolute_child_does_not_affect_parent_measure",
        Path("src") / "ui" / "element.cpp",
    ),
]


def run_test(filter_name: str, profile: str) -> tuple[int, str]:
    st = ROOT / "build" / profile / "bin" / "st.exe"
    if not st.exists():
        st = ROOT / "build" / "bin" / "st.exe"
    proc = subprocess.run(
        [str(st), "test", filter_name],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    raw = (proc.stdout or "") + (proc.stderr or "")
    # **颜色码必须先剥掉**：st 的 PASS/FAIL 是带 ANSI 着色的，`FAIL <名>` 之间夹着
    # `\x1b[0m`——直接搜 "FAIL 名字" 会全部落空（实测第一版就是这样，四条回退都被
    # 误判成“没变红”，而输出里明明写着 FAIL）。
    return proc.returncode, ANSI.sub("", raw)


def expect_red(name: str, test_name: str, profile: str) -> bool:
    code, output = run_test(test_name, profile)
    failed = re.search(rf"FAIL {re.escape(test_name)}", output) is not None
    print(f"  [{name}] exit={code} FAIL行={failed}")
    if not failed:
        if code == 125 or ("PASS " not in output and "FAIL " not in output):
            print("    ! 回退版**没跑起来**（构建失败）——这是回退写法的问题，不是断言无效：")
            for line in output.splitlines():
                if "error" in line or "错误" in line or "失败" in line:
                    print("      " + line.strip()[:220])
        else:
            print("    ! 回退后仍然绿——这条测试钉不住该缺陷：")
            for line in output.splitlines():
                if "PASS" in line or "FAIL" in line or "passed" in line:
                    print("      " + line.strip())
    return failed


def expect_green(test_name: str, profile: str) -> bool:
    code, output = run_test(test_name, profile)
    passed = re.search(rf"PASS {re.escape(test_name)}", output) is not None
    if not passed:
        print(f"    恢复后仍然红（exit={code}）：")
        for line in output.splitlines():
            if "FAIL" in line or "断言失败" in line:
                print("      " + line.strip())
    return passed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", default="dev")
    args = parser.parse_args()

    original = SOURCE.read_text(encoding="utf-8")
    failures: list[str] = []
    try:
        for name, pairs, test_name in CASES:
            missing = [fixed for fixed, _ in pairs if fixed not in original]
            if missing:
                print(f"X [{name}] 回退点在源码里找不到（锚点已过期）：{missing[0][:60]!r}")
                failures.append(name)
                continue
            print(f"-> [{name}] 回退…")
            broken_text = original
            for fixed, broken in pairs:
                broken_text = broken_text.replace(fixed, broken, 1)
            SOURCE.write_text(broken_text, encoding="utf-8")
            if not expect_red(name, test_name, args.profile):
                failures.append(name)
            print(f"-> [{name}] 恢复…")
            SOURCE.write_text(original, encoding="utf-8")
            if not expect_green(test_name, args.profile):
                failures.append(name + ":restore")
    finally:
        SOURCE.write_text(original, encoding="utf-8")

    # —— 框架级回退（同一套流程，只换被改文件）——
    for name, pairs, test_name, path in FRAMEWORK_CASES:
        target = ROOT / path
        baseline = target.read_text(encoding="utf-8")
        try:
            missing = [fixed for fixed, _ in pairs if fixed not in baseline]
            if missing:
                print(f"X [{name}] 回退点在源码里找不到（锚点已过期）：{missing[0][:60]!r}")
                failures.append(name)
                continue
            print(f"-> [{name}] 回退 {path}…")
            broken_text = baseline
            for fixed, broken in pairs:
                broken_text = broken_text.replace(fixed, broken, 1)
            target.write_text(broken_text, encoding="utf-8")
            if not expect_red(name, test_name, args.profile):
                failures.append(name)
            print(f"-> [{name}] 恢复…")
            target.write_text(baseline, encoding="utf-8")
            if not expect_green(test_name, args.profile):
                failures.append(name + ":restore")
        finally:
            target.write_text(baseline, encoding="utf-8")

    if failures:
        print("\n逆向验证失败：", ", ".join(failures))
        return 1
    print("\n逆向验证通过：全部回退各自让对应用例变红，恢复后全绿。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
