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

SDXL_MODEL = r"C:\Users\Administrator\Downloads\PetalRain亚洲动漫-Illustrious_V2.safetensors"
SDXL_CONFIG = r"C:\Users\Administrator\code\gebai\resources\models\imagegen\sdxl-turbo"
CONTROLNET_DIR = "build/cn-ill"

# ── 角色造型 ──
#
# ## 甜甜圈的美国国旗棒球帽（这一条改了三轮）
#
# 只写 `navy baseball cap` 会得到一顶**纯深蓝帽子**——实测放大头部看，
# 完全没有红白条纹和星点。国旗帽必须把三个要素都说出来：
# **红白横条 + 蓝色底 + 白色星星**，而且要用 `american flag pattern` 这种
# 模型认识的现成概念，而不是逐个描述（逐个描述会被合并成"红蓝条纹"）。
#
# 帽子是这套造型最有辨识度的部件，所以放在角色描述**最前面**。
# ⚠ **必须显式写 `looking at viewer` / `facing forward`**。
#
# 实测两轮都是"背面朝镜头"：OpenPose 骨架从背面看是对称的（左右点镜像一致），
# 模型无法据此分辨正反，于是随机选——两轮都选了背面。
# 这是骨架约束的固有盲区：**它能定姿势，定不了朝向**。
# 朝向必须由提示词给。
#
# 另一个坑：`american flag` 这个短语会被理解成"美式校园风"——
# 实测帽子变成纯蓝、头发变红、衣服印上 "NAVY" 校队字样。
# 所以不能靠这个词组，要把国旗要素**拆成具体形状**写：
# 「蓝底 + 红白横条 + 白色星星」。
# ⚠ 朝向必须放在**最前面**，而且要多个同义表达叠加。
#
# 实测：`facing forward looking at viewer` 写在角色描述中间时，
# 17 个动作里有 10 个画成**背面或侧面**，衣服背后还印了数字（12/11/18/8）——
# 那是"运动服背面"这个训练数据先验压过了朝向指令。
#
# 改法：把朝向提到最前 + 叠加三个说法（`from front`/`facing viewer`/`front view`），
# 并在负面里显式压背面。
DONUT_LOOK = (
    "front view, from front, facing viewer, looking at viewer, "
    "1boy, young man, "
    "navy blue baseball cap with red and white horizontal stripes and white star pattern, "
    "red track jacket with white stripes on sleeves, navy blue track pants, white sneakers, "
    "short dark brown hair"
)

# ## 户圣的亮片眼镜 + 灰 polo
#
# 三个问题（放大截图看出来的）：
# 1. `gold-rimmed glasses` 画出来是**普通细框**，没有亮片感
#    ⇒ 改 `rhinestone glasses` / `sparkling glasses`（模型认识这两个概念）
# 2. 他多穿了一件**深色西装外套**——`grey polo shirt` 单独写时，
#    模型会自动给他"配一件外套"（动漫里成年男性穿 polo 的常见搭配）
#    ⇒ 必须用 **`only a grey polo shirt`** + 负面压 `jacket, blazer, suit, coat`
# 3. 头发偏长偏乱 ⇒ 加 `neat short hair`
# ## 户圣：寸头 + 灰色短袖
#
# 用户要求改成"寸头灰色短袖"。几条实测经验：
# 1. `buzz cut` 是模型认识的寸头；`short hair` 只会给"稍微短一点的长发"。
# 2. `short sleeve` 比 `t-shirt` 可靠——后者会画出印花/图案。
# 3. 仍然要 `only a ... shirt` + 负面压 `jacket, blazer`（之前四轮都在这栽）。
# 4. 亮片眼镜保留（`rhinestone glasses` 已验证有效）。
HUSHENG_LOOK = (
    "front view, from front, facing viewer, looking at viewer, "
    "1boy, middle-aged man, buzz cut, very short cropped hair, "
    "gold framed glasses with sparkling rhinestones on the rims, "
    "only a light grey short sleeve shirt with collar, "
    "dark grey slacks, black leather shoes"
)

STYLE = "full body, simple background, solid white background, clean lineart, anime style"

NEGATIVE = (
    "lowres, bad anatomy, bad hands, text, error, missing fingers, extra digit, fewer digits, "
    "extra limbs, extra legs, extra arms, mutated hands, cropped, worst quality, low quality, "
    "jpeg artifacts, signature, watermark, blurry, multiple views, multiple people, "
    "out of frame, cut off, close-up, portrait, background scenery, gradient background, "
    "pattern, texture background, complex background, barefoot, sandals, long hair, "
    # 户圣多穿外套：polo 单独出现时模型会自动配西装配件
    "jacket, blazer, suit, coat, cardigan, vest, layered clothing, "
    # 帽子画错形态
    "plain hat, blank cap, no pattern, "
    # 背面/侧面（模型爱画背对镜头，尤其穿运动服时）
    "from behind, back view, rear view, seen from behind, turning away, "
    "side view, profile, back of head, jersey number on back, "
    # T 恤印花：实测 `short sleeve shirt` 会画出骷髅头 + 品牌字样
    "graphic print, printed t-shirt, logo on shirt, text on shirt, skull print, "
    "brand name, lettering on clothes"
)

CHARACTERS = {
    "donut": {"look": DONUT_LOOK, "seed": 42},
        # seed 11：实测这个种子下是**纯色灰短袖**（1337 会给骷髅印花 T 恤）
    "husheng": {"look": HUSHENG_LOOK, "seed": 11},
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
