"""批量合成歌白宣传片配音（本地 VITS，离线）。

为何不用 tts_speak 工具：本环境下该工具的神经路径返回「python 未返回报告」
（同一份模型/解释器手搓脚本正常），所以这里直接用同一个 venv 跑同一套 sherpa-onnx 调用。
逐句落盘并打印实测时长——镜头窗口按这个时长重排（画面等解说，不是解说追画面）。
"""
import json
import os
import wave

import numpy as np
import sherpa_onnx

DIR = r"C:\Users\Administrator\code\gebai\resources\models\tts\vits-zh-theresa"
OUT_DIR = r"C:\Users\Administrator\code\gebai\shuangtian\examples\caiyun\gbfilm\vo"

LINES = [
    ("v1", "旧世界的一切技术，都是可以收编的躯体。"),
    ("v2", "歌白把任意语言、任意进程，收进同一个工具面。"),
    ("v3", "读，写，跑。二十三种能力，一次装载。"),
    ("v4", "它在无头的屏幕后面，构建、运行、验证。"),
    ("v5", "它驱动三维的世界，一层一层搭起来。"),
    ("v6", "而它改自己，也走同一条回路。"),
    ("v7", "歌白。智能无状态，责任在智体。"),
]

onnx = next(os.path.join(DIR, n) for n in os.listdir(DIR) if n.endswith(".onnx"))
fsts = [os.path.join(DIR, n) for n in ("date.fst", "number.fst", "phone.fst", "new_heteronym.fst")
        if os.path.exists(os.path.join(DIR, n))]
cfg = sherpa_onnx.OfflineTtsConfig(
    model=sherpa_onnx.OfflineTtsModelConfig(
        vits=sherpa_onnx.OfflineTtsVitsModelConfig(
            model=onnx,
            tokens=os.path.join(DIR, "tokens.txt"),
            lexicon=os.path.join(DIR, "lexicon.txt"),
        ),
        num_threads=4,
    ),
    rule_fsts=",".join(fsts),
)
assert cfg.validate(), "sherpa 配置无效"
tts = sherpa_onnx.OfflineTts(cfg)
print("speakers:", tts.num_speakers)

os.makedirs(OUT_DIR, exist_ok=True)
report = {}
for key, text in LINES:
    audio = tts.generate(text, sid=0, speed=1.0)
    if not audio or not len(audio.samples):
        raise RuntimeError(f"{key} 合成空音频")
    pcm = (np.clip(audio.samples, -1.0, 1.0) * 32767.0).astype(np.int16)
    path = os.path.join(OUT_DIR, key + ".wav")
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(audio.sample_rate)
        f.writeframes(pcm.tobytes())
    dur = len(audio.samples) / float(audio.sample_rate)
    report[key] = {"text": text, "duration": round(dur, 3), "path": path}
    print(f"{key}: {dur:.2f}s  {text}")

with open(os.path.join(OUT_DIR, "durations.json"), "w", encoding="utf-8") as f:
    json.dump(report, f, ensure_ascii=False, indent=2)
print("total:", round(sum(v["duration"] for v in report.values()), 2), "s")
