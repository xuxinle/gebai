"""验证 ControlNet 到底有没有生效。

## 要回答的问题

前两次生成（`control_scale` 0.72 / 0.95）产出的立绘都是"站着插兜"，
而线稿明确画的是"手臂水平前伸"。两种可能：

1. **ControlNet 没生效**（权重没挂上、或者 `image` 参数没传进去）；
2. **生效了但被提示词压过**（CFG 4.0 仍让"角色描述"主导了构图）。

区分办法：用一张**极端线稿**（大字型 T-pose，正常人不会自发站成那样），
扫几个 `control_scale`，看姿势什么时候屈服于线稿。
如果扫到 1.5 还是站着，那就是第 1 种。
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
CONTROLNET_DIR = "build/controlnet"

LOOK = "1boy, young man, navy baseball cap, red track jacket, navy track pants"
STYLE = "full body, white background, clean lineart, anime style"
NEG = "lowres, bad anatomy, worst quality, low quality, multiple people, cropped"


def main() -> int:
    import torch
    from diffusers import (ControlNetModel, EulerAncestralDiscreteScheduler,
                           StableDiffusionXLControlNetPipeline)
    from PIL import Image

    # ⚠ 关键：ControlNet 的 config 必须**显式读 json 再构造**。
    #
    # 实测：`ControlNetModel.from_single_file(权重, config=目录)` 会**静默**得到一个
    # "能用但没用"的 ControlNet——推理不报错，但线稿完全不约束姿势
    #（探针图里 scale 拉到 1.4 人物还是站着，只有线稿残影透在画面上）。
    # 原因是它把目录当成了 diffusers 的 model 目录去解析，字段对不上时用了默认值。
    import json
    with open(os.path.join(CONTROLNET_DIR, "config.json"), encoding="utf-8") as handle:
        cn_config = json.load(handle)
    controlnet = ControlNetModel.from_config(cn_config)
    from safetensors.torch import load_file
    state = load_file(os.path.join(CONTROLNET_DIR, "diffusion_pytorch_model.fp16.safetensors"))
    missing, unexpected = controlnet.load_state_dict(state, strict=False)
    controlnet = controlnet.to(torch.float16)
    print(f"ControlNet 权重：加载 {len(state)} 个张量，缺失 {len(missing)}，多余 {len(unexpected)}",
          flush=True)
    if missing:
        print(f"  缺失示例：{missing[:3]}", flush=True)
    pipe = StableDiffusionXLControlNetPipeline.from_single_file(
        MODEL, config=SDXL_CONFIG, controlnet=controlnet,
        torch_dtype=torch.float16, use_safetensors=True)
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)

    # ⚠ 必须做 **Canny 边缘提取**再把图喂给 Canny ControlNet。
    #
    # 实测：直接把"白底黑粗线"当条件图，ControlNet 会把它当成"要画的黑色形状"，
    # 生成结果里出现线稿残影（淡灰色十字透在画面上），而人物姿势完全不受控。
    # 原因是这个 ControlNet 训练时看的是 **2~3px 的细边缘**，
    # 而不是"粗线条的色块"。
    import cv2
    import numpy as np
    raw = cv2.imread("build/tpose-test.png", cv2.IMREAD_GRAYSCALE)
    edges = 255 - cv2.Canny(raw, 50, 150)   # Canny 输出黑底白线 -> 反转成白底黑线
    cv2.imwrite("build/tpose-canny.png", edges)
    print(f"边缘像素 {int((edges < 128).sum())}（原图黑像素 {int((raw < 128).sum())}）", flush=True)
    pose = Image.fromarray(edges).convert("RGB")

    os.makedirs("build/char-gen/cn-probe", exist_ok=True)
    for scale in (0.7, 1.0, 1.4):
        started = time.time()
        generator = torch.Generator(device="cuda").manual_seed(42)
        image = pipe(
            prompt=f"{LOOK}, {STYLE}", negative_prompt=NEG, image=pose,
            controlnet_conditioning_scale=scale, num_inference_steps=24,
            guidance_scale=4.0, width=832, height=1216, generator=generator,
        ).images[0]
        out = f"build/char-gen/cn-probe/tpose-{scale:.1f}.png"
        image.save(out)
        print(f"scale={scale:.1f}  {time.time() - started:.1f}s -> {out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
