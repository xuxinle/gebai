#!/usr/bin/env python3
"""逆向验证（终端滚动方向）：把修复临时回退，确认回归用例**真的变红**。

为什么必须做：一个"恒绿"的测试看起来像护栏，实际什么都拦不住。
本轮尤需如此——**单向断言对"符号被抹掉"这类错是无效的**
（"向上滚看到历史"在缺陷版下也成立），所以必须实测回归真的抓得住。

覆盖：
  ① 把 `wheel_scroll_lines` 回退成旧实现
     （`std::max(1, static_cast<int>(-delta * 3.0f))`）→ 向下滚断言必须红；
  ② 只把方向取反（保留 `max(1, …)`）→ 双向断言必须红。

用法：python3 tools/terminal_scroll_reverse_verify.py

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

FIXED = """  // 一格滚轮 = 3 行（与 `ScrollView` 同一族的滚动手感）。
  // 用 `trunc` 而非 `round`：半格（`|delta| < 1/3`）应当**不动**，
  // 而不是被四舍五入成 1 行（那种手感是“轻轻滚一下跳一大格”）。
  return static_cast<int>(wheel_delta * 3.0f);"""

OLD_DEFECT = """  // 回退：旧实现的符号被 `max(1, …)` 抹掉（缺陷版）
  return std::max(1, static_cast<int>(-wheel_delta * 3.0f));"""

CASES = [
    (
        "① 回退成旧实现（`max(1, -delta*3)`）——用户报的缺陷原形",
        TERMINAL_CPP,
        FIXED,
        OLD_DEFECT,
        "terminal_wheel_scrolls_down_for_negative_delta",
    ),
    (
        "① 同上回退，看**双向**断言是否也红",
        TERMINAL_CPP,
        FIXED,
        OLD_DEFECT,
        "terminal_wheel_directions_are_opposite",
    ),
    (
        "② 只把方向取反（保留 max(1, …)）——另一个可能的写法错",
        TERMINAL_CPP,
        FIXED,
        """  // 回退：方向写反（缺陷版）
  return std::max(1, static_cast<int>(wheel_delta * 3.0f));""",
        # ⚠ 这里必须选**向下/双向**的用例：
        # 对 `delta = 1.0`，`max(1, 1*3) = 3` 与正确值**完全相等**——
        # “向上”那条用例对此写法**天然无判别力**（实测确实仍绿）。
        # 能抓它的是“向下”与“双向”那两条。
        "terminal_wheel_directions_are_opposite",
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
    if not os.path.exists(TERMINAL_CPP):
        print(f"找不到 {TERMINAL_CPP}（请在框架根运行）", file=sys.stderr)
        return 2
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
                # 编译不过 = 回退写法本身有问题（见 CONVENTIONS §7.3），如实报出来
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
