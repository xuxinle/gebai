#!/usr/bin/env python3
"""七个风格模板示例的渲染驱动。

## 为什么每个示例只有一镜

这七个工程是**风格模板的参照物**，不是七部片子。一镜（4~6s）刚好够
读出一个模板的「承接方式」——而那正是模板之间唯一的本质差别。
做成一整部片会让每个模板的特性被叙事稀释，反而不好对照。

## 产物落哪里

**一律落会话目录**（`--out`，默认取环境或参数）；本目录只放脚本与构建脚本。
理由见系统提示词「八、交付」：代码目录只放框架代码与入库脚本，
产物进代码目录会被 `git clean` 的入仓检查弄丢，且并行时同名互相覆盖。

## 用法

    # 渲一个模板（自动 build.py 拼 kit + 渲染）
    py render_all.py --only 01 --out <会话目录>/styles

    # 全渲
    py render_all.py --out <会话目录>/styles
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# kit 与 styles 同在 examples/caiyun/ 下
KIT_BUILD = os.path.join(HERE, "..", "kit", "build.py")

# 每个示例的目录名（同时也是镜头脚本名）
STYLES = [
    "01-capability-list",
    "02-single-point",
    "03-causal-chain",
    "04-scale-dive",
    "05-contrast-split",
    "06-spine",
    "07-pointer",
]

# 裁云可执行文件：优先 dev 档（快），可用环境变量覆盖
def caiyun_exe(profile: str) -> str:
    """裁云可执行文件。

    本文件在 `shuangtian/examples/caiyun/styles/`，框架根是向上**三级**：
    styles → caiyun → examples → shuangtian。
    """
    root = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
    return os.path.join(root, "build", profile, "bin", "caiyun.exe")


def build_bundle(style_dir: str) -> str:
    """把 kit 与新示例的 layout 拼成单文件（裁云无模块系统）。"""
    body = os.path.join(style_dir, "layout.js")
    out = os.path.join(style_dir, "_build", "bundle.js")
    # Windows 控制台默认 GBK：裁云输出是 UTF-8，不指定会 UnicodeDecodeError
    rc = subprocess.run([sys.executable, os.path.abspath(KIT_BUILD),
                         "--script", body, "--out", out],
                        capture_output=True, text=True,
                        encoding="utf-8", errors="replace")
    if rc.returncode != 0:
        raise RuntimeError("build.py 失败：\n" + rc.stdout + rc.stderr)
    return out


def render_style(style: str, out_dir: str, profile: str, exe: str) -> dict:
    style_dir = os.path.join(HERE, style)
    if not os.path.isdir(style_dir):
        raise RuntimeError("缺目录 " + style_dir)

    bundle = build_bundle(style_dir)

    # json 里的 layout 指向临时 bundle 的相对路径
    src_json = os.path.join(style_dir, "film.json")
    data = json.load(open(src_json, encoding="utf-8"))
    rel = os.path.relpath(bundle, style_dir).replace("\\", "/")
    for scene in data.get("scenes", []):
        if "layout" in scene:
            scene["layout"] = rel

    # ## resolved json 必须与 layout 路径**同一基准**（踩过，必看）
    #
    # 裁云的 `asset_basis` = **json 文件所在目录**（`director.cpp` 的
    # `parse_layout_script(node, asset_basis)`），而 `layout` 字符串按它解析。
    #
    # 早先把 resolved json 写在 `_build/`、而 `layout` 写 `_build/bundle.js`，
    # 于是它去找 `_build/_build/bundle.js` —— 找不到 →**静默降级为纯 JSON**
    # → 画面停在初始帧、无任何报错。实测为此重新渲了三轮才定位。
    #
    # 现在：resolved json 写在 style_dir 根（与 film.json 同级），
    # `layout` 写相对 style_dir 的 `_build/bundle.js`，两边基准一致。
    os.makedirs(out_dir, exist_ok=True)
    mp4 = os.path.join(out_dir, style + ".mp4")

    work_json = os.path.join(style_dir, "_build", "film.resolved.json")
    os.makedirs(os.path.dirname(work_json), exist_ok=True)
    # 关键：layout 路径必须是**相对 style_dir**（resolved json 也放 _build 时
    # 必须写成 ../_build/bundle.js 才对；写 _build/... 会多一层）
    rel = os.path.relpath(bundle, os.path.dirname(work_json)).replace("\\", "/")
    for scene in data.get("scenes", []):
        if "layout" in scene:
            scene["layout"] = rel
    json.dump(data, open(work_json, "w", encoding="utf-8"), ensure_ascii=False, indent=1)

    rc = subprocess.run([exe, "--headless", "--script", work_json, "--render", mp4],
                        cwd=style_dir, capture_output=True, text=True,
                        encoding="utf-8", errors="replace")
    if rc.returncode != 0:
        raise RuntimeError("渲染失败 %s\n%s\n%s" % (style, rc.stdout[-2000:], rc.stderr[-2000:]))

    # ## 静默降级检查（本驱动最重要的一段）
    #
    # 裁云的 layout 脚本出错时**降级为纯 JSON 且照常出片**——渲染成功、退出码 0、
    # 成片有画面，只是所有动效消失、画面停在初始帧。实测为此外观上"完全正常"，
    # 白渲染了三轮才定位（`.probe.mjs` 里 `seg` 重复声明、`txtK` 未定义各一次）。
    #
    # 判据不是"看日志"，而是**量两件事**：
    #   ① 日志里有没有 `脚本编排层装载失败`
    #   ② 首帧与末帧的差异：脚本若生效，两帧必然不同（所有示例都有入场动效）
    warn = []
    if "装载失败" in rc.stderr or "装载失败" in rc.stdout:
        warn.append("裁云报告脚本装载失败（已降级为纯 JSON）")

    try:
        import numpy as np
        from PIL import Image

        dur = _probe_duration(mp4)
        with tempfile.TemporaryDirectory() as tmp:
            # 沿时间轴取 12 个点，量相邻点的差异曲线。
            #
            # ## 为什么不能用"首帧 vs 末帧"（逆向验证时抓到）
            #
            # 第一版就是这么写的，回退缺陷后它**没报警**（maxdiff=140）：
            # 因为静默降级时纯 JSON 的 `enter` 入场动画照样让首末帧不同。
            # 判据必须能区分"JSON 的入场"与"脚本的动效"——
            # 前者的差异集中在**开头 0.5s**，之后完全平坦；
            # 后者在整个片长内分段出现（这正是 layout 做的事）。
            # 取 12 个时间点，量**中段**的逐点最大差。
            #
            # ## 判据演进（两次逆向验证都失败才定成现在这版）
            #
            # ① "首帧 vs 末帧" → 回退缺陷后仍报 maxdiff=140（JSON 的 enter 入场就有差异）；
            # ② "尾段 1/3 有变化" → 仍报 112，因为 **`fade_out` 片尾淡出**就是一个大跳变
            #    （实测降级版 5.17s 处 maxdiff=112，而中段只有 7~14）；
            # ③ 现在：**只用中间 1/4~3/4 这一段**，跳开开头的 enter 与结尾的 fade，
            #    并看均值而不只是峰值（峰值会被单帧噪声带偏）。
            #
            # 实测对照：正常版中段平均 maxdiff ≈ 42、降级版 ≈ 12。
            n = 12
            frames = []
            for i in range(n):
                ts = dur * (i + 0.5) / n
                fp = os.path.join(tmp, "f%02d.png" % i)
                subprocess.run(["ffmpeg", "-v", "error", "-ss", "%.3f" % ts, "-i", mp4,
                                "-frames:v", "1", "-y", fp], check=True)
                frames.append(np.asarray(Image.open(fp).convert("L"), dtype=np.float32))

            maxds = [float(np.abs(frames[i] - frames[i - 1]).max())
                     for i in range(1, len(frames))]
            lo, hi = len(maxds) // 4, (len(maxds) * 3) // 4
            mid = maxds[lo:hi] if hi > lo else maxds
            mid_mean = float(np.mean(mid))
            mid_peak = float(max(mid))

            # 阈值 20：正常示例中段均值 42+，降级版 12
            if mid_mean < 20.0:
                warn.append(
                    "中段几乎不动（均值 maxdiff=%.1f，峰值 %.1f）：layout 很可能没生效"
                    % (mid_mean, mid_peak))

            result = {"style": style, "mp4": mp4, "bytes": os.path.getsize(mp4),
                      "mid_mean_maxdiff": mid_mean, "mid_peak_maxdiff": mid_peak}
    except Exception as exc:            # noqa: BLE001 — 检查失败不应阻断渲染
        warn.append("动效检查跳过：%s" % exc)
        result = {"style": style, "mp4": mp4, "bytes": os.path.getsize(mp4)}

    if warn:
        result["warn"] = warn
    return result


def _probe_duration(mp4: str) -> float:
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                        "-of", "csv=p=0", mp4],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 1.0


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="产物目录（会话目录下）")
    ap.add_argument("--only", default="", help="只渲某一个，如 01")
    ap.add_argument("--profile", default="dev")
    args = ap.parse_args()

    exe = caiyun_exe(args.profile)
    if not os.path.isfile(exe):
        print("找不到裁云可执行文件：%s" % exe, file=sys.stderr)
        return 1

    want = [s for s in STYLES if not args.only or s.startswith(args.only)]
    ok, fail = [], []
    for style in want:
        try:
            r = render_style(style, args.out, args.profile, exe)
            ok.append(r)
            flag = ""
            if r.get("warn"):
                flag = "  ⚠ " + "；".join(r["warn"])
            print("  ok   %-20s %8.1f KB  (中段均值 %.0f / 峰值 %.0f)%s" % (
                r["style"], r["bytes"] / 1024.0, r.get("mid_mean_maxdiff", 0), r.get("mid_peak_maxdiff", 0), flag))
        except Exception as exc:      # noqa: BLE001 — 驱动脚本，逐条报错继续
            fail.append((style, str(exc)))
            print("  FAIL %-20s %s" % (style, str(exc).splitlines()[0]))

    print("\n成功 %d / 失败 %d" % (len(ok), len(fail)))
    for style, msg in fail:
        print("\n--- %s ---\n%s" % (style, msg))
    return 0 if not fail else 1


if __name__ == "__main__":
    sys.exit(main())
