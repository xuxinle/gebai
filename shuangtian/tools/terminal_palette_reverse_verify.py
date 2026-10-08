#!/usr/bin/env python3
"""逆向验证（终端色板主题适配）：逐条把修复临时回退，确认回归用例**真的变红**。

为什么必须做：一个"恒绿"的测试看起来像护栏，实际什么都拦不住。

覆盖本轮对话色板缺陷：
  ① 把**亮色主题**的 16 色换回写死的那套深底调色板 → 对比度断言必须红；
  ② 让终端底色**借用** `surface_sunken`（旧行为）→ 底色断言必须红；
  ③ 抹平亮/暗底色的差异 → "两套色板"断言必须红。

用法：python3 tools/terminal_palette_reverse_verify.py

注意（见 CONVENTIONS §7.4）：本仓增量构建按 **mtime** 判定重编，
恢复时**不能**用 `copy2`/`git checkout`（会把旧 mtime 搬回来，比重编出的 .o 还旧，
于是"单跑绿、全量跑红"）。这里用 `shutil.copyfile`（只搬内容、mtime 取当下）。
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

THEME_CPP = "src/ui/theme.cpp"

# 亮色主题那组 16 色（修复版）——回退时整块换成深底调色板。
LIGHT_ANSI_OK = """  theme.terminal_.ansi = {
      hex(0x3A3A45FFU), hex(0xB32218FFU), hex(0x0A6234FFU), hex(0x7A5600FFU),
      hex(0x1A46C4FFU), hex(0x861F9BFFU), hex(0x0B6477FFU), hex(0x57616FFFU),
      hex(0x4E4E5CFFU), hex(0xBC1C12FFU), hex(0x0E6F3CFFU), hex(0x865700FFU),
      hex(0x1D4ED8FFU), hex(0x9317A8FFU), hex(0x096B80FFU), hex(0x3A3A45FFU)};"""

# 回退版 = 旧代码里写死的那套 xterm 深底调色板（放到白底上 12/16 色不可读）。
DARK_ON_LIGHT_ANSI = """  theme.terminal_.ansi = {
      hex(0x000000FFU), hex(0xCD3131FFU), hex(0x0DBC79FFU), hex(0xE5E510FFU),
      hex(0x2472C8FFU), hex(0xBC3FBCFFU), hex(0x11A8CDFFU), hex(0xE5E5E5FFU),
      hex(0x666666FFU), hex(0xF14C4CFFU), hex(0x23D18BFFU), hex(0xF5F543FFU),
      hex(0x3B8EEAFU), hex(0xD670D6FFU), hex(0x29B8DBFFU), hex(0xFFFFFFFFU)};"""

CASES = [
    (
        "① 亮色主题用回深底调色板（本轮缺陷原形）→ 对比度断言必须红",
        THEME_CPP,
        LIGHT_ANSI_OK,
        DARK_ON_LIGHT_ANSI,
        "terminal_ansi_palette_is_readable_on_light_background",
    ),
    (
        "② 终端底色借用 surface_sunken（旧行为）→ 底色断言必须红",
        THEME_CPP,
        "  theme.terminal_.bg = hex(0xFFFFFFFFU);",
        "  theme.terminal_.bg = hex(0xDDDDE4FFU);   // 回退：借凹槽色（缺陷版）",
        "terminal_background_is_not_borrowed_from_surface_sunken",
    ),
    (
        "③ 抹平亮/暗底色差异 → 让两套色板其实无法同时适配",
        THEME_CPP,
        "  theme.terminal_.bg = hex(0x0C0C0CU);",
        "  theme.terminal_.bg = hex(0xDDDDE4FFU);   // 回退：与亮色同底（缺陷版）",
        "terminal_palette_background_differs_between_light_and_dark",
    ),
]


def run_case(name: str) -> bool:
    """跑单个用例；返回是否**通过**（期望回退后为 False）。"""
    proc = subprocess.run(
        ["build/bin/st.exe", "test", "--profile", "dev", name, "--test-jobs", "1"],
        capture_output=True, text=True, errors="replace",
    )
    out = proc.stdout + proc.stderr
    return "0 failed" in out and "passed" in out


def main() -> int:
    if not os.path.exists(THEME_CPP):
        print(f"找不到 {THEME_CPP}（请在框架根运行）", file=sys.stderr)
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
        shutil.copyfile(path, backup)          # 只搬内容，mtime 取当下
        try:
            io.open(path, "w", encoding="utf-8", newline="").write(
                text.replace(original, reverted, 1))
            subprocess.run(["build/bin/st.exe", "build", "--profile", "dev", "--force"],
                           capture_output=True, text=True, errors="replace")
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
