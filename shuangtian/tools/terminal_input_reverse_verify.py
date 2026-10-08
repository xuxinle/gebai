#!/usr/bin/env python3
"""逆向验证（终端输入契约）：逐条把修复临时回退，确认对应回归用例**真的变红**。

为什么必须做：一个"恒绿"的测试看起来像护栏，实际什么都拦不住。
本脚本对每条关键契约做一次「回退 → 期望红 → 恢复」。

覆盖两条本轮缺陷：
  ① 可打印字符双来源（`KeyDown` 裸字符回落 + `TextInput`）→ 真窗口一字符回显两次
  ② 修饰 + 方向键的映射曾是死代码（裸键分支抢先匹配）→ Ctrl+← 只移一格

用法：python3 tools/terminal_input_reverse_verify.py

注意（见 CONVENTIONS §7.4）：本仓增量构建按 **mtime** 判定重编，
恢复时**不能**用 `copy2`/`git checkout`（会把旧 mtime 搬回来，比重编出的 .o 还旧，
于是"单跑绿、全量跑红"）。这里用 `shutil.copyfile`（只搬内容、mtime 取当下）。
"""
import io
import os
import shutil
import subprocess
import sys

# Windows 控制台默认 GBK，而输出里有 ✅/❌ 与中文——**必须显式改 UTF-8**，
# 否则 `print` 抛 UnicodeEncodeError，脚本在“恢复”分支里崩掉（实测踩到：
# 源码虽已恢复，但进程非零退出会被误读成“验证失败”）。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

TERMINAL_CPP = "src/ui/components/terminal.cpp"

# (说明, 文件, 原文, 回退后的文本, 期望变红的用例名)
CASES = [
    (
        "① 可打印字符双来源：把裸字符回落加回 key_bytes（真窗口会双回显）",
        TERMINAL_CPP,
        "  // 其余（含**无修饰的可打印字符**）一律不送：文本归 `TextInput`——见头文件说明。\n  return {};",
        "  if (key.size() == 1) return key;   // 回退：裸字符直接送（缺陷版）\n  return {};",
        "terminal_plain_character_keydown_sends_nothing",
    ),
    (
        "① 可打印字符双来源（成对判据）：同上回退，看核心用例是否也红",
        TERMINAL_CPP,
        "  // 其余（含**无修饰的可打印字符**）一律不送：文本归 `TextInput`——见头文件说明。\n  return {};",
        "  if (key.size() == 1) return key;   // 回退：裸字符直接送（缺陷版）\n  return {};",
        "terminal_keydown_then_textinput_sends_one_character",
    ),
    (
        "② 修饰方向键死代码：把修饰分支挪到裸键分支**之后**（等效于不可达）",
        TERMINAL_CPP,
        "  const bool is_nav = key == \"ArrowUp\" || key == \"ArrowDown\" || key == \"ArrowRight\" ||\n"
        "                      key == \"ArrowLeft\" || key == \"Home\" || key == \"End\";\n"
        "  if (is_nav && (event.ctrl || event.shift || event.alt)) {",
        "  const bool is_nav = false;   // 回退：修饰分支不可达（缺陷版）\n"
        "  if (is_nav && (event.ctrl || event.shift || event.alt)) {",
        "terminal_modified_navigation_sends_xterm_modifier_parameter",
    ),
]


def run_case(name: str) -> bool:
    """跑单个用例；返回是否**通过**（期望回退后为 False）。"""
    proc = subprocess.run(
        ["build/bin/st.exe", "test", "--profile", "dev", name, "--test-jobs", "1"],
        capture_output=True, text=True, errors="replace",
    )
    out = proc.stdout + proc.stderr
    # `st test` 在有用例失败时返回非零；也显式解析计数行以兼容。
    if "0 failed" in out and "passed" in out:
        return True
    return False


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
