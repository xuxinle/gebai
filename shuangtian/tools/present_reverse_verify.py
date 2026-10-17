#!/usr/bin/env python3
"""上屏路径改动的**逆向验证**：逐条回退修复 → 断言必须变红 → 恢复。

为什么必须做（`CONVENTIONS.md` §7.1/§7.3）：不做这一步很容易交付一个"恒绿的测试"——
它看起来像护栏，实际什么都拦不住。本项目已实测踩到过两次高危变体：
  - 桩把差异抹平了（§7.2）；
  - 回退文本编译不过（`-Werror` 打断编译）⇒ 跑的是上一份二进制 ⇒ **假绿**（§7.3）。

因此本脚本的每条用例都：
  ① 用**能编译过**的最小改动回退（保留 `(void)var;` 之类）；
  ② 备份用 `shutil.copy` + 显式 `os.utime` 打破 mtime 相等（`copy2` 会把旧 mtime
     搬回来，比回退版编出的 `.o` 还旧 → 跑的是上一份二进制）；
  ③ 先确认新二进制真的编出来了（比对 exe mtime），再判红绿。

用法：
    python3 tools/present_reverse_verify.py            # 全部用例
    python3 tools/present_reverse_verify.py --list
"""
from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
ST = ROOT / "build/bin/st"
EXE = ROOT / "build/dev/bin/st_tests"


class Case:
    def __init__(self, name, path, old, new, tests, note=""):
        self.name = name
        self.path = ROOT / path
        self.old = old
        self.new = new
        self.tests = tests
        self.note = note


CASES = [
    Case(
        "转换的 alpha 标记", "src/raster/simd.cpp",
        "    for (; index < count; ++index) dst[index] = ((src[index] >> 8U) & 0x00FFFFFFU) | 0xFF000000U;",
        "    for (; index < count; ++index) dst[index] = ((src[index] >> 8U) & 0x00FFFFFFU) | 0x00000000U;",
        ["simd_convert_rgba_to_xrgb_matches_reference"],
        "丢掉不透明标记 → 清屏像素在 depth 32 可视化下会变透明/黑边",
    ),
    Case(
        "向外取整（右/下缘）", "src/shell/backend.cpp",
        "  const int right = clamp_axis(\n"
        "      static_cast<int>(std::ceil(static_cast<double>(region.x + region.width) * factor)), width);\n"
        "  const int bottom = clamp_axis(\n"
        "      static_cast<int>(std::ceil(static_cast<double>(region.y + region.height) * factor)), height);",
        "  const int right = clamp_axis(\n"
        "      static_cast<int>(std::floor(static_cast<double>(region.x + region.width) * factor)), width);\n"
        "  const int bottom = clamp_axis(\n"
        "      static_cast<int>(std::floor(static_cast<double>(region.y + region.height) * factor)), height);",
        ["present_region_scales_outward_and_clamps"],
        "向内取整会漏掉右/下边缘（非整数 DPI 下一条边整块消失）",
    ),
    Case(
        "空区即整屏", "src/shell/backend.cpp",
        "  if (region.is_empty()) return math::IntRect{0, 0, width, height};",
        "  if (region.is_empty()) return math::IntRect{};",
        ["present_region_empty_means_full_canvas"],
        "把“没说改哪里”解释成“哪都没改” → 首帧/曝光后整屏不上屏",
    ),
    Case(
        "区域夹取到画布内", "src/shell/backend.cpp",
        "  const int left = clamp_axis(static_cast<int>(std::floor(static_cast<double>(region.x) * factor)),\n"
        "                              width);\n"
        "  const int top = clamp_axis(static_cast<int>(std::floor(static_cast<double>(region.y) * factor)),\n"
        "                             height);",
        # ⚠ 回退必须**能编译过**（`-Werror` 打断编译 ⇒ 跑的是上一份二进制 ⇒ 假绿，§7.3）：
        # 去掉 clamp 后 lambda 变未使用，要保留一句 `(void)clamp_axis;`。
        "  (void)clamp_axis;\n"
        "  const int left = static_cast<int>(std::floor(static_cast<double>(region.x) * factor));\n"
        "  const int top = static_cast<int>(std::floor(static_cast<double>(region.y) * factor));",
        ["present_region_scales_outward_and_clamps"],
        "去掉夹取 → 越界区会产生负偏移/越界写",
    ),
    # ⚠ **一条被逆向验证判死并删掉的用例**（记在这里，别再加回来）：
    #   曾经有一条“双缓冲交替（把 `(active_ + 1) % count_` 回退成 `0`）”的用例，
    #   逆向验证判它是**假绿**——因为轮转只决定“先试哪一块”，而选槽的**安全**靠的是
    #   随后的 `in_flight` 检查（回退后 0 号在途时仍会落到 1 号），两个实现行为等价。
    #   真正承重的那条是下面的“在途时不得覆写”。
    Case(
        "在途时不得覆写（无空闲槽要报 -1）",
        "include/st/shell/present_slots.hpp",
        "    if (!slots_[static_cast<std::size_t>(preferred)].in_flight) return preferred;\n"
        "    for (int index = 0; index < count_; ++index) {\n"
        "      if (!slots_[static_cast<std::size_t>(index)].in_flight) return index;\n"
        "    }\n"
        "    return -1;",
        # 回退：不看 in_flight 直接返回轮转槽 —— 覆写服务端正读的段（撕裂）
        "    return preferred;",
        ["present_slot_ring_alternates_between_buffers",
         "present_slot_ring_single_buffer_blocks_without_completion"],
        "无视在途标记：覆写服务端正读的段 = 撕裂（这条最贵的缺陷）",
    ),
    Case(
        "mark_idle 幂等（计数不得减成负）",
        "include/st/shell/present_slots.hpp",
        "    if (!slots_[static_cast<std::size_t>(slot)].in_flight) return;\n"
        "    slots_[static_cast<std::size_t>(slot)].in_flight = false;\n"
        "    if (in_flight_ > 0) --in_flight_;",
        # 回退：无条件递减（重复的完成事件/已统一放开后再来一枚 → 计数变负）
        "    (void)in_flight_;\n"
        "    slots_[static_cast<std::size_t>(slot)].in_flight = false;",
        ["present_slot_ring_mark_idle_is_idempotent"],
        "在途计数变负 → “还有段在被读”的判据恒假 → 无保护覆写",
    ),
    Case(
        "降级到单块要把状态一并重置",
        "include/st/shell/present_slots.hpp",
        "  void degrade_to_single() noexcept { reset(1); }",
        # 回退：只改槽数、不清在途标记 —— 降级后第一帧就没槽可用（卡住一帧）
        "  void degrade_to_single() noexcept { count_ = 1; }",
        ["present_slot_ring_degrade_to_single_keeps_one_slot"],
        "降级后仍报“在途”，第一帧拿不到槽（要么丢帧要么卡）",
    ),
    Case(
        "降级判据：从未见过才降",
        "include/st/shell/present_slots.hpp",
        "  if (completion_seen_now) return SyncVerdict::Continue;\n"
        "  return completions_seen_total == 0 ? SyncVerdict::Degrade : SyncVerdict::Continue;",
        # 回退：只要这次没收到就降级 —— 一次抖动就永久丢掉双缓冲（“看起来能用、
        # 实际每帧都在排空”）。或者反过来恒 Continue（永不降级）会无保护覆写。
        "  (void)completions_seen_total;\n"
        "  return completion_seen_now ? SyncVerdict::Continue : SyncVerdict::Degrade;",
        ["present_sync_verdict_degrades_only_when_never_seen"],
        "一次抖动就永久降级：双缓冲名存实亡",
    ),
    Case(
        "reset 要清在途计数",
        "include/st/shell/present_slots.hpp",
        "    active_ = -1;\n"
        "    in_flight_ = 0;\n"
        "    for (auto& slot : slots_) slot.in_flight = false;",
        # 回退：忘清计数（换尺寸后残留）—— 新缓冲一上来就“满载”
        "    active_ = -1;\n"
        "    for (auto& slot : slots_) slot.in_flight = false;",
        ["present_slot_ring_degrade_to_single_keeps_one_slot"],
        "换尺寸/降级后残留旧的“在途”计数：新缓冲一上来就没槽可用",
    ),
]


def build() -> bool:
    """编译并重链测试二进制。

    ⚠ **必须走 `st test <过滤器>` 而不是 `st build gallery`**：后者只建指定目标、
    **不重链 `st_tests`**（见 `docs/PITFALLS.md` 场景 1「不链测试二进制」）——
    那会让回退后的断言跑在**旧二进制**上，是假绿的经典成因。
    `st test` 无重编时也会无条件 `compile_units`+`link`（mtime 增量）。

    ⚠ **档位必须与 `run()` 读的产物一致**：`st test` 默认是 **debug** 档，
    而本脚本读 `build/dev/bin/st_tests`——不显式指定 `--profile dev` 就会
    “编译 debug × 运行 dev = 永远读到上一次的旧二进制”（本轮实测踩到：
    四条全报“假绿/无效”，而单跑二进制明明红）。
    """
    proc = subprocess.run([str(ST), "test", "--profile", "dev", "present_region"],
                          cwd=ROOT, capture_output=True, text=True, timeout=1800)
    # ⚠ **不能用返回码判“编译成功”**：`st test` 有失败用例时也返回非 0，
    # 而那正是我们要的“变红”。判据只能看输出里有没有编译失败。
    output = proc.stdout + proc.stderr
    if "编译失败" in output or "compile error" in output.lower():
        print(output[-2000:])
        return False
    if "运行测试" not in output:
        print(output[-2000:])
        return False
    return True


def run(tests: list[str]) -> tuple[int, int]:
    proc = subprocess.run([str(EXE), *tests], capture_output=True, text=True, timeout=600)
    # ⚠ 输出带 ANSI 颜色码（`\x1b[32mPASS\x1b[0m …`）——不剥掉的话
    # `startswith("PASS ")` 永远不成立，于是每条都报“全绿”＝**恒假的假绿判据**
    # （本轮实测踩到：四条全报 0 passed，而单跑二进制明明是红的）。
    ansi = re.compile("\\x1b\\[[0-9;]*m")
    passed = failed = 0
    for line in proc.stdout.splitlines():
        stripped = ansi.sub("", line).strip()
        if stripped.startswith("PASS "):
            passed += 1
        elif stripped.startswith("FAIL "):
            failed += 1
    return passed, failed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.list:
        for case in CASES:
            print(f"{case.name:24} {case.path.relative_to(ROOT)}  → {case.tests}")
        return 0

    if not build():
        print("基线构建失败：先修好再跑逆向验证")
        return 2
    failures = []
    for case in CASES:
        source = case.path.read_text(encoding="utf-8")
        if case.old not in source:
            print(f"[跳过] {case.name}: 回退锚点未命中（代码已变，请更新脚本）")
            failures.append(case.name)
            continue
        backup = case.path.with_suffix(case.path.suffix + ".revbak")
        shutil.copy(case.path, backup)
        try:
            case.path.write_text(source.replace(case.old, case.new, 1), encoding="utf-8")
            # 打破 mtime 相等：写入后显式 touch 到"现在"，保证增量构建按 mtime 判定会重编。
            now = time.time()
            import os
            os.utime(case.path, (now, now))
            if not build():
                # `-Werror` 打断编译 ⇒ 跑的是上一份二进制 ⇒ **假绿**（§7.3）。
                # 因此编译失败一律判"本条无效"，不算通过。
                print(f"[无效] {case.name}: 回退版本编译失败（不许当通过——那会跑旧二进制）")
                failures.append(case.name)
                continue
            passed, failed = run(case.tests)
            if failed == 0 and passed == 0:
                # 过滤没命中（用例名写错/没注册）⇒ 没跑起来，不算通过。
                print(f"[无效] {case.name}: 一条都没跑起来（过滤没命中？）—— 不算通过")
                failures.append(case.name)
            elif failed > 0:
                print(f"[通过] {case.name}: 回退后 {passed} passed / {failed} failed —— 测试真能抓住（{case.note}）")
            else:
                print(f"[假绿] {case.name}: 回退后仍全绿（{passed} passed）—— 测试拦不住这个缺陷")
                failures.append(case.name)
        finally:
            # 恢复：**先删再写**（`copy2` 会把旧 mtime 搬回来，比回退版编出的 .o 还旧）
            case.path.unlink()
            shutil.copy(backup, case.path)
            backup.unlink()
            now = time.time()
            import os
            os.utime(case.path, (now, now))
    if not build():
        print("恢复后构建失败——工作树可能已损坏，请检查 git status")
        return 2
    if failures:
        print(f"\n{len(failures)} 条未通过逆向验证：{', '.join(failures)}")
        return 1
    print(f"\n全部 {len(CASES)} 条逆向验证通过（每条都真能抓住它对应的缺陷）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
