"""只为**一个角色**生成全部动作立绘（不用 ControlNet，纯提示词）。

## 为什么单独一个脚本

`tools/sdxl_actions.py` 是批量双角色的，调试单个角色时它的输出混在一起、
不好对比。这个脚本只跑一个角色，并且**每张都打印 prompt**，
方便逐张判断"是提示词没描述到位，还是模型压根画不出来"。

## 姿势描述的三条经验（都是踩出来的）

1. **不用武术术语**。`"throwing a punch"` 在动漫数据里常和"前倾发力/打沙包"
   一起出现，实测画出来是**弯腰驼背**。换成几何描述
   （`"one arm extended straight forward horizontally"`）才站得直。
2. **每条都带 `standing upright`**（除躺地动作）。模型很爱画侧身/前倾，
   不写就歪。
3. **动作词放在最前面**。CLIP 只保留 77 token，从头截断；动作写后面会被整段截掉，
   姿势就完全失控。

## 输出

`<outdir>/<动作>.png` + `manifest.json`（含 prompt 与 seed）。
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
CONFIG_SOURCE = os.environ.get("GEBAI_SDXL_CONFIG",
    r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo")

# 角色的**造型**（所有动作共用，逐字一致）。
#
# 只写"是什么"，不写"在干嘛"——动作由 ACTIONS 负责。
DONUT_LOOK = (
    "1boy, young man, navy blue baseball cap, red track jacket with white stripes, "
    "navy blue track pants, white sneakers"
)

STYLE = "full body, white background, clean lineart, anime style, best quality"

NEGATIVE = (
    "lowres, bad anatomy, bad hands, text, error, missing fingers, extra digit, fewer digits, "
    "cropped, worst quality, low quality, normal quality, jpeg artifacts, signature, watermark, "
    "username, blurry, multiple views, multiple people, out of frame, cut off, close-up, "
    "portrait, background scenery, barefoot, shoes off, long hair, extra limbs"
)

# 动作描述（第一句永远是姿势，后面才是细节）。
ACTIONS: dict[str, str] = {
    "idle": "standing upright and relaxed, arms hanging down at sides, feet together, calm face",
    "walk": "standing upright, one leg stepping forward, opposite arm swinging forward, "
            "casual walking",
    "guard": "standing upright, both fists raised in front of face, elbows bent, "
             "boxing guard position",
    "light": "standing upright, right arm extended straight forward horizontally, fist closed, "
             "left arm bent at side, punching forward",
    "heavy": "standing upright, right arm swung horizontally across the chest, "
             "torso twisted to the left, powerful swing",
    "kick": "standing on left leg, right leg raised straight forward horizontally, "
            "both arms out to sides for balance",
    "special": "standing upright, both arms spread wide open to the sides, "
               "glowing energy aura around body",
    "donut_toss": "standing upright, right arm extended forward horizontally with open palm, "
                  "left hand on hip, tossing gesture",
    "puff_spray": "standing upright, both arms extended straight forward with palms open, "
                  "blowing something from hands",
    "waist_grab": "standing upright, both arms reaching forward with open hands, leaning forward",
    "apple_drop": "standing upright, right arm raised straight up above head, "
                  "hand open, looking upward",
    "apple_guard": "standing upright, both arms crossed in front of chest, "
                   "glowing ring of light around body",
    "apple_life": "standing upright, both arms spread wide open, bright light radiating "
                  "from body, eyes closed",
    "hurt": "standing upright, head tilted back, body arched backward, "
            "one hand raised in front, eyes squeezed shut, in pain",
    "knockdown": "lying flat on back on the ground, arms spread out to sides, legs bent",
    "victory": "standing upright, right fist raised high above head, big smile, confident",
    "defeat": "kneeling on one knee on the ground, head lowered, arms hanging down, exhausted",
}


def build_pipeline():
    import torch
    from diffusers import EulerAncestralDiscreteScheduler, StableDiffusionXLPipeline

    print(f"加载 {MODEL}", flush=True)
    started = time.time()
    pipe = StableDiffusionXLPipeline.from_single_file(
        MODEL, config=CONFIG_SOURCE, torch_dtype=torch.float16, use_safetensors=True
    )
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)
    print(f"就绪 {time.time() - started:.1f}s", flush=True)
    return pipe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", default="build/char-gen/donut-actions")
    parser.add_argument("--look", default=DONUT_LOOK)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--steps", type=int, default=28)
    parser.add_argument("--cfg", type=float, default=5.5)
    parser.add_argument("--width", type=int, default=832)
    parser.add_argument("--height", type=int, default=1216)
    parser.add_argument("--only", default="")
    parser.add_argument("--force", action="store_true", help="已存在的也重做")
    args = parser.parse_args()

    actions = ACTIONS
    if args.only:
        wanted = {x.strip() for x in args.only.split(",") if x.strip()}
        actions = {k: v for k, v in ACTIONS.items() if k in wanted}
        if not actions:
            print(f"没有匹配：{args.only}", file=sys.stderr)
            return 1

    os.makedirs(args.outdir, exist_ok=True)
    pipe = build_pipeline()

    import torch

    manifest = []
    total = len(actions)
    for index, (action, motion) in enumerate(actions.items(), start=1):
        out = os.path.join(args.outdir, f"{action}.png")
        prompt = f"{motion}, {args.look}, {STYLE}"
        if os.path.exists(out) and not args.force:
            print(f"[{index}/{total}] 跳过 {action}", flush=True)
            continue
        started = time.time()
        generator = torch.Generator(device="cuda").manual_seed(args.seed)
        image = pipe(
            prompt=prompt,
            negative_prompt=NEGATIVE,
            width=args.width,
            height=args.height,
            num_inference_steps=args.steps,
            guidance_scale=args.cfg,
            generator=generator,
        ).images[0]
        image.save(out)
        print(f"[{index}/{total}] {action}  {time.time() - started:.1f}s", flush=True)
        print(f"      {motion[:90]}", flush=True)
        manifest.append({"action": action, "path": out, "prompt": prompt, "seed": args.seed})

    with open(os.path.join(args.outdir, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
    print(f"完成 {len(manifest)} 张 -> {args.outdir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
