"""把 8 个素材串成一支演示片（生成裁云剧本）。

## 为何用生成脚本而不是手写剧本

每个素材是完整的 `SceneSpec`（自带 camera/lighting/objects），
要嵌进剧本的 `model_script` 字段。手写会把这些 JSON 复制 8 遍，
改一个素材就要改剧本——生成脚本让剧本始终跟素材同步。

## 用法

    py tools/build_showcase_script.py                 # 生成到 out/showcase/
    py tools/build_showcase_script.py --out x.json
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS = os.path.join(ROOT, "assets", "models")

# 每个镜头：素材名、主标题、副标题、背景双色、强调色
SHOTS = [
    ("phone",    "PHONE",    "金属机身 · 发光屏幕 · 三摄系统",   ["#0a0e18", "#141c2e"], "#4a90e2"),
    ("computer", "COMPUTER", "显示器 · 支架 · 背光键盘",          ["#0b1018", "#17202f"], "#5aa8ff"),
    ("camera",   "CAMERA",   "镜头筒 · 光圈环 · 机械拨盘",        ["#0d0b14", "#1a1626"], "#c9a227"),
    ("car",      "CAR",      "金属车漆 · 四轮 · 前灯",            ["#10100e", "#1f1d18"], "#e04a4a"),
    ("plane",    "PLANE",    "机身 · 机翼 · 双引擎",              ["#080d16", "#121d2e"], "#8fb8e8"),
    ("robot",    "ROBOT",    "发光核心 · 关节结构 · 哑光机身",    ["#070f14", "#0e1f2a"], "#4ee8ff"),
    ("man",      "MAN",      "基础人形 · 可作占位",               ["#0e1014", "#1a1f27"], "#8a9bb5"),
    ("woman",    "WOMAN",    "基础人形 · 可作占位",               ["#100e14", "#1f1a26"], "#c07a9e"),
]


def load_model(name):
    path = os.path.join(MODELS, f"{name}.json")
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def build():
    scenes = []
    for name, title, subtitle, background, accent in SHOTS:
        spec = load_model(name)
        # 素材自带的相机/光照保留；演示片只需要它成图
        scenes.append({
            "kind": "custom",
            "duration": 2.6,
            "background": background,
            "glow": 0.32,
            "glow_cx": 0.5,
            "glow_cy": 0.42,
            "glow_radius": 0.7,
            "particles": 48,
            "particle_alpha": 0.3,
            "layers": [
                {
                    "kind": "model3d",
                    "name": name,
                    "x": "22%", "y": "2%", "w": "56%", "h": "66%",
                    "model_script": spec,
                    "enter": "fade", "enter_at": 0.05, "enter_dur": 0.9,
                },
                {
                    "kind": "text",
                    "text": title,
                    "x": "8%", "y": "70%", "w": "84%", "h": "7%",
                    "size": 54, "color": accent, "bold": True,
                    "align": "center", "tracking": 0.16,
                    "enter": "rise", "enter_at": 0.5, "enter_dur": 0.6,
                },
                {
                    "kind": "text",
                    "text": subtitle,
                    "x": "8%", "y": "78.5%", "w": "84%", "h": "5%",
                    "size": 24, "color": "#93a4bd",
                    "align": "center",
                    "enter": "fade", "enter_at": 0.8, "enter_dur": 0.6,
                },
            ],
        })

    return {
        "name": "内置素材演示",
        "fps": 30,
        "size": [1920, 1080],
        "transition": 0.45,
        "fade_in": 0.4,
        "fade_out": 0.6,
        "brand_wordmark": "歌白",
        "brand_latin": "GEBAI ENGINE",
        "footer_note": "8 个内置程序化素材",
        "audio": {"enabled": False},
        "scenes": scenes,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join(ROOT, "out", "showcase", "assets-showcase-v1.json"))
    args = parser.parse_args()
    script = build()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(script, handle, ensure_ascii=False, indent=2)
    print(f"已生成 {args.out}（{len(script['scenes'])} 镜头）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
