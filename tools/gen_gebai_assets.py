#!/usr/bin/env python3
"""生成「歌白」品牌打底素材（一批 8 张，进裁云/镂月的素材目录）。

一次进程内加载模型、连续出图——比每张起一个进程快得多（模型加载约 3 秒，
单张出图 ~1.2 秒）。产物命名可读，方便脚本/人按名字引用。

用法：
    python tools/gen_gebai_assets.py [输出目录] [--size 768x512] [--force]

设计取向（写在提示词里，而不是事后调色）：
  - **深色底 + 冷蓝/青调**：与应用的暗色主题同族，叠文字后对比度足够；
  - **无文字、无主体**：素材是"底板"，不该抢镜；主体（标题/图表）由渲染器画；
  - **低对比 + 大留白**：压暗后仍要能看见层次，所以避免高频细节。
"""
import argparse
import os
import sys
import time

ASSETS = [
    # (文件名, 提示词)
    #
    # 选型原则（实测得出的）：**底板要能被压暗后仍让白字读得清**。
    # 所以题材一律偏"柔和、大块、低对比"——
    # 星云/电路这类高频题材虽然好看，但细节密到叠字后糊成一片（试过，弃用）。
    ("bg-aurora",
     "abstract dark navy gradient, two soft diagonal light bands, "
     "very smooth, minimal, cinematic, no text, no objects"),
    ("bg-mesh",
     "abstract flowing mesh gradient, deep blue to teal, smooth metaballs, "
     "dark elegant, soft focus, no text, no objects"),
    ("bg-dusk",
     "smooth dusk sky gradient, deep indigo to muted violet, "
     "hazy horizon glow, minimal, no clouds detail, no text"),
    ("bg-deep",
     "deep near-black blue gradient, single faint light source top center, "
     "vignette, minimal, no text, no objects"),
    ("bg-slate",
     "flat dark slate blue texture, extremely subtle vertical gradient, "
     "almost plain, matte, no text, no objects"),
    ("tex-noise",
     "subtle dark blue film grain texture, very low contrast, even, "
     "seamless, no objects, no text"),
    ("texture-bokeh",
     "dark background with a few soft blue bokeh spots, very blurry, "
     "sparse, elegant, no text, no objects"),
    ("accent-glow",
     "single soft cyan light bloom on dark navy background, minimal, "
     "centered soft glow, no text, no objects"),
]
# 负面词：除了"别画文字/别画人"，还要压掉**高频细节**——
# 素材是"底板"，主体（标题/图表）由渲染器画。一张细节爆炸的图既抢镜、
# 又让叠在上面的文字读不出来（实测：SDXL 默认出的星云细节密到文字糊掉）。
NEGATIVE = (
    "text, watermark, signature, logo, people, face, letters, words, "
    "high contrast, busy, cluttered, sharp details, harsh lighting, noise, grain"
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out", nargs="?", default=None)
    parser.add_argument("--size", default="768x512")
    parser.add_argument("--steps", type=int, default=3)
    parser.add_argument("--force", action="store_true")
    parser.add_argument(
        "--model",
        default=None,
        help="模型目录（缺省按 sdxl-turbo > sd-turbo > 环境变量的顺序选）",
    )
    args = parser.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = args.out or os.path.join(
        repo, "shuangtian", "examples", "caiyun", "assets", "media")
    os.makedirs(out_dir, exist_ok=True)
    width, height = (int(v) for v in args.size.lower().split("x"))

    import torch
    from diffusers import AutoPipelineForText2Image

    # 与 tools/imagegen.py 同一套选型逻辑（一处定义/两处调用，避免漂移）：
    # 环境变量 > sdxl-turbo（装了就用，质量优先）> sd-turbo。
    model = args.model or os.environ.get("GEBAI_IMAGEGEN_MODEL")
    if not model:
        base = os.path.join(repo, "resources", "models", "imagegen")
        for candidate in ("sdxl-turbo", "sd-turbo"):
            if os.path.isdir(os.path.join(base, candidate)):
                model = os.path.join(base, candidate)
                break
    if not model or not os.path.isdir(model):
        print(f"模型不存在：{model}", file=sys.stderr)
        return 2
    if "sdxl" in os.path.basename(model).lower():
        # SDXL 要求 64 的倍数、短边 >= 512（训练分辨率 1024²）。
        width = max(512, (width + 32) // 64 * 64)
        height = max(512, (height + 32) // 64 * 64)
    print(f"模型 {os.path.basename(model)}  尺寸 {width}x{height}  步数 {args.steps}", flush=True)
    if not torch.cuda.is_available():
        print("CUDA 不可用——出图会慢到不可用（先修环境）", file=sys.stderr)
        return 2

    print(f"加载模型 {model}", flush=True)
    pipeline = AutoPipelineForText2Image.from_pretrained(
        model, torch_dtype=torch.float16, safety_checker=None, local_files_only=True)
    pipeline = pipeline.to("cuda")
    pipeline.set_progress_bar_config(disable=True)

    made = 0
    skipped = 0
    for index, (name, prompt) in enumerate(ASSETS):
        path = os.path.join(out_dir, f"{name}.png")
        if os.path.exists(path) and not args.force:
            print(f"跳过（已存在）{name}", flush=True)
            skipped += 1
            continue
        began = time.time()
        generator = torch.Generator(device="cuda").manual_seed(1000 + index)
        # 统一追加"底板"约束：低对比 + 大留白 + 柔和过渡（省得每条都手写一遍）。
        final_prompt = (
            f"{prompt}, very low contrast, soft even lighting, large empty areas, "
            "smooth gentle gradient, understated, background plate for text overlay"
        )
        with torch.inference_mode():
            image = pipeline(
                prompt=final_prompt,
                negative_prompt=NEGATIVE,
                num_inference_steps=args.steps,
                guidance_scale=0.0,
                width=width,
                height=height,
                generator=generator,
            ).images[0]
        image.save(path)
        made += 1
        print(f"  {name}.png  {width}x{height}  {time.time()-began:.2f}s", flush=True)

    print(f"\n完成：新增 {made} 张，跳过 {skipped} 张 → {out_dir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
