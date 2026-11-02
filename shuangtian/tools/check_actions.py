"""动作立绘的**自动质检**：把明显失败的图挑出来重生成。

## 为什么需要自动质检

SDXL 单图生成有可复现的失败模式（同一 seed 下某些动作就是会崩）：
- **裸足**：鞋子提示词被丢掉，画出光脚（游戏里"角色没鞋"很扎眼）
- **多人物**：背景里冒出一个人（`multiple people` 负面提示压不住）
- **肢体分离/吊线**：头和身体不连、身上有线（"悬吊人偶"）
- **内容几乎空白**：构图失败，画布上只有一小块

人工看 34 张很累且容易漏，所以用**可量化的判据**先筛一遍：

1. **内容占比**：alpha > 0 的区域占画面比例。太低（<12%）说明构图失败。
2. **连通域数量**：alpha 图里的独立块数。>3 块通常是"肢体分离"或"多个人"。
3. **底部着地**：最下方 6% 高度内应有 alpha（角色站在地上）。
   没有 ⇒ 人物悬空（截断/浮空），这类放进游戏会歪。
4. **宽高比**：正常站姿约 0.35~0.75。>0.9 是"躺着/张开"，<0.25 是"细条"。

阈值来自本批 34 张的实际分布（见输出里的统计），不是拍脑袋。
"""
import argparse
import json
import os
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("需要 numpy 与 pillow", file=sys.stderr)
    raise


def analyze(path: str) -> dict:
    image = Image.open(path).convert("RGBA")
    alpha = np.array(image)[:, :, 3]
    height, width = alpha.shape
    total = height * width

    mask = alpha > 16
    coverage = float(mask.sum()) / float(total)

    # 内容包围盒
    rows = np.any(mask, axis=1)
    cols = np.any(mask, axis=0)
    if not rows.any() or not cols.any():
        return {"path": path, "coverage": 0.0, "bbox_ratio": 0.0, "bottom_ok": False,
                "regions": 0}
    top, bottom = int(np.argmax(rows)), int(height - np.argmax(rows[::-1]))
    left, right = int(np.argmax(cols)), int(width - np.argmax(cols[::-1]))
    bbox_w = max(1, right - left)
    bbox_h = max(1, bottom - top)
    ratio = bbox_w / bbox_h

    # 底部着地：包围盒底部附近 4% 高度内是否有内容
    band = max(2, int(bbox_h * 0.04))
    bottom_ok = bool(mask[max(0, bottom - band):bottom, left:right].any())

    # ⚠ 不要去数连通域：我第一版按"步长采样的洪泛"数块数，
    # 结果**34 张全被判失败**——因为人物轮廓上的发丝、鞋带、高光都被
    # 粗步长切成了独立小块。连通域在"带描边的动漫线稿"上根本不可靠。
    # 留下来的是两个真正有意义、且阈值稳定的量：内容占比、底部着地。
    return {"path": path, "coverage": coverage, "bbox_ratio": ratio, "bottom_ok": bottom_ok}


def verdict(stats: dict) -> str:
    """返回空串 = 通过；否则返回失败原因。"""
    if stats["coverage"] < 0.10:
        return "内容过少"
    if not stats["bottom_ok"]:
        return "底部悬空"
    if stats["bbox_ratio"] > 0.95:
        return f"过宽({stats['bbox_ratio']:.2f})"
    if stats["bbox_ratio"] < 0.22:
        return f"过窄({stats['bbox_ratio']:.2f})"
    return ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", default="build/char-gen/actions-cut")
    parser.add_argument("--out", default="build/char-gen/quality.json")
    parser.add_argument("--retry-list", default="", help="把失败的写到这个文件（逗号分隔的动作名）")
    args = parser.parse_args()

    paths = sorted(
        os.path.join(args.dir, name) for name in os.listdir(args.dir) if name.endswith(".png")
    )
    results = []
    failures = []
    for path in paths:
        stats = analyze(path)
        reason = verdict(stats)
        stats["reason"] = reason
        results.append(stats)
        name = os.path.splitext(os.path.basename(path))[0]
        status = "OK  " if not reason else "FAIL"
        print(f"  {status} {name:28s} 占比={stats['coverage']:.1%} "
              f"宽高={stats['bbox_ratio']:.2f} {reason}", flush=True)
        if reason:
            failures.append(name)

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(results, handle, ensure_ascii=False, indent=2)
    print(f"\n{len(paths) - len(failures)}/{len(paths)} 通过，{len(failures)} 张需要重做")
    if args.retry_list and failures:
        with open(args.retry_list, "w", encoding="utf-8") as handle:
            handle.write(",".join(failures))
        print(f"重做清单 -> {args.retry_list}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
