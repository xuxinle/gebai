#!/usr/bin/env python3
"""逆向验证：把「连点重复触发」的修复临时回退，确认回归用例**真的变红**。

为什么必须做：一个恒绿的测试看起来像护栏，实际什么都拦不住。本轮修的是
「一次物理手势算几次点击」，而这类缺陷的错法**不会崩、不会报错**，只是让用户
点一次被触发多次——正是最容易被一条假绿测试放过的那一类。

每条 CASE 做一次「回退 → 期望红 → 恢复」。回退的都是**修复本体**：

  ① `click_kind()` 恒返回 `Click`（= 双击不再升级，用户永远点不出双击）
  ② `confirmed_repeat` 那一行删掉（= 系统的双击判定被忽略）
  ③ 连击判定退回「不看位置」（= 相邻两处点击被并成一击）

用法：python3 tools/click_gesture_reverse_verify.py
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys

GESTURE_HPP = "src/shell/click_gesture.hpp"

# (说明, 文件, 原文, 回退后的文本, 期望变红的用例名)
CASES = [
    (
        "点击族不再升级（双击退化成单击）",
        GESTURE_HPP,
        "    if (count_ >= 3) return ui::EventKind::TripleClick;\n"
        "    if (count_ == 2) return ui::EventKind::DoubleClick;\n"
        "    return ui::EventKind::Click;",
        "    return ui::EventKind::Click;",
        "click_streak_quick_second_press_upgrades_to_double",
    ),
    (
        "忽略窗口系统的连击确认（WM_LBUTTONDBLCLK 的判定被丢掉）",
        GESTURE_HPP,
        "    if (confirmed_repeat && count_ < 2) count_ = 2;",
        "    (void)confirmed_repeat;   // 逆向验证：忽略系统判定",
        "click_streak_honours_window_system_confirmation",
    ),
    (
        "连击判定不看位置（相隔很远的两处点击被并成一击）",
        GESTURE_HPP,
        "                           std::abs(static_cast<double>(at.x) - static_cast<double>(last_.x)) <=\n"
        "                               config.slop &&\n"
        "                           std::abs(static_cast<double>(at.y) - static_cast<double>(last_.y)) <=\n"
        "                               config.slop;",
        "                           true;",
        "click_streak_far_second_press_restarts",
    ),
    (
        "连击判定不看时间（隔了 700ms 的两次点击被并成一击）",
        GESTURE_HPP,
        "    const bool continued = has_last_ && (at_ms - last_ms_) <= config.window_ms &&",
        "    const bool continued = has_last_ &&",
        "click_streak_slow_second_press_restarts",
    ),
    (
        "reset 不清账（窗口失活后仍把中断前后连成一击）",
        GESTURE_HPP,
        "  void reset() noexcept {\n    count_ = 0;",
        "  void reset() noexcept {\n    count_ = count_;",
        "click_streak_reset_starts_a_new_gesture",
    ),
]


def run_test(name: str) -> tuple[bool, str]:
    """跑一条用例，返回（是否仍然绿, 输出）。

    口径与 `tools/theme_reverse_verify.py` 同一套（那边有三条踩过的坑）：
    · 输出按 UTF-8 解码（Windows 默认 GBK 会解坏）；
    · 编译失败要单独判——沿用上一份二进制时本应变红的用例会"意外"保持绿；
    · **不看退出码**：用例正常变红时 `st test` 也返回非零。
    """
    proc = subprocess.run(
        ["./build/bin/st", "test", "--profile", "debug", name],
        capture_output=True, timeout=1800, env=os.environ,
    )
    out = (proc.stdout or b"").decode("utf-8", "replace") + (proc.stderr or b"").decode(
        "utf-8", "replace")
    if "编译失败" in out or "error:" in out:
        return True, out
    if "Assertion failed" in out or "Aborted" in out or "段错误" in out:
        return True, out
    failed = bool(re.search(r"FAIL", out)) or " 1 failed" in out
    return (not failed), out


def main() -> int:
    ok = True
    print(f"逆向验证：{len(CASES)} 条关键契约\n")
    for label, path, original, reverted, test_name in CASES:
        text = open(path, encoding="utf-8").read()
        if original not in text:
            print(f"[跳过] {label}\n       原文未找到（可能已被改动）：{original[:60]!r}")
            ok = False
            continue
        backup = path + ".reverse-backup"
        shutil.copy(path, backup)
        try:
            open(path, "w", encoding="utf-8").write(text.replace(original, reverted, 1))
            greened, out = run_test(test_name)
            if greened:
                print(f"[红][失败] {label}\n       回退后「{test_name}」**仍然是绿的**——这条测试抓不住这个缺陷")
                print("       末尾输出：" + out[-600:].replace("\n", " | "))
                ok = False
            else:
                print(f"[绿][通过] {label}\n       回退后「{test_name}」按预期变红")
        finally:
            # 删后重建（不能 move/copy 覆盖：本仓增量构建只比**内容哈希**，
            # 但备份文件的其它属性仍可能与期望不符，重建最稳）。
            content = open(backup, encoding="utf-8").read()
            try:
                os.remove(path)
            except OSError:
                pass
            open(path, "w", encoding="utf-8").write(content)
            try:
                os.remove(backup)
            except OSError:
                pass
            if original not in open(path, encoding="utf-8").read():
                print(f"[致命] {path} 未能恢复原状！请从 git 恢复后再跑。")
                return 1
    if not ok:
        print("\n存在未能抓住缺陷的用例，需要修测试或修实现。")
        return 1
    print(f"\n全部 {len(CASES)} 条都通过了逆向验证：回退即变红。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
