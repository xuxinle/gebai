"""为 A1111 单文件 SD1.5 模型造一份**完整的离线 diffusers 配置目录**。

## 为什么要这个

`from_single_file` 必须知道 UNet/VAE/TextEncoder 的超参才能把权重装进去。
它默认从 HF Hub 拉 `stable-diffusion-v1-5` 的配置——本机没网直接失败
（实测：WinError 10060 / OfflineModeIsEnabled）。

手工补目录的坑（我踩了四次）：
1. 只给 `model_index.json` → 它按**默认 CLIP 768 维**构造，
   而 SD1.5 是 **512 维**，报 shape mismatch；
2. 补了 `text_encoder/config.json` → 要求 `tokenizer`，而 tokenizer 又要
   `vocab.json` / `merges.txt`（好在这两个 SD1.5 与 SDXL 通用，本地就有）；
3. 补了 tokenizer → 又要求 `scheduler/config.json`；
4. 补了 scheduler → 又要求 `unet/config.json`、`vae/config.json`。

所以**一次性把所有子配置写全**，而不是"报错一个补一个"。

## SD1.5 的真实超参（照抄就没问题）

| 组件 | 关键值 |
|---|---|
| UNet | in=4, out=4, block=(320,640,1280,1280), layers=2, attn_head=8, cross_dim=768 |
| VAE | latent=4, block=(128,256,512,512), layers=2, attn_head=1 |
| TextEncoder | hidden=768, layers=12, heads=12, max_pos=77, projection=512 |
| Scheduler | PNDM（A1111 默认）→ 我们用 EulerAncestral 覆盖，行为更稳 |
"""
import json
import os
import shutil
import sys

OUT = "build/sd15/sd15-config"
TOKENIZER_SRC = (r"C:\Users\Administrator\code\gebai\resources\models\imagegen"
                 r"\sdxl-turbo\tokenizer")


def write(path: str, payload: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)


def main() -> int:
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT, exist_ok=True)

    write(os.path.join(OUT, "model_index.json"), {
        "_class_name": "StableDiffusionControlNetPipeline",
        "_diffusers_version": "0.35.2",
        "scheduler": ["diffusers", "EulerAncestralDiscreteScheduler"],
        "text_encoder": ["transformers", "CLIPTextModel"],
        "tokenizer": ["transformers", "CLIPTokenizer"],
        "unet": ["diffusers", "UNet2DConditionModel"],
        "vae": ["diffusers", "AutoencoderKL"],
        "safety_checker": [None, None],
        "feature_extractor": [None, None],
        "controlnet": ["diffusers", "ControlNetModel"],
    })

    write(os.path.join(OUT, "unet", "config.json"), {
        "_class_name": "UNet2DConditionModel",
        "_diffusers_version": "0.35.2",
        "act_fn": "silu",
        "addition_embed_type": None,
        "attention_head_dim": 8,
        "block_out_channels": [320, 640, 1280, 1280],
        "center_input_sample": False,
        "class_embed_type": None,
        "cross_attention_dim": 768,
        "down_block_types": ["CrossAttnDownBlock2D", "CrossAttnDownBlock2D",
                             "CrossAttnDownBlock2D", "DownBlock2D"],
        "downsample_padding": 1,
        "flip_sin_to_cos": True,
        "freq_shift": 0,
        "in_channels": 4,
        "layers_per_block": 2,
        "mid_block_scale_factor": 1,
        "norm_eps": 1e-05,
        "norm_num_groups": 32,
        "out_channels": 4,
        "projection_class_embeddings_input_dim": None,
        "resnet_time_scale_shift": "default",
        "sample_size": 64,
        "time_embedding_act_fn": None,
        "time_embedding_dim": None,
        "time_embedding_type": "positional",
        "transformer_layers_per_block": 1,
        "up_block_types": ["UpBlock2D", "CrossAttnUpBlock2D", "CrossAttnUpBlock2D",
                           "CrossAttnUpBlock2D"],
        "upcast_attention": False,
        "use_linear_projection": False,
    })

    write(os.path.join(OUT, "vae", "config.json"), {
        "_class_name": "AutoencoderKL",
        "_diffusers_version": "0.35.2",
        "act_fn": "silu",
        "block_out_channels": [128, 256, 512, 512],
        "down_block_types": ["DownEncoderBlock2D", "DownEncoderBlock2D",
                             "DownEncoderBlock2D", "DownEncoderBlock2D"],
        "force_upcast": True,
        "in_channels": 3,
        "latent_channels": 4,
        "layers_per_block": 2,
        "norm_num_groups": 32,
        "out_channels": 3,
        "sample_size": 512,
        "scaling_factor": 0.18215,
        "up_block_types": ["UpDecoderBlock2D", "UpDecoderBlock2D",
                           "UpDecoderBlock2D", "UpDecoderBlock2D"],
    })

    write(os.path.join(OUT, "text_encoder", "config.json"), {
        "_class_name": "CLIPTextModel",
        "_diffusers_version": "0.35.2",
        "architectures": ["CLIPTextModel"],
        "attention_dropout": 0.0,
        "bos_token_id": 49406,
        "eos_token_id": 49407,
        "hidden_act": "quick_gelu",
        "hidden_size": 768,
        "initializer_factor": 1.0,
        "initializer_range": 0.02,
        "intermediate_size": 3072,
        "layer_norm_eps": 1e-05,
        "max_position_embeddings": 77,
        "model_type": "clip_text_model",
        "num_attention_heads": 12,
        "num_hidden_layers": 12,
        "pad_token_id": 1,
        "projection_dim": 512,
        "torch_dtype": "float16",
    })

    write(os.path.join(OUT, "scheduler", "scheduler_config.json"), {
        "_class_name": "EulerAncestralDiscreteScheduler",
        "_diffusers_version": "0.35.2",
        "beta_end": 0.012,
        "beta_schedule": "scaled_linear",
        "beta_start": 0.00085,
        "num_train_timesteps": 1000,
        "prediction_type": "epsilon",
        "steps_offset": 1,
        "timestep_spacing": "leading",
        "trained_betas": None,
    })

    # tokenizer：SD1.5 与 SDXL 共用同一套 CLIP 词表，从本地拷。
    token_dir = os.path.join(OUT, "tokenizer")
    os.makedirs(token_dir, exist_ok=True)
    copied = 0
    for name in ("vocab.json", "merges.txt", "special_tokens_map.json", "tokenizer_config.json"):
        source = os.path.join(TOKENIZER_SRC, name)
        if os.path.exists(source):
            shutil.copy2(source, os.path.join(token_dir, name))
            copied += 1

    print(f"配置目录已生成：{OUT}")
    print(f"  子配置：unet / vae / text_encoder / scheduler / tokenizer({copied} 个文件)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
