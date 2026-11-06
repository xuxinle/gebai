#!/usr/bin/env python3
"""逆向验证：`Application::font_stack()` 的生命周期契约（**回退→变红→恢复**）。

`CONVENTIONS.md` §7.1：没做过这一步就等于没测。

回退点有两处，各自对应一个半边：

| # | 回退什么 | 应当变红 |
|---|---|---|
| 1 | 把 `font_stack()` 改回「解引用」（`return impl_->fonts.get()` → 返回引用语义，用 `*impl_->fonts` 空指针解引用表达） | `app_font_stack_is_null_before_start` / `app_font_stack_transitions_at_start` |
| 2 | 把字体装载**提前**到构造（让 `fonts` 在构造里就被填） | `app_font_stack_is_null_before_start` |

第 2 条是必要的：只做第 1 条时，有人可能把实现改成「构造时就装载」——
那样 `font_stack()` 恒非空，①② 都过，但**这个契约就没了**（等于把“时序依赖”
换成“启动多花 60ms”，且 `start()` 里那次装载变成重复）。
这条把「start 前后必须不同」写死，堵住那种“修法”。

用法：python tools/font_stack_lifetime_reverse_verify.py
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_CPP = ROOT / "src/app/app.cpp"
ST = ROOT / "build/bin/st.exe"

CASES = [
    {
        "name": "font_stack() 退回空指针解引用",
        "file": APP_CPP,
        "old": "  return impl_->fonts.get();",
        # 用「解空」表达旧的引用语义：读到空指针会断言失败/崩溃，测试必须因此变红。
        "new": "  return &*impl_->fonts;   // 反向验证：解空 unique_ptr",
        "expect": "app_font_stack",
        "expect_red": True,
    },
    {
        "name": "字体装载提前到构造（恒非空）",
        "file": APP_CPP,
        "old": "  return impl_->fonts.get();",
        "new": ("  if (!impl_->fonts) {\n"
                "    // 反向验证：构造期就装载，让 font_stack() 永远非空\n"
                "    auto warm = st::text::FontStack::system_default();\n"
                "    if (warm) impl_->fonts = std::make_unique<st::text::FontStack>(std::move(*warm));\n"
                "  }\n"
                "  return impl_->fonts.get();"),
        "expect": "app_font_stack",
        "expect_red": True,
    },
]


def run_test(name: str) -> tuple[bool, str]:
    """返回 (是否全绿, 理由)。

    为何用 `bytes` 而不是 `text=True`：断言/崩溃时 `st test` 的输出里会夹带
    未按 UTF-8 编码的字节（实测：GBK 片段的 0x96）；`text=True` 会在**读取线程**里
    抛 `UnicodeDecodeError`，让整个函数抛异常而不是返回“红”。第一版就这么漏判过。
    """
    result = subprocess.run(
        [str(ST), "test", name, "--profile", "debug"],
        cwd=str(ROOT), capture_output=True,
    )
    raw = (result.stdout or b"") + (result.stderr or b"")
    output = raw.decode("utf-8", errors="replace")
    if "编译失败" in output or "构建失败" in output:
        return False, "build-failed"
    for match in re.finditer(r"(\d+)\s+passed,\s+(\d+)\s+failed", output):
        if int(match.group(2)) > 0:
            return False, "red"
    if "passed," not in output:
        # 没有汇总行 = 测试进程自己死了（否则断言失败，那属于 red）。
        # 这是**真红**：空指针解引用会让测试二进制直接断言退出。
        return False, "crashed"
    return result.returncode == 0, "green"


def main() -> int:
    failures: list[str] = []
    print("=== 基线必须全绿 ===")
    green, why = run_test("app_font_stack")
    print(f"  基线 app_font_stack: {why}")
    if not green:
        print("  基线不绿，先修好再做逆向验证")
        return 1

    for case in CASES:
        path: pathlib.Path = case["file"]
        original = path.read_text(encoding="utf-8")
        crlf = "\r\n" in original
        normalized = original.replace("\r\n", "\n")
        if case["old"] not in normalized:
            print(f"[?] 找不到回退点：{case['name']}")
            failures.append(case["name"])
            continue
        patched = normalized.replace(case["old"], case["new"], 1)
        path.write_text(patched.replace("\n", "\r\n") if crlf else patched, encoding="utf-8")
        try:
            ok, why = run_test(case["expect"])
            # “红色”包括两种：断言失败（red）与测试进程直接死（crashed，本例就是它）。
            turned_red = why in ("red", "crashed")
            verdict = "变红" if turned_red else f"**没变红（{why}）**"
            print(f"[{'OK' if turned_red else 'X'}] 回退「{case['name']}」→ {case['expect']} {verdict}")
            if not turned_red:
                failures.append(case["name"])
        finally:
            path.write_text(original, encoding="utf-8")
            # 恢复后必须重新编过：否则下一轮跑的还是回退版二进制
            subprocess.run([str(ST), "build", "--no-tests", "st_tests", "--profile", "debug"],
                           cwd=str(ROOT), capture_output=True)

    green, why = run_test("app_font_stack")
    print(f"[{'OK' if green else 'X'}] 恢复后全绿（{why}）")
    if not green:
        failures.append("恢复后全绿")
    print()
    if failures:
        print(f"失败 {len(failures)} 项：{failures}")
        return 1
    print("全部通过：契约的每条都被测试钉住")
    return 0


if __name__ == "__main__":
    sys.exit(main())
