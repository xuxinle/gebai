"""`Table` 纵向滚动 + 行首连线图的**逆向验证**：逐条回退 → 期望对应用例变红 → 恢复。

## 要验的六条

| # | 回退点 | 期望变红的用例 |
|---|---|---|
| 1 | `max_scroll_y` 不扣表头高 | `table_vertical_scroll_range_excludes_header` |
| 2 | `row_rect` 不减纵向偏移（滚了但画面不动） | `table_vertical_scroll_moves_rows_and_clamps` |
| 3 | 滚轮只认 Shift（纵向恒不可滚） | `table_wheel_scrolls_vertically_without_shift` |
| 4 | 可见区间不扣纵向偏移（滚一格漏一行） | `table_visible_range_accounts_for_vertical_offset` |
| 5 | `set_lane_links` 里 `resize(rows_.size())`（图被截空） | `table_lane_links_draw_lines_and_reserve_inset` |
| 6 | `add_row` 无条件给图补格子（图与行错位） | `table_lane_links_survive_rows_added_later` |

## 两个已知的判定链路坑（本脚本踩过）

- **ANSI 颜色码**：`st test` 的 `FAIL <名>` 之间夹着颜色转义，不先剥掉就全部落空。
- **回退版编译不过**：那样跑的是上一份二进制（假绿）。回退写法必须能编译。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ui" / "components" / "table.cpp"
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

CASES: list[tuple[str, list[tuple[str, str]], str]] = [
    (
        "max-scroll-y-includes-header",
        [(
            "  const float viewport = std::max(0.0f, bounds_.height - kHeaderHeight);\n  return std::max(0.0f, content - viewport);",
            "  const float viewport = std::max(0.0f, bounds_.height);\n  return std::max(0.0f, content - viewport);",
        )],
        "table_vertical_scroll_range_excludes_header",
    ),
    (
        "row-rect-ignores-scroll-offset",
        [(
            "  return math::Rect{bounds_.x, bounds_.y + kHeaderHeight + top - scroll_offset_y_,",
            "  return math::Rect{bounds_.x, bounds_.y + kHeaderHeight + top,",
        )],
        "table_vertical_scroll_moves_rows_and_clamps",
    ),
    (
        "wheel-requires-shift",
        [(
            "      const float limit = event.shift ? max_scroll() : max_scroll_y();",
            "      if (!event.shift) return false;\n      const float limit = max_scroll();",
        )],
        "table_wheel_scrolls_vertically_without_shift",
    ),
    (
        "visible-range-ignores-scroll-offset",
        [(
            "  const float rows_top = bounds.y + header_height - table.scroll_offset_y();",
            "  const float rows_top = bounds.y + header_height;",
        )],
        "table_visible_range_accounts_for_vertical_offset",
    ),
    (
        "set-lane-links-truncates",
        [(
            "  row_links_ = std::move(rows);\n  lane_columns_ = 0;",
            "  row_links_ = std::move(rows);\n  row_links_.resize(rows_.size());\n  lane_columns_ = 0;",
        )],
        "table_lane_links_draw_lines_and_reserve_inset",
    ),
    (
        "add-row-always-pads-links",
        [(
            "  if (!row_links_.empty() && row_links_.size() < rows_.size()) row_links_.emplace_back();",
            "  if (!row_links_.empty()) row_links_.emplace_back();",
        )],
        "table_lane_links_survive_rows_added_later",
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
    return proc.returncode, ANSI.sub("", (proc.stdout or "") + (proc.stderr or ""))


def expect_red(name: str, test_name: str, profile: str) -> bool:
    code, output = run_test(test_name, profile)
    failed = re.search(rf"FAIL {re.escape(test_name)}", output) is not None
    print(f"  [{name}] exit={code} FAIL={failed}")
    if not failed:
        if code == 125 or ("PASS " not in output and "FAIL " not in output):
            print("    ! 回退版没跑起来（构建失败）——回退写法的问题，不是断言无效：")
            for line in output.splitlines():
                if "error" in line or "错误" in line:
                    print("      " + line.strip()[:200])
        else:
            print("    ! 回退后仍然绿——这条测试钉不住该缺陷：")
            for line in output.splitlines():
                if "FAIL" in line or "passed" in line:
                    print("      " + line.strip())
    return failed


def expect_green(test_name: str, profile: str) -> bool:
    code, output = run_test(test_name, profile)
    passed = re.search(rf"PASS {re.escape(test_name)}", output) is not None
    if not passed:
        print(f"    恢复后仍然红（exit={code}）")
    return passed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--log", default="", help="把进度也写进这个文件（后台跑时用）")
    args = parser.parse_args()

    # 进度双写：控制台 + 文件。后台运行时控制台常被缓冲，看不到实时进展。
    log_file = None
    if args.log:
        log_file = open(args.log, "w", encoding="utf-8", buffering=1)
        original_print = print

        def tee(*values, **kwargs):
            original_print(*values, **kwargs)
            kwargs.pop("file", None)
            original_print(*values, file=log_file, **kwargs)
            log_file.flush()

        globals()["print"] = tee

    original = SOURCE.read_text(encoding="utf-8")
    failures: list[str] = []
    try:
        for name, pairs, test_name in CASES:
            missing = [fixed for fixed, _ in pairs if fixed not in original]
            if missing:
                print(f"X [{name}] 回退点找不到（锚点已过期）：{missing[0][:70]!r}")
                failures.append(name)
                continue
            print(f"-> [{name}] 回退…")
            broken = original
            for fixed, replacement in pairs:
                broken = broken.replace(fixed, replacement, 1)
            SOURCE.write_text(broken, encoding="utf-8")
            if not expect_red(name, test_name, args.profile):
                failures.append(name)
            print(f"-> [{name}] 恢复…")
            SOURCE.write_text(original, encoding="utf-8")
            if not expect_green(test_name, args.profile):
                failures.append(name + ":restore")
    finally:
        SOURCE.write_text(original, encoding="utf-8")

    if failures:
        print("\n逆向验证失败：", ", ".join(failures))
        return 1
    print("\n逆向验证通过：全部回退各自让对应用例变红，恢复后全绿。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
