"""为每个动作 × 每个角色生成一张立绘图。

## 为什么要"一动作一图"

之前是"待机用 AI 立绘、出招切手绘矢量"，结果是**两种画风打架**
（切的那一瞬间人物从精致立绘变成色块人，还伴随尺寸跳动）。
给每个动作都出一张 AI 图，就能做到**全程同一画风**。

## 提示词怎么设计

每个动作有两段：**角色特征**（固定，决定"是谁"）+ **动作描述**（决定"在干嘛"）。

角色特征必须在所有图里**逐字一致**——这是"同一个角色"的唯一保障。
SDXL 没有角色一致性机制，靠的就是：
1. 同一段角色描述文本；
2. 同一张参考图的种子（同 seed 在同 prompt 下产出的脸/发色接近）；
3. 负面提示里压掉"变装/换发型"之类的漂移。

⚠ 但要说清楚：**SDXL 单图生成做不到严格一致**（那是 IP-Adapter / LoRA 的活）。
同一 seed + 同一角色描述能把差异压到"看起来是同一个人的不同动作"，
不是"两个不同角色"。这是本方案的已知限界。

## 动作描述的关键

游戏动作要**可读**：出拳必须手真的伸出去、受击必须身体后仰。
所以每个动作都写明：
- **which part moves**（拳头/腿/全身）
- **direction**（朝观众左/右/前）
- **body state**（前倾/后仰/蹲低）

## 输出

`<outdir>/<角色>-<动作>.png` + `manifest.json`（含全部 prompt 与 seed，便于复现）。
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

# ── 角色特征（所有动作共用，逐字一致）──
# ⚠ 角色描述要**短**：CLIP 只有 77 个 token，而动作描述必须排在最前面
# （实测：动作写在后面会被整段截掉，姿势完全不受控——出拳变成弯腰）。
#
# 取舍：动作 > 造型细节。造型用 5~6 个关键词点到即可，
# 细节差异（帽子的 N 标、夹克的拉链）交给模型自由发挥——反正游戏里看不清。
DONUT_LOOK = (
    "1boy, young man, navy baseball cap, red track jacket with white stripes, "
    "navy track pants, white sneakers"
)
HUSHENG_LOOK = (
    "1boy, middle-aged man, short black hair, gold-rimmed glasses, "
    "grey polo shirt, dark grey slacks"
)

# ── 通用风格与构图 ──
# 风格也压短：这些词价值低于动作描述
STYLE = "full body, front view, white background, clean lineart, anime style, best quality"

NEGATIVE = (
    "lowres, bad anatomy, bad hands, text, error, missing fingers, extra digit, fewer digits, "
    "cropped, worst quality, low quality, normal quality, jpeg artifacts, signature, watermark, "
    "username, blurry, multiple views, multiple people, out of frame, cut off, close-up, portrait, "
    "background scenery, gradient background"
)

# ── 每个动作的动作描述 ──
# 键与 `MoveId` 对应（见 pose.cpp 的 move_name）。
# ⚠ 动作描述的策略（踩过两次才想明白）
#
# 第一次用武术术语（"throwing a straight punch"）→ 模型画出**弯腰驼背**
# ——动漫数据里 "punch" 常和"打沙包/前倾发力"一起出现，它就照着画了。
#
# 第二次把动作提到最前、精简角色描述 → 姿势还是不对。
#
# 结论：**不要用术语，用几何描述**。
# "standing upright, one arm extended horizontally forward" 这种句子模型能直接
# 落到画面上，而 "straight punch" 要它自己联想。
#
# 另外每条都带上 `standing upright`：模型很爱画前倾/侧身，不写就歪。
ACTIONS: dict[str, str] = {
    "idle": (
        "standing upright, arms hanging down at sides, feet together, calm face"
    ),
    "walk": (
        "standing upright, one leg forward and one leg back, arms swinging"
    ),
    "guard": (
        "standing upright, both forearms raised in front of face, fists closed"
    ),
    "light": (
        "standing upright, one arm extended straight forward horizontally, "
        "other arm bent at side"
    ),
    "heavy": (
        "standing upright, one arm swung horizontally across the body, torso rotated"
    ),
    "kick": (
        "standing on one leg, other leg raised straight forward horizontally, arms out"
    ),
    "special": (
        "standing upright, both arms spread wide open, glowing aura around body"
    ),
    "donut_toss": (
        "standing upright, one arm extended forward horizontally with open palm, "
        "other hand on hip, smiling"
    ),
    "puff_spray": (
        "standing upright, both arms extended straight forward with open palms"
    ),
    "waist_grab": (
        "standing upright, both arms reaching forward with open hands"
    ),
    "apple_drop": (
        "standing upright, one arm raised straight up above head, looking up"
    ),
    "apple_guard": (
        "standing upright, both arms crossed over chest, glowing ring of light around body"
    ),
    "apple_life": (
        "standing upright, both arms spread wide open, bright radiance around body, "
        "eyes closed"
    ),
    "hurt": (
        "standing upright, head tilted back, one hand raised in front, "
        "eyes squeezed shut in pain"
    ),
    "knockdown": (
        "lying on back on the ground, arms spread out, legs bent"
    ),
    "victory": (
        "standing upright, one fist raised high above head, big smile"
    ),
    "defeat": (
        "kneeling on one knee on the ground, head lowered, arms hanging down"
    ),
}

CHARACTERS = {
    "donut": {"look": DONUT_LOOK, "seed": 42},
    "husheng": {"look": HUSHENG_LOOK, "seed": 1337},
}


def build_pipeline():
    import torch
    from diffusers import EulerAncestralDiscreteScheduler, StableDiffusionXLPipeline

    print(f"加载模型：{MODEL}", flush=True)
    started = time.time()
    pipe = StableDiffusionXLPipeline.from_single_file(
        MODEL, config=CONFIG_SOURCE, torch_dtype=torch.float16, use_safetensors=True
    )
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)
    print(f"加载完成 {time.time() - started:.1f}s", flush=True)
    return pipe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", default="build/char-gen/actions")
    parser.add_argument("--steps", type=int, default=26)
    parser.add_argument("--cfg", type=float, default=5.0)
    parser.add_argument("--width", type=int, default=832)
    parser.add_argument("--height", type=int, default=1216)
    parser.add_argument("--only", default="", help="只生成指定动作（逗号分隔）")
    args = parser.parse_args()

    if not os.path.exists(MODEL):
        print(f"模型不存在：{MODEL}", file=sys.stderr)
        return 1

    actions = ACTIONS
    if args.only:
        wanted = {name.strip() for name in args.only.split(",") if name.strip()}
        actions = {k: v for k, v in ACTIONS.items() if k in wanted}
        if not actions:
            print(f"没有匹配的动作：{args.only}", file=sys.stderr)
            return 1

    os.makedirs(args.outdir, exist_ok=True)
    pipe = build_pipeline()

    # 长度自检：CLIP 上限 77 token，超了会**静默截断**（只打一行 warning）。
    # 截掉的如果正好是动作描述，姿势就完全失控——这类"看起来生成了、
    # 其实没按指令"的失败必须提前挡住，不能靠事后看图发现。
    tokenizer = pipe.tokenizer
    tokenizer_2 = pipe.tokenizer_2
    over = []
    for character, spec in CHARACTERS.items():
        for action, motion in actions.items():
            prompt = f"{motion}, {spec['look']}, {STYLE}"
            n1 = len(tokenizer(prompt).input_ids)
            n2 = len(tokenizer_2(prompt).input_ids)
            if max(n1, n2) > 77:
                over.append((character, action, n1, n2))
    if over:
        print("[WARN] 这些提示词超过 77 token（会被截断）：", flush=True)
        for item in over:
            print(f"    {item[0]}-{item[1]}: {item[2]} / {item[3]} token", flush=True)
    else:
        print("OK 全部提示词在 77 token 内", flush=True)

    import torch

    manifest = []
    total = len(actions) * len(CHARACTERS)
    done = 0
    for character, spec in CHARACTERS.items():
        for action, motion in actions.items():
            done += 1
            # 顺序很关键：**动作在前**。CLIP 从头保留 77 token，
            # 动作写后面会被截掉（实测：姿势完全不受控）。
            prompt = f"{motion}, {spec['look']}, {STYLE}"
            out = os.path.join(args.outdir, f"{character}-{action}.png")
            if os.path.exists(out):
                print(f"[{done}/{total}] 跳过（已存在）{out}", flush=True)
                manifest.append(
                    {"character": character, "action": action, "path": out, "prompt": prompt,
                     "seed": spec["seed"]}
                )
                continue
            started = time.time()
            generator = torch.Generator(device="cuda").manual_seed(spec["seed"])
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
            print(f"[{done}/{total}] {character}-{action}  {time.time() - started:.1f}s", flush=True)
            manifest.append(
                {"character": character, "action": action, "path": out, "prompt": prompt,
                 "seed": spec["seed"]}
            )

    with open(os.path.join(args.outdir, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
    print(f"完成 {len(manifest)} 张 → {args.outdir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
