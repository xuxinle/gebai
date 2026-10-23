"""逆向验证：逐条回退 schema 相关的修复，确认对应用例真的变红。

## 为什么必须做这一步

一条永远绿的测试不是护栏，只是一段代码。本仓实测过三种"假绿"：
① 回退是空操作（改的东西不影响行为）；
② 回退版编译不过（跑的是上一份二进制）；
③ 桩把差异抹平（错误实现也判绿）。

所以每条回退都必须**真的改变行为**、**能编译过**、并让**指定的用例**变红。
任一不满足就说明那条护栏是假的。

## 用法

    python tools/caiyun_schema_reverse_verify.py           # 全部用例
    python tools/caiyun_schema_reverse_verify.py --list    # 只列清单

## 注意（踩过的坑）

· 恢复必须**删旧文件再写**：`copy2` 会把旧 mtime 一起搬回来，比回退版编出的
  `.o` 还旧 → 增量构建认为源文件没变 → 跑的是旧二进制 → 假绿。
· 回退文本必须能编译过（`-Werror` 会打断编译，于是"测试跑的是上一份二进制"）。
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA_CPP = ROOT / "examples" / "caiyun" / "schema.cpp"
DIRECTOR_CPP = ROOT / "examples" / "caiyun" / "director.cpp"
ST_BIN = ROOT / "build" / "bin" / "st.exe"

# 每条：名字、目标文件、原文（须唯一）、回退写法、应当变红的用例
CASES = [
    {
        "name": "未知字段不再产生诊断",
        "file": DIRECTOR_CPP,
        "original": "        diagnostics->push_back(unknown_key_diagnostic(SchemaScope::Layer, key, path));",
        "broken": "        (void)key;  // 回退：不再报未知字段",
        "expect_fail": [
            "parse_diagnostics_report_typo_with_suggestion",
            "parse_diagnostics_report_all_typos_not_just_first",
            "parse_diagnostics_report_scene_level_typo",
        ],
    },
    {
        "name": "别名表被清空（建议退回纯距离算法）",
        "file": SCHEMA_CPP,
        "original": '              Aliases("rotation", "rotate", "model_rot")},',
        "broken": '              Aliases()},',
        "expect_fail": ["schema_suggest_spin_for_model_rot"],
    },
    {
        "name": "子串启发不再限长度（单字母抢走长名建议）",
        "file": SCHEMA_CPP,
        "original": "    constexpr std::size_t kSubstringMinLength = 6;",
        "broken": "    constexpr std::size_t kSubstringMinLength = 0;",
        "expect_fail": ["schema_suggest_azimuth_for_camera_azimuth"],
    },
    {
        "name": "越界不再报告",
        "file": DIRECTOR_CPP,
        "original": "      collect_range_diagnostics(node, SchemaScope::Layer, path, *diagnostics);",
        "broken": "      (void)path;  // 回退：不再检查越界",
        "expect_fail": ["parse_diagnostics_report_clamped_value"],
    },
    {
        "name": "tracks 子键不再校验",
        "file": DIRECTOR_CPP,
        "original": "      collect_track_diagnostics(node, path, *diagnostics);",
        "broken": "      (void)node;  // 回退：不再校验 tracks 子键",
        "expect_fail": ["parse_diagnostics_report_unsupported_track_property"],
    },
    {
        "name": "序列化丢掉 bounce 系列",
        "file": DIRECTOR_CPP,
        "original": '  if (layer.bounce > 0.0f) node["bounce"] = static_cast<double>(layer.bounce);',
        "broken": "  // 回退：不再写回 bounce",
        "expect_fail": ["roundtrip_keeps_layer_animation_fields"],
    },
    {
        "name": "序列化丢掉 model_group",
        "file": DIRECTOR_CPP,
        "original": '    if (!layer.model_group.empty()) node["model_group"] = layer.model_group;',
        "broken": "    // 回退：不再写回 model_group",
        "expect_fail": ["roundtrip_keeps_3d_world_and_shadow_fields"],
    },
    {
        "name": "schema 表漏登 model_spin（覆盖度用例必须抓住）",
        "file": SCHEMA_CPP,
        "original": '    FieldSpec{"model_spin", SchemaScope::Layer, SchemaType::Number, true, -720.0, 720.0, true,',
        "broken": '    FieldSpec{"model_spin_X", SchemaScope::Layer, SchemaType::Number, true, -720.0, 720.0, true,',
        "expect_fail": ["schema_covers_every_layer_key_the_parser_reads"],
    },
]


def run_test(case_name: str) -> tuple[bool, str]:
    """跑单条用例；返回（是否通过, 输出）。"""
    result = subprocess.run(
        [str(ST_BIN), "test", case_name, "--profile", "dev"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    output = (result.stdout or "") + (result.stderr or "")
    # 判据用 "PASS <case>" / "FAIL <case>"；0 passed/0 failed 说明过滤没命中，
    # 那种情况不能算"通过"（会掩盖"用例根本不存在"）。
    passed = f"PASS {case_name}" in output
    return passed, output


def verify(case: dict) -> tuple[bool, str]:
    """回退 → 跑用例（应红）→ 恢复。返回（是否成功验证, 说明）。"""
    target: pathlib.Path = case["file"]
    original_text = target.read_text(encoding="utf-8")
    original = case["original"]
    if original_text.count(original) != 1:
        return False, f"原文出现 {original_text.count(original)} 次（需恰好 1 次），无法安全回退"

    broken_text = original_text.replace(original, case["broken"], 1)
    try:
        # 回退：删旧写新（不 copy2，避免旧 mtime 造成假绿）
        target.unlink()
        target.write_text(broken_text, encoding="utf-8")

        results = {}
        for name in case["expect_fail"]:
            passed, _ = run_test(name)
            results[name] = passed

        still_green = [n for n, ok in results.items() if ok]
        if still_green:
            return False, f"回退后这些用例**仍然绿**（护栏是假的）：{still_green}"
        return True, f"回退后 {len(results)} 条用例全部变红 [OK]"
    finally:
        target.unlink()
        target.write_text(original_text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="只列清单，不执行")
    parser.add_argument("--only", help="只跑名字包含该子串的用例")
    args = parser.parse_args()

    # Windows 控制台默认 GBK，打不出 ✓ 这类字符会直接抛 UnicodeEncodeError
    # （而不是“显示成问号”——那反而会掩盖问题）。统一换成可容纳任何字符的编码。
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")

    cases = CASES
    if args.only:
        cases = [c for c in cases if args.only in c["name"]]

    if args.list:
        for index, case in enumerate(cases, 1):
            print(f"{index}. {case['name']} → 期望变红: {', '.join(case['expect_fail'])}")
        return 0

    if not ST_BIN.exists():
        print(f"找不到 {ST_BIN}（先跑一次 st build / st test）", file=sys.stderr)
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
        print(f"{failures}/{len(cases)} 条回退**没有**让测试变红——护栏不可信", file=sys.stderr)
        return 1
    print(f"全部 {len(cases)} 条回退都让测试变红：护栏可复验")
    return 0


if __name__ == "__main__":
    sys.exit(main())
