#!/usr/bin/env python
# prepare-convert-mirror.py -- 为「分片命名不符合惯例」的 HF 仓库造一个转换器友好的镜像目录
#
# 背景（实测缺陷）：convert_hf_to_gguf.py 先按 `model*.safetensors` 前缀探测分片
# （conversion/base.py:232-236）。像 Qwen3.6-35B-A3B-FP8 这种把权重拆成
# `layers-N.safetensors` / `outside.safetensors` / `mtp.safetensors` 的仓库探测失败，
# 于是退化去找 `pytorch_model.bin.index.json`，最终 **0 张量**——转换"成功"但只导出词表空壳。
#
# 本脚本不改引擎源码，而是造一个镜像目录：
#   * 权重分片用**硬链接**改名成 model-000NN-of-000MM.safetensors（同卷零拷贝）
#   * 重写 model.safetensors.index.json 的 weight_map 指向新名字
#   * 复制 config / tokenizer 等小文件
#
# 用法:
#   python scripts/prepare-convert-mirror.py --hf <src_dir> --out <mirror_dir>
import argparse
import json
import os
import shutil
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hf", required=True, help="原始 HF 目录")
    ap.add_argument("--out", required=True, help="镜像目录（会被创建/覆盖索引）")
    ap.add_argument("--force", action="store_true", help="已存在时重建链接")
    a = ap.parse_args()

    src = os.path.abspath(a.hf)
    dst = os.path.abspath(a.out)
    idx_src = os.path.join(src, "model.safetensors.index.json")
    if not os.path.isfile(idx_src):
        print(f"[err] 找不到 {idx_src}", file=sys.stderr)
        return 1
    with open(idx_src, encoding="utf-8") as f:
        index = json.load(f)
    weight_map = index["weight_map"]
    old_parts = sorted(set(weight_map.values()))
    print(f"[读] {len(weight_map)} 张量 / {len(old_parts)} 个分片")

    os.makedirs(dst, exist_ok=True)
    n = len(old_parts)
    rename = {old: f"model-{i + 1:05d}-of-{n:05d}.safetensors" for i, old in enumerate(old_parts)}

    linked = copied = skipped = 0
    for old, new in rename.items():
        s, d = os.path.join(src, old), os.path.join(dst, new)
        if os.path.exists(d) and os.path.getsize(d) == os.path.getsize(s):
            skipped += 1
            continue
        if os.path.exists(d):
            os.remove(d)
        try:
            os.link(s, d)                      # NTFS 硬链接：零拷贝
            linked += 1
        except OSError:
            shutil.copy2(s, d)                 # 跨卷回退
            copied += 1
        print(f"  {old} -> {new} ({os.path.getsize(s)/2**30:.2f} GB)", flush=True)
    print(f"[链接] 硬链接 {linked}，复制 {copied}，已存在 {skipped}")

    new_map = {k: rename[v] for k, v in weight_map.items()}
    with open(os.path.join(dst, "model.safetensors.index.json"), "w", encoding="utf-8") as f:
        json.dump({**index, "weight_map": new_map}, f, indent=2, ensure_ascii=False)
    print("[索引] 已重写 model.safetensors.index.json")

    for name in sorted(os.listdir(src)):
        if name.endswith((".safetensors",)) or name == "model.safetensors.index.json":
            continue
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if os.path.isfile(s) and not os.path.exists(d):
            shutil.copy2(s, d)
    print(f"[元数据] 小文件已复制到 {dst}")
    print("MIRROR-OK", dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
