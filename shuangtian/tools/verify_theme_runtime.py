#!/usr/bin/env python3
"""运行时验证（2026-11-23 主题重构）：**看像素**，不看存档。

## 为什么必须是像素

本次重构的经验教训：用 `get` 回读 `top_highlight` 做断言是**假绿**——
那条路径读的是 `Style` 字段，而 `set` 本身就写进了 `Style`，
于是“声明是否被 `apply_theme` 盖掉”根本测不出来（把 `apply_declared_paint`
整个删掉它依然 PASS）。唯一不可骗的观测是**渲染出来的像素**。

## 两个判据

1. **显式声明活过多帧**：`set top_highlight=<亮橙>` 后截图，卡片顶部边缘带出现
   明显的橙色；**连跑 ≥10 帧**每帧都仍在（旧缺陷是“第二帧被 `Card::apply_theme`
   按档位重写成透明”）。
2. **主题变更真的下发**：`theme mode=light` 后整帧平均亮度显著上升（暗→亮）。

## 退出码

0 = 全过；非 0 = 失败（逐条打印差异，便于定位是第几帧坏的）。
"""
import base64
import io
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import st_client_lib  # noqa: E402

from PIL import Image  # noqa: E402

ROOT = os.environ.get("SHUANGTIAN_ROOT",
                      os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
PROFILE = os.environ.get("ST_PROFILE", "dev")
APP = os.environ.get("ST_APP", "gallery")
TMP = os.environ.get("ST_VTMP", os.path.join(ROOT, "build", "theme-verify"))
os.makedirs(TMP, exist_ok=True)
CTL = os.path.join(TMP, "control.json")
LOG = os.path.join(TMP, "app.log")
FRAMES = int(os.environ.get("ST_FRAMES", "12"))
TARGET = os.environ.get("ST_TARGET_ID", "stat-backend")
# 亮橙：在亮/暗两套主题下都与卡片底色（浅灰 / 深灰）差异极大，不会被误判。
DECLARED = os.environ.get("ST_DECLARED_HIGHLIGHT", "#ff8800")

if os.path.exists(CTL):
    os.remove(CTL)


def start_app():
    import subprocess

    log = open(LOG, "wb")
    # 必须持有进程句柄并在结束时 kill：不回收会占着 exe，
    # 下一次 `st build` 链接报 “failed to write output ... permission denied”。
    process = subprocess.Popen(
        [os.path.join(ROOT, "build", PROFILE, "bin", APP), "--headless",
         "--control-port", "0", "--control-file", CTL],
        stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(120):
        if os.path.exists(CTL):
            try:
                info = json.load(open(CTL))
                if info.get("port"):
                    return info["port"], info.get("token", ""), process
            except Exception:
                pass
        time.sleep(0.25)
    process.kill()
    raise SystemExit("控制通道没起来（看 %s）" % LOG)


def main():
    port, token, process = start_app()
    print(f"[{APP}] port={port}")
    try:
        return run_checks(port, token)
    finally:
        try:
            process.kill()
            process.wait(timeout=10)
        except Exception:
            pass


def run_checks(port, token):
    def call(method, params=None):
        return st_client_lib.call_with_token(port, method, params or {}, token)

    failures = []

    def check(ok, message):
        print(("  PASS " if ok else "  FAIL ") + message)
        if not ok:
            failures.append(message)

    def grab(region=None):
        params = {}
        if region is not None:
            params["region"] = region
        result = call("capture", params).get("result") or {}
        raw = base64.b64decode(result["base64"])
        return Image.open(io.BytesIO(raw)).convert("RGB")

    def mean_luma(image):
        pixels = list(image.getdata())
        return sum(0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in pixels) / len(pixels)

    def orange_count(image):
        """数“明显偏橙”的像素（R 高、G 中等、B 低）——高光边的特征色。"""
        count = 0
        for r, g, b in image.getdata():
            if r > 180 and 90 < g < 200 and b < 110 and r - b > 90:
                count += 1
        return count

    # 找靶子（Card 的 apply_theme 无条件重写 top_highlight，是最严苛的对象）。
    listing = call("find", {"selector": "Card", "limit": 4}).get("result") or {}
    matches = listing.get("matches") or []
    chosen = None
    for item in matches:
        if item.get("id") == TARGET:
            chosen = item
            break
    if chosen is None and matches:
        chosen = matches[0]
    check(chosen is not None, f"找到靶子 Card（{chosen and chosen.get('id')}）")
    if chosen is None:
        return 1

    target_id = chosen["id"]
    bounds = chosen["bounds"]
    # 只截卡片顶部 6px 高的条带：高光边就画在那里，背景干扰最小。
    band = {
        "x": int(bounds["x"]),
        "y": int(bounds["y"]),
        "width": max(8, int(bounds["width"])),
        "height": 6,
    }

    before = orange_count(grab(band))
    print(f"  （靶子 {target_id} 顶部条带初始橙色像素 {before}）")

    reply = call("set", {"id": target_id, "props": {"top_highlight": DECLARED}})
    changed = (reply.get("result") or {}).get("changed") or []
    check("top_highlight" in changed, f"set top_highlight 被接受（changed={changed}）")

    first = orange_count(grab(band))
    check(first > before + 200,
          f"首帧顶部边缘出现橙色高光（{before} → {first} 像素）")

    survived = True
    for index in range(FRAMES):
        call("metrics")   # 驱动一帧
        value = orange_count(grab(band))
        if value <= before + 200:
            check(False, f"第 {index + 1} 帧高光消失了（橙色像素 {value}，基线 {before}）")
            survived = False
            break
    if survived:
        check(True, f"连跑 {FRAMES} 帧高光仍在（每帧都 > {before + 200} 橙色像素）")

    # 判据 2：切亮色 → 整帧平均亮度显著上升。
    dark_luma = mean_luma(grab())
    call("theme", {"mode": "light"})
    call("metrics")
    light_luma = mean_luma(grab())
    check(light_luma > dark_luma + 20.0,
          f"切亮色后整帧亮度上升（{dark_luma:.1f} → {light_luma:.1f}）")

    # 判据 3（**最关锰的一条**）：显式声明必须活过**主题变更**。
    #
    # 为什么它比“活过帧”更关锰：切主题会让 `Card::apply_theme` **真的重算一次**
    #（`theme_version_` 变了），而它会把 `top_highlight` 按档位重写成透明。
    # 这正是 `top_highlight_override` 当年存在的理由；也只有这条能区分
    # “声明回放（`apply_declared_paint`）在不在”—— 单看“多帧存活”区分不出（主题不重算，
    # 声明本来就不会被盖）。
    after_switch = orange_count(grab(band))
    check(after_switch > before + 200,
          f"切到亮色后高光仍在（橙色像素 {after_switch}）")

    # 再切回来：跨两次主题变更仍存活。
    call("theme", {"mode": "dark"})
    call("metrics")
    back = orange_count(grab(band))
    check(back > before + 200, f"切回暗色后高光仍在（橙色像素 {back}）")

    print()
    if failures:
        print(f"── 失败 {len(failures)} 项 ──")
        return 1
    print("── 全部通过 ──")
    return 0


if __name__ == "__main__":
    sys.exit(main())
