"""SD1.5 动漫模型 + OpenPose ControlNet 生成动作立绘。

## 为什么换到这条路线

前面在 SDXL + Canny 上耗了几轮，结论是 **Canny 管不住姿势**：
线稿画了"水平出拳"，生成出来还是插兜站着（scale 从 0.7 扫到 1.4，
只有 1.0 时手臂略微张开，1.4 画面就崩）。

Canny 的本质是"给你看轮廓"，模型得自己猜"这条线是胳膊还是背景"。
**OpenPose 直接告诉它 18 个关节的坐标和左右归属**，几乎没有歧义——
这是姿势控制能做到"精确"的根本原因。

## SD1.5 与 SDXL 的差别（用的时候要注意）

- 分辨率：SD1.5 在 512 训练，出图用 **768×1024** 最稳（再大容易出双头）。
- 模型文件：Anything V3 是 **A1111 单文件**（ckpt/safetensors），
  用 `from_single_file` 加载。
- ControlNet：`lllyasviel/control_v11p_sd15_openpose`（722MB fp16）。
  它的 config.json 是 diffusers 格式，直接 `from_pretrained` 那个目录最稳。

## 参数

- `controlnet_conditioning_scale`：SD1.5 的 OpenPose 在 **0.9~1.2** 效果好
  （比 SDXL Canny 高——SD1.5 的 ControlNet 训练更充分，扛得住高权重）。
- `guidance_scale`：7 左右（SD1.5 的常规值；太高会过饱和）。
- 负面提示里必须压 `extra limbs / bad anatomy`，SD1.5 更容易出多手多脚。
"""
import argparse
import json
import os
import sys
import time

SD15_MODEL = "build/sd15/Anything-V3.0-pruned-fp16.safetensors"
CONTROLNET_DIR = "build/sd15/controlnet-openpose"
# `from_single_file` 需要一份 diffusers 目录来知道"哪个权重叫什么"。
# 联网时它去 HF Hub 拉，本机没网会直接超时（实测 WinError 10060）。
# 这里用 diffusers 自带的 SD1.5 配置模板**本地生成**一份，彻底离线。
SD15_CONFIG = "build/sd15/sd15-config"

# 角色造型（每个角色逐字一致，只描述"是谁"）
DONUT_LOOK = (
    "1boy, young man, navy blue baseball cap, red track jacket with white stripes, "
    "navy blue track pants, white sneakers"
)
HUSHENG_LOOK = (
    "1boy, middle-aged man, short black hair, gold-rimmed glasses, grey polo shirt, "
    "dark grey slacks, black leather shoes"
)

# ⚠ "white background" 在 SD1.5 上**不够**——实测仍会画出粉色斜线纹理背景。
# 换成 `simple_background, solid white background`（SD1.5 圈里更常用的说法），
# 并在负面里明确压 `background scenery, gradient background, pattern`。
STYLE = ("full body, simple background, solid white background, clean lineart, "
         "anime style, masterpiece, best quality")

NEGATIVE = (
    "lowres, bad anatomy, bad hands, text, error, missing fingers, extra digit, fewer digits, "
    "extra limbs, extra legs, extra arms, mutated hands, cropped, worst quality, low quality, "
    "normal quality, jpeg artifacts, signature, watermark, username, blurry, "
    "multiple views, multiple people, out of frame, cut off, close-up, portrait, "
    "background scenery, gradient background, pattern, texture background, "
    "pink background, complex background, barefoot, sandals, long hair, hair ornament"
)

CHARACTERS = {
    "donut": {"look": DONUT_LOOK, "seed": 42},
    "husheng": {"look": HUSHENG_LOOK, "seed": 1337},
}


def ensure_sd15_config() -> None:
    """生成一份**离线可用的 SD1.5 配置目录**。

    为什么要自己造：`from_single_file` 必须知道 UNet/VAE/TextEncoder 的超参
    （通道数、层数、cross-attention 维度…），它默认去 HF Hub 拉
    `stable-diffusion-v1-5` 的配置——本机没网会超时失败。

    diffusers 自带这些配置的默认值，写一份 model_index.json 指向它们即可。
    这样**不下载任何东西**就能加载 A1111 单文件模型。
    """
    import json as _json

    if os.path.exists(os.path.join(SD15_CONFIG, "model_index.json")):
        return
    os.makedirs(SD15_CONFIG, exist_ok=True)
    index = {
        "_class_name": "StableDiffusionControlNetPipeline",
        "_diffusers_version": "0.35.2",
        "scheduler": [None, None],
        "text_encoder": ["transformers", "CLIPTextModel"],
        "tokenizer": ["transformers", "CLIPTokenizer"],
        "unet": ["diffusers", "UNet2DConditionModel"],
        "vae": ["diffusers", "AutoencoderKL"],
        "safety_checker": [None, None],
        "feature_extractor": [None, None],
    }
    with open(os.path.join(SD15_CONFIG, "model_index.json"), "w", encoding="utf-8") as handle:
        _json.dump(index, handle, indent=2)

    # ⚠ **必须显式写 text_encoder 的子配置**。
    #
    # 只给 model_index.json 时，diffusers 会按"默认 CLIP"构造——
    # 而默认是 **768 维（SD2.x）**，SD1.5 是 **512 维**。实测报错：
    # `text_model.embeddings.position_embedding.weight expected [77,512] but got [77,768]`。
    # 把 SD1.5 的真实超参写全，才能对上单文件里的权重。
    text_dir = os.path.join(SD15_CONFIG, "text_encoder")
    os.makedirs(text_dir, exist_ok=True)
    clip_config = {
        "_class_name": "CLIPTextModel",
        "_diffusers_version": "0.35.2",
        "architectures": ["CLIPTextModel"],
        "attention_dropout": 0.0,
        "bos_token_id": 49406,
        "eos_token_id": 49407,
        "hidden_act": "quick_gelu",
        "hidden_size": 768,
        "initializer_factor": 1.0,
        "initializer_range": 0.02,
        "intermediate_size": 3072,
        "layer_norm_eps": 1e-05,
        "max_position_embeddings": 77,
        "model_type": "clip_text_model",
        "num_attention_heads": 12,
        "num_hidden_layers": 12,
        "pad_token_id": 1,
        "projection_dim": 512,
    }
    with open(os.path.join(text_dir, "config.json"), "w", encoding="utf-8") as handle:
        _json.dump(clip_config, handle, indent=2)

    # tokenizer：SD1.5 与 SDXL 用的是**同一套 CLIP 词表**（vocab.json / merges.txt），
    # 歌白自带的 sdxl-turbo 里就有，直接复制过来，不必联网。
    tokenizer_src = os.path.join(
        r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo",
        "tokenizer")
    token_dir = os.path.join(SD15_CONFIG, "tokenizer")
    os.makedirs(token_dir, exist_ok=True)
    import shutil as _shutil
    for name in ("vocab.json", "merges.txt", "special_tokens_map.json", "tokenizer_config.json"):
        source = os.path.join(tokenizer_src, name)
        if os.path.exists(source):
            _shutil.copy2(source, os.path.join(token_dir, name))

    print(f"已生成本地配置 {SD15_CONFIG}", flush=True)


def build_pipeline():
    import torch
    from diffusers import (ControlNetModel, StableDiffusionControlNetPipeline,
                           EulerAncestralDiscreteScheduler)

    if not os.path.exists(SD15_MODEL):
        print(f"SD1.5 模型不存在：{SD15_MODEL}", file=sys.stderr)
        return None
    if not os.path.isdir(CONTROLNET_DIR):
        print(f"ControlNet 目录不存在：{CONTROLNET_DIR}", file=sys.stderr)
        return None

    print("加载 OpenPose ControlNet…", flush=True)
    controlnet = ControlNetModel.from_pretrained(CONTROLNET_DIR, torch_dtype=torch.float16)

    print("加载 SD1.5 动漫模型…", flush=True)
    started = time.time()
    # 用本地造好的完整配置目录（`tools/make_sd15_config.py`）。
    #
    # 试过的弯路：直接 `from_single_file` 不传 config → 它去 HF Hub 拉配置，
    # 本机没网直接超时；改用 `StableDiffusionPipeline` → 同样联网。
    # 只有"传 config=<完整目录>"才完全离线，而那个目录必须
    # **每个子组件都有 config.json**（缺一个就报错，逐个补是无底洞）。
    pipe = StableDiffusionControlNetPipeline.from_single_file(
        SD15_MODEL,
        config=SD15_CONFIG,
        controlnet=controlnet,
        torch_dtype=torch.float16,
        use_safetensors=True,
    )
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)
    print(f"就绪 {time.time() - started:.1f}s", flush=True)
    return pipe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--poses", default="build/char-gen/openpose")
    parser.add_argument("--outdir", default="build/char-gen/sd15-out")
    parser.add_argument("--characters", default="donut")
    parser.add_argument("--only", default="")
    parser.add_argument("--steps", type=int, default=28)
    parser.add_argument("--cfg", type=float, default=7.0)
    parser.add_argument("--control-scale", type=float, default=1.0)
    parser.add_argument("--width", type=int, default=768)
    parser.add_argument("--height", type=int, default=1024)
    parser.add_argument("--seeds", default="", help="逗号分隔的多个种子（每种子出一张）")
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
        seeds = [spec["seed"]]
        if args.seeds:
            seeds = [int(s) for s in args.seeds.split(",") if s.strip()]
        for pose_name in poses:
            done += 1
            for seed in seeds:
                suffix = f"-s{seed}" if len(seeds) > 1 else ""
                out = os.path.join(args.outdir, f"{character}-{pose_name}{suffix}.png")
                if os.path.exists(out) and not args.force:
                    print(f"[{done}/{total}] 跳过 {out}", flush=True)
                    continue

                source = os.path.join(args.poses, f"openpose-{pose_name}.png")
                pose_image = Image.open(source).convert("RGB")
                pose_image = pose_image.resize((args.width, args.height), Image.LANCZOS)

                prompt = f"{spec['look']}, {STYLE}"
                started = time.time()
                generator = torch.Generator(device="cuda").manual_seed(seed)
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
                print(f"[{done}/{total}] {os.path.basename(out)}  {time.time() - started:.1f}s",
                      flush=True)
                manifest.append({"character": character, "action": pose_name, "seed": seed,
                                 "path": out, "prompt": prompt,
                                 "control_scale": args.control_scale})

    with open(os.path.join(args.outdir, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
    print(f"完成 {len(manifest)} 张 -> {args.outdir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
