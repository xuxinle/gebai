"""诊断 Union ControlNet 为什么没生效。

## 要区分的三件事

1. **权重没装进去**（空壳 ControlNet）→ 用"输入图对输出的影响"测：
   同一 prompt、同一 seed，一张空白图 vs 一张骨架图，输出应当**明显不同**。
   如果两张几乎一样 ⇒ ControlNet 根本没参与。
2. **权重装了但 scale 不够** → 扫 0.5 / 1.0 / 2.0，看姿势是否随 scale 变化。
3. **都正常但模型无视它** → 那就是"SDXL 动漫模型 + 通用 ControlNet"的
   兼容性问题（动漫模型的线稿/骨架先验与写实训练集差异大）。

## 判据：像素差

用 `visual_diff` 式的思路——直接算两张图的平均像素差。
差得小说明 ControlNet 没起作用，差得大说明它在起作用。
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
NEG = "lowres, worst quality, low quality, multiple people, cropped"


def main() -> int:
    import json

    import numpy as np
    import torch
    from diffusers import (ControlNetModel, EulerAncestralDiscreteScheduler,
                           StableDiffusionXLControlNetPipeline)
    from PIL import Image
    from safetensors.torch import load_file

    with open(os.path.join(CONTROLNET_DIR, "config.json"), encoding="utf-8") as handle:
        config = json.load(handle)
    controlnet = ControlNetModel.from_config(config)
    state = load_file(os.path.join(CONTROLNET_DIR, "diffusion_pytorch_model.safetensors"))
    missing, unexpected = controlnet.load_state_dict(state, strict=False)
    controlnet = controlnet.to(torch.float16)
    print(f"权重 {len(state)} 张量 / 缺失 {len(missing)} / 多余 {len(unexpected)}", flush=True)
    if unexpected:
        print(f"  多余示例（可能是 control_mode 相关分支）：{unexpected[:5]}", flush=True)

    pipe = StableDiffusionXLControlNetPipeline.from_single_file(
        MODEL, config=SDXL_CONFIG, controlnet=controlnet,
        torch_dtype=torch.float16, use_safetensors=True)
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)

    pose = Image.open("build/char-gen/openpose/openpose-light.png").convert("RGB").resize((832, 1216))
    blank = Image.new("RGB", (832, 1216), (0, 0, 0))

    os.makedirs("build/char-gen/diag", exist_ok=True)
    outputs = {}
    cases = [
        ("blank-scale1.0", blank, 1.0),
        ("pose-scale0.5", pose, 0.5),
        ("pose-scale1.0", pose, 1.0),
        ("pose-scale2.0", pose, 2.0),
    ]
    for name, image, scale in cases:
        started = time.time()
        generator = torch.Generator(device="cuda").manual_seed(42)
        result = pipe(
            prompt=f"{LOOK}, {STYLE}", negative_prompt=NEG, image=image,
            controlnet_conditioning_scale=scale, num_inference_steps=20,
            guidance_scale=5.0, width=832, height=1216, generator=generator,
        ).images[0]
        path = f"build/char-gen/diag/{name}.png"
        result.save(path)
        outputs[name] = np.asarray(result, dtype=np.float32)
        print(f"  {name}  {time.time() - started:.1f}s", flush=True)

    print("\n=== 像素差（相对 blank-scale1.0）===", flush=True)
    base = outputs["blank-scale1.0"]
    for name, array in outputs.items():
        if name == "blank-scale1.0":
            continue
        diff = float(np.abs(array - base).mean())
        print(f"  {name:20s} 平均差 = {diff:7.2f}  {'← 有影响' if diff > 8 else '← 几乎没影响'}", flush=True)

    print("\n=== pose 各 scale 之间 ===", flush=True)
    print(f"  0.5 vs 1.0 = {float(np.abs(outputs['pose-scale0.5'] - outputs['pose-scale1.0']).mean()):.2f}",
          flush=True)
    print(f"  1.0 vs 2.0 = {float(np.abs(outputs['pose-scale1.0'] - outputs['pose-scale2.0']).mean()):.2f}",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
