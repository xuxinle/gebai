"""用 ControlNet（Canny）按**姿势线稿**生成动作立绘。

## 为什么用 ControlNet

前两版都栽在同一件事上：**提示词控制不了精确姿势**。
`"throwing a straight punch"` 画出来是弯腰，`"one arm extended forward"`
只是"大概站着"。于是 34 张图里真正有动作感的没几张。

ControlNet 换了一条路：**画给它看**。我们拿游戏自己的 2D 骨架
（`tools/make_poses.py` 渲染的线稿）当条件，模型必须照着这个骨架放人。
姿势因此是**几何约束**，不是"语义暗示"。

## 参数怎么定

- `controlnet_conditioning_scale`：线稿的约束强度。太低（<0.5）姿势放开，
  太高（>0.9）画面僵成线稿本身。实测 0.7 左右平衡最好。
- `guidance_scale`：提示词自由发挥度。ControlNet 场景下取 5~6，
  配合"线稿已经定了姿势"，提示词只管角色外观。
- 线稿要**粗线**（22px）：细线在 ControlNet 里权重不足，会被当噪声。

## 与"一动作一图"的关系

ControlNet 让每张图的姿势可控，但这仍是**每个动作一张静态图**——
动作之间的过渡靠游戏里切图。要真正的连贯动画，得按骨架的
**每个关键帧**各出一张（那是"序列帧"路线，图量 ×3~5）。

本脚本先解决"姿势对不对"这个更基础的问题。
"""
import argparse
import json
import os
import sys
import time

# 路径可用环境变量覆盖（`GEBAI_SDXL_MODEL` / `GEBAI_SDXL_CONFIG`）。
# 不写死绝对路径的原因：这些脚本要能在别人的机器上跑。

MODEL = os.environ.get("GEBAI_SDXL_MODEL",
    r"C:\Users\Administrator\Downloads\PetalRain亚洲动漫-Illustrious_V2.safetensors")
SDXL_CONFIG = os.environ.get("GEBAI_SDXL_CONFIG",
    r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo")
CONTROLNET_DIR = r"C:\Users\Administrator\code\gebai\shuangtian\build\controlnet"

DONUT_LOOK = (
    "1boy, young man, navy baseball cap, red track jacket with white stripes, "
    "navy track pants, white sneakers"
)
HUSHENG_LOOK = (
    "1boy, middle-aged man, short black hair, gold-rimmed glasses, "
    "grey polo shirt, dark grey slacks, black leather shoes"
)
# ⚠ 风格词里**不能出现姿势词**（standing / pose / view 之类）。
# 实测：提示词写 "standing" 而线稿是"出拳"，两者打架时模型听了提示词——
# 出来的图是双手插兜站着，线稿的姿势完全没生效。
STYLE = "full body, white background, clean lineart, anime style, detailed"

NEGATIVE = (
    "lowres, bad anatomy, bad hands, text, error, missing fingers, extra digit, fewer digits, "
    "cropped, worst quality, low quality, jpeg artifacts, signature, watermark, blurry, "
    "multiple views, multiple people, out of frame, cut off, close-up, portrait, "
    "background scenery, barefoot, shoes off, long hair, extra limbs"
)

CHARACTERS = {
    "donut": {"look": DONUT_LOOK, "seed": 42},
    "husheng": {"look": HUSHENG_LOOK, "seed": 1337},
}


def build_pipeline():
    import torch
    from diffusers import (ControlNetModel, StableDiffusionXLControlNetPipeline,
                           EulerAncestralDiscreteScheduler)

    controlnet_path = os.path.join(CONTROLNET_DIR, "diffusion_pytorch_model.fp16.safetensors")
    config_path = os.path.join(CONTROLNET_DIR, "config.json")
    if not os.path.exists(controlnet_path):
        print(f"ControlNet 权重不存在：{controlnet_path}", file=sys.stderr)
        return None

    print("加载 ControlNet…", flush=True)
    started = time.time()
    # 从单文件 + 本地 config 构造（与主模型同样的离线策略）
    controlnet = ControlNetModel.from_single_file(
        controlnet_path, config=CONTROLNET_DIR, torch_dtype=torch.float16
    )

    print("加载主模型…", flush=True)
    pipe = StableDiffusionXLControlNetPipeline.from_single_file(
        MODEL, config=SDXL_CONFIG, controlnet=controlnet,
        torch_dtype=torch.float16, use_safetensors=True,
    )
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)
    print(f"就绪 {time.time() - started:.1f}s", flush=True)
    _ = config_path
    return pipe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--poses", default="build/char-gen/poses")
    parser.add_argument("--outdir", default="build/char-gen/cn")
    parser.add_argument("--steps", type=int, default=28)
    parser.add_argument("--cfg", type=float, default=4.0)
    parser.add_argument("--control-scale", type=float, default=0.95)
    parser.add_argument("--width", type=int, default=832)
    parser.add_argument("--height", type=int, default=1216)
    parser.add_argument("--only", default="")
    parser.add_argument("--characters", default="donut,husheng")
    args = parser.parse_args()

    poses = sorted(name[5:-4] for name in os.listdir(args.poses)
                   if name.startswith("pose-") and name.endswith(".png"))
    if args.only:
        wanted = {x.strip() for x in args.only.split(",") if x.strip()}
        poses = [p for p in poses if p in wanted]
    if not poses:
        print("没有找到姿势线稿", file=sys.stderr)
        return 1

    characters = [c.strip() for c in args.characters.split(",") if c.strip() in CHARACTERS]
    os.makedirs(args.outdir, exist_ok=True)

    pipe = build_pipeline()
    if pipe is None:
        return 1

    import torch
    from PIL import Image

    manifest = []
    total = len(poses) * len(characters)
    done = 0
    for character in characters:
        spec = CHARACTERS[character]
        for pose_name in poses:
            done += 1
            out = os.path.join(args.outdir, f"{character}-{pose_name}.png")
            if os.path.exists(out):
                print(f"[{done}/{total}] 跳过 {out}", flush=True)
                continue

            # ⚠ **必须做 Canny 边缘提取**。
            #
            # 这一步漏掉时症状是"ControlNet 好像没生效"：生成的立绘总是
            # 站着插兜，姿势完全不听线稿。原因不是权重没加载（实测 844 个张量
            # 0 缺失），而是 Canny ControlNet 训练时看的是 **2~3px 细边缘**，
            # 把"白底黑粗线"直接喂进去，它理解成"画面里有个黑色色块"。
            #
            # 做了 Canny 之后，粗线被提成单条细边缘，姿势跟随就正常了。
            import cv2
            import numpy as np

            raw = os.path.join(args.poses, f"pose-{pose_name}.png")
            gray = cv2.imread(raw, cv2.IMREAD_GRAYSCALE)
            edges = 255 - cv2.Canny(gray, 50, 150)   # Canny 黑底白线 → 白底黑线
            pose_image = Image.fromarray(edges).convert("RGB")
            pose_image = pose_image.resize((args.width, args.height), Image.LANCZOS)

            prompt = f"{spec['look']}, standing, {STYLE}"
            started = time.time()
            generator = torch.Generator(device="cuda").manual_seed(spec["seed"])
            image = pipe(
                prompt=prompt,
                negative_prompt=NEGATIVE,
                image=pose_image,
                controlnet_conditioning_scale=args.control_scale,
                width=args.width,
                height=args.height,
                num_inference_steps=args.steps,
                guidance_scale=args.cfg,
                generator=generator,
            ).images[0]
            image.save(out)
            print(f"[{done}/{total}] {character}-{pose_name}  {time.time() - started:.1f}s", flush=True)
            manifest.append({"character": character, "action": pose_name, "path": out,
                             "prompt": prompt, "control_scale": args.control_scale,
                             "seed": spec["seed"]})

    with open(os.path.join(args.outdir, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
    print(f"完成 {len(manifest)} 张 -> {args.outdir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
