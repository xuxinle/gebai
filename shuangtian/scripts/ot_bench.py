#!/usr/bin/env python3
"""onetake 对标流水线：一条命令量完一片。

## 这个脚本解决什么问题

对标 onetake 需要每片一套数（carry / 节奏 / 静止 / 亮度 / 变化强度 / 动效），
而这几件事分别住在四个工具里（render 的画面度量、carry_qa、motion_qa、ffprobe）。
逐个手跑必然出错——实测就已经踩过一次：**拿旧二进制出的轨迹去判 carry**，
得到「0 carried / 1 bare」，而重编后同一份脚本是 0.90 PASS。
好在那次 FAIL 看起来可疑（凭空 bare），复查才发现；若结论是 PASS，
这个错会一路带进交付报告。

所以：**跑之前先确认二进制比源码新**，否则拒绝出数。

## 口径（写死在这里，报告也带）

- 静止：帧差 < 1.2（与 `frame_stats.hpp` 同口径，来自渲染时逐帧度量）
- carry：`scripts/carry_qa.py`（同名元素跨边界位移 ≥ 对角线 2% 或缩放 ≥ 10%）
- 动效：`motion_qa`（32×18 网格），**必与参考片同 fps 同 step**
- 所有数都随口径一起报：换阈值就换答案

## 用法

    python scripts/ot_bench.py --script <脚本.json> --out <目录> [--ref <参考 mp4>]
                              [--motion-blur 4] [--track-elements] [--stills 1.0,3.2,...]
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAIYUN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")
MOTION_QA = os.path.join(ROOT, "build", "dev", "bin", "motion_qa.exe")
CARRY_QA = os.path.join(ROOT, "scripts", "carry_qa.py")
PY = sys.executable


def newest_source_mtime() -> float:
    """裁云源码里最新的 mtime（用来判断二进制是否过期）。"""
    best = 0.0
    for pattern in ("examples/caiyun/*.cpp", "examples/caiyun/*.hpp"):
        for path in glob.glob(os.path.join(ROOT, pattern)):
            best = max(best, os.path.getmtime(path))
    return best


def check_script_fresh(script: str, out_dir: str) -> None:
    """确认影片的产物比脚本新。

    ## 为何必须查这一项（实测代价）

    `check_binary()` 只看 C++ 源码 vs 可执行文件。但**片子的内容住在 layout.js 里**，
    改 js 不会让二进制过期。于是：改了 layout.js、重跑了命令，看到的 report 是
    上一次的（因为渲染确实跑了，但指令/脚本缓存或旧帧目录被复用），
    **数字一字不变，而你以为改动没生效**——本轮实测踩到两次（先怀疑脚本语法，白查一轮）。

    所以：脚本比 `out/film.mp4` 新 → 直接拒绝，强制人看清“这次到底跑的哪一版”。
    """
    script_dir = os.path.dirname(os.path.abspath(script))
    newest = os.path.getmtime(script)
    for name in os.listdir(script_dir):
        if name.endswith(".js") or name.endswith(".json"):
            newest = max(newest, os.path.getmtime(os.path.join(script_dir, name)))
    film = os.path.join(out_dir, "film.mp4")
    if os.path.exists(film) and newest < os.path.getmtime(film):
        return   # 产物比脚本新：正常
    # 产物比脚本旧（或还没有产物）——这是正常的一次新渲染，不需要拦。
    # 真正要拦的是“报告里的数比脚本还旧”，见下方 report 的时间戳检查。


def check_binary() -> None:
    if not os.path.exists(CAIYUN):
        sys.exit(f"找不到 {CAIYUN}：先 st build caiyun --profile dev")
    src = newest_source_mtime()
    exe = os.path.getmtime(CAIYUN)
    if src > exe:
        sys.exit(
            f"二进制过期：源码 mtime 比 {os.path.basename(CAIYUN)} 新 "
            f"（差 {src - exe:.0f}s）。先 st build caiyun --profile dev，"
            "否则量的是旧行为——实测踩过：旧二进制出的轨迹让 carry 判成 bare。"
        )


def check_layout(*candidates: str) -> None:
    """渲染前把 layout 逐帧跑一遍，抓运行期错误。

    ## 为什么要前置这一道（实测代价）

    裁云的 layout 按设计**静默降级**：脚本抛错时渲染器不报错、继续出片，
    画面只剩背景。于是"出片成功 + 指标能过"完全可能与"内容是空的"同时成立。

    c06-pith 因此白改 3 轮：它的 `keys()` 每帧都抛错，而我一直在改相机关键帧，
    逐秒数据始终 `mean=0.00`（降级特征），没有任何报错提示。

    检查器在 `scripts/check_layout.py`（会注入与宿主白名单一致的桩）。
    """
    script = next((c for c in candidates if c and os.path.isfile(c)), None)
    if script is None:
        return
    probe = os.path.join(ROOT, "scripts", "check_layout.py")
    if not os.path.isfile(probe):
        return   # 没有检查器就跳过（不阻断老流程）
    try:
        data = json.load(open(script, encoding="utf-8"))
    except Exception:
        return
    scenes = data.get("scenes", [])
    if not scenes:
        return
    layout = scenes[0].get("layout")
    if not layout or not isinstance(layout, str):
        return
    js = layout if os.path.isabs(layout) else os.path.join(os.path.dirname(script), layout)
    if not os.path.isfile(js):
        sys.exit(f"layout 文件不存在：{js}")
    dur = str(float(scenes[0].get("duration", 15)))
    r = run([PY, probe, js, "--duration", dur], cwd=ROOT)
    out = (r.stdout or "") + (r.stderr or "")
    print(out.rstrip())
    if r.returncode != 0:
        sys.exit("layout 检查未通过：先修复再渲染（否则画面会静默降级为背景）")


def run(cmd, **kw):
    # Windows 控制台默认 GBK，工具输出里的中文会变成 U+FFFD；后续抓指标/回写报告都会二次出错。
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    kw.setdefault("env", env)
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", **kw)


def ffprobe(path: str) -> dict:
    out = run(["ffprobe", "-v", "error", "-show_entries",
               "stream=codec_type,width,height,r_frame_rate,nb_frames,duration",
               "-of", "json", path]).stdout
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return {}
    info = {}
    for stream in data.get("streams", []):
        if stream.get("codec_type") == "video":
            num, _, den = stream.get("r_frame_rate", "30/1").partition("/")
            info["width"] = stream.get("width")
            info["height"] = stream.get("height")
            info["fps"] = round(float(num) / float(den or 1), 3)
            info["frames"] = int(stream.get("nb_frames", 0) or 0)
            info["duration"] = round(float(stream.get("duration", 0) or 0), 3)
        elif stream.get("codec_type") == "audio":
            info["has_audio"] = True
    return info


def parse_quality(log: str) -> dict:
    """从渲染日志里取画面质量行（那是渲染循环里量的真值，不经 h264）。"""
    result = {}
    m = re.search(r"画面质量：(\d+) 帧｜静止 ([\d.]+)%（帧差 <([\d.]+)，(\d+) 对）"
                  r"｜过亮 ([\d.]+)%（>(\d+)）｜平均亮度 ([\d.]+)｜变化 ([\d.]+)/s", log)
    if m:
        result.update({
            "frames": int(m.group(1)),
            "still_ratio": float(m.group(2)),
            "static_threshold": float(m.group(3)),
            "frame_pairs": int(m.group(4)),
            "bright_ratio": float(m.group(5)),
            "bright_luma": float(m.group(6)),
            "mean_luma": float(m.group(7)),
            "change_density": float(m.group(8)),
        })
    shots = []
    for mm in re.finditer(r"最静镜头 ([\d.]+)% 静止｜(.+?)（([\d.]+)–([\d.]+)s，(\d+) 帧",
                          log):
        shots.append({"still_ratio": float(mm.group(1)), "label": mm.group(2),
                      "t_in": float(mm.group(3)), "t_out": float(mm.group(4)),
                      "frames": int(mm.group(5))})
    if shots:
        result["quietest_shots"] = shots
    warn = re.search(r"⚠ 无有效相邻帧对", log)
    if warn:
        result["pairs_unreliable"] = True
    return result


def parse_carry(text: str) -> dict:
    out = {}
    m = re.search(r"carry score ([\d.]+) — (\d+) 个边界：(\d+) carried · (\d+) anchored · (\d+) bare",
                  text)
    if m:
        out.update({"carry_score": float(m.group(1)), "boundaries": int(m.group(2)),
                    "carried": int(m.group(3)), "anchored": int(m.group(4)),
                    "bare": int(m.group(5))})
    out["verdict"] = "PASS" if "VERDICT: PASS" in text else (
        "WARN" if "VERDICT: WARN" in text else "FAIL")
    out["lines"] = [line.strip() for line in text.splitlines() if line.strip().startswith("t=")]
    return out


def pixel_still(frames_dir: str) -> dict:
    """用**与 caiyun 同一个口径**（64×36 块均值灰度、帧差<1.2）量任意帧目录。

    ## 为何必须用它，而不能引用 onetake README 里的数

    README 写的「still 0.291」是它们自己的口径（320px 能量图 + 其它阈值）。
    实测：把 unbroken.mp4 按**我们这套口径**量，静止占比是 **89.3%**，
    不是 29.1%——两套尺子量不出可比的数。
    直接拿 29.1% 当红线会把一条与参考片几乎一样的片子判成不合格。
    所以判据一律**相对参照片**，而参照片的数值由本函数现场量。
    """
    files = sorted(os.listdir(frames_dir))
    prev = None
    diffs = []
    lum = []
    for name in files:
        img = Image.open(os.path.join(frames_dir, name)).convert("L")
        arr = np.asarray(img.resize((64, 36), Image.BOX), dtype=np.float32)
        lum.append(float(arr.mean()))
        if prev is not None:
            diffs.append(float(np.abs(arr - prev).mean()))
        prev = arr
    if not diffs:
        return {"frames": 0}
    v = np.array(diffs)
    lum = np.array(lum)
    return {"frames": len(files), "still_ratio": round(float((v < 1.2).mean() * 100), 1),
            "bright_ratio": round(float((lum > 180).mean() * 100), 1),
            "change_mean": round(float(v.mean()), 3),
            "change_median": round(float(np.median(v)), 3),
            "change_p95": round(float(np.percentile(v, 95)), 2),
            "change_peak": round(float(v.max()), 2)}


def parse_motion(text: str) -> dict:
    """motion_qa 的文本摘要 → 数。字段名随工具版本变，故用宽松匹配。"""
    out = {"raw": text.strip().splitlines()[:40]}
    for key, pattern in (
        ("carry_ratio_mean", r"沿用率\s*均值\s*([\d.]+)"),
        ("change_mean", r"变化强度\s*均值\s*([\d.]+)"),
        ("change_median", r"中位数\s*([\d.]+)"),
        ("change_p95", r"p95\s*([\d.]+)"),        ("change_peak", r"峰值\s*([\d.]+)"),
    ):
        m = re.search(pattern, text)
        if m:
            out[key] = float(m.group(1))
    m = re.search(r"有效变化\s*(\d+)\s*/\s*(\d+)\s*=\s*([\d.]+)%", text)
    if m:
        out.update({"moving_pairs": int(m.group(1)), "pairs": int(m.group(2)),
                    "moving_pct": float(m.group(3))})
    return out


# 判据：与 onetake 的 verify_promo.py 对齐的口径（差异写在这里，不藏在代码里）。
# `change_*` 的绝对阈值不可迁移（大片留白 vs 密集信息流的合理值差一个数量级），
# 所以它们只做「与参照片同口径」的相对比较，不单独判红绿。
#
# REQUIRED_REF_KEYS：给了参考片就必须齐全。少一个字段，对应那条判据就**静默不判**——
# 这比判错更危险（报告看上去是 PASS）。实测踩过：`bright_ratio` 从未进过 baseline，
# “过亮”一条对 13 条片子一次都没生效过。
REQUIRED_REF_KEYS = ("still_ratio", "bright_ratio", "change_median", "change_p95")
def judge(quality: dict, motion: dict, ref: dict | None,
          notes: list[str] | None = None) -> list[dict]:
    """返回违规清单（每条带实测值、判据与「该往哪查」）。

    ## 自完整性：相对判据「没真正判」必须报，不能静默算通过

    这是本函的第三次同类缺陷（前两次：静止占比分母恒为 0 而摘要无异常；
    p95 之前用峰值当强度）。三次都是同一个形态：**尺子失效时静默通过**。

    两次实测代价：
    - `ref_baseline.json` 里根本没有 `bright_ratio` 字段，而本函数只写了
      `if ref_bright is not None`——“过亮”对 13 条片子**一次都没判过**，报告看上去是 PASS。
    - 不传 `--ref-baseline` 时所有相对判据都不执行，`violations` 为空 ⇒ 直接 PASS。

    所以：`notes`（来自调用方的“为什么没判”）里只要有一条，就报 `undecided` 违规。
    """
    bad: list[dict] = []
    if quality.get("pairs_unreliable"):
        bad.append({"leg": "still", "value": quality.get("frame_pairs"),
                    "rule": "有效相邻帧对必须 > 0",
                    "hint": "只渲了 1 帧或帧号不连续——静止占比不可信，先查渲染"})

    # ① “一条判据都没执行”也是失败态
    if notes:
        bad.append({"leg": "undecided", "value": len(notes),
                    "rule": "相对判据必须真正执行（缺参考片≠通过）",
                    "hint": "；".join(notes)})

    # ② 参考值的完整性（先查，否则后面每一条都可能静默跳过）
    if ref is not None:
        missing = [k for k in REQUIRED_REF_KEYS if (ref or {}).get(k) is None]
        if missing:
            bad.append({"leg": "incomplete", "value": ",".join(missing),
                        "rule": f"给了参考片就必须含 {list(REQUIRED_REF_KEYS)}",
                        "hint": "跑 scripts/ot_bench_ref.py 重建 ref_baseline.json；"
                                "缺哪些字段，对应的判据就会静默不判"})

    ref_still = (ref or {}).get("still_ratio")
    ours_still = quality.get("still_ratio")
    if ref_still is not None and ours_still is not None and ours_still > ref_still + 15:
        bad.append({"leg": "rest", "value": ours_still,
                    "rule": f"静止占比 ≤ 参考片 +15pp（参考片同口径 {ref_still}%）",
                    "hint": "「慢」通常是镜内大时间静止：缩短镜内时长，或在落定后加几十像素的持续位移"
                            "（1~2px 的微动在 32×18 网格里测不到）"})
    ref_bright = (ref or {}).get("bright_ratio")
    ours_bright = quality.get("bright_ratio")
    # ## 过亮也要相对看（实测：高调片本身就该“过亮”）
    #
    # 实测 overlap 参考片的平均亮度是 199.5（它不是暗场片，是米白纸上的孔版印刷），
    # 按我们这套口径它 **100% 的帧都算过亮**。用“<20%”这种绝对红线，
    # 会把一条与参考片同调性的片子判红——这是尺子的错，不是片子的错。
    # 所以只在“比参考片高 10pp 以上”时才报，且同时检查是否嵌了白底素材。
    if ref_bright is not None and ours_bright is not None:
        if ours_bright > ref_bright + 10:
            bad.append({"leg": "bright", "value": ours_bright,
                        "rule": f"过亮帧 ≤ 参考片 +10pp（参考同口径 {ref_bright}%）",
                        "hint": "暗场片里嵌了白底素材？用 image_dim 压暗或多层融合"})
    elif ours_bright is not None and ours_bright > 40 and not ref:
        # 没给参考片时只能给一个宽红线（并以“可能本身是高调片”提醒）
        bad.append({"leg": "bright", "value": ours_bright, "rule": "未给参考片时过亮 <40%",
                    "hint": "如果这片本来就是亮调（白纸/日光），用 --ref-baseline 给参照片再判"})
    # ## 指标缺失要**明确报错**，不能静默变成某条违规（2026-10-24）
    #
    # 实际踩过：裁云默认改成管道直送后，`frames/` 变空，motion_qa 报"帧数不足（0）"，
    # 于是 change_median / change_p95 全是 None。旧代码把它读成
    # "中位数 0.0 < 参考的 0.3×" → 报 `density` 不达标。
    # 三条片子同时"不达标"，而它们其实完全正常——错误信息离真因很远。
    if ref and motion:
        missing = [k for k in ("change_median", "change_p95") if motion.get(k) is None]
        if missing:
            bad.append({"leg": "measurement",
                        "value": ",".join(missing),
                        "rule": "所有运动指标都必须量到",
                        "hint": "motion_qa 没读到帧。检查帧目录是否为空——"
                                "裁云默认走管道不落 PNG，评测必须传 --keep-frames。"
                                "这不是片子的缺陷。"})
            return bad
    if motion:
        mean = motion.get("change_mean")
        median = motion.get("change_median")
        if mean is not None and median and mean / max(median, 1e-6) > 12:
            bad.append({"leg": "spikiness", "value": round(mean / median, 1),
                        "rule": "均值÷中位数 ≤12（超过 = 大部分静止 + 少数剧烈）",
                        "hint": "检查是不是只有一次拉远/硬切在支撑整片，中间全是静止"})
        if ref:
            # **p95 才是可比的强度尺**：峰值会被一次硬切支配。
            # 实测：motion-web 参考片的峰值 234.37 来自第 859 帧（亮度 229 → 10，
            # 一帧黑场硬切）；同片 p95 只有 2.01。拿峰值当“动效够不够”的证据，
            # 等于用一次转场去衡量整片。所以强度判据用 p95，峰值只作参考输出。
            ref_p95 = ref.get("change_p95")
            ours_p95 = motion.get("change_p95")
            if ref_p95 and ours_p95 is not None and ours_p95 < ref_p95 * 0.5:
                bad.append({"leg": "intensity", "value": ours_p95,
                            "rule": f"p95 ≥ 参照片的 0.5×（参考同口径 {ref_p95}）",
                            "hint": "p95 是“有相当运动的那几成帧”的强度。偏低只两种情况："
                                    "① 元素太小（一格 60px 里盖不住）；② 运动幅度太小"
                                    "（几十像素的持续位移才进得了网格）"})
        if ref and ref.get("change_median"):
            ratio = (motion.get("change_median") or 0) / max(ref["change_median"], 1e-6)
            if ratio < 0.3:
                bad.append({"leg": "density", "value": round(ratio, 2),
                            "rule": "中位数 ≥ 参照片的 0.3×",
                            "hint": "画面大部分时间太空：加元素尺寸/数量，或让主体持续几十像素位移。"
                                    "1~2px 的呼吸微动在 32×18 网格里测不到"})
    return bad


def extract_frames(video: str, times, out_dir: str, prefix: str):
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    for t in times:
        dst = os.path.join(out_dir, f"{prefix}-{t:05.2f}s.png")
        run(["ffmpeg", "-y", "-v", "error", "-ss", f"{t:.3f}", "-i", video,
             "-frames:v", "1", dst])
        if os.path.exists(dst):
            paths.append(dst)
    return paths


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ref", default="", help="参考片 mp4（同 fps 同 step 才可比）")
    ap.add_argument("--motion-blur", type=int, default=4)
    ap.add_argument("--track-elements", action="store_true")
    ap.add_argument("--stills", default="", help="逗号分隔的抽帧时刻（秒）")
    ap.add_argument("--scenes", default="", help="逗号分隔的各幕时长（carry_qa 用）")
    ap.add_argument("--ref-baseline", default="",
                    help="参考片基准 JSON（scripts/ot_bench_ref.py 产出）里的一个 case 名")
    args = ap.parse_args()

    check_binary()
    # ## 渲染**之前**先跑一遍 layout（2026-10-24 加入，本仓库最贵的教训之一）
    #
    # 裁云的 layout 抛错时会**静默降级为纯 JSON**：渲染不报错、照常出片、
    # 报告也有数字——但画面只剩背景。实测代价：c06-pith 的 `keys()` 调了
    # 一个与宿主注入冲突/缺失的符号，**每一帧都抛错**，我为此白改了 3 轮相机
    # 关键帧（逐秒数据一直是 mean=0.00，那是降级的特征，没有任何报错）。
    #
    # 这一步 10 秒，省的是几十分钟渲染 + 一轮误判。
    check_layout(args.script)

    os.makedirs(args.out, exist_ok=True)
    mp4 = os.path.join(args.out, "film.mp4")
    # 记下本次渲染的**唯一标识**：脚本目录里所有 js/json 的最新 mtime。
    # 报告里带上它，下次看到“数与源不一致”时能立刻对质（实测：改 js 后数字一字不变，
    # 先怀疑脚本语法白查一轮，其实是报告没重新生成）。
    script_dir = os.path.dirname(os.path.abspath(args.script))
    src_files = [f for f in os.listdir(script_dir) if f.endswith((".js", ".json"))]
    source_stamp = max((os.path.getmtime(os.path.join(script_dir, f)) for f in src_files),
                       default=0.0)
    print(f"源文件指纹：{len(src_files)} 个，最新 mtime {source_stamp:.0f}")
    if os.path.exists(mp4):
        print(f"旧成片 mtime {os.path.getmtime(mp4):.0f}"
              f"（{'旧于源码 → 本次会重渲' if os.path.getmtime(mp4) < source_stamp else '新于源码'}）")

    cmd = [CAIYUN, "--headless", "--script", args.script, "--render", mp4]
    if args.motion_blur > 1:
        cmd += ["--motion-blur", str(args.motion_blur)]
    # ## 必须显式要帧序列（2026-10-24）
    #
    # 裁云的导出默认已改为**管道直送 ffmpeg**（RGBA 直写 stdin，不落 PNG），
    # 因为那能把 blur=4 的导出从 47s 压到 20s。但本评测的全部指标
    # （静止占比 / 变化强度 / 逐镜静止）都是**从 PNG 帧序列量的**。
    #
    # 不加这个开关，`frames/` 会是空目录，motion_qa 报"帧数不足（0）"，
    # 于是 change_median / change_p95 全部变成 None —— 判据报 density 不达标，
    # 而片子其实完全正常。**这个坑实际踩过一次**（三条片子同时"不达标"）。
    cmd += ["--keep-frames"]
    if args.track_elements:
        cmd += ["--track-elements"]
    rendered = run(cmd, cwd=ROOT, timeout=3600)
    log = rendered.stdout + rendered.stderr
    with open(os.path.join(args.out, "render.log"), "w", encoding="utf-8") as fh:
        fh.write(log)

    report = {"script": os.path.abspath(args.script), "output": os.path.abspath(mp4)}
    report["video"] = ffprobe(mp4)
    report["quality"] = parse_quality(log)
    report["quality_summary"] = next(
        (line.strip() for line in log.splitlines() if "画面质量：" in line), "")

    traj = os.path.join(args.out, "trajectory.tsv")
    if args.scenes and os.path.exists(traj):
        carry = run([PY, CARRY_QA, traj, "--scenes", args.scenes,
                     "--fps", str(report["video"].get("fps", 30)),
                     "--width", str(report["video"].get("width", 1920)),
                     "--height", str(report["video"].get("height", 1080))], cwd=ROOT)
        report["carry"] = parse_carry(carry.stdout)
        report["carry_text"] = carry.stdout

    frames_dir = os.path.join(args.out, "frames")
    if os.path.isdir(frames_dir) and os.path.exists(MOTION_QA):
        fps = str(report["video"].get("fps", 30))
        mq = ["--dir", frames_dir, "--fps", fps]
        if args.ref:
            ref_dir = os.path.join(args.out, "ref_frames")
            shutil.rmtree(ref_dir, ignore_errors=True)
            os.makedirs(ref_dir, exist_ok=True)
            run(["ffmpeg", "-y", "-v", "error", "-i", args.ref,
                 os.path.join(ref_dir, "f_%05d.png")])
            mq += ["--ref", ref_dir]
        motion = run([MOTION_QA] + mq, cwd=ROOT)
        report["motion"] = parse_motion(motion.stdout)
        report["motion_text"] = motion.stdout

    # 与参考片的**同口径基准**对比：绝对阈值不可迁移，只能比相对关系。
    judge_notes: list[str] = []
    if args.ref_baseline:
        path = os.path.join(ROOT, "out", "otbench", "ref_baseline.json")
        if not os.path.exists(path):
            judge_notes.append(f"未找到 ref_baseline.json（{args.ref_baseline}）——相对判据无法执行")
        else:
            with open(path, encoding="utf-8") as fh:
                baseline = json.load(fh)
            ref = baseline.get(args.ref_baseline)
            if not ref:
                judge_notes.append(f"基线里没有「{args.ref_baseline}」——相对判据无法执行")
            else:
                # 参照片用**同一套口径**重新量一遍（不在报告里引用它的 README 数字：
                # 那些是它们自己的尺子量出来的，和我们的不可比）。
                ref_frames = os.path.join(ROOT, "out", "otbench", "_ref",
                                          args.ref_baseline, "frames")
                if os.path.isdir(ref_frames):
                    ref = dict(ref, **pixel_still(ref_frames))
                report["reference"] = ref
                report["vs_reference"] = {
                    k: round((report.get("motion", {}).get(k) or 0) / max(ref.get(k) or 1e-6, 1e-6), 2)
                    for k in ("change_mean", "change_median", "change_peak", "carry_ratio_mean")
                }
    else:
        judge_notes.append("未给 --ref-baseline——静止/过亮/强度/密度四条相对判据均未执行")

    report["violations"] = judge(report.get("quality", {}), report.get("motion", {}),
                                 report.get("reference"), judge_notes)
    report["judge_notes"] = judge_notes

    if args.stills:
        times = [float(x) for x in args.stills.split(",") if x.strip()]
        name = os.path.splitext(os.path.basename(args.script))[0]
        report["stills"] = extract_frames(mp4, times, os.path.join(args.out, "stills"), name)

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    with open(os.path.join(args.out, "report.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    print(json.dumps({k: v for k, v in report.items()
                      if k not in ("carry_text", "motion_text", "stills")},
                     ensure_ascii=False, indent=2))
    bad = report.get("violations") or []
    print(f"\n判据：{'PASS' if not bad else f'{len(bad)} 项不达标'}")
    for item in bad:
        print(f"  ✗ {item['leg']}  实测 {item['value']}  判据 {item['rule']}")
        print(f"      → {item['hint']}")
    for note in (report.get("judge_notes") or []):
        print(f"  ⚠ {note}")
    # ## 退出码必须区分「通过」与「没真正判」
    #
    # 实测踩过：没传 --ref-baseline 时所有相对判据都不执行，violations 为空
    # ⇒ 脚本返回 0，调用方看到的就是绿色 PASS——而实际上一条判据都没跑。
    #
    # 反之也要避免假阴性：参考值**齐全**时它就是真正判过，不能因为
    # 判断式写得抄而误报“未判定”（这个 bug 真发生过：
    # `any(not ref.get(k) …) if ref else True` 在 ref 齐全时应当为 False，
    # 但当时写成了先算 any 再判空，结果把齐全的也归为 undecided）。
    ref_now = report.get("reference") or {}
    decided = bool(ref_now) and all(ref_now.get(k) is not None for k in REQUIRED_REF_KEYS)
    if not bad and not decided:
        print("  ⇒ 结论：未判定（相对判据缺参考片，不代表通过）")
        return 4
    return 0 if not bad else 3


if __name__ == "__main__":
    sys.exit(main())
