"""用本地的 SDXL 动漫模型生成角色素材。

为什么单独一个脚本而不是改 `imagegen` 子 Agent：
那个子 Agent 走的是 diffusers 目录格式（model_index.json + unet/vae/text_encoder 分目录），
而用户手里的是 **A1111 单文件格式**（一个 6.5GB 的 safetensors）。两者架构相同（SDXL），
用 `StableDiffusionXLPipeline.from_single_file()` 直接加载即可，不必先转换目录结构。

用法：
  python tools/sdxl_char.py --prompt "..." --out out.png
  python tools/sdxl_char.py --sheet            # 生成一整套角色素材
"""
import argparse
import json
import os
import sys
import time

MODEL = r"C:\Users\Administrator\Downloads\PetalRain亚洲动漫-Illustrious_V2.safetensors"

# 配置源：`from_single_file` 需要一份 diffusers 的目录结构来知道"哪个权重叫什么"。
# 联网时它会去 HF Hub 拉，但本机没网/被墙时会直接失败。
# 歌白自带的 `sdxl-turbo` 正好是同架构（SDXL）的完整 diffusers 目录，
# 拿它当配置源即可——**只用它的目录结构，不读它的权重**。
CONFIG_SOURCE = r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo"


def build_pipeline():
    import torch
    from diffusers import StableDiffusionXLPipeline, EulerAncestralDiscreteScheduler

    print(f"加载模型：{MODEL}", flush=True)
    started = time.time()
    if not os.path.isdir(CONFIG_SOURCE):
        print(f"配置源不存在：{CONFIG_SOURCE}", file=sys.stderr)
        return None
    pipe = StableDiffusionXLPipeline.from_single_file(
        MODEL,
        config=CONFIG_SOURCE,          # ← 关键：用本地 SDXL 目录当配置，不依赖联网
        torch_dtype=torch.float16,
        use_safetensors=True,
    )
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.to("cuda")
    # 显存够就开；16GB 跑 1024² 的 SDXL 有余。
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)
    print(f"加载完成 {time.time() - started:.1f}s", flush=True)
    return pipe


def generate(pipe, prompt, negative, width, height, steps, cfg, seed, out):
    import torch

    generator = torch.Generator(device="cuda").manual_seed(seed)
    started = time.time()
    image = pipe(
        prompt=prompt,
        negative_prompt=negative,
        width=width,
        height=height,
        num_inference_steps=steps,
        guidance_scale=cfg,
        generator=generator,
    ).images[0]
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    image.save(out)
    print(f"  生成 {time.time() - started:.1f}s → {out}", flush=True)
    return out


# 角色素材的提示词。
#
# 关键约定（这几条是"能不能直接当游戏角色用"的分水岭）：
# - **full body / standing**：要站姿全身，裁到半身就没法放进游戏
# - **simple background / white background**：白底便于抠图
# - **facing viewer / front view**：正脸站立，方便做左右镜像
# - 负面提示里压掉 "cropped / multiple views / text"，否则会出多视图或带字
NEGATIVE = (
    "lowres, bad anatomy, bad hands, text, error, missing fingers, extra digit, "
    "fewer digits, cropped, worst quality, low quality, normal quality, jpeg artifacts, "
    "signature, watermark, username, blurry, multiple views, multiple people, "
    "out of frame, cut off, close-up, portrait"
)

CHARACTERS = {
    "donut": {
        "desc": "甜甜圈：年轻人，美国国旗棒球帽，红色运动夹克",
        "prompt": (
            "1boy, solo, full body, standing, facing viewer, front view, "
            "young man, casual athletic look, wearing a navy blue baseball cap with "
            "red and white stripes, red track jacket with white stripes on the sleeves, "
            "dark navy pants, white sneakers, friendly smile, "
            "simple white background, clean lineart, vibrant colors, game character concept art, "
            "masterpiece, best quality, highly detailed"
        ),
    },
    "husheng": {
        "desc": "户圣：中年男子，金框亮片眼镜，灰色 polo 衫",
        "prompt": (
            "1boy, solo, full body, standing, facing viewer, front view, "
            "middle-aged man, short black hair, wearing gold-rimmed glasses with "
            "sparkling rhinestones, grey polo shirt with collar, dark grey slacks, "
            "black leather shoes, calm expression, "
            "simple white background, clean lineart, vibrant colors, game character concept art, "
            "masterpiece, best quality, highly detailed"
        ),
    },
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prompt", default="")
    parser.add_argument("--negative", default=NEGATIVE)
    parser.add_argument("--out", default="out.png")
    parser.add_argument("--width", type=int, default=832)
    parser.add_argument("--height", type=int, default=1216)
    parser.add_argument("--steps", type=int, default=28)
    parser.add_argument("--cfg", type=float, default=5.5)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--sheet", action="store_true", help="生成一整套角色素材")
    parser.add_argument("--outdir", default="build/char-gen")
    args = parser.parse_args()

    if not os.path.exists(MODEL):
        print(f"模型不存在：{MODEL}", file=sys.stderr)
        return 1

    pipe = build_pipeline()

    if args.sheet:
        manifest = []
        for name, spec in CHARACTERS.items():
            # 每个角色出 2 个种子（挑一个更合适的）
            for seed in (42, 1337):
                out = os.path.join(args.outdir, f"{name}-{seed}.png")
                generate(pipe, spec["prompt"], NEGATIVE, args.width, args.height, args.steps,
                         args.cfg, seed, out)
                manifest.append({"name": name, "seed": seed, "path": out, "desc": spec["desc"],
                                 "prompt": spec["prompt"]})
        with open(os.path.join(args.outdir, "manifest.json"), "w", encoding="utf-8") as handle:
            json.dump(manifest, handle, ensure_ascii=False, indent=2)
        print(f"清单 → {os.path.join(args.outdir, 'manifest.json')}", flush=True)
        return 0

    if not args.prompt:
        print("要么给 --prompt，要么加 --sheet", file=sys.stderr)
        return 1
    generate(pipe, args.prompt, args.negative, args.width, args.height, args.steps, args.cfg,
             args.seed, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
