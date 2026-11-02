"""扫 ControlNet scale × CFG 组合，找"姿势准 + 画质不掉"的甜点。

## 为什么需要扫

单点试出来的结论不可靠。诊断脚本已确认 ControlNet **在起作用**
（空白图 vs 骨架图 像素差 20~35），但 scale=1.0 时姿势只是"略微贴近"，
scale=2.0 时明显朝骨架靠拢——说明**这个动漫模型需要更高的控制权重**
（写实模型 0.7 就够，动漫模型的先验更强、更顽固）。

同时要防另一头：scale 太高会把画面压成"线稿涂色"，画质崩掉。
所以要同时看两个量：**姿势贴合度** + **画质**。

## 姿势贴合度怎么量（不用人眼）

把生成的图与**骨架图**做对比不可行（画风完全不同）。改用**轮廓包围盒**：
- 骨架图的非黑区域包围盒（已知）
- 生成图抠掉白底后的包围盒

两者接近 ⇒ 人在画面里的位置/伸展程度与骨架一致。
这能抓住"手伸没伸出去"（出拳时包围盒明显更宽）。

## 画质怎么量

用 `Laplacian` 方差（清晰度）：太低说明糊成一片或过曝。
这项只做交叉检查，最终仍以人眼为准。
"""
import os
import sys
import time

# 路径可用环境变量覆盖（`GEBAI_SDXL_MODEL` / `GEBAI_SDXL_CONFIG`）。
# 不写死绝对路径的原因：这些脚本要能在别人的机器上跑。

MODEL = os.environ.get("GEBAI_SDXL_MODEL",
    r"C:\Users\Administrator\Downloads\PetalRain亚洲动漫-Illustrious_V2.safetensors")
SDXL_CONFIG = os.environ.get("GEBAI_SDXL_CONFIG",
    r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo")
CONTROLNET_DIR = "build/cn-union"

LOOK = "1boy, young man, navy blue baseball cap, red track jacket, navy track pants"
STYLE = "full body, simple background, solid white background, anime style"
NEG = ("lowres, bad anatomy, worst quality, low quality, multiple people, cropped, "
       "background scenery, gradient background, pattern")

# 扫这些组合
SCALES = [1.4, 1.8, 2.2]
CFGS = [3.5, 5.0]


def content_bbox(image) -> tuple[int, int, int, int]:
    """抠掉白底后的内容包围盒。"""
    import numpy as np

    array = np.asarray(image.convert("RGB"), dtype=np.int16)
    # 非白：任一通道明显低于 245
    mask = array.min(axis=2) < 235
    rows = np.any(mask, axis=1)
    cols = np.any(mask, axis=0)
    if not rows.any():
        return (0, 0, 0, 0)
    top, bottom = int(np.argmax(rows)), int(len(rows) - np.argmax(rows[::-1]))
    left, right = int(np.argmax(cols)), int(len(cols) - np.argmax(cols[::-1]))
    return (left, top, right - left, bottom - top)


def sharpness(image) -> float:
    """Laplacian 方差（清晰度；越高越锐，过高可能是噪声）。"""
    import numpy as np

    gray = np.asarray(image.convert("L"), dtype=np.float32)
    lap = np.abs(4 * gray[1:-1, 1:-1] - gray[:-2, 1:-1] - gray[2:, 1:-1]
                 - gray[1:-1, :-2] - gray[1:-1, 2:])
    return float(lap.var())


def main() -> int:
    import json

    import torch
    from diffusers import (ControlNetModel, EulerAncestralDiscreteScheduler,
                           StableDiffusionXLControlNetPipeline)
    from PIL import Image
    from safetensors.torch import load_file

    with open(os.path.join(CONTROLNET_DIR, "config.json"), encoding="utf-8") as handle:
        config = json.load(handle)
    controlnet = ControlNetModel.from_config(config)
    state = load_file(os.path.join(CONTROLNET_DIR, "diffusion_pytorch_model.safetensors"))
    controlnet.load_state_dict(state, strict=False)
    controlnet = controlnet.to(torch.float16)

    pipe = StableDiffusionXLControlNetPipeline.from_single_file(
        MODEL, config=SDXL_CONFIG, controlnet=controlnet,
        torch_dtype=torch.float16, use_safetensors=True)
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)

    pose = Image.open("build/char-gen/openpose/openpose-light.png").convert("RGB").resize((832, 1216))
    pose_box = content_bbox(pose)
    print(f"骨架图包围盒: x={pose_box[0]} y={pose_box[1]} w={pose_box[2]} h={pose_box[3]}", flush=True)

    os.makedirs("build/char-gen/sweep", exist_ok=True)
    rows = []
    for scale in SCALES:
        for cfg in CFGS:
            started = time.time()
            generator = torch.Generator(device="cuda").manual_seed(42)
            image = pipe(
                prompt=f"{LOOK}, {STYLE}", negative_prompt=NEG, image=pose,
                controlnet_conditioning_scale=scale, num_inference_steps=24,
                guidance_scale=cfg, width=832, height=1216, generator=generator,
            ).images[0]
            name = f"s{scale:.1f}-cfg{cfg:.1f}"
            image.save(f"build/char-gen/sweep/{name}.png")
            box = content_bbox(image)
            rows.append((name, scale, cfg, box, sharpness(image), time.time() - started))
            print(f"  {name:14s} bbox w={box[2]:4d} h={box[3]:4d}  清晰度={rows[-1][4]:6.1f}"
                  f"  {rows[-1][5]:.1f}s", flush=True)

    print("\n=== 汇总（骨架 bbox w={} h={}）===".format(pose_box[2], pose_box[3]), flush=True)
    print(f"{'组合':16s} {'bbox宽':>7s} {'bbox高':>7s} {'宽差':>7s} {'清晰度':>8s}", flush=True)
    for name, scale, cfg, box, sharp, _ in rows:
        print(f"{name:16s} {box[2]:7d} {box[3]:7d} {abs(box[2]-pose_box[2]):7d} {sharp:8.1f}",
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
