"""多候选动作立绘：每个动作 × 多个种子，交给人工挑。

## 为什么这样做（而不是继续调 ControlNet）

实测三种 ControlNet 路线都不行：
- SDXL + Canny：把线稿当"画面里的黑块"画，姿势不受控；
- SD1.5 + OpenPose：姿势准了，但 SD1.5 画质差、常出畸形；
- SDXL + Union：**骨架线直接被画进画面**（scale≥1.4 时）。

根因是这个 Illustrious V2 动漫模型的"人体姿势先验"太强，
会压过 ControlNet 的训练信号——通用 ControlNet 是写实数据训的，
动漫模型理解不了那种约束形态。

所以回到**纯提示词**，用工程手段把成功率拉上去：

1. **每个动作换不同种子**。之前 17 个动作共用 seed 42，一旦模型进入
   "弯腰模式"就全歪（实测 10/17 都在弯腰）。
2. **修掉会歧义的动词**。`heavy` 的 "swing" 被画成**荡秋千**（真的画了绳和板），
   改成 "arm across chest"。
3. **每条都显式 `standing upright`**（躺地动作除外）。
4. 出 3 张候选，人工挑。

## 种子策略

每个动作从基础种子 + 动作序号派生，保证：
- **动作之间**种子不同（避免同一个"气质"贯穿全套）
- **可复现**（种子写进 manifest）
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
    "pattern, texture background, complex background, barefoot, sandals, long hair, "
    "small person, chibi, doll, rope, hanging"
)

# 动作描述：**几何化、无歧义**。
#
# 修掉的坑（都来自实测）：
# - `"swing"` → 画出荡秋千（带绳子和板）⇒ 改 `"arm across chest"`
# - `"punch"` → 弯腰驼背 ⇒ 改 `"arm extended straight forward"`
# - 缺 `standing upright` → 侧身前倾 ⇒ 每条都加
ACTIONS: dict[str, str] = {
    # ── 已成功的（保持不变，避免"改好了又改坏"）──
    "light": "standing upright, right arm extended straight forward horizontally, fist closed",
    "kick": "standing on left leg, right leg raised straight forward diagonally, arms out",
    "special": "standing upright, both arms spread wide open to the sides, glowing aura",
    "victory": "standing upright, right fist raised high above head, smiling",

    # ── 本轮重做的 10 个 ──
    #
    # 失败原因与修法（都来自实测）：
    #
    # ① `idle`：三个种子**全都在弯腰鞠躬**。根因是 `standing upright` +
    #    `arms hanging at sides` 这组词在动漫数据里对应"鞠躬/敬礼"。
    #    修法：给**具体的手部动作**（插兜）+ 重心偏移，模型就不会往鞠躬上想。
    "idle": ("standing upright with weight on the back leg, both hands tucked into jacket "
             "pockets, chin up, looking ahead"),
    #
    # ② `walk`：画面里**多出一个小人**（`multiple people` 负面没压住）。
    #    而且"one leg stepping forward"会被理解成"跨步/跳跃"。
    #    修法：用 `mid-stride` + 明确"前脚掌着地"，并把 `small person` 加进负面。
    "walk": ("standing upright mid-stride, front foot flat on the ground with knee slightly "
             "bent, rear heel lifted, arms counter-swinging naturally"),
    #
    # ③ `guard`：画成**举拳欢呼**（`both forearms raised in front of face` 被理解成举手）。
    #    修法：用格斗术语 `boxing guard` + 明确"拳头在下巴高度、肘部内收"。
    "guard": ("standing upright in a boxing guard stance, both fists held at chin level, "
              "elbows tucked down in front of ribs, knees slightly bent"),
    #
    # ④ `heavy`：三张都是普通站姿，**没画出挥拳**。
    #    `arm swung across chest` 太抽象。改成"手臂横扫过身体+躯干扭转+重心转动"。
    "heavy": ("standing upright, right arm swung horizontally across the body toward the left, "
              "torso twisted, hips rotated, left arm pulled back for balance"),
    #
    # ⑤ `donut_toss`：c0 还行，其余两个画成了"张开双臂"。
    #    强化"单手前伸 + 手心向上 + 另一只手在腰侧"。
    "donut_toss": ("standing upright, right arm extended forward at shoulder height with "
                   "palm facing up, left hand resting on hip, slight forward lean"),
    #
    # ⑥ `puff_spray`：两张像"张开手臂"。改成"双掌并拢前推"的动作。
    "puff_spray": ("standing upright, both arms extended forward together at shoulder height, "
                   "palms facing away, fingers spread"),
    #
    # ⑦ `waist_grab`：之前画成弯腰。改成"双手向前伸抓、身体略前倾但保持直立"。
    "waist_grab": ("standing upright, both arms reaching forward at waist height with open "
                   "hands, slight forward lean, legs apart in a stable stance"),
    #
    # ⑧ `hurt`：之前被理解成"伸展"。改成明确的受击反应。
    "hurt": ("standing upright recoiling backward, head snapped back, torso arched away, "
             "right hand raised to the cheek, eyes squeezed shut"),
    #
    # ⑨ `knockdown`：头发变长 + 姿势不明。明确"仰面躺地、四肢摊开"。
    "knockdown": ("lying flat on the back on the ground, both arms spread out to the sides, "
                  "knees bent, head tilted sideways"),
    #
    # ⑩ `defeat`：之前画成"站着"。明确"单膝跪地、一只手撑地"。
    "defeat": ("kneeling on one knee on the ground, one hand touching the floor for support, "
               "head lowered, shoulders slumped"),
}

CHARACTERS = {
    "donut": {"look": DONUT_LOOK, "seed": 42},
    "husheng": {"look": HUSHENG_LOOK, "seed": 1337},
}


def build_pipeline():
    import torch
    from diffusers import EulerAncestralDiscreteScheduler, StableDiffusionXLPipeline

    print(f"加载模型…", flush=True)
    started = time.time()
    pipe = StableDiffusionXLPipeline.from_single_file(
        MODEL, config=CONFIG_SOURCE, torch_dtype=torch.float16, use_safetensors=True)
    pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(pipe.scheduler.config)
    pipe.enable_model_cpu_offload()
    pipe.set_progress_bar_config(disable=True)
    print(f"就绪 {time.time() - started:.1f}s", flush=True)
    return pipe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", default="build/char-gen/multi")
    parser.add_argument("--characters", default="donut")
    parser.add_argument("--only", default="")
    parser.add_argument("--candidates", type=int, default=3, help="每个动作出几张")
    parser.add_argument("--steps", type=int, default=28)
    parser.add_argument("--cfg", type=float, default=5.5)
    parser.add_argument("--width", type=int, default=832)
    parser.add_argument("--height", type=int, default=1216)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    actions = ACTIONS
    if args.only:
        wanted = {x.strip() for x in args.only.split(",") if x.strip()}
        actions = {k: v for k, v in ACTIONS.items() if k in wanted}

    characters = [c.strip() for c in args.characters.split(",") if c.strip() in CHARACTERS]
    os.makedirs(args.outdir, exist_ok=True)
    pipe = build_pipeline()

    import torch

    manifest = []
    total = len(actions) * len(characters) * args.candidates
    done = 0
    for character in characters:
        spec = CHARACTERS[character]
        for action_index, (action, motion) in enumerate(actions.items()):
            for candidate in range(args.candidates):
                done += 1
                out = os.path.join(args.outdir, f"{character}-{action}-c{candidate}.png")
                if os.path.exists(out) and not args.force:
                    print(f"[{done}/{total}] 跳过 {os.path.basename(out)}", flush=True)
                    continue

                # 种子派生：每个动作、每个候选都不同。
                # `action_index * 101 + candidate * 7919` 让相邻动作的种子差异足够大
                # （实测相邻种子常得到几乎相同的构图）。
                seed = spec["seed"] + action_index * 101 + candidate * 7919
                prompt = f"{motion}, {spec['look']}, {STYLE}"

                started = time.time()
                generator = torch.Generator(device="cuda").manual_seed(seed)
                image = pipe(
                    prompt=prompt, negative_prompt=NEGATIVE,
                    width=args.width, height=args.height,
                    num_inference_steps=args.steps, guidance_scale=args.cfg,
                    generator=generator,
                ).images[0]
                image.save(out)
                print(f"[{done}/{total}] {os.path.basename(out)}  seed={seed}  "
                      f"{time.time() - started:.1f}s", flush=True)
                manifest.append({"character": character, "action": action,
                                 "candidate": candidate, "seed": seed,
                                 "path": out, "prompt": prompt})

    with open(os.path.join(args.outdir, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
    print(f"完成 {len(manifest)} 张 -> {args.outdir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
