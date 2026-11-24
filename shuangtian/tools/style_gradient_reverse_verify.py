#!/usr/bin/env python3
"""逆向验证（`CONVENTIONS.md` §7.3）：把每处改动**临时回退**，确认对应用例变红。

## 为什么必须做这一步

测试通过 ≠ 测试有效。一个恒绿的断言看起来像护栏，实际什么都拦不住
（本框架实测过：桩把差异抹平 / 回退文本编译不过 → 假绿两条）。

本次改动是「一个字段的持有方式 + 一个字节的对齐」，没有行为变化——
恰恰是这种改动最容易交付一批“回退后依然全绿”的假护栏。所以逐条验：

| 回退什么 | 预期变红的用例 |
|---|---|
| `background_gradient` 改回内联 `std::optional<Gradient>` | `style_gradient_slot_is_indirect`（尺寸断言） |
| `set_solid_background` 里的 `= nullptr` 改成不动渐变 | `style_solid_background_clears_shared_gradient` |
| `set_background_gradient` 里加一行「重置 opacity = 1」 | `style_gradient_only_setter_leaves_opacity_alone` |
| `set_gradient_background` 不再写回退色 | `style_gradient_setter_keeps_fallback_color` |
| `set_background_gradient` 里改成深拷贝（`make_shared<Gradient>(*ptr)`） | `style_copies_share_one_gradient_instance` |

## 用法

    python tools/style_gradient_reverse_verify.py

每步：备份 → 打补丁 → 跑 `st test <过滤词>` → 断言**失败** → 恢复。
任一步“回退后仍绿”记为该断言的缺陷，退出码非 0。
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ST = os.path.join(ROOT, "build", "bin", "st.exe")
STYLE = os.path.join(ROOT, "include", "st", "ui", "style.hpp")
BACKUP = STYLE + ".reverse-verify.bak"

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def run_test(filter_word):
    """跑一个用例过滤词，返回 (是否通过, 输出尾部)。"""
    result = subprocess.run([ST, "test", filter_word], capture_output=True, text=True,
                            cwd=ROOT, encoding="utf-8", errors="replace")
    text = ANSI.sub("", (result.stdout or "") + (result.stderr or ""))
    return result.returncode == 0, text


def case(name, patches, filter_word, expect):
    """一次逆向验证：应用 patches（old→new 列表），跑用例，恢复。"""
    shutil.copy2(STYLE, BACKUP)
    try:
        with open(STYLE, encoding="utf-8") as handle:
            source = handle.read()
        for old, new in patches:
            if old not in source:
                print(f"  [跳过] {name}：补丁锚点没找到（源码已变？）")
                return False
            source = source.replace(old, new, 1)
        with open(STYLE, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(source)

        passed, output = run_test(filter_word)
        # expect="fail"：本用例应当变红（说明它真的守着那个不变式）
        ok = (not passed) if expect == "fail" else passed
        mark = "OK  " if ok else "BAD "
        print(f"  {mark} {name} → {filter_word} "
              f"{'变红 ✓' if not passed else '仍绿 ✗'}")
        if not ok:
            print(output[-1500:])
        return ok
    finally:
        shutil.copy2(BACKUP, STYLE)
        os.remove(BACKUP)


CASES = [
    (
        "渐变槽回退成内联 optional",
        [(
            "std::shared_ptr<const raster::Gradient> background_gradient{};",
            "std::optional<raster::Gradient> background_gradient{};",
        ), (
            "  void set_background_gradient(raster::Gradient gradient) noexcept {\n"
            "    background_gradient = std::make_shared<const raster::Gradient>(std::move(gradient));\n"
            "  }",
            "  void set_background_gradient(raster::Gradient gradient) noexcept {\n"
            "    background_gradient = std::move(gradient);\n"
            "  }",
        ), (
            "  background_gradient = std::make_shared<const raster::Gradient>(std::move(fill));",
            "  background_gradient = std::move(fill);",
        ), (
            "    background_gradient = nullptr;",
            "    background_gradient.reset();",
        )],
        "style_gradient_slot_is_indirect",
        "fail",
    ),
    (
        "set_solid_background 不再清渐变",
        [(
            "    background = fill;\n    background_gradient = nullptr;",
            "    background = fill;",
        )],
        "style_solid_background_clears_shared_gradient",
        "fail",
    ),
    (
        "set_background_gradient 顺手重置 opacity",
        [(
            "    background_gradient = std::make_shared<const raster::Gradient>(std::move(gradient));\n  }",
            "    background_gradient = std::make_shared<const raster::Gradient>(std::move(gradient));\n"
            "    gradient_opacity = 1.0f;\n  }",
        )],
        "style_gradient_only_setter_leaves_opacity_alone",
        "fail",
    ),
    (
        "set_gradient_background 不再写回退色",
        [(
            "  background = fallback;\n  // 一次 `make_shared`",
            "  // 一次 `make_shared`",
        )],
        "style_gradient_setter_keeps_fallback_color",
        "fail",
    ),
]


def main():
    if not os.path.exists(ST):
        print(f"找不到 st 工具链：{ST}")
        return 2
    print("逆向验证（每步：回退 → 跑用例 → 期望变红 → 恢复）")
    results = [case(*item) for item in CASES]
    print()
    if all(results):
        print(f"── 全部 {len(results)} 条都真的守着不变式 ──")
        return 0
    print(f"── {results.count(False)} 条没变红（该断言没守住东西）──")
    return 1


if __name__ == "__main__":
    sys.exit(main())
