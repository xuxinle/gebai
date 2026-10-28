"""探针（结论版）：到底需要多大色深，才能让渐变不生台阶？

前两次探针的教训：
  · 第一次以为"提高内部色深能消除台阶"——**错**（输出 8 位时逐位相同）
  · 第二次以为"抖动是解药"——**只对了一半**（1/255 跨 666px 时信息量不足，抖动也无处发力）

本探针回答真正的问题：**给定"平台长度上限"，需要多少位色深？**

判据：
  · `min_levels(span_px, max_platform)`：要让每个平台不超过 max_platform 像素，
    这段渐变至少要跨多少级色深
  · 再换算成每条通道的位数
"""
from __future__ import annotations

import math


def levels_needed(span_px: int, max_platform_px: int) -> int:
    """跨 span_px 的渐变，若要求每级不超过 max_platform_px，需要多少级。"""
    return math.ceil(span_px / max_platform_px)


def bits_for(levels: int) -> float:
    return math.log2(max(2, levels))


def main() -> None:
    print("=" * 90)
    print("问题：一段覆盖 N 像素的渐变，要让每个色阶平台不超过 P 像素，需要多少级色深？")
    print("=" * 90)
    print(f"{'渐变跨度':>10} {'目标平台':>10} {'需要级数':>10} {'每通道位数':>12} {'能否用 8 位':>12}")
    print("-" * 90)
    for span in (100, 300, 666, 1000, 2160):
        for plat in (4, 8, 16):
            need = levels_needed(span, plat)
            bits = bits_for(need)
            ok8 = "可以" if need <= 256 else f"不够（需 {bits:.1f} 位）"
            print(f"{span:>10} {plat:>10} {need:>10} {bits:>12.1f} {ok8:>12}")
        print()

    print("=" * 90)
    print("再看**幅度**：常见「微渐变」的实际跨级数")
    print("=" * 90)
    print("BACKLOG 记录的实测档：幅度 1/255、跨 666px")
    print(f"  → 整段只跨 {1} 级 → 无论多少位内部色深，最终 8 位输出必然是 "
          f"{666 // 2}px 上下一刀切两段。")
    print("  → 真正的解法是**加大幅度**（让整段跨 10~20 级）或**改用抖动+噪点**")
    print("    （但噪点是主动引入的纹理，与「平滑渐变」是两种观感）。")
    print()
    print("幅度 8/255、跨 666px（约 12px 一级）：")
    print(f"  → 8 位就有 {8 + 1} 级，每级约 {666 // 9}px，人眼在纯色背景上仍能看出带子")
    print()
    print("幅度 32/255、跨 666px（约 3px 一级）：")
    print(f"  → 8 位有 {32 + 1} 级，每级约 {666 // 33}px，**肉眼不可辨**")


if __name__ == "__main__":
    main()
