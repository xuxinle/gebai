#!/usr/bin/env python3
"""逆向验证（图标尺寸与排版）：把本轮修复逐条回退，确认回归用例**真的变红**。

为什么必须做：一个"恒绿"的测试看起来像护栏，实际什么都拦不住。
本轮尤其需要——三条修复里有**两条第一版护栏是假的**（实测踩到，见下）。

覆盖（每条：回退 → 断言变红 → 恢复 → 断言变绿）：
  ① 光学归一化按**几何**而不扣描边外扩 → `ui_icon_stroked_and_filled_...` 必须红；
  ② `Icon::ink_size` 的描边项多乘一个 `fit` → `ui_icon_ink_size_matches_...` 必须红；
  ③ `Button` 排版按**盒宽**而非墨迹宽 → `button_icon_and_text_together_are_centered` 必须红；
  ④ `Button` 绘制不按墨迹左缘对齐 → 同上必须红。

## 本轮实测踩到的四个坑（都写在代码注释里，这里列一份便于自查）

1. **`st build gbcode` 不会重编测试的 `.o`**：脚本里用它触发重编，测试跑的是
   上一份对象，四条回退全报"仍绿"——差一点误判成"护栏无效"。必须用
   `st build --profile dev --force`（**不带 target**、全量强制）逼出真重编。
2. **增量靠 mtime，同源第二次改动不被拾取**：先删 `.o` 再走增量时，
   第一条回退生效、第二条失效（时红时绿）。恢复写入的 mtime 与回退版太接近。
   所以用 `--force`（与仓内其它 `*_reverse_verify.py` 同口径）。
3. **`re.search` 取第一条汇总会取到空分片**：分片输出里有几十条
   `N passed, M failed`，第一条几乎总是 `0 passed, 0 failed`。要**取全部并累加**，
   并且“全绿”必须要求 `passed > 0`（否则“一个都没跑”也算绿）。
   另：默认多分片并行时个别分片会在重编窗口里跑，结论不稳定——带 `--test-jobs 1`。
4. **回退写法若编译不过**（`-Werror`）→ 跑的是上一份二进制 → 假绿。见
   `CONVENTIONS` §7.3。本脚本显式报出这种情况。
5. **判据量不到差异**：`ui_icon_ink_size_matches_what_is_drawn` 起初在 64px 盒里
   量（误差 ~0.8px 与抗锯齿噪声 ~0.7px 同量级）→ 回退后仍绿。**放大到 256px**
   让被测差（~3px）超过噪声后才有判别力。
6. **写文件不关闭**：`io.open(p, "w").write(s)` 不保证在下一句 `subprocess.run`
   前落盘，重编会读到旧内容（表现为“恢复后仍红”的假异常）。一律用 `with`。

用法：python3 tools/icon_metrics_reverse_verify.py
"""

import io
import os
import re
import subprocess
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ICON_CPP = "src/ui/icon.cpp"
BASIC_CPP = "src/ui/components/basic.cpp"
ST = os.path.join("build", "bin", "st.exe")

CASES = [
    (
        "① 光学归一化按几何（不扣描边外扩）",
        ICON_CPP,
        "icon",
        "  const float stroke_units = glyph.filled ? 0.0f : glyph.stroke;",
        "  const float stroke_units = 0.0f;  // REV（回退：忽略描边外扩）",
        1,
        "ui_icon_stroked_and_filled_render_at_the_same_size",
    ),
    (
        "② ink_size 的描边项多乘 fit",
        ICON_CPP,
        "icon",
        "  const float stroke_extent = glyph->filled ? 0.0f : glyph->stroke * scale;",
        "  const float stroke_extent = glyph->filled ? 0.0f : glyph->stroke * scale * fit;  // REV",
        1,
        "ui_icon_ink_size_matches_what_is_drawn",
    ),
    (
        "③ Button 排版按盒宽（而非墨迹宽）",
        BASIC_CPP,
        "basic",
        "    width += icon_ink_size().width;",
        "    width += icon_size_;  // REV（回退：按盒宽排版）",
        1,
        "button_icon_and_text_together_are_centered",
    ),
    (
        "④ Button 绘制不按墨迹左缘对齐",
        BASIC_CPP,
        "basic",
        "    const float icon_left = cursor - (icon_size - icon_ink) * 0.5f;",
        "    const float icon_left = cursor;  // REV（回退：不做墨迹对齐）",
        2,
        "button_icon_and_text_together_are_centered",
    ),
]


def purge_objects(stem):
    """删掉该源在两档下的 .o，逼出真重编（mtime 不可靠）。"""
    for profile in ("dev", "debug"):
        path = os.path.join("build", profile, "obj")
        if not os.path.isdir(path):
            continue
        for name in os.listdir(path):
            if stem in name and (name.endswith(".o") or name.endswith(".o.d")):
                try:
                    os.remove(os.path.join(path, name))
                except OSError:
                    pass
    for profile in ("dev", "debug"):
        exe = os.path.join("build", profile, "bin", "st_tests.exe")
        try:
            os.remove(exe)
        except FileNotFoundError:
            pass


def build_and_test(stem, case_name):
    """强制全量重编后跑用例；返回 (编译是否成功, 是否全绿, 诊断字符串)。

    ⚠ 与仓内其它 `*_reverse_verify.py` 同口径：`st build --profile dev --force`
    **不带 target**（全量强制重编），再 `st test --test-jobs 1` 跑单用例。

    为何必须 `--force` + 全量（本脚本实测踩到）：只删目标 `.o` 再走增量时，
    **同一个源文件的第二次改动不被拾取**（表现为同一条回退时红时绿——
    四条回退里第一条生效、第二条失效）。增量靠 mtime，而恢复写入的 mtime
    与回退版太接近，`st` 判定“无需重编”。

    为何 `--test-jobs 1`：默认多分片并行时个别分片会在重编窗口里跑，
    读到不完整的产物，结论不稳定。
    """
    build = subprocess.run([ST, "build", "--profile", "dev", "--force"],
                           capture_output=True, text=True, errors="replace")
    if build.returncode != 0:
        return False, False, "编译错误"
    proc = subprocess.run([ST, "test", "--profile", "dev", case_name, "--test-jobs", "1"],
                          capture_output=True, text=True, errors="replace")
    out = proc.stdout + proc.stderr
    if "error:" in out:
        return False, False, "编译错误"
    # ⚠ 取**全部**汇总行并累加：第一条几乎总是空分片的 `0 passed, 0 failed`。
    totals = re.findall(r"(\d+) passed, (\d+) failed", out)
    if not totals:
        return True, False, "无汇总行"
    passed = sum(int(p) for p, _ in totals)
    failed = sum(int(f) for _, f in totals)
    # ⚠ “全绿”必须要求**真的跑了**：passed > 0。只用 failed==0 会把
    # “一个用例都没跑”误判成绿。
    green = failed == 0 and passed > 0
    return True, green, f"passed={passed} failed={failed} rc={proc.returncode}"


def main() -> int:
    if not os.path.exists(ICON_CPP):
        print(f"找不到 {ICON_CPP}（请在框架根运行）", file=sys.stderr)
        return 2
    problems = 0
    results = []
    for index, (desc, path, stem, fixed, broken, count, case) in enumerate(CASES, 1):
        print(f"\n=== [{index}/{len(CASES)}] 回退：{desc}")
        print(f"    期望变红: {case}")
        text = io.open(path, encoding="utf-8").read()
        found = text.count(fixed)
        if found != count:
            print(f"    ⚠ 原文出现 {found} 次（期望 {count}）——源码已变？跳过")
            problems += 1
            continue
        try:
            # ⚠ 必须用 `with` 显式关闭：`io.open(...).write(s)` 不保证在下一句
            # `subprocess.run` 前落盘——缓冲区未清时重编读到的是**旧内容**，
            # 表现为“恢复后仍红”（本脚本实测踩到，两处假异常全源于此）。
            with io.open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(text.replace(fixed, broken, count))
            compiled, green, diag = build_and_test(stem, case)
            if not compiled:
                print("    ❌ 回退写法编译失败（该回退不可用，测试跑的是上一份二进制）")
                problems += 1
                results.append((desc, case, "回退版编不过"))
                continue
            print(f"    （重编后跑用例：{diag}）")
            if green:
                print("    ❌ 依然全绿 —— 该测试抓不住这个缺陷（假护栏）")
                problems += 1
                results.append((desc, case, "假护栏"))
            else:
                print("    ✅ 变红（测试确实抓得住）")
                results.append((desc, case, "变红 OK"))
        finally:
            with io.open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(text)
            compiled, green, diag = build_and_test(stem, case)
            state = "✅ 通过" if (compiled and green) else "❌ 仍红（恢复不完整）"
            print(f"    恢复后: {state}（{diag}）")
            if not (compiled and green):
                problems += 1

    print("\n=== 汇总 ===")
    for desc, case, state in results:
        print(f"  {state:<12} {case}   （{desc}）")
    print(f"\n结论: {'全部通过（每条修复都被真实抓住）' if problems == 0 else f'{problems} 处异常'}")
    return 0 if problems == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
