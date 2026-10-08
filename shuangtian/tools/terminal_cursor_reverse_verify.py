#!/usr/bin/env python3
"""逆向验证（终端光标）：把修复临时回退，确认回归用例**真的变红**。

为什么必须做：一个"恒绿"的测试看起来像护栏，实际什么都拦不住。

覆盖：
  ① 去掉光标的 `request_animation()` —— 用户报的「闪烁不稳定」原形
     （`fmod(time)` 还在算，但没有新帧 ⇒ 冻在停住的相位）；
  ② 把默认形状回退成 `Block`（旧行为）—— 「光标改为竖线」的反向；
  ③ 让宿主默认**无条件**覆盖屏幕模型 —— 会把 `vim` 的插入光标撞掉。

⚠ **跑之前先确保没有别的构建在跑**。本仓构建会写 `build/<档>/obj/*.o.tmp` 再 rename，
两个构建并行时后者会以 `io: rename ... Resource device` 失败——
实测因此把整轮逆向验证的结果搞乱（出现"恢复后仍红"这种矛盾读数）。
构建完再用 `st test` 确认基线是绿的，避免拿着脏二进制跑。

用法：python3 tools/terminal_cursor_reverse_verify.py

注意（CONVENTIONS §7.4）：恢复**不能**用 `copy2`/`git checkout`（会把旧 mtime 搬回来，
比重编出的 .o 还旧，于是"单跑绿、全量跑红"）。这里用 `shutil.copyfile`。
"""
import io
import os
import shutil
import subprocess
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

TERMINAL_CPP = "src/ui/components/terminal.cpp"
SCREEN_CPP = "src/text/ansi_screen.cpp"
SCREEN_HPP = "include/st/text/ansi_screen.hpp"

# ① 续帧
REQUEST_FIXED = """  const bool blinking = screen.cursor_visible() && sb_offset == 0 && focused();
  if (blinking) request_animation();   // 续帧：一秒周期，本组件自己管"""
REQUEST_REVERTED = """  // 回退：不请求续帧（缺陷版）——闪烁会冻在当前相位
  const bool blinking = screen.cursor_visible() && sb_offset == 0 && focused();"""

# ② 默认形状
SHAPE_FIXED = "  AnsiCursorShape cursor_shape_{AnsiCursorShape::Bar};"
SHAPE_REVERTED = "  AnsiCursorShape cursor_shape_{AnsiCursorShape::Block};   // 回退：旧默认"

# ③ 程序优先
GUARD_FIXED = """  void set_default_cursor_shape(AnsiCursorShape shape) noexcept {
    if (!shape_explicit_) cursor_shape_ = shape;
  }"""
GUARD_REVERTED = """  void set_default_cursor_shape(AnsiCursorShape shape) noexcept {
    cursor_shape_ = shape;   // 回退：无条件覆盖（会撞掉 vim 的竖线）
  }"""

CASES = [
    (
        "① 去掉光标续帧 ⇒ 闪烁冻住（用户报的「闪烁不稳定」原形）",
        TERMINAL_CPP, REQUEST_FIXED, REQUEST_REVERTED,
        "terminal_cursor_requests_followup_frames_while_focused",
    ),
    (
        "② 默认形状回退成 Block ⇒ 「改为竖线」失效",
        SCREEN_HPP, SHAPE_FIXED, SHAPE_REVERTED,
        "ansi_screen_default_cursor_shape_is_bar",
    ),
    (
        "③ 宿主默认无条件覆盖 ⇒ 会撞掉程序（vim）指定的竖线",
        SCREEN_HPP, GUARD_FIXED, GUARD_REVERTED,
        "ansi_screen_host_default_does_not_clobber_explicit",
    ),
]


def run_case(name: str) -> bool:
    proc = subprocess.run(
        ["build/bin/st.exe", "test", "--profile", "dev", name, "--test-jobs", "1"],
        capture_output=True, text=True, errors="replace",
    )
    out = proc.stdout + proc.stderr
    return "0 failed" in out and "passed" in out


def main() -> int:
    for path in (TERMINAL_CPP, SCREEN_CPP, SCREEN_HPP):
        if not os.path.exists(path):
            print(f"找不到 {path}（请在框架根运行）", file=sys.stderr)
            return 2
    # 先验证基线是绿的：脏二进制会让"回退后变红"变成假结论。
    print("先构建一次并确认基线全绿…")
    build = subprocess.run(["build/bin/st.exe", "build", "--profile", "dev", "--force"],
                           capture_output=True, text=True, errors="replace")
    if build.returncode != 0:
        print("❌ 基线构建失败——先解决它（可能有别的构建在并行占用 .o.tmp）")
        print((build.stdout + build.stderr).strip()[-600:])
        return 2
    baseline_names = sorted({case[4] for case in CASES})
    for name in baseline_names:
        if not run_case(name):
            print(f"❌ 基线就不是绿的：{name}——改用例或先修实现，否则结论无意义")
            return 2
    print("基线全绿。\n")
    failures = 0
    for index, (desc, path, original, reverted, test_name) in enumerate(CASES, 1):
        print(f"\n=== [{index}/{len(CASES)}] {desc}")
        print(f"    期望变红: {test_name}")
        text = io.open(path, encoding="utf-8").read()
        if original not in text:
            print("    ⚠ 找不到原文（源码已变？）——跳过")
            failures += 1
            continue
        backup = path + ".revbak"
        shutil.copyfile(path, backup)
        try:
            io.open(path, "w", encoding="utf-8", newline="").write(
                text.replace(original, reverted, 1))
            build = subprocess.run(["build/bin/st.exe", "build", "--profile", "dev", "--force"],
                                   capture_output=True, text=True, errors="replace")
            if build.returncode != 0:
                print("    ❌ 回退写法编译失败（该回退不可用，测试跑的是上一份二进制）")
                print("       ", (build.stdout + build.stderr).strip().splitlines()[-1:])
                failures += 1
                continue
            passed = run_case(test_name)
            if passed:
                print("    ❌ 依然是绿的 —— 这个测试抓不住该缺陷（假绿）")
                failures += 1
            else:
                print("    ✅ 变红（测试确实抓得住）")
        finally:
            shutil.copyfile(backup, path)
            os.remove(backup)
            subprocess.run(["build/bin/st.exe", "build", "--profile", "dev", "--force"],
                           capture_output=True, text=True, errors="replace")
            restored = run_case(test_name)
            print(f"    恢复后: {'✅ 通过' if restored else '❌ 仍红（恢复不完整！）'}")
            if not restored:
                failures += 1
    print(f"\n结论: {'全部通过（每条修复都被真实抓住）' if failures == 0 else f'{failures} 处异常'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
