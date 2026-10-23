#!/usr/bin/env python3
"""逆向验证：逐条回退 carry 的修复，确认对应用例真的变红。

## 为什么必须做

一条永远绿的测试不是护栏，只是一段代码。本仓实测过三种"假绿"：
① 回退是空操作（改的东西不影响行为）；
② 回退版编译不过（跑的是上一份二进制）；
③ 桩把差异抹平（错误实现也判绿）。

每条回退都必须**真的改变行为**、**能编译过**、并让**指定的用例**变红。

## 用法

    python scripts/carry_reverse_verify.py            # 全部
    python scripts/carry_reverse_verify.py --list     # 只列清单

## 注意（踩过的坑）

· 恢复必须**删旧文件再写**：`copy2` 会把旧 mtime 一起搬回来，比回退版编出的
  `.o` 还旧 → 增量构建认为源文件没变 → 跑的是旧二进制 → 假绿。
· 回退文本必须能编译过（`-Werror` 会打断编译，于是"测试跑的是上一份二进制"）。
· Windows 控制台默认 GBK，打出 ✓ 会抛 UnicodeEncodeError（不是"显示成问号"）——
  输出统一转 UTF-8。
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CARRY_CPP = ROOT / "examples" / "caiyun" / "carry.cpp"
ST_BIN = ROOT / "build" / "bin" / "st.exe"

CASES = [
    {
        "name": "活动区间用开区间（起点那一帧没有承接）",
        "file": CARRY_CPP,
        "original": "  if (into_span < 0.0 || into_span > span) return pose;",
        "broken": "  if (into_span <= 0.0 || into_span > span) return pose;",
        "expect_fail": ["carry_is_active_at_its_first_frame"],
    },
    {
        "name": "终点那一帧被排除",
        "file": CARRY_CPP,
        "original": "  if (into_span < 0.0 || into_span > span) return pose;",
        "broken": "  if (into_span < 0.0 || into_span >= span) return pose;",
        "expect_fail": ["carry_is_active_at_its_last_frame"],
    },
    {
        "name": "没名字也算承接（验收器失去身份）",
        "file": CARRY_CPP,
        "original": "  if (carry.name.empty()) return pose;  // 没名字 = 不算承接（见 CarryOut 注释）",
        "broken": "  // 回退：不再要求名字",
        "expect_fail": ["carry_without_name_never_activates"],
    },
    {
        "name": "缓动名被忽略（全部退化成同一条曲线）",
        "file": CARRY_CPP,
        "original": "  const CarryCurve curve = carry_curve_by_name(carry.curve);",
        "broken": "  const CarryCurve curve = static_cast<CarryCurve>(nullptr);  // 回退：始终用默认",
        "expect_fail": ["carry_uses_named_curve"],
    },
    {
        "name": "百分比后空格导致静默回退（本轮的实测缺陷）",
        "file": CARRY_CPP,
        "original": "  while (!number.empty() &&\n         std::isspace(static_cast<unsigned char>(number.back())) != 0) {\n    number.remove_suffix(1);\n  }",
        "broken": "  // 回退：不再去尾部空格",
        "expect_fail": ["carry_dim_percent_matches_design_pixels"],
    },
    {
        "name": "collect 不看活动区间（任何时候都收）",
        "file": CARRY_CPP,
        "original": "    if (!pose.active) continue;",
        "broken": "    // 回退：不判断活动区间",
        "expect_fail": ["carry_collect_reports_the_active_one"],
    },
]


def run_test(case_name: str) -> bool:
    result = subprocess.run(
        [str(ST_BIN), "test", case_name, "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    output = (result.stdout or "") + (result.stderr or "")
    return f"PASS {case_name}" in output


def verify(case: dict) -> tuple[bool, str]:
    target: pathlib.Path = case["file"]
    original_text = target.read_text(encoding="utf-8")
    original = case["original"]
    if original_text.count(original) != 1:
        return False, f"原文出现 {original_text.count(original)} 次（需恰好 1 次），无法安全回退"
    broken_text = original_text.replace(original, case["broken"], 1)
    try:
        target.unlink()
        target.write_text(broken_text, encoding="utf-8")
        still_green = [n for n in case["expect_fail"] if run_test(n)]
        if still_green:
            return False, f"回退后这些用例**仍然绿**（护栏是假的）：{still_green}"
        return True, f"回退后 {len(case['expect_fail'])} 条用例全部变红"
    finally:
        target.unlink()
        target.write_text(original_text, encoding="utf-8")


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
        print(f"{failures}/{len(cases)} 条回退没有让测试变红——护栏不可信", file=sys.stderr)
        return 1
    print(f"全部 {len(cases)} 条回退都让测试变红：护栏可复验")
    return 0


if __name__ == "__main__":
    sys.exit(main())
