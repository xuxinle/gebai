#!/usr/bin/env python3
"""逆向验证：逐条回退弹簧族的实现，确认对应用例真的变红。

## 为什么必须做

一条永远绿的测试不是护栏。本仓实测过三种"假绿"：
① 回退是空操作；② 回退版编译不过（跑的是上一份二进制）；③ 桩把差异抹平。
本文件的第 ③ 类尤其危险——弹簧测试第一版就是**在测试里把公式又抄了一遍**，
那是"实现错、抄件照着错"的自证；现在的用例一律经 `LayoutEngine`
执行真实脚本，所以回退 `layout.cpp` 一定能被抓住。

## 用法

    python scripts/spring_reverse_verify.py [--only 关键词] [--list]
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LAYOUT_CPP = ROOT / "examples" / "caiyun" / "layout.cpp"
ST_BIN = ROOT / "build" / "bin" / "st.exe"

CASES = [
    {
        "name": "欠阻尼公式退化成临界（不再过冲）",
        "original": """        if (z < 1.0) {
          // 欠阻尼：衰减余弦 + 正弦（经典二阶阶跃响应）
          const double wd = w * std::sqrt(1.0 - z * z);
          value = 1.0 - std::exp(-z * w * tau) *
                            (std::cos(wd * tau) + (z * w / wd) * std::sin(wd * tau));
        } else if (z == 1.0) {""",
        "broken": """        if (z < 1.0) {
          value = 1.0 - std::exp(-w * tau) * (1.0 + w * tau);
        } else if (z == 1.0) {""",
        "expect_fail": ["spring_underdamped_overshoots",
                        "spring_more_damping_means_less_overshoot"],
    },
    {
        "name": "ζ 与过冲的关系被反转（越大过冲越多）",
        "original": "        const double z = std::clamp(zeta, 0.05, 2.0);\n        const double w = std::clamp(omega, 1.0, 80.0);\n        double value = 0.0;\n        if (z < 1.0) {\n          // 欠阻尼：衰减余弦 + 正弦（经典二阶阶跃响应）",
        "broken": "        const double z = std::clamp(zeta, 0.05, 2.0);\n        const double w = std::clamp(omega, 1.0, 80.0);\n        double value = 0.0;\n        // 回退：阻尼参数被忽略\n        const double z_used = 0.3;\n        if (z_used < 1.0) {",
        "expect_fail": ["spring_more_damping_means_less_overshoot"],
    },
    {
        "name": "spring_vel 写成 spring 本身（不是导数）",
        "original": """        if (z < 1.0) {
          const double s = std::sqrt(1.0 - z * z);
          value = (w / s) * std::exp(-z * w * tau) * std::sin(w * s * tau);
        } else if (z == 1.0) {
          value = w * w * tau * std::exp(-w * tau);""",
        "broken": """        if (z < 1.0) {
          // 回退：漏掉 (w/s) 与 sin 相位，退化成近似常数
          value = 0.0;
        } else if (z == 1.0) {
          value = w * w * tau * std::exp(-w * tau);""",
        "expect_fail": ["spring_velocity_matches_finite_difference"],
    },
    {
        "name": "settle 算得太早（节奏会被砍断）",
        "original": "        const double envelope = std::log(1.0 / std::clamp(eps, 0.001, 0.5)) / (z * w);\n        return st::Json(envelope * 1.2);",
        "broken": "        const double envelope = std::log(1.0 / std::clamp(eps, 0.001, 0.5)) / (z * w);\n        // 回退：不留余量，且缩短一半\n        return st::Json(envelope * 0.5);",
        "expect_fail": ["spring_settle_bound_is_actually_settled"],
    },
    {
        "name": "负时间不归零（入场会先抖一下）",
        "original": "        const double tau = args[0].get<double>();\n        if (tau <= 0.0) return st::Json(0.0);\n        const double zeta = args.size() > 1 && args[1].is_number() ? args[1].get<double>() : 0.6;\n        const double omega = args.size() > 2 && args[2].is_number() ? args[2].get<double>() : 20.0;\n        // 参数护栏",
        "broken": "        const double tau = args[0].get<double>();\n        // 回退：不再拦负时间\n        const double zeta = args.size() > 1 && args[1].is_number() ? args[1].get<double>() : 0.6;\n        const double omega = args.size() > 2 && args[2].is_number() ? args[2].get<double>() : 20.0;\n        // 参数护栏",
        "expect_fail": ["spring_starts_at_zero_and_settles_at_one"],
    },
    {
        "name": "expo_out 没接上（退化成 out）",
        "original": '        if (name == "expo_out") {',
        "broken": '        if (name == "expo_out_disabled") {',
        "expect_fail": ["layout_ease_supports_the_new_curves"],
    },
]


def run_test(name: str) -> bool:
    result = subprocess.run(
        [str(ST_BIN), "test", name, "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    output = (result.stdout or "") + (result.stderr or "")
    return f"PASS {name}" in output


def verify(case: dict) -> tuple[bool, str]:
    text = LAYOUT_CPP.read_text(encoding="utf-8")
    original = case["original"]
    if text.count(original) != 1:
        return False, f"原文出现 {text.count(original)} 次（需恰好 1 次）"
    broken = text.replace(original, case["broken"], 1)
    try:
        # 删旧文件再写：`copy2` 会把旧 mtime 搬回来，增量构建会认为源没变
        # → 跑的是旧二进制 → 假绿。
        LAYOUT_CPP.unlink()
        LAYOUT_CPP.write_text(broken, encoding="utf-8")
        green = [n for n in case["expect_fail"] if run_test(n)]
        if green:
            return False, f"回退后仍绿（护栏无效）：{green}"
        return True, f"回退后 {len(case['expect_fail'])} 条全部变红"
    finally:
        LAYOUT_CPP.unlink()
        LAYOUT_CPP.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")

    cases = [c for c in CASES if not args.only or args.only in c["name"]]
    if args.list:
        for index, case in enumerate(cases, 1):
            print(f"{index}. {case['name']} → {', '.join(case['expect_fail'])}")
        return 0
    if not ST_BIN.exists():
        print(f"找不到 {ST_BIN}（先跑一次 st test）", file=sys.stderr)
        return 2

    failures = 0
    for index, case in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] {case['name']} … ", end="", flush=True)
        ok, detail = verify(case)
        print(detail)
        if not ok:
            failures += 1
    print()
    if failures:
        print(f"{failures}/{len(cases)} 条回退没让测试变红——护栏不可信", file=sys.stderr)
        return 1
    print(f"全部 {len(cases)} 条回退都让测试变红：护栏可复验")
    return 0


if __name__ == "__main__":
    sys.exit(main())
