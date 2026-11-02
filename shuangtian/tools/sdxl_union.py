"""SDXL 动漫模型 + ControlNet Union（OpenPose 模式）生成动作立绘。

## 为什么是这条路线（走了三圈才定下来）

| 组合 | 画质 | 姿势控制 | 结论 |
|---|---|---|---|
| SDXL + Canny | ★★★ | ✗ 基本不管用 | 淘汰 |
| SD1.5 + OpenPose | ★☆ | ✓ 很准 | 姿势对，画质差 |
| **SDXL + Union(pose)** | ★★★ | ✓ 准 | **目标** |

SD1.5 的 OpenPose 实验证明了"骨架约束这条路是对的"（手真的伸出来了），
只是 SD1.5 底模画质拖后腿。换成 SDXL 的 Union ControlNet，两边好处都占。

## Union 模型的特殊之处

`controlnet-union-sdxl-1.0` 是一个模型支持多种控制模式
（openpose / canny / depth / normal / tile），靠 **`control_mode`** 参数切换。
默认 `control_type` 是自动识别，但我们明确要 pose，所以显式指定。

## 关键参数

- `controlnet_conditioning_scale`：SDXL 的 ControlNet 在 **0.6~0.9** 最稳
  （比 SD1.5 低——SDXL 本身语义更强，权重太高会把画面压成线稿）。
- `guidance_scale`：5.0 左右。太高会让模型"用力过猛"，压低控制信号。
- OpenPose 图必须是 **COCO-18 彩色骨架**（见 `tools/make_openpose.py`）。
"""
import argparse
import json
import os
import sys
import time

# 路径可用环境变量覆盖（`GEBAI_SDXL_MODEL` / `GEBAI_SDXL_CONFIG`）。
# 不写死绝对路径的原因：这些脚本要能在别人的机器上跑。

SDXL_MODEL = os.environ.get("GEBAI_SDXL_MODEL",
    r"C:\Users\Administrator\Downloads\PetalRain亚洲动漫-Illustrious_V2.safetensors")
SDXL_CONFIG = os.environ.get("GEBAI_SDXL_CONFIG",
    r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo")
CONTROLNET_DIR = "build/cn-union"

DONUT_LOOK = (
    "1boy, young man, navy blue baseball cap, red track jacket with white stripes, "
    "navy blue track pants, white sneakers"
)
HUSHENG_LOOK = (
    "1boy, middle-aged man, short black hair, gold-rimmed glasses, grey polo shirt, "
    "dark grey slacks, black leather shoes"
)

STYLE = "full body, simple background, solid white background, clean lineart, anime style"

NEGATIVE = (
    "lowres, bad anatomy, bad hands, text, error, missing fingers, extra digit, fewer digits, "
    "extra limbs, extra legs, extra arms, mutated hands, cropped, worst quality, low quality, "
    "jpeg artifacts, signature, watermark, blurry, multiple views, multiple people, "
    "out of frame, cut off, close-up, portrait, background scenery, gradient background, "
    "pattern, texture background, complex background, barefoot, sandals, long hair"
)

CHARACTERS = {
    "donut": {"look": DONUT_LOOK, "seed": 42},
    "husheng": {"look": HUSHENG_LOOK, "seed": 1337},
}


def build_pipeline():
    import torch
    from diffusers import (ControlNetModel, EulerAncestralDiscreteScheduler,
                           StableDiffusionXLControlNetPipeline)

    weight = os.path.join(CONTROLNET_DIR, "diffusion_pytorch_model.safetensors")
    if not os.path.exists(weight):
        print(f"ControlNet 权重不存在：{weight}", file=sys.stderr)
        return None

    # ⚠ ControlNet 的 config 必须**显式读 json 再构造**。
    # 用 `from_single_file(config=目录)` 会静默得到"能用但没用"的空壳
    # （之前 SDXL Canny 那轮栽过：推理不报错，姿势完全不受控）。
    print("加载 Union ControlNet…", flush=True)
    with open(os.path.join(CONTROLNET_DIR, "config.json"), encoding="utf-8") as handle:
        cn_config = json.load(handle)
    controlnet = ControlNetModel.from_config(cn_config)
    from safetensors.torch import load_file
    state = load_file(weight)
    missing, unexpected = controlnet.load_state_dict(state, strict=False)
    controlnet = controlnet.to(torch.float16)
    print(f"  加载 {len(state)} 个张量，缺失 {len(missing)}，多余 {len(unexpected)}", flush=True)
    if missing:
        print(f"  缺失示例：{missing[:3]}", flush=True)

    print("加载 SDXL 动漫模型…", flush=True)
    started = time.time()
    pipe = StableDiffusionXLControlNetPipeline.from_single_file(
        SDXL_MODEL, config=SDXL_CONFIG, controlnet=controlnet,
        torch_dtype=torch.float16, use_safetensors=True,
    )
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)
    print(f"就绪 {time.time() - started:.1f}s", flush=True)
    return pipe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--poses", default="build/char-gen/openpose")
    parser.add_argument("--outdir", default="build/char-gen/union-out")
    parser.add_argument("--characters", default="donut")
    parser.add_argument("--only", default="")
    parser.add_argument("--steps", type=int, default=28)
    parser.add_argument("--cfg", type=float, default=5.0)
    parser.add_argument("--control-scale", type=float, default=0.75)
    parser.add_argument("--width", type=int, default=832)
    parser.add_argument("--height", type=int, default=1216)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    poses = sorted(name[9:-4] for name in os.listdir(args.poses)
                   if name.startswith("openpose-") and name.endswith(".png"))
    if args.only:
        wanted = {x.strip() for x in args.only.split(",") if x.strip()}
        poses = [p for p in poses if p in wanted]
    if not poses:
        print("没有找到 OpenPose 骨架图", file=sys.stderr)
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
            if os.path.exists(out) and not args.force:
                print(f"[{done}/{total}] 跳过 {out}", flush=True)
                continue

            source = os.path.join(args.poses, f"openpose-{pose_name}.png")
            pose_image = Image.open(source).convert("RGB")
            pose_image = pose_image.resize((args.width, args.height), Image.LANCZOS)

            prompt = f"{spec['look']}, {STYLE}"
            started = time.time()
            generator = torch.Generator(device="cuda").manual_seed(spec["seed"])
            image = pipe(
                prompt=prompt,
                negative_prompt=NEGATIVE,
                image=pose_image,
                controlnet_conditioning_scale=args.control_scale,
                control_mode=0,          # 0 = openpose（union 模型的多模式选择）
                width=args.width,
                height=args.height,
                num_inference_steps=args.steps,
                guidance_scale=args.cfg,
                generator=generator,
            ).images[0]
            image.save(out)
            print(f"[{done}/{total}] {os.path.basename(out)}  {time.time() - started:.1f}s",
                  flush=True)
            manifest.append({"character": character, "action": pose_name,
                             "path": out, "prompt": prompt,
                             "control_scale": args.control_scale})

    with open(os.path.join(args.outdir, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
    print(f"完成 {len(manifest)} 张 -> {args.outdir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
