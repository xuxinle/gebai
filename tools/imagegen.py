#!/usr/bin/env python3
"""本地图像生成（sd-turbo / SD2.1-Turbo，1–4 步出图）。

## 为什么用 sd-turbo 而不是 SDXL
本机 16GB 显存两者都能跑，但 sd-turbo 全量约 5GB、512² 出图 **< 1 秒**，
适合"给视频/海报造素材"这个用途（一次要出好几张、还要反复改提示词）。
SDXL-Turbo 质量更高但 13GB、单张数秒——需要时换 `--model` 指到另一个目录即可。

## 为什么是独立脚本而不是塞进框架
霜天是 C++ 原生 UI 框架，不该内嵌 Python 推理栈。分工：**本脚本负责出图**（跑在
GPU 上），产物落进素材目录，裁云/镂月在渲染时按普通素材使用。这样：
- 应用不需要知道有模型这回事（素材就是文件）；
- 换模型/升版本不影响已编译的应用。

用法：
    python tools/imagegen.py --prompt "..." --out <路径> [--size 512x512]
                             [--steps 2] [--seed 42] [--model <目录>]
    python tools/imagegen.py --list-models
    python tools/imagegen.py --check          # 只检查环境（GPU/模型/版本）

退出码：0 成功；2 环境或模型不可用（原因打到 stderr，不用异常栈糊弄）。
"""
import argparse
import os
import sys
import time

DEFAULT_MODEL_HINT = "resources/models/imagegen/sd-turbo"


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_model():
    env = os.environ.get("GEBAI_IMAGEGEN_MODEL")
    if env:
        return env
    # 装了更大的模型就用它（质量优先）；只有小模型时退回去。
    #
    # 为何默认不是所有模型都列出：调用方（子代理/工具）只需要一个“当前该用哪个”，
    # 选型是**部署决策**而不是每次出图时的选择——把选择权留给环境变量与目录现状。
    base = os.path.join(repo_root(), "resources", "models", "imagegen")
    for candidate in ("sdxl-turbo", "sd-turbo"):
        if os.path.isdir(os.path.join(base, candidate)):
            return os.path.join(base, candidate)
    return os.path.join(repo_root(), DEFAULT_MODEL_HINT)


def check_env(verbose=True):
    """返回 (ok, 说明)。不抛——调用方要能如实告诉用户缺什么。"""
    problems = []
    try:
        import torch
    except Exception as exc:  # noqa: BLE001
        return False, f"torch 不可用：{exc}"
    if verbose:
        print(f"torch {torch.__version__}  cuda={torch.cuda.is_available()}", flush=True)
    if not torch.cuda.is_available():
        problems.append("CUDA 不可用（会退到 CPU，出图慢到不可用）")
    try:
        import diffusers  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        return False, f"diffusers 不可用：{exc}"
    model = default_model()
    if not os.path.isdir(model):
        problems.append(f"模型目录不存在：{model}")
    elif not os.path.exists(os.path.join(model, "model_index.json")):
        problems.append(f"模型目录缺少 model_index.json：{model}")
    return (not problems), "; ".join(problems) if problems else "ok"


def generate(prompt, out_path, size, steps, seed, model, guidance=0.0):
    import torch
    from diffusers import AutoPipelineForText2Image

    device = "cuda" if torch.cuda.is_available() else "cpu"
    dtype = torch.float16 if device == "cuda" else torch.float32
    pipeline = AutoPipelineForText2Image.from_pretrained(
        model, torch_dtype=dtype, safety_checker=None, local_files_only=True
    )
    pipeline = pipeline.to(device)
    if hasattr(pipeline, "set_progress_bar_config"):
        pipeline.set_progress_bar_config(disable=True)

    # —— 两种模型的硬约束不同，且违反后不是“效果差一点”而是**坏掉** ——
    #
    # 1) 蒸馏 turbo 模型（sd-turbo / sdxl-turbo）：必须 `guidance_scale=0`（关 CFG）
    #    且只跑 1~4 步。按普通 SD 的 30 步/7.5 跑会得到噪点（实测）。
    # 2) SDXL 系列：
    #    · 尺寸必须是 **64 的倍数**，且短边建议 >= 512（训练分辨率 1024²）；
    #      给 500×500 这类值会直接抛错或出扭曲构图。
    #    · 16GB 显存下 UNet（fp16 约 5GB）+ 双 text encoder 能装下，但再大尺寸需
    #      切分注意力——这里不自动开（切分降低速度且对出图质量有影晌），
    #      超过 1024² 时由调用方自己权衡。
    is_sdxl = "sdxl" in os.path.basename(model.rstrip("/\\")).lower() or os.path.exists(
        os.path.join(model, "text_encoder_2")
    )
    width, height = int(size[0]), int(size[1])
    target_guidance = float(guidance)
    if is_sdxl:
        width = max(512, (width + 32) // 64 * 64)
        height = max(512, (height + 32) // 64 * 64)
        if "turbo" in os.path.basename(model.rstrip("/\\")).lower():
            target_guidance = 0.0   # sdxl-turbo 同样是蒸馏模型

    generator = torch.Generator(device=device).manual_seed(int(seed))
    began = time.time()
    with torch.inference_mode():
        image = pipeline(
            prompt=prompt,
            num_inference_steps=int(steps),
            guidance_scale=target_guidance,
            width=width,
            height=height,
            generator=generator,
        ).images[0]
    elapsed = time.time() - began
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    image.save(out_path)
    return elapsed, image.size


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--prompt", default=None)
    parser.add_argument("--negative", default=None,
                        help="负面提示词（sd-turbo 的 CFG 关闭时无效，仅为接口一致保留）")
    parser.add_argument("--out", default=None)
    parser.add_argument("--size", default="512x512")
    parser.add_argument("--steps", type=int, default=2)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--guidance", type=float, default=0.0)
    parser.add_argument("--model", default=None)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--list-models", action="store_true")
    args = parser.parse_args()

    if args.list_models:
        base = os.path.join(repo_root(), "resources", "models", "imagegen")
        if os.path.isdir(base):
            for name in sorted(os.listdir(base)):
                path = os.path.join(base, name)
                if os.path.isdir(path):
                    size = sum(
                        os.path.getsize(os.path.join(root, f))
                        for root, _, files in os.walk(path)
                        for f in files
                    )
                    print(f"{name}  {size/1024/1024:.0f} MB  {path}")
        else:
            print("（无模型目录）")
        return 0

    if args.check:
        ok, note = check_env()
        print(("OK: " if ok else "不可用: ") + note)
        return 0 if ok else 2

    if not args.prompt or not args.out:
        print("需要 --prompt 与 --out（或用 --check / --list-models）", file=sys.stderr)
        return 2
    ok, note = check_env()
    if not ok:
        print(f"图像生成不可用：{note}", file=sys.stderr)
        return 2

    try:
        width, height = (int(v) for v in args.size.lower().split("x"))
    except Exception:
        print(f"--size 需要 WxH 形式，得到：{args.size}", file=sys.stderr)
        return 2

    model = args.model or default_model()
    try:
        elapsed, actual = generate(args.prompt, args.out, (width, height), args.steps,
                                   args.seed, model, args.guidance)
    except Exception as exc:  # noqa: BLE001
        print(f"生成失败：{type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    print(f"{args.out}  {actual[0]}x{actual[1]}  {elapsed:.2f}s  steps={args.steps} seed={args.seed}",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
