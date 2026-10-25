"""生成导演脚本 JSON（歌白能力片「收编」）。

为何用脚本生成：7 幕 × 每幕十几项特效字段 + 三维图层，手写必然漂移；
而字段名写错在裁云里是**静默忽略**的（画面不对但没有报错）。集中生成便于统一改口径。

配音起始时间按 `vo/durations.json` 的**实测时长**回填（画面等解说，不是解说追画面）。
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
VO = os.path.join(HERE, "vo")

with open(os.path.join(VO, "durations.json"), encoding="utf-8") as f:
    VOICE = json.load(f)

# 配音排布：每句起点（秒）。第二轮按实测微调过——句与句之间留 ≥0.6s 呼吸。
VOICE_START = {
    "v1": 0.90,
    "v2": 5.05,
    "v3": 10.10,
    "v4": 16.05,
    "v5": 21.35,
    "v6": 27.35,
    "v7": 32.85,
}
VOICE_GAIN = {"v1": 1.0, "v2": 1.0, "v3": 1.0, "v4": 1.0, "v5": 1.0, "v6": 1.0, "v7": 1.0}

ACCENT = "#3B82F6"
BG_TOP = "#08090D"
BG_BOT = "#101724"

# 幕 2 的三维共享世界（组首层矩形 = 整个可见画面；成员 y 越大越远）
WORLD_GROUP = "world"
MODEL3D_COMMON = {
    "cam_azimuth": 0,
    "cam_elevation": 22,
    "cam_distance": 3.4,
    "cam_fov": 30,
    "light_azimuth": 128,
    "light_elevation": 44,
    "light_ambient": 0.30,
    "light_rim": 0.46,
}


def scene_body_layers() -> list:
    """幕 2 的三维图层：一个共享世界（地面放在很远、缩到极小，只做透视线索）。"""
    layers = [{
        "kind": "model3d",
        "name": "head",
        "model_group": WORLD_GROUP,
        "x": "46%", "y": "6%", "w": "52%", "h": "86%",
        "model_shape": "plane",
        "model_color": "#000000",
        "model_color_alt": "#000000",
        # 亚像素尺度：组首只用来定“画框 + 相机/光照”，它的网格不能出现在画面上。
        # **两条实测约束**：
        # ① `model_scale` 在组内被夹到 **≥0.01**（写 0.0008 无效），所以缩不小；
        # ② 它在世界原点，正好落在框中心 ⇒ 画面中间一个 27px 黑方地。
        # 解：把它**侧对相机**（tilt 89°），平面变成一条细线 = 不可见。
        # 世界映射只取决于组首矩形的高（head_state.h），与 tilt 无关。
        "model_scale": 0.01,
        "model_tilt": 89,
        "model_backdrop": False,
        "enter": 0,
        # 组首只做“画框 + 相机/光照”。**颜色置全透明**：它的网格（plane）
        # 即使缩到 0.02 仍会在框中心留下一个黑方地（实测在环与球之间很显眼）。
        "model_shadow": 0.0,
        **MODEL3D_COMMON,
    }]
    objects = [
        # name, shape, x%, y%, w%, h%, spin, tilt, scale, 主色, 次色
        # 摆位实测（两条都得遵守）：
        # · 成员 y **越大越远**（深度）；
        # · 成员矩形中心相对**组首矩形中心**决定世界位置 ⇒ 要聚在框内就得靠近框中心。
        # 组首框：x 46%~98%、y 6%~92% ⇒ 可见区中心约在 (72%, 49%)。
        ("ring",  "torus",    "60%", "42%", "26%", "34%", 8, 20, 0.52, "#4D7DFF", "#16306B"),
        ("ball",  "sphere",   "52%", "52%", "15%", "22%", 6, 0,  0.26, "#9EC4FF", "#1B3F8F"),
        ("cube",  "box",      "72%", "56%", "13%", "19%", 10, 14, 0.20, "#3B82F6", "#0E1F44"),
    ]
    for name, shape, x, y, w, h, spin, tilt, scale, c1, c2 in objects:
        layers.append({
            "kind": "model3d",
            "name": name,
            "model_group": WORLD_GROUP,
            "x": x, "y": y, "w": w, "h": h,
            "model_shape": shape,
            "model_color": c1,
            "model_color_alt": c2,
            "model_spin": spin,
            "model_tilt": tilt,
            "model_scale": scale,
            # 上下浮动给“活体”感（幅度小但持续，且不改变构图）
            "model_bob": 0.05,
            "model_bob_hz": 0.42,
            "model_shadow": 0.58,
            "model_shadow_color": "#03050A",
            "model_backdrop": False,
            "enter": 0,
        })
    return layers


def kintext(text, x, y, w, h, size, color, **kw) -> dict:
    base = {
        "kind": "kintext",
        "text": text,
        "x": x, "y": y, "w": w, "h": h,
        "size": size,
        "color": color,
        "align": 1,
        "enter": 0,
        "kin_stagger": 0.06,
        "kin_dur": 0.5,
        "kin_enter": 1,
        "kin_rise": 24,
    }
    base.update(kw)
    return base


def scenes() -> list:
    return [
        # ── 幕 1：收编 4.6s ──
        {
            "kind": "custom",
            "duration": 4.6,
            "layout": "gbfilm-s1.js",
            "accent": ACCENT,
            "background": [BG_TOP, BG_BOT],
            "title": "", "subtitle": "", "wrap": False,
            "grain": 900, "grain_alpha": 0.10,
            "glow": 0.45, "glow_cx": 0.5, "glow_cy": 0.47, "glow_radius": 0.48,
            "particles": 60, "particle_alpha": 0.30,
            "cam_zoom_from": 1.0, "cam_zoom_to": 1.045,
            "cam_rotate": 0.5,
            "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                        "fill": "#000000", "opacity": 0.01, "enter": 0}],
        },
        # ── 幕 2：躯体 5.2s（三维共享世界）──
        {
            "kind": "custom",
            "duration": 5.2,
            "layout": "gbfilm-s2.js",
            "accent": ACCENT,
            "background": ["#070A10", "#0B1220"],
            "title": "", "subtitle": "", "wrap": False,
            "grain": 820, "grain_alpha": 0.09,
            "glow": 0.38, "glow_cx": 0.68, "glow_cy": 0.46, "glow_radius": 0.42,
            "particles": 46, "particle_alpha": 0.26,
            "cam_zoom_from": 1.0, "cam_zoom_to": 1.03,
            "cam_window_start": 0.2, "cam_window_end": 3.4,
            # 三维共享世界（组首 head + 三个物体），二维标识由 gbfilm-s2.js 画
            "layers": scene_body_layers(),
        },
        # ── 幕 3：工具面 5.8s ──
        {
            "kind": "custom",
            "duration": 5.8,
            "layout": "gbfilm-s3.js",
            "accent": ACCENT,
            "background": [BG_TOP, "#0E1420"],
            "title": "", "subtitle": "", "wrap": False,
            "grain": 760, "grain_alpha": 0.09,
            "glow": 0.22, "glow_cx": 0.5, "glow_cy": 0.55, "glow_radius": 0.55,
            "particles": 34, "particle_alpha": 0.20,
            "cam_zoom_from": 1.015, "cam_zoom_to": 1.0,
            "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                        "fill": "#000000", "opacity": 0.01, "enter": 0}],
        },
        # ── 幕 4：钻穿 5.4s（矢量相机）──
        {
            "kind": "custom",
            "duration": 5.4,
            "layout": "gbfilm-s4.js",
            "accent": ACCENT,
            "background": ["#05070C", "#0A1018"],
            "title": "", "subtitle": "", "wrap": False,
            "grain": 780, "grain_alpha": 0.09,
            "glow": 0.30, "glow_cx": 0.5, "glow_cy": 0.5, "glow_radius": 0.46,
            "particles": 38, "particle_alpha": 0.22,
            # 相机字段对本幕**无效**（shapes 不过相机）；推镜由脚本自己算
            "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                        "fill": "#000000", "opacity": 0.01, "enter": 0}],
        },
        # ── 幕 5：扩张 6.0s（336 点阵 + 承接）──
        {
            "kind": "custom",
            "duration": 6.0,
            "layout": "gbfilm-s5.js",
            "accent": ACCENT,
            "background": ["#06080E", "#0C1220"],
            "title": "", "subtitle": "", "wrap": False,
            "grain": 900, "grain_alpha": 0.10,
            "glow": 0.40, "glow_cx": 0.5, "glow_cy": 0.5, "glow_radius": 0.5,
            "particles": 52, "particle_alpha": 0.26,
            "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                        "fill": "#000000", "opacity": 0.01, "enter": 0}],
            # 承接：中心一块实心方块长满全屏后淡出，把"量级"交给下一幕
            # （at < 边界 < at+dur：4.35 < 6.0 < 5.05+... 见下方 into/dur）
            "carry_out": {
                "name": "承接·量级",
                "into": 5,
                "at": 5.35,
                "dur": 0.62,
                "from": {"x": "47%", "y": "46%", "w": "6%", "h": "8%", "radius": 6},
                "to": {"x": "-4%", "y": "-4%", "w": "108%", "h": "108%", "radius": 0},
                "fill": "#0E1A3044",
                "stroke": "#4D7DFF",
                "stroke_width": 2,
                "curve": "expo_out",
                "fade_out": 0.55,
            },
        },
        # ── 幕 6：回路 5.4s ──
        {
            "kind": "custom",
            "duration": 5.4,
            "layout": "gbfilm-s6.js",
            "accent": ACCENT,
            "background": ["#06080E", "#0B111C"],
            "title": "", "subtitle": "", "wrap": False,
            "grain": 820, "grain_alpha": 0.09,
            "glow": 0.34, "glow_cx": 0.5, "glow_cy": 0.47, "glow_radius": 0.44,
            "particles": 44, "particle_alpha": 0.24,
            "cam_zoom_from": 1.0, "cam_zoom_to": 1.035,
            "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                        "fill": "#000000", "opacity": 0.01, "enter": 0}],
        },
        # ── 幕 7：字标 6.0s ──
        {
            "kind": "custom",
            "duration": 6.0,
            "layout": "gbfilm-s7.js",
            "accent": ACCENT,
            "background": ["#05070C", "#0A1120"],
            "title": "", "subtitle": "", "wrap": False,
            "grain": 1000, "grain_alpha": 0.11,
            "glow": 0.58, "glow_cx": 0.5, "glow_cy": 0.46, "glow_radius": 0.52,
            "particles": 70, "particle_alpha": 0.30,
            "god_ray": 0.14, "god_ray_angle": -1.32,
            "light_sweep": 0.22,
            "cam_zoom_from": 1.02, "cam_zoom_to": 1.0,
            "fade_out": 0.0,
            "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                        "fill": "#000000", "opacity": 0.01, "enter": 0}],
        },
    ]


def main() -> None:
    voices = []
    for key in ("v1", "v2", "v3", "v4", "v5", "v6", "v7"):
        info = VOICE[key]
        voices.append({
            "path": "vo/" + key + ".wav",
            "start": round(VOICE_START[key], 2),
            "gain": VOICE_GAIN[key],
        })

    total = sum(s["duration"] for s in scenes())
    script = {
        "name": "歌白 · 收编（能力演示）",
        "fps": 30,
        "size": [1920, 1080],
        "transition": 0.30,
        "fade_in": 0.55,
        "fade_out": 0.85,
        "brand_wordmark": "",
        "brand_latin": "",
        "footer_note": "GEBAI AGENT · 能力演示",
        "audio": {
            "enabled": True,
            "style": "calm",
            "bpm": 96,
            "gain": 0.42,
            "voice": voices,
        },
        "scenes": scenes(),
    }

    out = os.path.join(HERE, "gbfilm.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(script, f, ensure_ascii=False, indent=2)

    print(f"写入 {out}")
    print(f"总时长 {total:.2f}s = {total * 30:.0f} 帧 @30fps")
    print("配音排布：")
    for key in ("v1", "v2", "v3", "v4", "v5", "v6", "v7"):
        s = VOICE_START[key]
        d = VOICE[key]["duration"]
        print(f"  {key}: {s:.2f}s → {s + d:.2f}s  ({d:.2f}s)  「{VOICE[key]['text']}」")


if __name__ == "__main__":
    main()
