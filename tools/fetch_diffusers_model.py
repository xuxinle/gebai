#!/usr/bin/env python3
"""从 hf-mirror 下载任意 diffusers 模型（分段并行 + 断点续传 + 体积可控）。

为什么不用 huggingface_hub：本机直连 huggingface.co 超时（国内网络），hf-mirror 可用，
而 huggingface_hub 改端点在部分版本上不生效（实测）。自己按文件清单下载更可控：
能显示进度、能断点续传、失败只重下那一个文件、能只取 fp16 变体（体积砍一半）。

用法：
    python tools/fetch_diffusers_model.py <repo-id> <目标目录> [选项]

选项：
    --variant fp16        只下 fp16 变体（默认；体积约为全精度一半）
    --variant all         连全精度一起下（SDXL 会到 ~13GB）
    --workers N           并行段数（默认 8）
    --skip-unused         跳过用不到的权重变体文件（如 sd_xl_turbo_1.0*.safetensors 单文件版）
    --list                只列出将要下载的文件与总体积，不实际下载

示例：
    python tools/fetch_diffusers_model.py stabilityai/sdxl-turbo resources/models/imagegen/sdxl-turbo
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.request

BASE = "https://hf-mirror.com"
UA = {"User-Agent": "Mozilla/5.0"}

# diffusers 加载一个 pipeline 真正需要的文件（按 model_index 的结构）。
# 不在这个集合里的（如整包单文件 _fp16.safetensors、vae_decoder/vae_encoder 的权重）
# 下下来也用不上——体积却翻倍，所以默认跳过。
NEEDED_PATTERNS = (
    "model_index.json",
    "/config.json",
    "/scheduler_config.json",
    "/vocab.json",
    "/merges.txt",
    "/special_tokens_map.json",
    "/tokenizer_config.json",
    "/added_tokens.json",
    "/diffusion_pytorch_model.safetensors",
    "/diffusion_pytorch_model.fp16.safetensors",
    "/model.safetensors",
    "/model.fp16.safetensors",
)
# 明确不要的单文件整包权重（diffusers 分层加载用不到）
SKIP_PATTERNS = ("/sd_xl_turbo_1.0", "/sd_xl_base_1.0")


def api_files(repo):
    url = f"{BASE}/api/models/{repo}"
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return [item["rfilename"] for item in data.get("siblings", [])]


def size_of(repo, rel):
    """HEAD 拿 Content-Length（LFS 文件走重定向，需 allow_redirects）。"""
    url = f"{BASE}/{repo}/resolve/main/{rel}"
    req = urllib.request.Request(url, method="HEAD", headers=UA)
    with urllib.request.urlopen(req, timeout=30) as resp:
        value = resp.headers.get("Content-Length")
        return int(value) if value else 0


def pick(files, variant):
    chosen = []
    for rel in files:
        if rel.endswith(".md") or rel.endswith(".gitattributes"):
            continue
        if any(rel.startswith(p.strip("/")) for p in []):
            continue
        if any(p in rel for p in SKIP_PATTERNS):
            continue
        is_fp16 = "fp16" in rel
        if variant == "fp16" and not is_fp16 and any(
            rel.endswith(name) for name in ("model.safetensors", "diffusion_pytorch_model.safetensors")
        ):
            # 只要 fp16 变体时，跳过全精度同名权重
            continue
        if not any(p in rel or rel == p.strip("/") for p in NEEDED_PATTERNS):
            # 也保留根级 model_index.json
            if rel != "model_index.json":
                continue
        chosen.append(rel)
    return chosen


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("repo", help="HF 仓库 id，如 stabilityai/sdxl-turbo")
    parser.add_argument("target", help="本地目标目录")
    parser.add_argument("--variant", default="fp16", choices=["fp16", "all"])
    parser.add_argument("--workers", default="8")
    parser.add_argument("--skip-unused", action="store_true", default=True)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    print(f"仓库 {args.repo} → {args.target}", flush=True)
    try:
        files = api_files(args.repo)
    except Exception as exc:  # noqa: BLE001
        print(f"取文件清单失败：{exc}", file=sys.stderr)
        return 2
    chosen = pick(files, args.variant)
    print(f"待下载 {len(chosen)} 个文件（variant={args.variant}）", flush=True)

    total = 0
    entries = []
    for rel in chosen:
        try:
            size = size_of(args.repo, rel)
        except Exception:  # noqa: BLE001
            size = 0
        entries.append((rel, size))
        total += size
        print(f"  {size/1024/1024:8.1f} MB  {rel}", flush=True)
    print(f"合计 {total/1024/1024/1024:.2f} GB", flush=True)
    if args.list:
        return 0

    pfetch = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pfetch.py")
    os.makedirs(args.target, exist_ok=True)
    failed = []
    for rel, size in entries:
        dest = os.path.join(args.target, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.exists(dest) and os.path.getsize(dest) == size and size > 0:
            print(f"跳过（已完成）{rel}", flush=True)
            continue
        url = f"{BASE}/{args.repo}/resolve/main/{rel}"
        print(f"下载 {rel}（{size/1024/1024:.1f} MB）", flush=True)
        if size > 4 * 1024 * 1024:
            code = subprocess.run([sys.executable, pfetch, url, dest, str(args.workers)]).returncode
        else:
            try:
                req = urllib.request.Request(url, headers=UA)
                with urllib.request.urlopen(req, timeout=120) as resp, open(dest, "wb") as out:
                    out.write(resp.read())
                code = 0
            except Exception as exc:  # noqa: BLE001
                print(f"  失败：{exc}", file=sys.stderr, flush=True)
                code = 1
        if code != 0:
            failed.append(rel)

    got = sum(
        os.path.getsize(os.path.join(args.target, rel.replace("/", os.sep)))
        for rel, _ in entries
        if os.path.exists(os.path.join(args.target, rel.replace("/", os.sep)))
    )
    print(f"\n完成 {len(entries) - len(failed)}/{len(entries)} 个文件，{got/1024/1024/1024:.2f} GB", flush=True)
    if failed:
        print("失败：\n  " + "\n  ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
