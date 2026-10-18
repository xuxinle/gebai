#!/usr/bin/env python3
"""从 hf-mirror 下载 sd-turbo（分段并行 + 断点续传）。

为什么不用 huggingface_hub：直连 huggingface.co 超时（国内网络），而 hf-mirror 是
可用镜像；`huggingface_hub` 默认走直连，改端点在部分版本上不生效（实测）。
自己按文件清单下载更可控：能显示进度、能断点续传、失败只重下那一个文件。

用法：
    python tools/fetch_sd_turbo.py <目标目录> [并发=8]
"""
import json
import os
import subprocess
import sys
import urllib.request

REPO = "stabilityai/sd-turbo"
BASE = "https://hf-mirror.com"
# 只要 fp16 与本体的必要文件（int8/全精度权重会让体积翻倍而质量无感提升）
FILES = [
    "model_index.json",
    "scheduler/scheduler_config.json",
    "text_encoder/config.json",
    "text_encoder/model.fp16.safetensors",
    "tokenizer/merges.txt",
    "tokenizer/special_tokens_map.json",
    "tokenizer/tokenizer_config.json",
    "tokenizer/vocab.json",
    "unet/config.json",
    "unet/diffusion_pytorch_model.fp16.safetensors",
    "vae/config.json",
    "vae/diffusion_pytorch_model.fp16.safetensors",
]
UA = {"User-Agent": "Mozilla/5.0"}


def url_of(rel):
    return f"{BASE}/{REPO}/resolve/main/{rel}"


def already_there(path):
    """已存在且非空即视为完成（HF 的 LFS 文件用重定向，长度在 HEAD 里能拿到）。"""
    return os.path.exists(path) and os.path.getsize(path) > 0


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    target = sys.argv[1]
    workers = sys.argv[2] if len(sys.argv) > 2 else "8"
    os.makedirs(target, exist_ok=True)
    pfetch = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pfetch.py")

    for rel in FILES:
        dest = os.path.join(target, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if already_there(dest):
            print(f"跳过（已存在）{rel}  {os.path.getsize(dest)/1024/1024:.1f} MB", flush=True)
            continue
        print(f"下载 {rel}", flush=True)
        # 小文件直接取；大文件（safetensors）走并行分段
        if rel.endswith(".safetensors"):
            subprocess.run([sys.executable, pfetch, url_of(rel), dest, workers], check=True)
        else:
            req = urllib.request.Request(url_of(rel), headers=UA)
            with urllib.request.urlopen(req, timeout=60) as resp, open(dest, "wb") as out:
                out.write(resp.read())
            print(f"  完成 {rel}（{os.path.getsize(dest)} 字节）", flush=True)

    total = sum(
        os.path.getsize(os.path.join(target, rel.replace("/", os.sep)))
        for rel in FILES
        if os.path.exists(os.path.join(target, rel.replace("/", os.sep)))
    )
    print(f"\n全部完成：{target}（{total/1024/1024:.1f} MB / {len(FILES)} 个文件）", flush=True)


if __name__ == "__main__":
    main()
