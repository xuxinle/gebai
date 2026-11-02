"""用 inpainting 把户圣的**西装外套抹掉**，只留灰色 polo 衫。

## 为什么不用提示词

试过三轮，全部失败：

| 写法 | 结果 |
|---|---|
| `only a grey polo shirt` | 还是穿外套 |
| 负面压 `jacket, blazer, suit, coat, cardigan, vest, layered clothing` | 还是穿 |
| 换成 `short sleeve shirt, t-shirt style` | 还是穿 |

根因是训练数据先验：**中年男性 + polo 衫**在动漫/照片里几乎总是配一件外套，
模型把这个组合当成一个整体，词语级否定改不掉。

## 为什么 inpainting 能行

inpainting 不是"让模型不要画外套"，而是**把外套区域擦掉，让它按周围的
polo 衫材质重新补全**。模型的注意力被局限在那一小块，先验影响不到全局。

## 遮罩怎么来

外套是**深色**（近黑），polo 是浅灰。所以：
1. 在躯干区域找深色像素 → 那是外套；
2. 但深色也包含头发、鞋、阴影 ⇒ 用**连通性**限定：只保留与躯干中心连通的深色块；
3. 遮罩**向外扩 3~5px**，确保把外套边缘和阴影都盖住（只擦一半会留一圈深色描边）。
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

# 路径可用环境变量覆盖（`GEBAI_SDXL_MODEL` / `GEBAI_SDXL_CONFIG`）。
# 不写死绝对路径的原因：这些脚本要能在别人的机器上跑。

MODEL = os.environ.get("GEBAI_SDXL_MODEL",
    r"C:\Users\Administrator\Downloads\PetalRain亚洲动漫-Illustrious_V2.safetensors")
CONFIG_SOURCE = os.environ.get("GEBAI_SDXL_CONFIG",
    r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo")

INPAINT_PROMPT = (
    "light grey polo shirt with collar and buttons, plain shirt, flat fabric, "
    "clean anime lineart, white background"
)
INPAINT_NEGATIVE = (
    "jacket, blazer, suit, coat, cardigan, vest, outerwear, layered clothing, "
    "dark clothing, buttons row, pockets, lowres, blurry"
)


def build_mask(image: Image.Image, threshold: int = 128, expand: int = 6) -> Image.Image:
    """造遮罩：白色 = 要重绘（外套），黑色 = 保留（polo/裤子/皮肤）。

    只在**躯干区域**找，避免把头发和鞋也标进来。
    """
    from scipy import ndimage  # 局部导入：只有这个工具需要连通性分析

    array = np.asarray(image.convert("RGB")).astype(int)
    height, width, _ = array.shape

    # 躯干区域：画面上部 25%~62%（避开头部帽子与下半身的裤子/鞋）
    top, bottom = int(height * 0.24), int(height * 0.66)
    left, right = int(width * 0.18), int(width * 0.82)
    torso = array[top:bottom, left:right]

    # ⚠ 阈值别取太低（最初 88）：外套中间是深灰，亮度 90~120，
    # 于是只标出了**轮廓线**、没标出实体，inpainting 后外套变成半透明。
    # 128 能把外套整块吃进来，同时仍然排除浅灰的 polo（约 200+）。
    luminance = torso.mean(axis=2)
    dark = luminance < threshold

    # 连通性：只保留与躯干中心那一列连通的深色块。
    # 外套是横跨身体的一大块，而阴影/线条是零碎的，靠这个能分开。
    labeled, count = ndimage.label(dark)
    if count == 0:
        return Image.new("L", image.size, 0)
    center_column = labeled[:, (right - left) // 2]
    keep = set()
    for value in np.unique(center_column):
        if value != 0:
            keep.add(int(value))
    # 再补上"面积够大"的块（袖子可能不经过中线）
    sizes = ndimage.sum(dark, labeled, range(1, count + 1))
    for index, size in enumerate(sizes, start=1):
        if size > dark.size * 0.02:
            keep.add(index)

    mask = np.zeros_like(dark)
    for value in keep:
        mask |= labeled == value

    # 填孔洞：外套上的亮色纽扣/高光会在遮罩里留下小洞，
    # 不填的话 inpainting 会在衣服上留几块"没重绘的旧像素"。
    mask = ndimage.binary_fill_holes(mask)
    # 扩一圈：只擦一半会在原地留一道深色描边
    mask = ndimage.binary_dilation(mask, iterations=expand)

    full = np.zeros((height, width), dtype=np.uint8)
    full[top:bottom, left:right] = (mask * 255).astype(np.uint8)
    result = Image.fromarray(full)
    # 羽化：硬边在 inpainting 里会留一圈接缝
    return result.filter(ImageFilter.GaussianBlur(radius=2.5))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--indir", default="build/char-gen/final2")
    parser.add_argument("--outdir", default="build/char-gen/husheng-inpaint")
    parser.add_argument("--only", default="")
    parser.add_argument("--steps", type=int, default=32)
    parser.add_argument("--strength", type=float, default=0.95)
    parser.add_argument("--save-mask", action="store_true")
    args = parser.parse_args()

    paths = sorted(glob.glob(os.path.join(args.indir, "husheng-*.png")))
    if args.only:
        wanted = {x.strip() for x in args.only.split(",") if x.strip()}
        paths = [p for p in paths if os.path.basename(p)[8:-4] in wanted]
    if not paths:
        print("没有找到 husheng 图", file=sys.stderr)
        return 1

    import torch
    from diffusers import EulerAncestralDiscreteScheduler, StableDiffusionXLInpaintPipeline

    print("加载 inpainting 管线…", flush=True)
    # ⚠ 不能用 `AutoPipelineForInpainting`——那个工厂类没有 `from_single_file`。
    # 直接用 `StableDiffusionXLInpaintPipeline.from_single_file`：它接受普通
    # SDXL 权重（内部按 img2img + mask 工作），不需要专门的 inpainting 模型。
    pipe = StableDiffusionXLInpaintPipeline.from_single_file(
        MODEL, config=CONFIG_SOURCE, torch_dtype=torch.float16, use_safetensors=True)
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)

    os.makedirs(args.outdir, exist_ok=True)
    for path in paths:
        name = os.path.basename(path)
        image = Image.open(path).convert("RGB")
        mask = build_mask(image)
        covered = float((np.asarray(mask) > 128).mean())
        if args.save_mask:
            os.makedirs("build/masks", exist_ok=True)
            mask.save(os.path.join("build/masks", name))

        # 遮罩太小时不重绘（可能是这张图本来就没外套）
        if covered < 0.01:
            print(f"  {name}: 遮罩仅 {covered:.2%}，跳过", flush=True)
            image.save(os.path.join(args.outdir, name))
            continue

        started = __import__("time").time()
        generator = torch.Generator(device="cuda").manual_seed(7)
        result = pipe(
            prompt=INPAINT_PROMPT,
            negative_prompt=INPAINT_NEGATIVE,
            image=image,
            mask_image=mask,
            strength=args.strength,
            num_inference_steps=args.steps,
            guidance_scale=6.0,
            generator=generator,
        ).images[0]
        result.save(os.path.join(args.outdir, name))
        print(f"  {name}: 遮罩 {covered:.1%}  {__import__('time').time() - started:.1f}s",
              flush=True)

    print(f"完成 -> {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
